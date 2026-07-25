import AudioToolbox
import Foundation
@testable import SceneAudio
import SceneRuntimeBridge
import XCTest

@MainActor
final class SceneAudioTests: XCTestCase {
    func testOwnerCoordinatorPrefersActiveMainScreen() {
        var changes: [(String?, String?)] = []
        let coordinator = SceneAudioOwnerCoordinator(mainScreenId: "main") {
            changes.append(($0, $1))
        }

        coordinator.register(screenId: "secondary")
        XCTAssertEqual(coordinator.ownerScreenId, "secondary")
        coordinator.register(screenId: "main")

        XCTAssertEqual(coordinator.ownerScreenId, "main")
        XCTAssertTrue(coordinator.isAudible(screenId: "main"))
        XCTAssertFalse(coordinator.isAudible(screenId: "secondary"))
        XCTAssertEqual(changes.count, 2)
        XCTAssertNil(changes[0].0)
        XCTAssertEqual(changes[0].1, "secondary")
        XCTAssertEqual(changes[1].0, "secondary")
        XCTAssertEqual(changes[1].1, "main")
    }

    func testOwnerCoordinatorFallbackIsLexicographicAndRegistrationOrderIndependent() {
        let first = SceneAudioOwnerCoordinator()
        for screenId in ["screen-c", "screen-a", "screen-b"] {
            first.register(screenId: screenId)
        }
        let second = SceneAudioOwnerCoordinator()
        for screenId in ["screen-b", "screen-c", "screen-a"] {
            second.register(screenId: screenId)
        }

        XCTAssertEqual(first.activeScreenIds, ["screen-a", "screen-b", "screen-c"])
        XCTAssertEqual(first.ownerScreenId, "screen-a")
        XCTAssertEqual(second.ownerScreenId, "screen-a")
        XCTAssertEqual(
            first.activeScreenIds.filter { first.isAudible(screenId: $0) },
            ["screen-a"]
        )
    }

    func testOwnerCoordinatorUnregistersOwnerAndEventuallyBecomesSilent() {
        var changes: [(String?, String?)] = []
        let coordinator = SceneAudioOwnerCoordinator {
            changes.append(($0, $1))
        }
        coordinator.register(screenId: "screen-b")
        coordinator.register(screenId: "screen-a")
        XCTAssertEqual(coordinator.ownerScreenId, "screen-a")

        coordinator.unregister(screenId: "screen-a")
        XCTAssertEqual(coordinator.ownerScreenId, "screen-b")
        coordinator.unregister(screenId: "screen-b")

        XCTAssertNil(coordinator.ownerScreenId)
        XCTAssertEqual(changes.count, 4)
        XCTAssertEqual(changes[2].0, "screen-a")
        XCTAssertEqual(changes[2].1, "screen-b")
        XCTAssertEqual(changes[3].0, "screen-b")
        XCTAssertNil(changes[3].1)
    }

    func testOwnerCoordinatorTracksMainScreenChangesOnlyWhenActive() {
        var changes: [(String?, String?)] = []
        let coordinator = SceneAudioOwnerCoordinator(mainScreenId: "screen-a") {
            changes.append(($0, $1))
        }
        coordinator.register(screenId: "screen-a")
        coordinator.register(screenId: "screen-b")
        XCTAssertEqual(coordinator.ownerScreenId, "screen-a")

        coordinator.updateMainScreenId("screen-b")
        XCTAssertEqual(coordinator.ownerScreenId, "screen-b")
        coordinator.updateMainScreenId("screen-z")
        XCTAssertEqual(coordinator.ownerScreenId, "screen-a")
        coordinator.updateMainScreenId(nil)

        XCTAssertEqual(coordinator.ownerScreenId, "screen-a")
        XCTAssertEqual(changes.count, 3, "An unchanged owner must not emit a duplicate notification")
        XCTAssertEqual(changes[1].0, "screen-a")
        XCTAssertEqual(changes[1].1, "screen-b")
        XCTAssertEqual(changes[2].0, "screen-b")
        XCTAssertEqual(changes[2].1, "screen-a")
    }

