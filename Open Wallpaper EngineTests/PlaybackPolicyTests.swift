import AppKit
import XCTest
@testable import Open_Wallpaper_Engine

final class PlaybackPolicyTests: XCTestCase {
    func testLegacyDisplaySleepSettingDoesNotInvalidateOtherSettings() throws {
        let data = try XCTUnwrap(
            #"{"displayAsleep":"stop","otherApplicationFocused":"mute"}"#
                .data(using: .utf8)
        )

        let settings = try JSONDecoder().decode(GlobalSettings.self, from: data)

        XCTAssertEqual(settings.otherApplicationFocused, .mute)
        XCTAssertEqual(
            settings.playbackPolicyConfiguration.otherApplicationFocused,
            .mute
        )
    }

    func testDisplaySleepAppliesAnUnconditionalPause() {
        var state = PlaybackPolicyState()

        let transition = state.reduce(.displayAsleep(true))

        XCTAssertEqual(transition.previous, .keepRunning)
        XCTAssertEqual(transition.current, .pause)
        XCTAssertEqual(state.effectiveAction, .pause)
    }

    func testDisplayWakeRecomputesTheRemainingActiveConditions() {
        var state = PlaybackPolicyState(
            configuration: PlaybackPolicyConfiguration(
                otherApplicationFocused: .pause,
                otherApplicationFullscreen: .stop
            )
        )
        state.reduce(.otherApplicationFocused(true))
        state.reduce(.otherApplicationFullscreen(true))
        state.reduce(.displayAsleep(true))

        let wakeTransition = state.reduce(.displayAsleep(false))

        XCTAssertEqual(wakeTransition.previous, .stop)
        XCTAssertEqual(wakeTransition.current, .stop)
        XCTAssertEqual(state.effectiveAction, .stop)
    }

    func testDisplayWakeRemovesOnlyTheTemporaryPause() {
        var state = PlaybackPolicyState()
        state.reduce(.displayAsleep(true))

        let wakeTransition = state.reduce(.displayAsleep(false))

        XCTAssertEqual(wakeTransition.previous, .pause)
        XCTAssertEqual(wakeTransition.current, .keepRunning)
        XCTAssertEqual(state.effectiveAction, .keepRunning)
    }
}

@MainActor
final class PlaybackConditionMonitorTests: XCTestCase {
    func testScreenWakeClearsTheDisplaySleepCondition() {
        var displaySleepEvents: [Bool] = []
        let monitor = PlaybackConditionMonitor { event in
            guard case .displayAsleep(let isAsleep) = event else { return }
            displaySleepEvents.append(isAsleep)
        }
        monitor.start()
        defer { monitor.stop() }
        displaySleepEvents.removeAll()

        let workspaceCenter = NSWorkspace.shared.notificationCenter
        workspaceCenter.post(
            name: NSWorkspace.screensDidSleepNotification,
            object: nil
        )
        workspaceCenter.post(
            name: NSWorkspace.screensDidWakeNotification,
            object: nil
        )

        XCTAssertEqual(displaySleepEvents, [true, false])
    }
}
