import AppKit
import SwiftUI
import WebKit
import XCTest
@testable import Open_Wallpaper_Engine

final class PlaybackPolicyTests: XCTestCase {
    @MainActor
    func testSceneMediaProviderAdvancesOnlyTheChangedEventRevision() throws {
        let provider = SceneMediaSnapshotProvider()
        let content: (
            Double, SceneMediaPlaybackState, SceneMediaThumbnailRGBA8?
        ) -> SceneMediaContent = { position, playback, thumbnail in
            SceneMediaContent(
                playbackState: playback,
                title: "Track",
                artist: "Artist",
                contentType: "music",
                albumTitle: "Album",
                subTitle: "",
                albumArtist: "Artist",
                genres: "",
                position: position,
                duration: 120,
                thumbnail: thumbnail,
                primaryColor: .black,
                secondaryColor: .black,
                tertiaryColor: .black,
                textColor: .black,
                highContrastColor: .black
            )
        }

        try provider.publish(content(1, .playing, nil))
        let initialSnapshot = provider.snapshot
        guard case .available(let initial, _) = provider.snapshot else {
            return XCTFail("Expected an available media snapshot")
        }
        XCTAssertEqual(
            initial,
            SceneMediaRevisions(
                status: 1,
                metadata: 1,
                playback: 1,
                timeline: 1,
                thumbnail: 1
            )
        )

        try provider.publish(content(2, .playing, nil))
        let timelineSnapshot = provider.snapshot
        guard case .available(let timelineOnly, _) = provider.snapshot else {
            return XCTFail("Expected an available media snapshot")
        }
        XCTAssertEqual(timelineOnly.status, initial.status)
        XCTAssertEqual(timelineOnly.metadata, initial.metadata)
        XCTAssertEqual(timelineOnly.playback, initial.playback)
        XCTAssertEqual(timelineOnly.timeline, initial.timeline + 1)
        XCTAssertEqual(timelineOnly.thumbnail, initial.thumbnail)
        XCTAssertFalse(
            timelineSnapshot.hasThumbnailUpdate(since: initialSnapshot)
        )

        let cover = SceneMediaThumbnailRGBA8(
            width: 1,
            height: 1,
            bytesPerRow: 4,
            pixels: Data([0, 255, 0, 255])
        )
        try provider.publish(content(2, .playing, cover))
        let thumbnailSnapshot = provider.snapshot
        guard case .available(let thumbnailOnly, _) = provider.snapshot else {
            return XCTFail("Expected an available media snapshot")
        }
        XCTAssertEqual(thumbnailOnly.timeline, timelineOnly.timeline)
        XCTAssertEqual(thumbnailOnly.thumbnail, timelineOnly.thumbnail + 1)
        XCTAssertTrue(
            thumbnailSnapshot.hasThumbnailUpdate(since: timelineSnapshot)
        )

        try provider.markUnavailable()
        guard case .unavailable(let unavailable) = provider.snapshot else {
            return XCTFail("Expected an unavailable media snapshot")
        }
        XCTAssertEqual(unavailable.status, thumbnailOnly.status + 1)
        XCTAssertEqual(unavailable.metadata, thumbnailOnly.metadata)
        XCTAssertEqual(unavailable.playback, thumbnailOnly.playback)
        XCTAssertEqual(unavailable.timeline, thumbnailOnly.timeline)
        XCTAssertEqual(unavailable.thumbnail, thumbnailOnly.thumbnail)
        XCTAssertFalse(
            provider.snapshot.hasThumbnailUpdate(since: thumbnailSnapshot),
            "Losing media availability must not clear an unchanged cover"
        )
    }

    func testOnlyStopUnloadsTheWallpaperRuntime() {
        XCTAssertTrue(GSPlayback.keepRunning.keepsRuntimeLoaded)
        XCTAssertTrue(GSPlayback.mute.keepsRuntimeLoaded)
        XCTAssertTrue(GSPlayback.pause.keepsRuntimeLoaded)
        XCTAssertFalse(GSPlayback.stop.keepsRuntimeLoaded)
    }