    func testLinuxSpectrumUsesLatestWindowAndKeepsStereoArraysBound() {
        let analyzer = LinuxAudioSpectrumAnalyzer()
        let dc = [Float](repeating: 1, count: LinuxAudioSpectrumAnalyzer.sampleCount)

        let first = analyzer.push(samples: dc)
        XCTAssertEqual(first, .zero)

        let second = analyzer.push(samples: dc)
        XCTAssertEqual(second.spectrum64Left, second.spectrum64Right)
        XCTAssertEqual(second.spectrum32Left, second.spectrum32Right)
        XCTAssertEqual(second.spectrum16Left, second.spectrum16Right)
        XCTAssertEqual(second.spectrum64Left[0], 0.3, accuracy: 0.0001)
        XCTAssertTrue(second.spectrum64Left.allSatisfy(\.isFinite))
    }

    func testPlanarFloatCaptureDownmixesEveryBuffer() throws {
        var left: [Float] = [1, -1, 0.5]
        var right: [Float] = [0, 0.5, -0.5]
        let format = AudioStreamBasicDescription(
            mSampleRate: 44_100,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagsNativeFloatPacked |
                kAudioFormatFlagIsNonInterleaved,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 2,
            mBitsPerChannel: 32,
            mReserved: 0
        )

        let frameCount = left.count
        let decoded = left.withUnsafeMutableBytes { leftBytes in
            right.withUnsafeMutableBytes { rightBytes in
                withAudioBufferList(maximumBuffers: 2) { buffers in
                    buffers[0] = AudioBuffer(
                        mNumberChannels: 1,
                        mDataByteSize: UInt32(leftBytes.count),
                        mData: leftBytes.baseAddress
                    )
                    buffers[1] = AudioBuffer(
                        mNumberChannels: 1,
                        mDataByteSize: UInt32(rightBytes.count),
                        mData: rightBytes.baseAddress
                    )
                    return decodePCMBufferList(
                        buffers,
                        frameCount: frameCount,
                        format: format
                    )
                }
            }
        }

        let values = try XCTUnwrap(decoded)
        XCTAssertEqual(values[0], 0.5, accuracy: 0.0001)
        XCTAssertEqual(values[1], -0.25, accuracy: 0.0001)
        XCTAssertEqual(values[2], 0, accuracy: 0.0001)
    }

    func testInterleavedSignedIntegerCaptureDownmixesChannels() throws {
        var interleaved: [Int16] = [Int16.max, 0, Int16.min + 1, Int16.max]
        let format = AudioStreamBasicDescription(
            mSampleRate: 44_100,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 2,
            mBitsPerChannel: 16,
            mReserved: 0
        )

        let decoded = interleaved.withUnsafeMutableBytes { bytes in
            withAudioBufferList(maximumBuffers: 1) { buffers in
                buffers[0] = AudioBuffer(
                    mNumberChannels: 2,
                    mDataByteSize: UInt32(bytes.count),
                    mData: bytes.baseAddress
                )
                return decodePCMBufferList(
                    buffers,
                    frameCount: 2,
                    format: format
                )
            }
        }

        let values = try XCTUnwrap(decoded)
        XCTAssertEqual(values[0], 0.5, accuracy: 0.0001)
        XCTAssertEqual(values[1], 0, accuracy: 0.0001)
    }

    func testPlayerDecodesWaveAndPreservesLoopAndVolume() throws {
        let player = try SceneAudioPlayer(data: waveData(), loop: true, volume: 0.25)

        XCTAssertGreaterThan(player.duration, 0)
        XCTAssertTrue(player.loops)
        XCTAssertEqual(player.volume, 0.25, accuracy: 0.001)
        try player.setVolume(0.75)
        XCTAssertEqual(player.volume, 0.75, accuracy: 0.001)
        player.stop()
        XCTAssertEqual(player.currentTime, 0, accuracy: 0.001)
    }

    func testPlayerRejectsEmptyDataAndInvalidVolumeExplicitly() {
        XCTAssertThrowsError(try SceneAudioPlayer(data: Data(), loop: false, volume: 1)) {
            XCTAssertEqual($0 as? SceneAudioError, .emptyData)
        }
        XCTAssertThrowsError(try SceneAudioPlayer(data: waveData(), loop: false, volume: .nan)) {
            guard case .invalidVolume = $0 as? SceneAudioError else {
                return XCTFail("Unexpected error: \($0)")
            }
        }
    }

