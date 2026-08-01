import Cocoa
import CoreGraphics
import IOKit.ps
import SceneAudio

/// Converts macOS host state into the explicit events consumed by the pure
/// playback-policy reducer. Detection has no playback side effects of its own.
@MainActor
final class PlaybackConditionMonitor {
    typealias EventHandler = (PlaybackPolicyEvent) -> Void
    typealias IssueHandler = (String?) -> Void

    private let eventHandler: EventHandler
    private let issueHandler: IssueHandler
    private var workspaceObservers: [NSObjectProtocol] = []
    private var notificationObservers: [NSObjectProtocol] = []
    private var pollTimer: Timer?
    private var previousFullscreenOrMaximized: Bool?
    private var previousAudio: Bool?
    private var previousBattery: Bool?
    private var displayAsleep = false
    private var audioDetectionEnabled = false
    private var audioDetectionRequestID = 0
    private var currentAudioIssue: String?
    private var audioCaptureLease: SceneAudioCaptureLease?

    init(
        eventHandler: @escaping EventHandler,
        issueHandler: @escaping IssueHandler = { _ in }
    ) {
        self.eventHandler = eventHandler
        self.issueHandler = issueHandler
    }

    func start() {
        guard pollTimer == nil else { return }
        let workspaceCenter = NSWorkspace.shared.notificationCenter
        workspaceObservers = [
            workspaceCenter.addObserver(
                forName: NSWorkspace.didActivateApplicationNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.refreshFullscreenOrMaximized() }
            },
            workspaceCenter.addObserver(
                forName: NSWorkspace.screensDidSleepNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.setDisplayAsleep(true) }
            },
            workspaceCenter.addObserver(
                forName: NSWorkspace.screensDidWakeNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.setDisplayAsleep(false) }
            },
        ]

        notificationObservers = [
            NotificationCenter.default.addObserver(
                forName: NSApplication.didChangeScreenParametersNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.refreshFullscreenOrMaximized() }
            },
            NotificationCenter.default.addObserver(
                forName: .NSProcessInfoPowerStateDidChange,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.refreshBattery() }
            },
        ]

        let timer = Timer(timeInterval: 0.5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.refreshFullscreenOrMaximized()
                self?.refreshAudio()
                self?.refreshBattery()
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        pollTimer = timer

        refreshFullscreenOrMaximized()
        refreshAudio()
        refreshBattery()
        eventHandler(.displayAsleep(displayAsleep))
    }

    /// Audio capture is a privacy-sensitive resource. Start it only when the
    /// selected policy actually needs an audio condition; SceneRuntime owns
    /// the same provider independently for wallpapers that use audio input.
    func setAudioDetectionEnabled(_ enabled: Bool) {
        guard audioDetectionEnabled != enabled else { return }
        audioDetectionEnabled = enabled
        audioDetectionRequestID &+= 1
        previousAudio = nil
        if !enabled {
            audioCaptureLease?.cancel()
            audioCaptureLease = nil
            reportAudioIssue(nil)
            eventHandler(.otherApplicationPlayingAudio(false))
            return
        }
        let requestID = audioDetectionRequestID
        reportAudioIssue(nil)
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let lease = try await SceneSystemAudioSpectrumProvider.shared.acquire()
                guard self.audioDetectionEnabled,
                      self.audioDetectionRequestID == requestID else {
                    lease.cancel()
                    return
                }
                self.audioCaptureLease?.cancel()
                self.audioCaptureLease = lease
                self.refreshAudio()
            } catch {
                guard self.audioDetectionEnabled,
                      self.audioDetectionRequestID == requestID else { return }
                self.reportAudioIssue(
                    "System audio detection unavailable: \(error.localizedDescription)"
                )
                self.eventHandler(.otherApplicationPlayingAudio(false))
            }
        }
    }

    func stop() {
        audioDetectionRequestID &+= 1
        audioDetectionEnabled = false
        audioCaptureLease?.cancel()
        audioCaptureLease = nil
        previousAudio = nil
        reportAudioIssue(nil)
        pollTimer?.invalidate()
        pollTimer = nil
        let workspaceCenter = NSWorkspace.shared.notificationCenter
        workspaceObservers.forEach(workspaceCenter.removeObserver)
        workspaceObservers.removeAll()
        notificationObservers.forEach(NotificationCenter.default.removeObserver)
        notificationObservers.removeAll()
    }

    private func setDisplayAsleep(_ value: Bool) {
        guard displayAsleep != value else { return }
        displayAsleep = value
        eventHandler(.displayAsleep(value))
    }

    private func refreshAudio() {
        guard audioDetectionEnabled else { return }
        let provider = SceneSystemAudioSpectrumProvider.shared
        if case .unavailable(let message) = provider.status {
            reportAudioIssue("System audio detection unavailable: \(message)")
            eventHandler(.otherApplicationPlayingAudio(false))
            return
        }
        if case .running = provider.status {
            reportAudioIssue(nil)
        }
        let value = provider.isOtherApplicationPlayingAudio
        guard previousAudio != value else { return }
        previousAudio = value
        eventHandler(.otherApplicationPlayingAudio(value))
    }

    private func reportAudioIssue(_ message: String?) {
        guard currentAudioIssue != message else { return }
        currentAudioIssue = message
        issueHandler(message)
    }

    private func refreshBattery() {
        let value = Self.isRunningOnBattery()
        guard previousBattery != value else { return }
        previousBattery = value
        eventHandler(.laptopOnBattery(value))
    }

    private func refreshFullscreenOrMaximized() {
        let value = Self.isOtherApplicationFullscreenOrMaximized()
        guard previousFullscreenOrMaximized != value else { return }
        previousFullscreenOrMaximized = value
        eventHandler(.otherApplicationFullscreenOrMaximized(value))
    }

    private static func isRunningOnBattery() -> Bool {
        guard let snapshot = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let sources = IOPSCopyPowerSourcesList(snapshot)?.takeRetainedValue()
                as? [CFTypeRef] else {
            return false
        }
        for source in sources {
            guard let description = IOPSGetPowerSourceDescription(snapshot, source)?
                    .takeUnretainedValue() as? [String: Any],
                  let state = description[kIOPSPowerSourceStateKey] as? String else {
                continue
            }
            if state == kIOPSBatteryPowerValue { return true }
            if state == kIOPSACPowerValue { return false }
        }
        return false
    }

    private static func isOtherApplicationFullscreenOrMaximized() -> Bool {
        guard let rawWindows = CGWindowListCopyWindowInfo(
            [.optionOnScreenOnly, .excludeDesktopElements],
            kCGNullWindowID
        ) as? [[String: Any]] else { return false }

        let displays: [PlaybackDisplayGeometry] = NSScreen.screens.compactMap { screen in
            guard let displayID = screen.deviceDescription[
                NSDeviceDescriptionKey("NSScreenNumber")
            ] as? CGDirectDisplayID else { return nil }
            return PlaybackDisplayGeometry(
                screenFrame: screen.frame,
                visibleFrame: screen.visibleFrame,
                quartzFrame: CGDisplayBounds(displayID)
            )
        }

        let windows: [PlaybackWindowCandidate] = rawWindows.compactMap { window in
            guard let ownerProcessID = window[kCGWindowOwnerPID as String] as? pid_t,
                  (window[kCGWindowLayer as String] as? Int) == 0,
                  let boundsDictionary = window[kCGWindowBounds as String] as? NSDictionary,
                  let bounds = CGRect(dictionaryRepresentation: boundsDictionary),
                  (window[kCGWindowAlpha as String] as? Double ?? 1) > 0 else {
                return nil
            }
            return PlaybackWindowCandidate(
                ownerProcessID: ownerProcessID,
                frame: bounds
            )
        }
        return PlaybackWindowGeometry.hasFullscreenOrMaximizedWindow(
            windows,
            on: displays,
            excludingOwnerProcessIDs: [getpid()]
        )
    }
}