    func testAudioPlaybackRuleRequiresSystemAudioCaptureOnlyWhenActive() {
        var settings = GlobalSettings()

        XCTAssertFalse(settings.requiresSystemAudioCaptureForAudioRule)

        settings.otherApplicationPlayingAudio = .mute
        XCTAssertTrue(settings.requiresSystemAudioCaptureForAudioRule)

        settings.systemAudioCaptureEnabled = true
        XCTAssertFalse(settings.requiresSystemAudioCaptureForAudioRule)

        settings.systemAudioCaptureEnabled = false
        settings.otherApplicationPlayingAudio = .pause
        XCTAssertTrue(settings.requiresSystemAudioCaptureForAudioRule)

        settings.otherApplicationPlayingAudio = .keepRunning
        XCTAssertFalse(settings.requiresSystemAudioCaptureForAudioRule)
    }

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

    func testPersistedFullscreenSettingFeedsFullscreenOrMaximizedPolicy() throws {
        let data = try XCTUnwrap(
            #"{"otherApplicationFullscreen":"pause"}"#.data(using: .utf8)
        )

        let settings = try JSONDecoder().decode(GlobalSettings.self, from: data)

        XCTAssertEqual(
            settings.playbackPolicyConfiguration
                .otherApplicationFullscreenOrMaximized,
            .pause
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
                otherApplicationFullscreenOrMaximized: .stop
            )
        )
        state.reduce(.otherApplicationFocused(true))
        state.reduce(.otherApplicationFullscreenOrMaximized(true))
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

    func testFullscreenGeometryAcceptsWholeDisplayAndUsableDesktopArea() {
        let display = PlaybackDisplayGeometry(
            fullFrame: CGRect(x: 0, y: 0, width: 1_470, height: 956),
            usableFrame: CGRect(x: 0, y: 34, width: 1_470, height: 922)
        )

        XCTAssertTrue(
            PlaybackWindowGeometry.isFullscreenOrMaximized(
                CGRect(x: 0, y: 0, width: 1_470, height: 956),
                on: [display]
            )
        )
        XCTAssertTrue(
            PlaybackWindowGeometry.isFullscreenOrMaximized(
                CGRect(x: 0, y: 34, width: 1_470, height: 922),
                on: [display]
            )
        )
    }

    func testFullscreenGeometryRejectsLargeWindowThatDoesNotReachUsableEdges() {
        let display = PlaybackDisplayGeometry(
            fullFrame: CGRect(x: 0, y: 0, width: 1_470, height: 956),
            usableFrame: CGRect(x: 0, y: 34, width: 1_470, height: 922)
        )

        XCTAssertFalse(
            PlaybackWindowGeometry.isFullscreenOrMaximized(
                CGRect(x: 12, y: 46, width: 1_446, height: 898),
                on: [display]
            )
        )
    }

    func testFullscreenDetectionFindsBackgroundAppBehindSmallFrontWindow() {
        let display = PlaybackDisplayGeometry(
            fullFrame: CGRect(x: 0, y: 0, width: 1_470, height: 956),
            usableFrame: CGRect(x: 0, y: 34, width: 1_470, height: 922)
        )
        let frontToBackWindows = [
            PlaybackWindowCandidate(
                ownerProcessID: 100,
                frame: CGRect(x: 200, y: 180, width: 700, height: 500)
            ),
            PlaybackWindowCandidate(
                ownerProcessID: 200,
                frame: display.usableFrame
            ),
        ]

        XCTAssertTrue(
            PlaybackWindowGeometry.hasFullscreenOrMaximizedWindow(
                frontToBackWindows,
                on: [display],
                excludingOwnerProcessIDs: [999]
            )
        )
    }

