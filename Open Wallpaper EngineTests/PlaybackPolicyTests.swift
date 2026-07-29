import AppKit
import SwiftUI
import WebKit
import XCTest
@testable import Open_Wallpaper_Engine

@MainActor
private final class TestNowPlayingSource: SceneNowPlayingSource {
    typealias Completion = @MainActor (
        Result<SceneNowPlayingObservation?, Error>
    ) -> Void

    var startError: Error?
    private(set) var startCount = 0
    private(set) var stopCount = 0
    private(set) var fetchCount = 0
    private var changeHandler: (@MainActor () -> Void)?
    private var completion: Completion?

    func start(changeHandler: @escaping @MainActor () -> Void) throws {
        startCount += 1
        if let startError { throw startError }
        self.changeHandler = changeHandler
    }

    func stop() {
        stopCount += 1
    }

    func fetch(completion: @escaping Completion) {
        fetchCount += 1
        self.completion = completion
    }

    func complete(_ result: Result<SceneNowPlayingObservation?, Error>) {
        completion?(result)
    }

    func notifyChange() {
        changeHandler?()
    }
}

private enum TestNowPlayingError: LocalizedError {
    case unavailable

    var errorDescription: String? { "test source unavailable" }
}

final class PlaybackPolicyTests: XCTestCase {
    func testNowPlayingObservationRejectsEmptyInformation() {
        XCTAssertThrowsError(
            try MediaRemoteNowPlayingSource.observation(
                fromHelperPayload: NSDictionary()
            )
        )
    }

    func testNowPlayingObservationNormalizesMetadataAndTimeline() throws {
        let timestamp = Date(timeIntervalSince1970: 100)
        let observation = try XCTUnwrap(
            try MediaRemoteNowPlayingSource.observation(
                fromHelperPayload: [
                    "title": "  Track  ",
                    "artist": "Artist",
                    "album": "Album",
                    "genre": ["Rock", "Indie"],
                    "mediaType": "kMRMediaRemoteNowPlayingInfoTypeAudio",
                    "duration": 120,
                    "elapsedTime": 10,
                    "playbackRate": 1,
                    "playing": true,
                    "timestamp": ISO8601DateFormatter().string(from: timestamp),
                ] as NSDictionary
            )
        )

        XCTAssertEqual(observation.playbackState, .playing)
        XCTAssertEqual(observation.title, "Track")
        XCTAssertEqual(observation.artist, "Artist")
        XCTAssertEqual(observation.albumTitle, "Album")
        XCTAssertEqual(observation.contentType, "music")
        XCTAssertEqual(observation.genres, "Rock, Indie")
        XCTAssertEqual(
            observation.position(at: timestamp.addingTimeInterval(3)),
            13,
            accuracy: 0.001
        )
        XCTAssertEqual(
            observation.position(at: timestamp.addingTimeInterval(200)),
            120,
            accuracy: 0.001
        )
    }

    func testNowPlayingObservationUsesZeroForMissingTimelineWhenPaused() throws {
        let observation = try XCTUnwrap(
            try MediaRemoteNowPlayingSource.observation(
                fromHelperPayload: [
                    "title": "Track",
                    "playing": false,
                ] as NSDictionary
            )
        )

        XCTAssertEqual(observation.playbackState, .paused)
        XCTAssertEqual(observation.duration, 0)
        XCTAssertEqual(observation.elapsedTime, 0)
        XCTAssertEqual(observation.position(at: Date()), 0)
    }

    func testNowPlayingObservationRejectsInvalidTimeline() {
        XCTAssertThrowsError(
            try MediaRemoteNowPlayingSource.observation(
                fromHelperPayload: [
                    "title": "Track",
                    "playing": true,
                    "duration": Double.infinity,
                ] as NSDictionary
            )
        )
    }

    func testArtworkPaletteUsesVisibleRGBAPixels() throws {
        let thumbnail = SceneMediaThumbnailRGBA8(
            width: 3,
            height: 1,
            bytesPerRow: 12,
            pixels: Data([
                255, 0, 0, 255,
                0, 255, 0, 255,
                0, 0, 255, 255,
            ])
        )

        let palette = try SceneMediaArtworkProcessor.palette(for: thumbnail)

        XCTAssertEqual(palette.count, 3)
        XCTAssertTrue(palette.contains(SceneMediaColor(red: 1, green: 0, blue: 0)))
        XCTAssertTrue(palette.contains(SceneMediaColor(red: 0, green: 1, blue: 0)))
        XCTAssertTrue(palette.contains(SceneMediaColor(red: 0, green: 0, blue: 1)))
    }

