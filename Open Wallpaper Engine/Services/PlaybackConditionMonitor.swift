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
    private var previousFullscreen: Bool?
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
                MainActor.assumeIsolated { self?.refreshFullscreen() }
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
                MainActor.assumeIsolated { self?.refreshFullscreen() }
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
                self?.refreshFullscreen()
                self?.refreshAudio()
                self?.refreshBattery()
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        pollTimer = timer

        refreshFullscreen()
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

    private func refreshFullscreen() {
        let value = Self.isOtherApplicationFullscreen()
        guard previousFullscreen != value else { return }
        previousFullscreen = value
        eventHandler(.otherApplicationFullscreen(value))
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

    private static func isOtherApplicationFullscreen() -> Bool {
        guard let application = NSWorkspace.shared.frontmostApplication,
              application.bundleIdentifier != Bundle.main.bundleIdentifier,
              application.bundleIdentifier != "com.apple.finder" else {
            return false
        }
        guard let rawWindows = CGWindowListCopyWindowInfo(
            [.optionOnScreenOnly, .excludeDesktopElements],
            kCGNullWindowID
        ) as? [[String: Any]] else { return false }

        let displayBounds: [CGRect] = NSScreen.screens.compactMap { screen in
            guard let displayID = screen.deviceDescription[
                NSDeviceDescriptionKey("NSScreenNumber")
            ] as? CGDirectDisplayID else { return nil }
            return CGDisplayBounds(displayID)
        }
        let tolerance: CGFloat = 2

        return rawWindows.contains { window in
            guard (window[kCGWindowOwnerPID as String] as? pid_t) == application.processIdentifier,
                  (window[kCGWindowLayer as String] as? Int) == 0,
                  let boundsDictionary = window[kCGWindowBounds as String] as? NSDictionary,
                  let bounds = CGRect(dictionaryRepresentation: boundsDictionary),
                  (window[kCGWindowAlpha as String] as? Double ?? 1) > 0 else {
                return false
            }
            return displayBounds.contains { display in
                abs(bounds.minX - display.minX) <= tolerance &&
                    abs(bounds.minY - display.minY) <= tolerance &&
                    abs(bounds.width - display.width) <= tolerance &&
                    abs(bounds.height - display.height) <= tolerance
            }
        }
    }
}