    func testControllerReplacementAndStopAreDeterministic() throws {
        let controller = SceneAudioController()
        try controller.load(
            identifier: "sound:1",
            data: waveData(),
            loop: true,
            volume: 0.4,
            autoplay: false
        )
        XCTAssertEqual(controller.playerCount, 1)
        var state = try controller.playerState(identifier: "sound:1")
        XCTAssertFalse(state.isPlaying)
        XCTAssertTrue(state.loops)
        XCTAssertEqual(state.volume, 0.4, accuracy: 0.001)
        controller.pauseAll()
        try controller.resumeAll()
        XCTAssertFalse(try controller.playerState(identifier: "sound:1").isPlaying)

        try controller.load(
            identifier: "sound:1",
            data: waveData(),
            loop: false,
            volume: 0.8,
            autoplay: false
        )
        XCTAssertEqual(controller.playerCount, 1)
        state = try controller.playerState(identifier: "sound:1")
        XCTAssertFalse(state.loops)
        XCTAssertEqual(state.volume, 0.8, accuracy: 0.001)

        try controller.stop(identifier: "sound:1")
        XCTAssertEqual(controller.playerCount, 0)
        XCTAssertThrowsError(try controller.stop(identifier: "sound:1")) {
            XCTAssertEqual($0 as? SceneAudioError, .unknownPlayer("sound:1"))
        }
    }