    @MainActor
    func testMediaRemoteHelperReturnsSystemMetadataAndArtwork() async throws {
        let source = MediaRemoteNowPlayingSource()
        try source.start(changeHandler: {})
        defer { source.stop() }
        let completed = expectation(description: "MediaRemote Now Playing fetch")
        var fetchedResult: Result<SceneNowPlayingObservation?, Error>?

        source.fetch { result in
            fetchedResult = result
            completed.fulfill()
        }

        await fulfillment(of: [completed], timeout: 3)
        let observation = try XCTUnwrap(fetchedResult).get()
        guard let observation else {
            throw XCTSkip("No system Now Playing item is currently available")
        }
        XCTAssertFalse(observation.title.isEmpty)
        XCTAssertFalse(observation.artist.isEmpty)
        let artworkData = try XCTUnwrap(observation.artworkData)
        XCTAssertFalse(artworkData.isEmpty)
    }

    @MainActor
    func testNowPlayingMonitorPublishesSourceMetadata() throws {
        let provider = SceneMediaSnapshotProvider()
        let source = TestNowPlayingSource()
        let monitor = SceneNowPlayingMonitor(
            provider: provider,
            source: source,
            timelineInterval: 3_600
        )
        defer { monitor.stop() }
        let timestamp = Date()

        monitor.start()
        source.complete(.success(SceneNowPlayingObservation(
            playbackState: .playing,
            title: "Track",
            artist: "Artist",
            contentType: "music",
            albumTitle: "Album",
            genres: "Electronic",
            elapsedTime: 12,
            duration: 180,
            timestamp: timestamp,
            playbackRate: 1,
            artworkData: nil
        )))

        XCTAssertEqual(source.startCount, 1)
        XCTAssertEqual(source.fetchCount, 1)
        guard case .available(_, let content) = provider.snapshot else {
            return XCTFail("Expected monitor metadata to become available")
        }
        XCTAssertEqual(content.playbackState, .playing)
        XCTAssertEqual(content.title, "Track")
        XCTAssertEqual(content.artist, "Artist")
        XCTAssertEqual(content.albumTitle, "Album")
        XCTAssertEqual(content.genres, "Electronic")
        XCTAssertEqual(content.position, 12, accuracy: 0.1)
        XCTAssertNil(provider.inputIssue)
    }

    @MainActor
    func testNowPlayingTimelineTimerDoesNotPollSystemSource() async throws {
        let provider = SceneMediaSnapshotProvider()
        let source = TestNowPlayingSource()
        let monitor = SceneNowPlayingMonitor(
            provider: provider,
            source: source,
            timelineInterval: 0.01
        )
        defer { monitor.stop() }

        monitor.start()
        source.complete(.success(SceneNowPlayingObservation(
            playbackState: .playing,
            title: "Track",
            artist: "Artist",
            contentType: "music",
            albumTitle: "",
            genres: "",
            elapsedTime: 0,
            duration: 60,
            timestamp: Date(),
            playbackRate: 1,
            artworkData: nil
        )))
        try await Task.sleep(nanoseconds: 50_000_000)

        XCTAssertEqual(source.fetchCount, 1)
        source.notifyChange()
        XCTAssertEqual(source.fetchCount, 2)
    }

    @MainActor
    func testNowPlayingMonitorPublishesStoppedWhenSystemHasNoTrack() {
        let provider = SceneMediaSnapshotProvider()
        let source = TestNowPlayingSource()
        let monitor = SceneNowPlayingMonitor(
            provider: provider,
            source: source,
            timelineInterval: 3_600
        )
        defer { monitor.stop() }

        monitor.start()
        source.complete(.success(nil))

        guard case .available(_, let content) = provider.snapshot else {
            return XCTFail("Expected an available stopped media snapshot")
        }
        XCTAssertEqual(content, .stopped)
        XCTAssertNil(provider.inputIssue)
    }

    @MainActor
    func testNowPlayingMonitorRejectsLateFetchAfterStop() {
        let provider = SceneMediaSnapshotProvider()
        let source = TestNowPlayingSource()
        let monitor = SceneNowPlayingMonitor(
            provider: provider,
            source: source,
            timelineInterval: 3_600
        )

        monitor.start()
        monitor.stop()
        let stoppedSnapshot = provider.snapshot
        source.complete(.success(SceneNowPlayingObservation(
            playbackState: .playing,
            title: "Late Track",
            artist: "Artist",
            contentType: "music",
            albumTitle: "",
            genres: "",
            elapsedTime: 0,
            duration: 30,
            timestamp: Date(),
            playbackRate: 1,
            artworkData: nil
        )))

        XCTAssertEqual(source.stopCount, 1)
        XCTAssertEqual(provider.snapshot, stoppedSnapshot)
        XCTAssertNil(provider.inputIssue)
    }

