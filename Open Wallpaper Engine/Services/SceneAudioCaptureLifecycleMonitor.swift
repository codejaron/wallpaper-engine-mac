import AppKit
import SceneAudio

@MainActor
final class SceneAudioCaptureLifecycleMonitor {
    private enum SuspensionReason: Hashable {
        case sessionInactive
        case screensAsleep
    }

    private let provider: SceneSystemAudioSpectrumProvider
    private var observers: [NSObjectProtocol] = []
    private var suspensionReasons: Set<SuspensionReason> = []

    init(provider: SceneSystemAudioSpectrumProvider = .shared) {
        self.provider = provider
    }

    func start() {
        guard observers.isEmpty else { return }
        suspensionReasons.removeAll()
        provider.setCaptureSuspended(false)

        let center = NSWorkspace.shared.notificationCenter
        observers = [
            center.addObserver(
                forName: NSWorkspace.sessionDidResignActiveNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.setSuspended(true, for: .sessionInactive)
                }
            },
            center.addObserver(
                forName: NSWorkspace.sessionDidBecomeActiveNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.setSuspended(false, for: .sessionInactive)
                }
            },
            center.addObserver(
                forName: NSWorkspace.screensDidSleepNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.setSuspended(true, for: .screensAsleep)
                }
            },
            center.addObserver(
                forName: NSWorkspace.screensDidWakeNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.setSuspended(false, for: .screensAsleep)
                }
            },
        ]
    }

    func stop() {
        let center = NSWorkspace.shared.notificationCenter
        observers.forEach(center.removeObserver)
        observers.removeAll()
        suspensionReasons.removeAll()
        provider.setCaptureSuspended(true)
    }

    private func setSuspended(_ suspended: Bool, for reason: SuspensionReason) {
        if suspended {
            suspensionReasons.insert(reason)
        } else {
            suspensionReasons.remove(reason)
        }
        provider.setCaptureSuspended(!suspensionReasons.isEmpty)
    }
}
