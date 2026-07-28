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

    func testOwnerCoordinatorKeepsScreenActiveUntilEveryRegistrationIsReleased() {
        let coordinator = SceneAudioOwnerCoordinator(mainScreenId: "screen-a")

        coordinator.register(screenId: "screen-a")
        coordinator.register(screenId: "screen-a")
        coordinator.unregister(screenId: "screen-a")

        XCTAssertEqual(coordinator.activeScreenIds, ["screen-a"])
        XCTAssertTrue(coordinator.isAudible(screenId: "screen-a"))

        coordinator.unregister(screenId: "screen-a")

        XCTAssertTrue(coordinator.activeScreenIds.isEmpty)
        XCTAssertNil(coordinator.ownerScreenId)
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

    func testCapturePolicyRequiresPermissionActiveSessionAndDemand() {
        var policy = SceneAudioCapturePolicy(
            isCaptureAllowed: false,
            isSuspended: false,
            demandCount: 1
        )
        XCTAssertFalse(policy.shouldRun)

        policy.isCaptureAllowed = true
        policy.demandCount = 0
        XCTAssertFalse(policy.shouldRun)

        policy.demandCount = 1
        XCTAssertTrue(policy.shouldRun)

        policy.isSuspended = true
        XCTAssertFalse(policy.shouldRun)

        policy.isSuspended = false
        XCTAssertTrue(policy.shouldRun)

        policy.demandCount = 0
        XCTAssertFalse(policy.shouldRun)
    }

    func testBlockingSystemAudioStartupDoesNotBlockMainActor() async throws {
        let startupBegan = expectation(description: "Core Audio startup began")
        let waitStartedAt = Date()
        let task = Task { @MainActor in
            try await performSystemAudioCaptureStartup {
                startupBegan.fulfill()
                Thread.sleep(forTimeInterval: 0.5)
                return 7
            }
        }

        await fulfillment(of: [startupBegan], timeout: 1)

        XCTAssertLessThan(
            Date().timeIntervalSince(waitStartedAt),
            0.25,
            "A blocking Core Audio call must not occupy MainActor"
        )
        let result = try await task.value
        XCTAssertEqual(result, 7)
    }

    func testSystemAudioTapIsPrivateGlobalMixExcludingCurrentProcess() {
        let processObjectID = AudioObjectID(42)
        let uuid = UUID(uuidString: "61A39357-10FE-4A51-8A92-1B96F210F890")!

        let description = makeSystemAudioTapDescription(
            excluding: processObjectID,
            uuid: uuid
        )

        XCTAssertEqual(description.processes, [processObjectID])
        XCTAssertTrue(description.isExclusive)
        XCTAssertTrue(description.isPrivate)
        XCTAssertTrue(description.isMono)
        XCTAssertTrue(description.isMixdown)
        XCTAssertEqual(description.muteBehavior, .unmuted)
        XCTAssertEqual(description.uuid, uuid)
    }

    func testSystemAudioAggregateStartsImmediatelyWithoutPhysicalSubdevices() throws {
        let tapUUID = UUID(uuidString: "61A39357-10FE-4A51-8A92-1B96F210F890")!
        let aggregateUUID = UUID(uuidString: "16CCDF99-7C31-4C8F-81C2-D7C2E8AA23CE")!

        let description = makeSystemAudioAggregateDescription(
            tapUUID: tapUUID,
            aggregateUUID: aggregateUUID
        )

        XCTAssertEqual(description[kAudioAggregateDeviceIsPrivateKey] as? Bool, true)
        XCTAssertEqual(description[kAudioAggregateDeviceIsStackedKey] as? Bool, false)
        XCTAssertEqual(description[kAudioAggregateDeviceTapAutoStartKey] as? Bool, false)
        let subdevices = try XCTUnwrap(
            description[kAudioAggregateDeviceSubDeviceListKey] as? [Any]
        )
        XCTAssertTrue(subdevices.isEmpty)
        let taps = try XCTUnwrap(
            description[kAudioAggregateDeviceTapListKey] as? [[String: Any]]
        )
        XCTAssertEqual(taps.count, 1)
        XCTAssertEqual(taps[0][kAudioSubTapUIDKey] as? String, tapUUID.uuidString)
        XCTAssertEqual(taps[0][kAudioSubTapDriftCompensationKey] as? Bool, true)
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
        let sounds = [sound(
            objectId: 7,
            visible: true,
            sources: [
                source(index: 0, resource: "a.wav"),
                source(index: 1, resource: "b.wav"),
            ],
            volume: 0.8,
            startSilent: true
        )]

        try controller.reconcile(sounds, masterVolume: 0.5, audioOutput: 0.25, loadAsset: loader)
        XCTAssertEqual(controller.playerCount, 1)
        XCTAssertEqual(loads, ["a.wav"])
        XCTAssertEqual(
            try controller.playerState(identifier: "sound:7").volume,
            0.1,
            accuracy: 0.001
        )

        try controller.reconcile(sounds, masterVolume: 0.25, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(loads.count, 1, "A stable sound object must not decode again")
        XCTAssertEqual(
            try controller.playerState(identifier: "sound:7").volume,
            0.2,
            accuracy: 0.001
        )
    }

    func testSoundObjectOwnsOnlyOneActivePlayerAcrossCandidateResources() throws {
        let (controller, created) = controlledController()
        let sounds = [sound(
            objectId: 70,
            visible: true,
            sources: [
                source(index: 0, resource: "first.wav"),
                source(index: 1, resource: "second.wav"),
            ]
        )]

        try controller.reconcile(
            sounds,
            masterVolume: 1,
            audioOutput: 1,
            loadAsset: { _ in self.waveData() }
        )

        XCTAssertEqual(controller.playerCount, 1)
        XCTAssertEqual(created().count, 1)
        XCTAssertEqual(created().first?.playbackState, .playing)
    }

    func testLoopModeAdvancesCandidateResourcesSequentially() throws {
        var created: [ControlledPlayback] = []
        let controller = SceneAudioController(makePlayer: { _, loop, volume in
            let player = ControlledPlayback(loop: loop, volume: volume)
            created.append(player)
            return player
        })
        let snapshot = sound(
            objectId: 71,
            visible: true,
            sources: [
                source(index: 0, resource: "first.wav"),
                source(index: 1, resource: "second.wav"),
            ]
        )
        var loads: [String] = []
        let loader: (String) throws -> Data = {
            loads.append($0)
            return self.waveData()
        }

        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(loads, ["first.wav"])
        XCTAssertEqual(created.count, 1)
        XCTAssertFalse(created[0].loops)
        XCTAssertTrue(created[0].isPlaying)

        created[0].finishNaturally()
        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(loads, ["first.wav", "second.wav"])
        XCTAssertEqual(created.count, 2)
        XCTAssertEqual(created[0].playbackState, .stopped)
        XCTAssertTrue(created[1].isPlaying)

        created[1].finishNaturally()
        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(loads, ["first.wav", "second.wav", "first.wav"])
        XCTAssertEqual(controller.playerCount, 1)
        XCTAssertEqual(created.count, 3)
        XCTAssertEqual(created[1].playbackState, .stopped)
        XCTAssertTrue(created[2].isPlaying)
    }

    func testSingleModeChoosesOneCandidateAndStopsAfterCompletion() throws {
        var created: [ControlledPlayback] = []
        var selections = [1]
        let controller = SceneAudioController(
            makePlayer: { _, loop, volume in
                let player = ControlledPlayback(loop: loop, volume: volume)
                created.append(player)
                return player
            },
            selectRandomSource: { count in
                let selected = selections.removeFirst()
                XCTAssertTrue((0..<count).contains(selected))
                return selected
            }
        )
        let snapshot = sound(
            objectId: 72,
            visible: true,
            sources: [
                source(index: 0, resource: "first.wav"),
                source(index: 1, resource: "second.wav"),
            ],
            playbackMode: .single
        )
        var loads: [String] = []
        let loader: (String) throws -> Data = {
            loads.append($0)
            return self.waveData()
        }

        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(loads, ["second.wav"])
        XCTAssertTrue(selections.isEmpty)
        let player = try XCTUnwrap(created.first)
        XCTAssertEqual(player.playCount, 1)

        player.finishNaturally()
        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(loads, ["second.wav"])
        XCTAssertEqual(player.playbackState, .ended)
        XCTAssertEqual(player.playCount, 1)
    }

    func testRandomModeWaitsWithinBoundsThenChoosesNextCandidate() throws {
        var now: TimeInterval = 100
        var selections = [1, 0]
        var randomUnits = [0.25]
        var created: [ControlledPlayback] = []
        let controller = SceneAudioController(
            makePlayer: { _, loop, volume in
                let player = ControlledPlayback(loop: loop, volume: volume)
                created.append(player)
                return player
            },
            currentTime: { now },
            selectRandomSource: { count in
                let selected = selections.removeFirst()
                XCTAssertTrue((0..<count).contains(selected))
                return selected
            },
            randomUnit: { randomUnits.removeFirst() }
        )
        let snapshot = sound(
            objectId: 73,
            visible: true,
            sources: [
                source(index: 0, resource: "first.wav"),
                source(index: 1, resource: "second.wav"),
            ],
            playbackMode: .random,
            minimumTime: 2,
            maximumTime: 6
        )
        var loads: [String] = []
        let loader: (String) throws -> Data = {
            loads.append($0)
            return self.waveData()
        }

        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(loads, ["second.wav"])
        let firstPlayer = try XCTUnwrap(created.first)
        firstPlayer.finishNaturally()

        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(loads, ["second.wav"])
        XCTAssertEqual(firstPlayer.playbackState, .ended)

        now = 102.999
        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(created.count, 1)

        now = 103
        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(loads, ["second.wav", "first.wav"])
        XCTAssertEqual(created.count, 2)
        XCTAssertEqual(firstPlayer.playbackState, .stopped)
        XCTAssertTrue(created[1].isPlaying)
        XCTAssertTrue(selections.isEmpty)
        XCTAssertTrue(randomUnits.isEmpty)
    }

    func testRandomStartSilentDelaysInsteadOfDisablingAutomaticPlayback() throws {
        var now: TimeInterval = 10
        var randomUnits = [0.5]
        var created: [ControlledPlayback] = []
        let controller = SceneAudioController(
            makePlayer: { _, loop, volume in
                let player = ControlledPlayback(loop: loop, volume: volume)
                created.append(player)
                return player
            },
            currentTime: { now },
            selectRandomSource: { _ in 0 },
            randomUnit: { randomUnits.removeFirst() }
        )
        let snapshot = sound(
            objectId: 74,
            visible: true,
            sources: [source(index: 0, resource: "random.wav")],
            playbackMode: .random,
            startSilent: true,
            minimumTime: 4,
            maximumTime: 8
        )
        let loader: (String) throws -> Data = { _ in self.waveData() }

        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        let player = try XCTUnwrap(created.first)
        XCTAssertFalse(player.isPlaying)
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:74"))

        now = 15.999
        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertFalse(player.isPlaying)

        now = 16
        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.playCount, 1)
        XCTAssertTrue(randomUnits.isEmpty)
    }

    func testRandomWaitFreezesAcrossHostPauseAndVisibilityChanges() throws {
        var now: TimeInterval = 0
        var selections = [0, 0]
        var randomUnits = [0.0, 0.0]
        var created: [ControlledPlayback] = []
        let controller = SceneAudioController(
            makePlayer: { _, loop, volume in
                let player = ControlledPlayback(loop: loop, volume: volume)
                created.append(player)
                return player
            },
            currentTime: { now },
            selectRandomSource: { _ in selections.removeFirst() },
            randomUnit: { randomUnits.removeFirst() }
        )
        let visible = sound(
            objectId: 75,
            visible: true,
            sources: [source(index: 0, resource: "random.wav")],
            playbackMode: .random,
            startSilent: true,
            minimumTime: 10,
            maximumTime: 10
        )
        let hidden = sound(
            objectId: 75,
            visible: false,
            sources: visible.sources,
            playbackMode: .random,
            startSilent: true,
            minimumTime: 10,
            maximumTime: 10
        )
        let loader: (String) throws -> Data = { _ in self.waveData() }

        try controller.reconcile(
            [visible], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        let player = try XCTUnwrap(created.first)
        now = 3
        controller.pauseAll()
        now = 30
        try controller.resumeAll()

        now = 36.999
        try controller.reconcile(
            [visible], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertFalse(player.isPlaying)
        now = 37
        try controller.reconcile(
            [visible], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertTrue(player.isPlaying)

        now = 40
        player.finishNaturally()
        try controller.reconcile(
            [visible], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        now = 43
        try controller.reconcile(
            [hidden], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        now = 80
        try controller.reconcile(
            [visible], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        now = 86.999
        try controller.reconcile(
            [visible], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(player.playCount, 1)
        now = 87
        try controller.reconcile(
            [visible], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(player.playCount, 2)
        XCTAssertTrue(player.isPlaying)
        XCTAssertTrue(selections.isEmpty)
        XCTAssertTrue(randomUnits.isEmpty)
    }

    func testCandidateFailuresTryRemainingAssetsAndReportWhenAllFail() throws {
        let (controller, created) = controlledController()
        let snapshot = sound(
            objectId: 76,
            visible: true,
            sources: [
                source(index: 0, resource: "broken.wav"),
                source(index: 1, resource: "working.wav"),
            ]
        )
        var loads: [String] = []
        try controller.reconcile(
            [snapshot], masterVolume: 1, audioOutput: 1
        ) { resource in
            loads.append(resource)
            if resource == "broken.wav" {
                throw SyntheticAudioLoadFailure()
            }
            return self.waveData()
        }
        XCTAssertEqual(loads, ["broken.wav", "working.wav"])
        XCTAssertEqual(controller.playerCount, 1)
        XCTAssertEqual(created().count, 1)
        XCTAssertTrue(created()[0].isPlaying)

        let failedController = SceneAudioController()
        let allBroken = sound(
            objectId: 77,
            visible: true,
            sources: [
                source(index: 0, resource: "first.wav"),
                source(index: 1, resource: "second.wav"),
            ]
        )
        XCTAssertThrowsError(try failedController.reconcile(
            [allBroken],
            masterVolume: 1,
            audioOutput: 1,
            loadAsset: { _ in throw SyntheticAudioLoadFailure() }
        )) { error in
            guard case let SceneAudioError.allCandidateAssetsFailed(objectId, failures) = error else {
                return XCTFail("Unexpected error: \(error)")
            }
            XCTAssertEqual(objectId, 77)
            XCTAssertEqual(failures.count, 2)
            XCTAssertTrue(failures[0].contains("first.wav"))
            XCTAssertTrue(failures[1].contains("second.wav"))
        }
        XCTAssertEqual(failedController.playerCount, 0)
    }

    func testReconcileRebuildsForCandidateChangeAndStopsMissingObject() throws {
        let controller = SceneAudioController()
        var loads: [String] = []
        let loader: (String) throws -> Data = { path in
            loads.append(path)
            return self.waveData()
        }

        try controller.reconcile([
            sound(
                objectId: 1,
                visible: true,
                sources: [
                    source(index: 0, resource: "first.wav"),
                    source(index: 1, resource: "second.wav"),
                ],
                startSilent: true
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        try controller.reconcile([
            sound(
                objectId: 1,
                visible: true,
                sources: [source(index: 0, resource: "replacement.wav")],
                startSilent: true
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)

        XCTAssertEqual(loads, ["first.wav", "replacement.wav"])
        XCTAssertEqual(controller.playerCount, 1)
        XCTAssertTrue(try controller.playerState(identifier: "sound:1").loops)

        try controller.reconcile([], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(controller.playerCount, 0)
    }

    func testReconcileFailureLeavesPreviousSetUntouched() throws {
        let controller = SceneAudioController()
        try controller.reconcile([
            sound(
                objectId: 3,
                visible: true,
                sources: [source(index: 0, resource: "valid.wav")],
                playbackMode: .single,
                volume: 0.4,
                startSilent: true
            ),
        ], masterVolume: 1, audioOutput: 1) { _ in self.waveData() }

        XCTAssertThrowsError(try controller.reconcile([
            sound(
                objectId: 3,
                visible: true,
                sources: [source(index: 0, resource: "broken.wav")],
                playbackMode: .single,
                volume: 0.9,
                startSilent: true
            ),
        ], masterVolume: 1, audioOutput: 1) { _ in Data() }) {
            XCTAssertEqual($0 as? SceneAudioError, .emptyData)
        }

        XCTAssertEqual(controller.playerCount, 1)
        let state = try controller.playerState(identifier: "sound:3")
        XCTAssertEqual(state.volume, 0.4, accuracy: 0.001)
        XCTAssertFalse(state.loops)
    }

    func testPreparationFailureStopsPlayersCreatedEarlierInTransaction() throws {
        var created: [ControlledPlayback] = []
        let controller = SceneAudioController(makePlayer: { _, loop, volume in
            let player = ControlledPlayback(loop: loop, volume: volume)
            created.append(player)
            return player
        })
        let sounds = [
            sound(
                objectId: 78,
                visible: true,
                sources: [source(index: 0, resource: "working.wav")],
                startSilent: true
            ),
            sound(
                objectId: 79,
                visible: true,
                sources: [source(index: 0, resource: "broken.wav")],
                startSilent: true
            ),
        ]

        XCTAssertThrowsError(try controller.reconcile(
            sounds,
            masterVolume: 1,
            audioOutput: 1
        ) { resource in
            if resource == "broken.wav" {
                throw SyntheticAudioLoadFailure()
            }
            return self.waveData()
        })
        XCTAssertEqual(controller.playerCount, 0)
        XCTAssertEqual(created.count, 1)
        XCTAssertEqual(created[0].stopCount, 1)
    }

    func testReconcileValidatesEveryVolumeBeforeLoadingOrMutating() throws {
        let controller = SceneAudioController()
        var loadCount = 0
        let invalid = [sound(
            objectId: 4,
            visible: true,
            sources: [source(index: 0, resource: "invalid.wav")],
            playbackMode: .single,
            volume: 1.01,
            startSilent: true
        )]

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
        let once = [sound(
            objectId: 10,
            visible: true,
            sources: [source(index: 0, resource: "once.wav")],
            playbackMode: .single
        )]

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
        let visible = [sound(
            objectId: 11,
            visible: true,
            sources: [source(index: 0, resource: "once.wav")],
            playbackMode: .single
        )]
        let hidden = [sound(
            objectId: 11,
            visible: false,
            sources: visible[0].sources,
            playbackMode: .single
        )]
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
        let sounds = [
            sound(
                objectId: 12,
                visible: true,
                sources: [source(index: 0, resource: "once.wav")],
                playbackMode: .single
            ),
            sound(
                objectId: 13,
                visible: true,
                sources: [source(index: 0, resource: "loop.wav")]
            ),
        ]
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
        let silent = [sound(
            objectId: 5,
            visible: true,
            sources: [source(index: 0, resource: "silent.wav")],
            startSilent: true
        )]
        try controller.reconcile(silent, masterVolume: 1, audioOutput: 1) { _ in self.waveData() }
        XCTAssertFalse(try controller.playerState(identifier: "sound:5").isPlaying)
        XCTAssertFalse(try controller.playbackIntent(identifier: "sound:5"))
        let enabled = [sound(
            objectId: 5,
            visible: true,
            sources: [source(index: 0, resource: "silent.wav")]
        )]
        try controller.reconcile(enabled, masterVolume: 1, audioOutput: 1) { _ in
            XCTFail("Changing startSilent must not rebuild the player")
            return self.waveData()
        }
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:5"))

        let automaticVisible = [sound(
            objectId: 6,
            visible: true,
            sources: [source(index: 0, resource: "auto.wav")]
        )]
        try controller.reconcile(automaticVisible, masterVolume: 1, audioOutput: 1) { _ in self.waveData() }
        XCTAssertFalse(try controller.playerState(identifier: "sound:6").isPlaying)
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:6"))

        let automaticHidden = [sound(objectId: 6, visible: false, sources: automaticVisible[0].sources)]
        try controller.reconcile(automaticHidden, masterVolume: 1, audioOutput: 1) { _ in
            XCTFail("Visibility changes must not reload the asset")
            return self.waveData()
        }
        XCTAssertFalse(try controller.playerState(identifier: "sound:6").isPlaying)
        XCTAssertFalse(try controller.playbackIntent(identifier: "sound:6"))
        try controller.reconcile(automaticVisible, masterVolume: 1, audioOutput: 1) { _ in
            XCTFail("Visibility restoration must not reload the asset")
            return self.waveData()
        }
        XCTAssertFalse(try controller.playerState(identifier: "sound:6").isPlaying)
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:6"))
    }

    func testAutomaticPlaybackResumesWhenStartSilentIsDisabledAgain() throws {
        let (controller, created) = controlledController()
        let enabled = sound(
            objectId: 14,
            visible: true,
            sources: [source(index: 0, resource: "auto.wav")]
        )
        let silent = sound(
            objectId: 14,
            visible: true,
            sources: enabled.sources,
            startSilent: true
        )
        let loader: (String) throws -> Data = { _ in self.waveData() }

        try controller.reconcile(
            [enabled], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        let player = try XCTUnwrap(created().first)
        XCTAssertTrue(player.isPlaying)

        try controller.reconcile(
            [silent], masterVolume: 1, audioOutput: 1, loadAsset: loader
        )
        XCTAssertEqual(player.playbackState, .paused)

        try controller.reconcile(
            [enabled], masterVolume: 1, audioOutput: 1
        ) { _ in
            XCTFail("Changing startSilent must not reload the asset")
            return self.waveData()
        }
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.playCount, 2)
    }

    func testStopAllClearsReconciledSourcesAndPauseState() throws {
        let controller = SceneAudioController()
        let sounds = [sound(
            objectId: 8,
            visible: true,
            sources: [source(index: 0, resource: "a.wav")],
            playbackMode: .single,
            startSilent: true
        )]
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
            resource: "script.wav"
        )
        try controller.reconcile([
            sound(
                objectId: 20,
                visible: true,
                sources: [baseSource],
                playbackMode: .single,
                startSilent: true,
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
                playbackMode: .single,
                startSilent: true,
                playbackCommand: command(.play, generation: 2)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.playCount, 1, "play() while playing must be a no-op")
    }

    func testDeferredFreshPlayUsesTheSameCandidateTransitionAsImmediatePlay() throws {
        var selections = [0, 1]
        var created: [ControlledPlayback] = []
        let controller = SceneAudioController(
            makePlayer: { _, loop, volume in
                let player = ControlledPlayback(loop: loop, volume: volume)
                created.append(player)
                return player
            },
            selectRandomSource: { _ in selections.removeFirst() }
        )
        let sources = [
            source(index: 0, resource: "first.wav"),
            source(index: 1, resource: "second.wav"),
        ]
        func snapshot(
            visible: Bool,
            generation: UInt64
        ) -> SceneSoundSnapshot {
            sound(
                objectId: 24,
                visible: visible,
                sources: sources,
                playbackMode: .single,
                startSilent: true,
                playbackCommand: command(.play, generation: generation)
            )
        }
        var loads: [String] = []
        let loader: (String) throws -> Data = {
            loads.append($0)
            return self.waveData()
        }

        try controller.reconcile(
            [snapshot(visible: true, generation: 1)],
            masterVolume: 1,
            audioOutput: 1,
            loadAsset: loader
        )
        created[0].finishNaturally()
        try controller.reconcile(
            [snapshot(visible: false, generation: 2)],
            masterVolume: 1,
            audioOutput: 1,
            loadAsset: loader
        )
        try controller.reconcile(
            [snapshot(visible: true, generation: 2)],
            masterVolume: 1,
            audioOutput: 1,
            loadAsset: loader
        )

        XCTAssertEqual(loads, ["first.wav", "second.wav"])
        XCTAssertEqual(created.count, 2)
        XCTAssertEqual(created[0].playbackState, .stopped)
        XCTAssertTrue(try XCTUnwrap(created.dropFirst().first).isPlaying)
        XCTAssertTrue(selections.isEmpty)
    }

    func testPauseThenPlayResumesWithoutResettingPosition() throws {
        let (controller, created) = controlledController()
        let loader: (String) throws -> Data = { _ in self.waveData() }
        let baseSource = source(
            index: 0,
            resource: "script.wav"
        )
        try controller.reconcile([
            sound(
                objectId: 21,
                visible: true,
                sources: [baseSource],
                playbackMode: .single,
                startSilent: true,
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
                playbackMode: .single,
                startSilent: true,
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
                playbackMode: .single,
                startSilent: true,
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
            resource: "script.wav"
        )
        func snapshot(_ action: SceneSoundPlaybackCommand.Action, _ generation: UInt64) -> [SceneSoundSnapshot] {
            [sound(
                objectId: 22,
                visible: true,
                sources: [baseSource],
                playbackMode: .single,
                startSilent: true,
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
                playbackMode: .single,
                startSilent: true,
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
            resource: "truth.wav"
        )
        let sounds = [sound(
            objectId: 23,
            visible: true,
            sources: [source],
            playbackMode: .single,
            startSilent: true,
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

        let playerState = try controller.playerState(identifier: "sound:23")
        XCTAssertEqual(playerState.state, .ended)
        XCTAssertFalse(playerState.isPlaying)
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:23"))
        XCTAssertEqual(
            controller.soundRuntimeSnapshots(),
            [SceneSoundRuntimeSnapshot(objectId: 23, state: .ended, position: player.duration)]
        )
    }

    func testPlaybackCommandGenerationCannotMoveBackwardOrChangeMeaning() throws {
        let (controller, created) = controlledController()
        let source = source(
            index: 0,
            resource: "ordered.wav"
        )
        let loader: (String) throws -> Data = { _ in self.waveData() }
        try controller.reconcile([
            sound(
                objectId: 26,
                visible: true,
                sources: [source],
                playbackMode: .single,
                startSilent: true,
                playbackCommand: command(.play, generation: 5)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        let player = try XCTUnwrap(created().first)

        XCTAssertThrowsError(try controller.reconcile([
            sound(
                objectId: 26,
                visible: true,
                sources: [source],
                playbackMode: .single,
                startSilent: true,
                playbackCommand: command(.pause, generation: 4)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)) {
            XCTAssertEqual(
                $0 as? SceneAudioError,
                .stalePlaybackCommand(identifier: "sound:26", current: 5, received: 4)
            )
        }
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.pauseCount, 0)

        XCTAssertThrowsError(try controller.reconcile([
            sound(
                objectId: 26,
                visible: true,
                sources: [source],
                playbackMode: .single,
                startSilent: true,
                playbackCommand: command(.pause, generation: 5)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)) {
            XCTAssertEqual(
                $0 as? SceneAudioError,
                .conflictingPlaybackCommand(identifier: "sound:26", generation: 5)
            )
        }
        XCTAssertTrue(player.isPlaying)
        XCTAssertEqual(player.pauseCount, 0)
    }

    func testPlaybackFailureRestoresRuntimeStateAndPositionTransactionally() throws {
        let (controller, created) = controlledController()
        let first = source(
            index: 0,
            resource: "first.wav"
        )
        let second = source(
            index: 0,
            resource: "second.wav"
        )
        let loader: (String) throws -> Data = { _ in self.waveData() }
        try controller.reconcile([
            sound(
                objectId: 27,
                visible: true,
                sources: [first],
                playbackMode: .single,
                startSilent: true,
                playbackCommand: command(.play, generation: 1)
            ),
            sound(
                objectId: 28,
                visible: true,
                sources: [second],
                playbackMode: .single,
                startSilent: true
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)
        XCTAssertEqual(created().count, 2)
        created()[0].advance(to: 4.25)
        created()[1].failsToPlay = true

        XCTAssertThrowsError(try controller.reconcile([
            sound(
                objectId: 27,
                visible: true,
                sources: [first],
                playbackMode: .single,
                startSilent: true,
                playbackCommand: command(.stop, generation: 2)
            ),
            sound(
                objectId: 28,
                visible: true,
                sources: [second],
                playbackMode: .single,
                startSilent: true,
                playbackCommand: command(.play, generation: 1)
            ),
        ], masterVolume: 1, audioOutput: 1, loadAsset: loader)) {
            XCTAssertEqual($0 as? SceneAudioError, .playbackFailed("sound:28"))
        }

        let restored = try controller.playerState(identifier: "sound:27")
        XCTAssertEqual(restored.state, .playing)
        XCTAssertEqual(restored.position, 4.25)
        XCTAssertEqual(
            try controller.playerState(identifier: "sound:28").state,
            .stopped
        )
    }

    func testTimedPlaybackBoundsAreAcceptedForEveryPlaybackMode() throws {
        for (objectId, mode) in [
            (24, SceneSoundPlaybackMode.loop),
            (25, .random),
            (26, .single),
        ] {
            let (controller, created) = controlledController()
            var loadCount = 0
            try controller.reconcile([
                sound(
                    objectId: objectId,
                    visible: true,
                    sources: [source(index: 0, resource: "timed.wav")],
                    playbackMode: mode,
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
            XCTAssertEqual(created()[0].loops, mode == .loop)
        }
    }

    func testSynchronizationFailureIsIsolatedToItsSoundObject() throws {
        let (controller, created) = controlledController()
        let working = sound(
            objectId: 31,
            visible: true,
            sources: [source(index: 0, resource: "working.wav")]
        )
        let broken = sound(
            objectId: 32,
            visible: true,
            sources: [source(index: 0, resource: "broken.wav")]
        )
        let issues = controller.synchronize(
            [working, broken],
            masterVolume: 1,
            audioOutput: 1
        ) { resource in
            if resource == "broken.wav" {
                throw SyntheticAudioLoadFailure()
            }
            return self.waveData()
        }

        XCTAssertEqual(issues, [SceneAudioSynchronizationIssue(
            objectId: 32,
            message: "synthetic audio asset failure"
        )])
        XCTAssertEqual(controller.playerCount, 1)
        XCTAssertEqual(created().count, 1)
        XCTAssertEqual(created().first?.playbackState, .playing)
        XCTAssertEqual(
            try controller.playerState(identifier: "sound:31").state,
            .playing
        )
        XCTAssertThrowsError(try controller.playerState(identifier: "sound:32")) {
            XCTAssertEqual($0 as? SceneAudioError, .unknownPlayer("sound:32"))
        }
    }

    func testSynchronizationGlobalFailureIsExplicitAndClearsAudioState() throws {
        let (controller, created) = controlledController()
        let working = sound(
            objectId: 33,
            visible: true,
            sources: [source(index: 0, resource: "working.wav")]
        )
        try controller.reconcile(
            [working],
            masterVolume: 1,
            audioOutput: 1
        ) { _ in self.waveData() }

        let issues = controller.synchronize(
            [working],
            masterVolume: 2,
            audioOutput: 1
        ) { _ in
            XCTFail("Global validation must happen before loading assets")
            return self.waveData()
        }

        XCTAssertEqual(issues, [SceneAudioSynchronizationIssue(
            message: SceneAudioError.invalidVolume(2).localizedDescription
        )])
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
            sound(
                objectId: 9,
                visible: true,
                sources: [source(index: 0, resource: "first.wav")]
            ),
            sound(
                objectId: 10,
                visible: true,
                sources: [source(index: 0, resource: "second.wav")]
            ),
        ], masterVolume: 1, audioOutput: 1) { _ in self.waveData() }
        XCTAssertEqual(created.count, 2)
        created[1].failsToPlay = true

        XCTAssertThrowsError(try controller.resumeAll()) {
            XCTAssertEqual($0 as? SceneAudioError, .playbackFailed("sound:10"))
        }
        XCTAssertFalse(created[0].isPlaying)
        XCTAssertFalse(created[1].isPlaying)
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:9"))
        XCTAssertTrue(try controller.playbackIntent(identifier: "sound:10"))

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
        playbackMode: SceneSoundPlaybackMode = .loop,
        volume: Float = 1,
        startSilent: Bool = false,
        playbackCommand: SceneSoundPlaybackCommand? = nil,
        minimumTime: Double = 0,
        maximumTime: Double = 0
    ) -> SceneSoundSnapshot {
        SceneSoundSnapshot(
            objectId: objectId,
            visible: visible,
            sources: sources,
            playbackMode: playbackMode,
            volume: volume,
            startSilent: startSilent,
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
        resource: String
    ) -> SceneSoundSourceSnapshot {
        SceneSoundSourceSnapshot(
            sourceIndex: index,
            resource: resource
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