    func testReconcileKeepsStablePlayersAndAppliesVolumeProduct() throws {
        let controller = SceneAudioController()
        var loads: [String] = []
        let loader: (String) throws -> Data = { path in
            loads.append(path)
            return self.waveData()
        }
        let sounds = [sound(objectId: 7, visible: true, sources: [
            source(index: 0, resource: "a.wav", loop: true, volume: 0.8, startSilent: true),
            source(index: 1, resource: "b.wav", loop: false, volume: 0.5, startSilent: true),
        ])]

        try controller.reconcile(sounds, masterVolume: 0.5, audioOutput: 0.25, loadAsset: loader)
        XCTAssertEqual(controller.playerCount, 2)
        XCTAssertEqual(Set(loads), Set(["a.wav", "b.wav"]))
        XCTAssertEqual(
            try controller.playerState(identifier: "sound:7:0").volume,
            0.1,
            accuracy: 0.001
        )
        XCTAssertEqual(
            try controller.playerState(identifier: "sound:7:1").volume,
            0.0625,
            accuracy: 0.001
        )

        try controller.reconcile(sounds, masterVolume: 0.25, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(loads.count, 2, "A stable resource/loop snapshot must not decode again")
        XCTAssertEqual(
            try controller.playerState(identifier: "sound:7:0").volume,
            0.2,
            accuracy: 0.001
        )
    }

    func testReconcileRebuildsOnlyForResourceOrLoopAndStopsMissingSources() throws {
        let controller = SceneAudioController()
        var loads: [String] = []
        let loader: (String) throws -> Data = { path in
            loads.append(path)
            return self.waveData()
        }

        try controller.reconcile([
            sound(objectId: 1, visible: true, sources: [
                source(index: 0, resource: "first.wav", loop: false, volume: 1, startSilent: true),
                source(index: 1, resource: "second.wav", loop: false, volume: 1, startSilent: true),
            ]),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        try controller.reconcile([
            sound(objectId: 1, visible: true, sources: [
                source(index: 0, resource: "replacement.wav", loop: true, volume: 1, startSilent: true),
            ]),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)

        XCTAssertEqual(loads, ["first.wav", "second.wav", "replacement.wav"])
        XCTAssertEqual(controller.playerCount, 1)
        XCTAssertTrue(try controller.playerState(identifier: "sound:1:0").loops)
        XCTAssertThrowsError(try controller.playerState(identifier: "sound:1:1")) {
            XCTAssertEqual($0 as? SceneAudioError, .unknownPlayer("sound:1:1"))
        }
    }

    func testReconcileFailureLeavesPreviousSetUntouched() throws {
        let controller = SceneAudioController()
        try controller.reconcile([
            sound(objectId: 3, visible: true, sources: [
                source(index: 0, resource: "valid.wav", loop: false, volume: 0.4, startSilent: true),
            ]),
        ], masterVolume: 1, audioOutput: 1) { _ in self.waveData() }

        XCTAssertThrowsError(try controller.reconcile([
            sound(objectId: 3, visible: true, sources: [
                source(index: 0, resource: "broken.wav", loop: false, volume: 0.9, startSilent: true),
            ]),
        ], masterVolume: 1, audioOutput: 1) { _ in Data() }) {
            XCTAssertEqual($0 as? SceneAudioError, .emptyData)
        }

        XCTAssertEqual(controller.playerCount, 1)
        let state = try controller.playerState(identifier: "sound:3:0")
        XCTAssertEqual(state.volume, 0.4, accuracy: 0.001)
        XCTAssertFalse(state.loops)
    }

    func testReconcileValidatesEveryVolumeBeforeLoadingOrMutating() throws {
        let controller = SceneAudioController()
        var loadCount = 0
        let invalid = [sound(objectId: 4, visible: true, sources: [
            source(index: 0, resource: "invalid.wav", loop: false, volume: 1.01, startSilent: true),
        ])]

        XCTAssertThrowsError(try controller.reconcile(
            invalid,
            masterVolume: 1,
            audioOutput: 1,
            loadAsset: { _ in loadCount += 1; return self.waveData() }
        )) {
            guard case .invalidVolume = $0 as? SceneAudioError else {
                return XCTFail("Unexpected error: \($0)")
            }
        }
        XCTAssertEqual(loadCount, 0)
        XCTAssertEqual(controller.playerCount, 0)
    }

    func testOncePlaybackDoesNotRestartAfterNaturalCompletion() throws {
        let (controller, created) = controlledController()
        let once = [sound(objectId: 10, visible: true, sources: [
            source(index: 0, resource: "once.wav", loop: false, volume: 1, startSilent: false),
        ])]

        try controller.reconcile(once, masterVolume: 1, audioOutput: 1) { _ in self.waveData() }
        let player = try XCTUnwrap(created().first)
        XCTAssertEqual(player.playCount, 1)
        player.finishNaturally()

        try controller.reconcile(once, masterVolume: 1, audioOutput: 1) { _ in
            XCTFail("A stable once source must not be decoded again")
            return self.waveData()
        }
        XCTAssertEqual(player.playCount, 1)
        XCTAssertFalse(player.isPlaying)
    }

    func testVisibilityOnlyResumesOnceSourceThatWasPlayingWhenHidden() throws {
        let (controller, created) = controlledController()
        let visible = [sound(objectId: 11, visible: true, sources: [
            source(index: 0, resource: "once.wav", loop: false, volume: 1, startSilent: false),
        ])]
        let hidden = [sound(objectId: 11, visible: false, sources: visible[0].sources)]
        let loader: (String) throws -> Data = { _ in self.waveData() }

        try controller.reconcile(visible, masterVolume: 1, audioOutput: 1, loadAsset: loader)
        let player = try XCTUnwrap(created().first)
        try controller.reconcile(hidden, masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(player.pauseCount, 1)
        try controller.reconcile(visible, masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(player.playCount, 2, "Visibility restoration must resume an interrupted once source")

        player.finishNaturally()
        try controller.reconcile(hidden, masterVolume: 1, audioOutput: 1, loadAsset: loader)
        try controller.reconcile(visible, masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(player.playCount, 2, "Hiding an already-finished once source must not arm a restart")
    }

    func testPauseResumeContinuesOnceButLoopRestartsAfterUnexpectedStop() throws {
        let (controller, created) = controlledController()
        let sounds = [sound(objectId: 12, visible: true, sources: [
            source(index: 0, resource: "once.wav", loop: false, volume: 1, startSilent: false),
            source(index: 1, resource: "loop.wav", loop: true, volume: 1, startSilent: false),
        ])]
        let loader: (String) throws -> Data = { _ in self.waveData() }
        try controller.reconcile(sounds, masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(created().count, 2)

        controller.pauseAll()
        try controller.resumeAll()
        XCTAssertEqual(created()[0].playCount, 2)
        XCTAssertEqual(created()[1].playCount, 2)

        created()[0].finishNaturally()
        created()[1].finishNaturally()
        try controller.reconcile(sounds, masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(created()[0].playCount, 2, "A completed once source must remain complete")
        XCTAssertEqual(created()[1].playCount, 3, "A looping source must recover from an unexpected stop")
    }

    func testVisibleAndStartSilentHaveExplicitPlaybackSemantics() throws {
        let controller = SceneAudioController()
        controller.pauseAll()
        let silent = [sound(objectId: 5, visible: true, sources: [
            source(index: 0, resource: "silent.wav", loop: true, volume: 1, startSilent: true),
        ])]
        try controller.reconcile(silent, masterVolume: 1, audioOutput: 1) { _ in self.waveData() }
        XCTAssertFalse(try controller.playerState(identifier: "sound:5:0").isPlaying)
        XCTAssertFalse(try controller.playbackIntent(identifier: "sound:5:0"))
        let enabled = [sound(objectId: 5, visible: true, sources: [
            source(index: 0, resource: "silent.wav", loop: true, volume: 1, startSilent: false),
        ])]
        try controller.reconcile(enabled, masterVolume: 1, audioOutput: 1) { _ in
            XCTFail("Changing startSilent must not rebuild the player")
            return self.waveData()
        }
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:5:0"))

        let automaticVisible = [sound(objectId: 6, visible: true, sources: [
            source(index: 0, resource: "auto.wav", loop: true, volume: 1, startSilent: false),
        ])]
        try controller.reconcile(automaticVisible, masterVolume: 1, audioOutput: 1) { _ in self.waveData() }
        XCTAssertFalse(try controller.playerState(identifier: "sound:6:0").isPlaying)
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:6:0"))

        let automaticHidden = [sound(objectId: 6, visible: false, sources: automaticVisible[0].sources)]
        try controller.reconcile(automaticHidden, masterVolume: 1, audioOutput: 1) { _ in
            XCTFail("Visibility changes must not reload the asset")
            return self.waveData()
        }
        XCTAssertFalse(try controller.playerState(identifier: "sound:6:0").isPlaying)
        XCTAssertFalse(try controller.playbackIntent(identifier: "sound:6:0"))
        try controller.reconcile(automaticVisible, masterVolume: 1, audioOutput: 1) { _ in
            XCTFail("Visibility restoration must not reload the asset")
            return self.waveData()
        }
        XCTAssertFalse(try controller.playerState(identifier: "sound:6:0").isPlaying)
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:6:0"))
    }

    func testStopAllClearsReconciledSourcesAndPauseState() throws {
        let controller = SceneAudioController()
        let sounds = [sound(objectId: 8, visible: true, sources: [
            source(index: 0, resource: "a.wav", loop: false, volume: 1, startSilent: true),
        ])]
        try controller.reconcile(sounds, masterVolume: 1, audioOutput: 1) { _ in self.waveData() }
        controller.pauseAll()
        controller.stopAll()
        XCTAssertEqual(controller.playerCount, 0)
        try controller.resumeAll()
    }

    func testFreshPlayCommandIsNoOpWhilePlayerIsAlreadyPlaying() throws {
        let (controller, created) = controlledController()
        let loader: (String) throws -> Data = { _ in self.waveData() }
        let baseSource = source(
            index: 0,
            resource: "script.wav",
            loop: false,
            volume: 1,
            startSilent: true
        )
        try controller.reconcile([
            sound(
                objectId: 20,
                visible: true,
                sources: [baseSource],
                playbackCommand: command(.play, generation: 1)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        let player = try XCTUnwrap(created().first)
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.playCount, 1)

        try controller.reconcile([
            sound(
                objectId: 20,
                visible: true,
                sources: [baseSource],
                playbackCommand: command(.play, generation: 2)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.playCount, 1, "play() while playing must be a no-op")
    }

    func testPauseThenPlayResumesWithoutResettingPosition() throws {
        let (controller, created) = controlledController()
        let loader: (String) throws -> Data = { _ in self.waveData() }
        let baseSource = source(
            index: 0,
            resource: "script.wav",
            loop: false,
            volume: 1,
            startSilent: true
        )
        try controller.reconcile([
            sound(
                objectId: 21,
                visible: true,
                sources: [baseSource],
                playbackCommand: command(.play, generation: 1)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        let player = try XCTUnwrap(created().first)
        player.advance(to: 3.5)

        try controller.reconcile([
            sound(
                objectId: 21,
                visible: true,
                sources: [baseSource],
                playbackCommand: command(.pause, generation: 2)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(player.playbackState, .paused)
        XCTAssertEqual(player.position, 3.5)

        try controller.reconcile([
            sound(
                objectId: 21,
                visible: true,
                sources: [baseSource],
                playbackCommand: command(.play, generation: 3)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.playCount, 2)
        XCTAssertEqual(player.position, 3.5, "pause -> play must resume instead of rewinding")
        XCTAssertEqual(player.stopCount, 0)
    }

    func testStoppedAndEndedPlaybackRestartOnlyForFreshPlayCommand() throws {
        let (controller, created) = controlledController()
        let loader: (String) throws -> Data = { _ in self.waveData() }
        let baseSource = source(
            index: 0,
            resource: "script.wav",
            loop: false,
            volume: 1,
            startSilent: true
        )
        func snapshot(_ action: SceneSoundPlaybackCommand.Action, _ generation: UInt64) -> [SceneSoundSnapshot] {
            [sound(
                objectId: 22,
                visible: true,
                sources: [baseSource],
                playbackCommand: command(action, generation: generation)
            )]
        }

        try controller.reconcile(
            snapshot(.play, 1), masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        let player = try XCTUnwrap(created().first)
        player.advance(to: 4)
        try controller.reconcile(
            snapshot(.stop, 2), masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(player.playbackState, .stopped)
        XCTAssertEqual(player.position, 0)

        try controller.reconcile(
            snapshot(.play, 3), masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(player.playCount, 2)
        XCTAssertEqual(player.position, 0)

        player.finishNaturally()
        XCTAssertEqual(player.playbackState, .ended)
        try controller.reconcile(
            snapshot(.play, 3), masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(player.playCount, 2, "A persistent command value is not a new play request")

        try controller.reconcile([
            sound(
                objectId: 22,
                visible: true,
                sources: [baseSource],
                playbackCommand: command(.play, generation: 4)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(player.playCount, 3)
        XCTAssertEqual(player.position, 0, "ended -> fresh play must restart from the beginning")
        XCTAssertEqual(player.stopCount, 2, "Ended playback must be rewound before restarting")
    }

    func testRuntimeSnapshotReportsPlayerTruthInsteadOfPersistentPlayRequest() throws {
        let (controller, created) = controlledController()
        let source = source(
            index: 0,
            resource: "truth.wav",
            loop: false,
            volume: 1,
            startSilent: true
        )
        let sounds = [sound(
            objectId: 23,
            visible: true,
            sources: [source],
            playbackCommand: command(.play, generation: 1)
        )]
        try controller.reconcile(
            sounds,
            masterVolume: 1,
            audioOutput: 1,
            loadAsset: { _ in self.waveData() }
        )
        let player = try XCTUnwrap(created().first)
        player.finishNaturally()

        let playerState = try controller.playerState(identifier: "sound:23:0")
        XCTAssertEqual(playerState.state, .ended)
        XCTAssertFalse(playerState.isPlaying)
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:23:0"))
        XCTAssertEqual(
            controller.soundRuntimeSnapshots(),
            [SceneSoundRuntimeSnapshot(objectId: 23, state: .ended, position: player.duration)]
        )
    }

    func testPlaybackCommandGenerationCannotMoveBackwardOrChangeMeaning() throws {
        let (controller, created) = controlledController()
        let source = source(
            index: 0,
            resource: "ordered.wav",
            loop: false,
            volume: 1,
            startSilent: true
        )
        let loader: (String) throws -> Data = { _ in self.waveData() }
        try controller.reconcile([
            sound(
                objectId: 26,
                visible: true,
                sources: [source],
                playbackCommand: command(.play, generation: 5)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        let player = try XCTUnwrap(created().first)

        XCTAssertThrowsError(try controller.reconcile([
            sound(
                objectId: 26,
                visible: true,
                sources: [source],
                playbackCommand: command(.pause, generation: 4)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)) {
            XCTAssertEqual(
                $0 as? SceneAudioError,
                .stalePlaybackCommand(identifier: "sound:26:0", current: 5, received: 4)
            )
        }
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.pauseCount, 0)

        XCTAssertThrowsError(try controller.reconcile([
            sound(
                objectId: 26,
                visible: true,
                sources: [source],
                playbackCommand: command(.pause, generation: 5)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)) {
            XCTAssertEqual(
                $0 as? SceneAudioError,
                .conflictingPlaybackCommand(identifier: "sound:26:0", generation: 5)
            )
        }
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.pauseCount, 0)
    }

    func testPlaybackFailureRestoresRuntimeStateAndPositionTransactionally() throws {
        let (controller, created) = controlledController()
        let first = source(
            index: 0,
            resource: "first.wav",
            loop: false,
            volume: 1,
            startSilent: true
        )
        let second = source(
            index: 0,
            resource: "second.wav",
            loop: false,
            volume: 1,
            startSilent: true
        )
        let loader: (String) throws -> Data = { _ in self.waveData() }
        try controller.reconcile([
            sound(
                objectId: 27,
                visible: true,
                sources: [first],
                playbackCommand: command(.play, generation: 1)
            ),
            sound(objectId: 28, visible: true, sources: [second]),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(created().count, 2)
        created()[0].advance(to: 4.25)
        created()[1].failsToPlay = true

        XCTAssertThrowsError(try controller.reconcile([
            sound(
                objectId: 27,
                visible: true,
                sources: [first],
                playbackCommand: command(.stop, generation: 2)
            ),
            sound(
                objectId: 28,
                visible: true,
                sources: [second],
                playbackCommand: command(.play, generation: 1)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)) {
            XCTAssertEqual($0 as? SceneAudioError, .playbackFailed("sound:28:0"))
        }

        let restored = try controller.playerState(identifier: "sound:27:0")
        XCTAssertEqual(restored.state, .playing)
        XCTAssertEqual(restored.position, 4.25)
        XCTAssertEqual(
            try controller.playerState(identifier: "sound:28:0").state,
            .stopped
        )
    }

    func testTimedPlaybackBoundsAreAcceptedLikeLinuxForEveryPlaybackMode() throws {
        for loop in [false, true] {
            let (controller, created) = controlledController()
            var loadCount = 0
            try controller.reconcile([
                sound(
                    objectId: loop ? 25 : 24,
                    visible: true,
                    sources: [source(
                        index: 0,
                        resource: "timed.wav",
                        loop: loop,
                        volume: 1,
                        startSilent: false
                    )],
                    minimumTime: 1,
                    maximumTime: 2
                ),
            ], masterVolume: 1, audioOutput: 1) { _ in
                loadCount += 1
                return self.waveData()
            }
            XCTAssertEqual(loadCount, 1)
            XCTAssertEqual(created().count, 1)
            XCTAssertEqual(created()[0].playbackState, .playing)
            XCTAssertEqual(created()[0].loops, loop)
        }
    }

    func testSynchronizationFailureIsExplicitAndClearsAudioState() throws {
        let (controller, created) = controlledController()
        let original = sound(objectId: 31, visible: true, sources: [
            source(
                index: 0,
                resource: "working.wav",
                loop: true,
                volume: 1,
                startSilent: false
            ),
        ])
        try controller.reconcile(
            [original],
            masterVolume: 1,
            audioOutput: 1
        ) { _ in self.waveData() }
        XCTAssertEqual(controller.playerCount, 1)
        XCTAssertEqual(created().first?.playbackState, .playing)

        let replacement = sound(objectId: 31, visible: true, sources: [
            source(
                index: 0,
                resource: "broken.wav",
                loop: true,
                volume: 1,
                startSilent: false
            ),
        ])
        let issue = controller.synchronize(
            [replacement],
            masterVolume: 1,
            audioOutput: 1
        ) { _ in
            throw SyntheticAudioLoadFailure()
        }

        XCTAssertEqual(issue?.message, "synthetic audio asset failure")
        XCTAssertEqual(controller.playerCount, 0)
        XCTAssertEqual(created().first?.playbackState, .stopped)
    }

    func testResumeAllRollsBackEveryPlayerWhenOneStartFails() throws {
        var created: [ControlledPlayback] = []
        let controller = SceneAudioController { _, loop, volume in
            let player = ControlledPlayback(loop: loop, volume: volume)
            created.append(player)
            return player
        }
        controller.pauseAll()
        try controller.reconcile([
            sound(objectId: 9, visible: true, sources: [
                source(index: 0, resource: "first.wav", loop: true, volume: 1, startSilent: false),
                source(index: 1, resource: "second.wav", loop: true, volume: 1, startSilent: false),
            ]),
        ], masterVolume: 1, audioOutput: 1) { _ in self.waveData() }
        XCTAssertEqual(created.count, 2)
        created[1].failsToPlay = true

        XCTAssertThrowsError(try controller.resumeAll()) {
            XCTAssertEqual($0 as? SceneAudioError, .playbackFailed("sound:9:1"))
        }
        XCTAssertFalse(created[0].isPlaying)
        XCTAssertFalse(created[1].isPlaying)
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:9:0"))
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:9:1"))

        created[1].failsToPlay = false
        try controller.resumeAll()
        XCTAssertTrue(created[0].isPlaying)
        XCTAssertTrue(created[1].isPlaying)
    }

    private func waveData() -> Data {
        let sampleRate: UInt32 = 8_000
        let samples = [Int16](repeating: 0, count: 800)
        let payloadSize = UInt32(samples.count * MemoryLayout<Int16>.size)
        var data = Data()
        data.append(contentsOf: Array("RIFF".utf8))
        append(UInt32(36) + payloadSize, to: &data)
        data.append(contentsOf: Array("WAVEfmt ".utf8))
        append(UInt32(16), to: &data)
        append(UInt16(1), to: &data)
        append(UInt16(1), to: &data)
        append(sampleRate, to: &data)
        append(sampleRate * 2, to: &data)
        append(UInt16(2), to: &data)
        append(UInt16(16), to: &data)
        data.append(contentsOf: Array("data".utf8))
        append(payloadSize, to: &data)
        for sample in samples { append(UInt16(bitPattern: sample), to: &data) }
        return data
    }

    private func sound(
        objectId: Int,
        visible: Bool,
        sources: [SceneSoundSourceSnapshot],
        playbackCommand: SceneSoundPlaybackCommand? = nil,
        minimumTime: Double = 0,
        maximumTime: Double = 0
    ) -> SceneSoundSnapshot {
        SceneSoundSnapshot(
            objectId: objectId,
            visible: visible,
            sources: sources,
            playbackCommand: playbackCommand,
            minimumTime: minimumTime,
            maximumTime: maximumTime
        )
    }

    private func command(
        _ action: SceneSoundPlaybackCommand.Action,
        generation: UInt64
    ) -> SceneSoundPlaybackCommand {
        SceneSoundPlaybackCommand(action: action, generation: generation)
    }

    private func source(
        index: Int,
        resource: String,
        loop: Bool,
        volume: Float,
        startSilent: Bool
    ) -> SceneSoundSourceSnapshot {
        SceneSoundSourceSnapshot(
            sourceIndex: index,
            resource: resource,
            loop: loop,
            volume: volume,
            startSilent: startSilent
        )
    }

    private func controlledController() -> (
        SceneAudioController,
        () -> [ControlledPlayback]
    ) {
        var created: [ControlledPlayback] = []
        let controller = SceneAudioController { _, loop, volume in
            let player = ControlledPlayback(loop: loop, volume: volume)
            created.append(player)
            return player
        }
        return (controller, { created })
    }

    private func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var littleEndian = value.littleEndian
        withUnsafeBytes(of: &littleEndian) { data.append(contentsOf: $0) }
    }

    private func withAudioBufferList<Result>(
        maximumBuffers: Int,
        _ body: (UnsafeMutableAudioBufferListPointer) throws -> Result
    ) rethrows -> Result {
        precondition(maximumBuffers > 0)
        let byteCount = MemoryLayout<AudioBufferList>.size +
            (maximumBuffers - 1) * MemoryLayout<AudioBuffer>.stride
        let raw = UnsafeMutableRawPointer.allocate(
            byteCount: byteCount,
            alignment: MemoryLayout<AudioBufferList>.alignment
        )
        defer { raw.deallocate() }
        let list = raw.bindMemory(to: AudioBufferList.self, capacity: 1)
        list.pointee.mNumberBuffers = UInt32(maximumBuffers)
        return try body(UnsafeMutableAudioBufferListPointer(list))
    }
}

@MainActor
private final class ControlledPlayback: SceneAudioPlayback {
    private(set) var playbackState = SceneAudioPlaybackState.stopped
    private(set) var position: TimeInterval = 0
    let duration: TimeInterval = 10
    let loops: Bool
    private(set) var volume: Float
    var failsToPlay = false
    private(set) var playCount = 0
    private(set) var pauseCount = 0
    private(set) var stopCount = 0

    init(loop: Bool, volume: Float) {
        loops = loop
        self.volume = volume
    }

    var isPlaying: Bool { playbackState == .playing }

    func play() throws {
        guard !failsToPlay else { throw SceneAudioError.playbackFailed(nil) }
        playCount += 1
        playbackState = .playing
    }

    func pause() {
        pauseCount += 1
        playbackState = .paused
    }
    func stop() {
        stopCount += 1
        playbackState = .stopped
        position = 0
    }

    func advance(to position: TimeInterval) {
        self.position = position
    }

    func finishNaturally() {
        playbackState = .ended
        position = duration
    }

    func setVolume(_ volume: Float) throws {
        guard volume.isFinite, (0...1).contains(volume) else {
            throw SceneAudioError.invalidVolume(volume)
        }
        self.volume = volume
    }

    func restore(_ snapshot: SceneAudioPlayerState) throws {
        volume = snapshot.volume
        playbackState = snapshot.state
        position = snapshot.position
    }
}

private struct SyntheticAudioLoadFailure: LocalizedError {
    var errorDescription: String? { "synthetic audio asset failure" }
}