/// A display represented in Quartz coordinates. `NSScreen.visibleFrame` uses
/// AppKit's bottom-left origin, so its menu-bar and Dock insets must be mapped
/// onto the top-left Quartz frame before comparing it with CGWindow bounds.
struct PlaybackDisplayGeometry: Equatable {
    let fullFrame: CGRect
    let usableFrame: CGRect

    init(fullFrame: CGRect, usableFrame: CGRect) {
        self.fullFrame = fullFrame
        self.usableFrame = usableFrame
    }

    init(screenFrame: CGRect, visibleFrame: CGRect, quartzFrame: CGRect) {
        let leftInset = visibleFrame.minX - screenFrame.minX
        let topInset = screenFrame.maxY - visibleFrame.maxY
        fullFrame = quartzFrame
        usableFrame = CGRect(
            x: quartzFrame.minX + leftInset,
            y: quartzFrame.minY + topInset,
            width: visibleFrame.width,
            height: visibleFrame.height
        )
    }
}

struct PlaybackWindowCandidate: Equatable {
    let ownerProcessID: pid_t
    let frame: CGRect
}

enum PlaybackWindowGeometry {
    static func hasFullscreenOrMaximizedWindow(
        _ windows: [PlaybackWindowCandidate],
        on displays: [PlaybackDisplayGeometry],
        excludingOwnerProcessIDs: Set<pid_t>
    ) -> Bool {
        windows.contains { window in
            !excludingOwnerProcessIDs.contains(window.ownerProcessID) &&
                isFullscreenOrMaximized(window.frame, on: displays)
        }
    }

    static func isFullscreenOrMaximized(
        _ windowFrame: CGRect,
        on displays: [PlaybackDisplayGeometry],
        tolerance: CGFloat = 2
    ) -> Bool {
        guard windowFrame.width > 0, windowFrame.height > 0 else { return false }
        return displays.contains { display in
            framesMatch(windowFrame, display.fullFrame, tolerance: tolerance) ||
                framesMatch(windowFrame, display.usableFrame, tolerance: tolerance)
        }
    }

    private static func framesMatch(
        _ windowFrame: CGRect,
        _ targetFrame: CGRect,
        tolerance: CGFloat
    ) -> Bool {
        abs(windowFrame.minX - targetFrame.minX) <= tolerance &&
            abs(windowFrame.minY - targetFrame.minY) <= tolerance &&
            abs(windowFrame.maxX - targetFrame.maxX) <= tolerance &&
            abs(windowFrame.maxY - targetFrame.maxY) <= tolerance
    }
}