    func testFullscreenDetectionExcludesWallpaperEngineWindows() {
        let display = PlaybackDisplayGeometry(
            fullFrame: CGRect(x: 0, y: 0, width: 1_470, height: 956),
            usableFrame: CGRect(x: 0, y: 34, width: 1_470, height: 922)
        )

        XCTAssertFalse(
            PlaybackWindowGeometry.hasFullscreenOrMaximizedWindow(
                [
                    PlaybackWindowCandidate(
                        ownerProcessID: 999,
                        frame: display.fullFrame
                    ),
                ],
                on: [display],
                excludingOwnerProcessIDs: [999]
            )
        )
    }

    func testUsableDisplayGeometryConvertsAppKitInsetsToQuartzCoordinates() {
        let display = PlaybackDisplayGeometry(
            screenFrame: CGRect(x: 0, y: 0, width: 1_470, height: 956),
            visibleFrame: CGRect(x: 0, y: 0, width: 1_470, height: 922),
            quartzFrame: CGRect(x: 0, y: 0, width: 1_470, height: 956)
        )

        XCTAssertEqual(
            display.usableFrame,
            CGRect(x: 0, y: 34, width: 1_470, height: 922)
        )
    }
}

@MainActor
final class WallpaperWindowPlaybackSuppressionTests: XCTestCase {
    func testWallpaperHostingDisablesContentDrivenWindowSizing() {
        let window = WallpaperWindow()
        window.setWallpaperContent(EmptyView())
        guard let hostingView = window.contentView as? NSHostingView<EmptyView> else {
            XCTFail("Wallpaper content was not installed in an NSHostingView")
            return
        }

        XCTAssertEqual(hostingView.sizingOptions, [])
    }

    func testLeavingStopRestoresWindowWithoutUsingPriorVisibilityAsIntent() async {
        let appDelegate = AppDelegate.shared
        let previousWindows = appDelegate.wallpaperWindows
        let previousAction = appDelegate.wallpaperViewModel.playbackPolicyAction
        let previousSuppression = appDelegate.playbackSuppressesWallpaperWindows
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 100, height: 100),
            styleMask: .borderless,
            backing: .buffered,
            defer: false
        )
        let viewModel = GlobalSettingsViewModel()
        defer {
            viewModel.stopPlaybackPolicyMonitoring(restorePlayback: false)
            window.close()
            appDelegate.wallpaperWindows = previousWindows
            appDelegate.wallpaperViewModel.setPlaybackPolicyAction(previousAction)
            appDelegate.setWallpaperWindowsSuppressedForPlayback(
                previousSuppression
            )
        }

        appDelegate.wallpaperWindows = ["playback-suppression-test": window]
        window.orderOut(nil)

        let configuration = PlaybackPolicyConfiguration(
            laptopOnBattery: .stop
        )
        viewModel.updatePlaybackPolicy(.configurationChanged(configuration))
        viewModel.updatePlaybackPolicy(.laptopOnBattery(true))

        XCTAssertEqual(
            appDelegate.wallpaperViewModel.playbackPolicyAction,
            .stop
        )
        XCTAssertFalse(window.isVisible)

        viewModel.updatePlaybackPolicy(.laptopOnBattery(false))
        let restorationScheduled = expectation(
            description: "Wallpaper window restoration was scheduled"
        )
        DispatchQueue.main.async {
            restorationScheduled.fulfill()
        }
        await fulfillment(of: [restorationScheduled], timeout: 1)

        XCTAssertEqual(
            appDelegate.wallpaperViewModel.playbackPolicyAction,
            .keepRunning
        )
        XCTAssertTrue(window.isVisible)
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

@MainActor
final class RuntimeResourceLifecycleTests: XCTestCase {
    func testVideoPlayerReleasesAndRecreatesItsCurrentItem() {
        let viewModel = VideoWallpaperViewModel(
            wallpaper: WallpaperViewModel.defaultWallpaper,
            screenId: "resource-lifecycle-test"
        )
        XCTAssertNotNil(viewModel.player.currentItem)

        viewModel.releasePlaybackResources()
        XCTAssertNil(viewModel.player.currentItem)

        viewModel.prepareForDisplay()
        XCTAssertNotNil(viewModel.player.currentItem)
        viewModel.releasePlaybackResources()
    }
}