    @MainActor
    func testNowPlayingMonitorMakesStartupFailureVisible() {
        let provider = SceneMediaSnapshotProvider()
        let source = TestNowPlayingSource()
        source.startError = TestNowPlayingError.unavailable
        let monitor = SceneNowPlayingMonitor(
            provider: provider,
            source: source,
            timelineInterval: 3_600
        )

        monitor.start()

        guard case .unavailable = provider.snapshot else {
            return XCTFail("Expected unavailable media after source failure")
        }
        XCTAssertEqual(source.startCount, 1)
        XCTAssertEqual(source.stopCount, 1)
        XCTAssertEqual(
            provider.inputIssue,
            "Now Playing unavailable: test source unavailable"
        )
    }

    @MainActor
    func testPublishingStoppedContentClearsTrackAndAdvancesRevisions() throws {
        let provider = SceneMediaSnapshotProvider()
        let playing = SceneMediaContent(
            playbackState: .playing,
            title: "Track",
            artist: "Artist",
            contentType: "music",
            albumTitle: "Album",
            subTitle: "",
            albumArtist: "",
            genres: "",
            position: 10,
            duration: 120,
            thumbnail: nil,
            primaryColor: .black,
            secondaryColor: .black,
            tertiaryColor: .black,
            textColor: .black,
            highContrastColor: .black
        )
        try provider.publish(playing)
        let playingRevisions = provider.snapshot.revisions

        try provider.publish(.stopped)

        guard case .available(let stoppedRevisions, let content) = provider.snapshot else {
            return XCTFail("Expected a stopped but available media snapshot")
        }
        XCTAssertEqual(content, .stopped)
        XCTAssertEqual(stoppedRevisions.status, playingRevisions.status)
        XCTAssertEqual(stoppedRevisions.metadata, playingRevisions.metadata + 1)
        XCTAssertEqual(stoppedRevisions.playback, playingRevisions.playback + 1)
        XCTAssertEqual(stoppedRevisions.timeline, playingRevisions.timeline + 1)
    }

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

    func testScenePresentationDefaultsToAspectFill() {
        XCTAssertEqual(GlobalSettings().scenePresentationScaling, .aspectFill)
    }

    func testLegacyAspectFillRemainsAspectFill() throws {
        let data = try XCTUnwrap(
            #"{"scenePresentationScaling":"aspectFill"}"#.data(using: .utf8)
        )

        let settings = try JSONDecoder().decode(GlobalSettings.self, from: data)

        XCTAssertEqual(settings.scenePresentationScaling, .aspectFill)
    }

    func testLegacyAspectFitSelectionIsPreserved() throws {
        let data = try XCTUnwrap(
            #"{"scenePresentationScaling":"aspectFit"}"#.data(using: .utf8)
        )

        let settings = try JSONDecoder().decode(GlobalSettings.self, from: data)

        XCTAssertEqual(settings.scenePresentationScaling, .aspectFit)
    }

    func testVersionedExplicitAspectFillSelectionIsPreserved() throws {
        var settings = GlobalSettings()
        settings.scenePresentationScaling = .aspectFill

        let restored = try JSONDecoder().decode(
            GlobalSettings.self,
            from: JSONEncoder().encode(settings)
        )

        XCTAssertEqual(restored.scenePresentationScaling, .aspectFill)
    }

    func testVersionOneStretchDefaultMigratesToAspectFill() throws {
        let data = try XCTUnwrap(
            #"{"scenePresentationScaling":"stretch","scenePresentationSettingsVersion":1}"#
                .data(using: .utf8)
        )

        let settings = try JSONDecoder().decode(GlobalSettings.self, from: data)

        XCTAssertEqual(settings.scenePresentationScaling, .aspectFill)
    }

    func testCurrentVersionExplicitStretchSelectionIsPreserved() throws {
        var settings = GlobalSettings()
        settings.scenePresentationScaling = .stretch

        let restored = try JSONDecoder().decode(
            GlobalSettings.self,
            from: JSONEncoder().encode(settings)
        )

        XCTAssertEqual(restored.scenePresentationScaling, .stretch)
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