@MainActor
final class WebMediaPolicyTests: XCTestCase {
    func testHTMLMediaMuteRestoresPageAuthoredAudioState() async throws {
        let configuration = WKWebViewConfiguration()
        configuration.userContentController.addUserScript(
            WebWallpaperViewModel.makeMediaPolicyUserScript()
        )
        let webView = WKWebView(frame: .zero, configuration: configuration)
        let navigationExpectation = expectation(description: "Web page loaded")
        let navigationObserver = WebNavigationObserver(
            expectation: navigationExpectation
        )
        webView.navigationDelegate = navigationObserver
        webView.loadHTMLString(
            "<html><body><video id='media'></video></body></html>",
            baseURL: nil
        )

        await fulfillment(of: [navigationExpectation], timeout: 5)
        if let navigationError = navigationObserver.error {
            throw navigationError
        }

        let result = try await callJavaScript(
            #"""
            const media=document.getElementById('media');
            await window.__weSetMediaPolicy({muted:false,volume:1});
            media.volume=0.8;
            media.muted=false;
            media.dispatchEvent(new Event('volumechange'));
            await window.__weSetMediaPolicy({muted:true,volume:0});
            const initiallyMuted={volume:media.volume,muted:media.muted};
            media.volume=0.6;
            media.muted=false;
            media.dispatchEvent(new Event('volumechange'));
            const policyReapplied={volume:media.volume,muted:media.muted};
            await window.__weSetMediaPolicy({muted:false,volume:1});
            return {
              initiallyMuted:initiallyMuted,
              policyReapplied:policyReapplied,
              restored:{volume:media.volume,muted:media.muted}
            };
            """#,
            in: webView
        )
        let output = try XCTUnwrap(result as? [String: Any])
        let initiallyMuted = try XCTUnwrap(
            output["initiallyMuted"] as? [String: Any]
        )
        let policyReapplied = try XCTUnwrap(
            output["policyReapplied"] as? [String: Any]
        )
        let restored = try XCTUnwrap(output["restored"] as? [String: Any])

        XCTAssertEqual(initiallyMuted["volume"] as? Double, 0)
        XCTAssertEqual(initiallyMuted["muted"] as? Bool, true)
        XCTAssertEqual(policyReapplied["volume"] as? Double, 0)
        XCTAssertEqual(policyReapplied["muted"] as? Bool, true)
        XCTAssertEqual(
            try XCTUnwrap(restored["volume"] as? Double),
            0.6,
            accuracy: 0.000_001
        )
        XCTAssertEqual(restored["muted"] as? Bool, false)
    }

    private func callJavaScript(
        _ script: String,
        in webView: WKWebView
    ) async throws -> Any? {
        try await withCheckedThrowingContinuation { continuation in
            webView.callAsyncJavaScript(
                script,
                arguments: [:],
                in: nil,
                in: .page
            ) { result in
                switch result {
                case .success(let value):
                    continuation.resume(returning: value)
                case .failure(let error):
                    continuation.resume(throwing: error)
                }
            }
        }
    }
}

@MainActor
private final class WebNavigationObserver: NSObject, WKNavigationDelegate {
    let expectation: XCTestExpectation
    private(set) var error: Error?

    init(expectation: XCTestExpectation) {
        self.expectation = expectation
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        expectation.fulfill()
    }

    func webView(
        _ webView: WKWebView,
        didFail navigation: WKNavigation!,
        withError error: Error
    ) {
        self.error = error
        expectation.fulfill()
    }

    func webView(
        _ webView: WKWebView,
        didFailProvisionalNavigation navigation: WKNavigation!,
        withError error: Error
    ) {
        self.error = error
        expectation.fulfill()
    }
}
