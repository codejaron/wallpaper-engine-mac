import AVFoundation
import Combine
import Foundation
import SceneRuntimeBridge

public enum SceneAudioError: LocalizedError, Equatable {
    case asset(String)
    case emptyData
    case decode(String)
    case invalidVolume(Float)
    case playbackFailed(String?)
    case unknownPlayer(String)
    case duplicateSource(String)
    case rollbackFailed(String)
    case invalidPlaybackCommand(String)
    case stalePlaybackCommand(identifier: String, current: UInt64, received: UInt64)
    case conflictingPlaybackCommand(identifier: String, generation: UInt64)
    case invalidTimingBounds(objectId: Int, minimumTime: Double, maximumTime: Double)
    case allCandidateAssetsFailed(objectId: Int, failures: [String])

    public var errorDescription: String? {
        switch self {
        case .asset(let message): return message
        case .emptyData: return "Scene audio asset is empty"
        case .decode(let message): return "Decoding Scene audio failed: \(message)"
        case .invalidVolume(let volume): return "Scene audio volume is outside 0...1: \(volume)"
        case .playbackFailed(let identifier):
            return identifier.map { "Starting Scene audio playback failed: \($0)" }
                ?? "Starting Scene audio playback failed"
        case .unknownPlayer(let identifier): return "Scene audio player does not exist: \(identifier)"
        case .duplicateSource(let identifier): return "Scene audio snapshot contains duplicate source: \(identifier)"
        case .rollbackFailed(let message): return "Rolling back Scene audio transaction failed: \(message)"
        case .invalidPlaybackCommand(let message): return "Scene audio playback command is invalid: \(message)"
        case .stalePlaybackCommand(let identifier, let current, let received):
            return "Scene audio playback command for \(identifier) moved backwards from generation \(current) to \(received)"
        case .conflictingPlaybackCommand(let identifier, let generation):
            return "Scene audio playback command generation \(generation) changed action for \(identifier)"
        case .invalidTimingBounds(let objectId, let minimumTime, let maximumTime):
            return "Scene sound object \(objectId) has invalid playback timing bounds \(minimumTime)...\(maximumTime)"
        case .allCandidateAssetsFailed(let objectId, let failures):
            return "Scene sound object \(objectId) could not load any candidate asset: " +
                failures.joined(separator: "; ")
        }
    }
}

public struct SceneSoundSourceSnapshot: Equatable, Sendable {
    public let sourceIndex: Int
    public let resource: String

    public init(sourceIndex: Int, resource: String) {
        self.sourceIndex = sourceIndex
        self.resource = resource
    }
}

public enum SceneSoundPlaybackMode: Int32, Equatable, Sendable {
    case loop = 1
    case random = 2
    case single = 3
}

public struct SceneSoundPlaybackCommand: Equatable, Sendable {
    public enum Action: Int, Equatable, Sendable {
        case play = 1
        case pause = 2
        case stop = 3
    }

    public let action: Action
    public let generation: UInt64

    public init(action: Action, generation: UInt64) {
        self.action = action
        self.generation = generation
    }
}

public struct SceneSoundSnapshot: Equatable, Sendable {
    public let objectId: Int
    public let visible: Bool
    public let sources: [SceneSoundSourceSnapshot]
    public let playbackMode: SceneSoundPlaybackMode
    public let volume: Float
    public let startSilent: Bool
    public let playbackCommand: SceneSoundPlaybackCommand?
    public let minimumTime: Double
    public let maximumTime: Double

    public init(
        objectId: Int,
        visible: Bool,
        sources: [SceneSoundSourceSnapshot],
        playbackMode: SceneSoundPlaybackMode = .loop,
        volume: Float = 1,
        startSilent: Bool = false,
        playbackCommand: SceneSoundPlaybackCommand? = nil,
        minimumTime: Double = 0,
        maximumTime: Double = 0
    ) {
        self.objectId = objectId
        self.visible = visible
        self.sources = sources
        self.playbackMode = playbackMode
        self.volume = volume
        self.startSilent = startSilent
        self.playbackCommand = playbackCommand
        self.minimumTime = minimumTime
        self.maximumTime = maximumTime
    }
}

public struct SceneAudioSynchronizationIssue: Equatable, Sendable {
    public let objectId: Int?
    public let message: String

    public init(objectId: Int? = nil, message: String) {
        self.objectId = objectId
        self.message = message
    }
}

public enum SceneAudioPlaybackState: Int32, Equatable, Sendable {
    case stopped = 0
    case playing = 1
    case paused = 2
    case ended = 3
}

public struct SceneAudioPlayerState: Equatable, Sendable {
    public let state: SceneAudioPlaybackState
    public let position: TimeInterval
    public let volume: Float
    public let loops: Bool

    public init(
        state: SceneAudioPlaybackState,
        position: TimeInterval,
        volume: Float,
        loops: Bool
    ) {
        self.state = state
        self.position = position
        self.volume = volume
        self.loops = loops
    }

    public var isPlaying: Bool { state == .playing }
}

public struct SceneSoundRuntimeSnapshot: Equatable, Sendable {
    public let objectId: Int
    public let state: SceneAudioPlaybackState
    public let position: TimeInterval

    public init(objectId: Int, state: SceneAudioPlaybackState, position: TimeInterval) {
        self.objectId = objectId
        self.state = state
        self.position = position
    }

    public var isPlaying: Bool { state == .playing }
}

@MainActor
public final class SceneAudioOwnerCoordinator: ObservableObject {
    public typealias OwnerDidChange = (
        _ previousOwnerScreenId: String?,
        _ ownerScreenId: String?
    ) -> Void

    private var activeScreenRegistrationCounts: [String: Int] = [:]
    private let ownerDidChange: OwnerDidChange?

    public private(set) var mainScreenId: String?
    @Published public private(set) var ownerScreenId: String?

    public init(
        mainScreenId: String? = nil,
        ownerDidChange: OwnerDidChange? = nil
    ) {
        self.mainScreenId = mainScreenId
        self.ownerDidChange = ownerDidChange
    }

    public var activeScreenIds: [String] {
        activeScreenRegistrationCounts.keys.sorted()
    }

    public func register(screenId: String) {
        let previousCount = activeScreenRegistrationCounts[screenId, default: 0]
        activeScreenRegistrationCounts[screenId] = previousCount + 1
        guard previousCount == 0 else { return }
        reconcileOwner()
    }

    public func unregister(screenId: String) {
        guard let previousCount = activeScreenRegistrationCounts[screenId] else {
            return
        }
        if previousCount > 1 {
            activeScreenRegistrationCounts[screenId] = previousCount - 1
            return
        }
        activeScreenRegistrationCounts.removeValue(forKey: screenId)
        reconcileOwner()
    }

    public func updateMainScreenId(_ screenId: String?) {
        guard mainScreenId != screenId else { return }
        mainScreenId = screenId
        reconcileOwner()
    }

    public func isAudible(screenId: String) -> Bool {
        ownerScreenId == screenId
    }

    private func reconcileOwner() {
        let nextOwner: String?
        if let mainScreenId,
           activeScreenRegistrationCounts[mainScreenId] != nil {
            nextOwner = mainScreenId
        } else {
            nextOwner = activeScreenRegistrationCounts.keys.min()
        }
        guard nextOwner != ownerScreenId else { return }
        let previousOwner = ownerScreenId
        ownerScreenId = nextOwner
        ownerDidChange?(previousOwner, nextOwner)
    }
}

public enum SceneAssetDataLoader {
    public static func load(runtime: WESceneRuntimeRef, path: String) throws -> Data {
        var error: WESceneRuntimeErrorRef?
        guard let asset = path.withCString({
            we_scene_runtime_asset_create(runtime, $0, &error)
        }) else {
            throw consumeBridgeError(error, operation: "Resolving Scene audio asset '\(path)'")
        }
        defer { we_scene_runtime_asset_destroy(asset) }

        let length = we_scene_runtime_asset_length(asset)
        guard length == 0 || we_scene_runtime_asset_bytes(asset) != nil else {
            throw SceneAudioError.asset(
                "Resolved Scene audio asset '\(path)' has \(length) bytes but no storage"
            )
        }
        guard length != 0 else { return Data() }
        return Data(bytes: we_scene_runtime_asset_bytes(asset)!, count: length)
    }

    private static func consumeBridgeError(
        _ error: WESceneRuntimeErrorRef?,
        operation: String
    ) -> SceneAudioError {
        let detail = we_scene_runtime_error_message(error).map(String.init(cString:))
            ?? "unknown Scene runtime failure"
        we_scene_runtime_error_destroy(error)
        return .asset("\(operation) failed: \(detail)")
    }
}

@MainActor
protocol SceneAudioPlayback: AnyObject {
    var playbackState: SceneAudioPlaybackState { get }
    var position: TimeInterval { get }
    var loops: Bool { get }
    var volume: Float { get }
    func play() throws
    func pause()
    func stop()
    func setVolume(_ volume: Float) throws
    func restore(_ snapshot: SceneAudioPlayerState) throws
}

@MainActor
public final class SceneAudioPlayer: SceneAudioPlayback {
    private let player: AVAudioPlayer
    private var lastKnownState = SceneAudioPlaybackState.stopped

    public init(data: Data, loop: Bool, volume: Float) throws {
        guard !data.isEmpty else { throw SceneAudioError.emptyData }
        guard volume.isFinite, (0...1).contains(volume) else {
            throw SceneAudioError.invalidVolume(volume)
        }
        do {
            player = try AVAudioPlayer(data: data)
        } catch {
            throw SceneAudioError.decode(error.localizedDescription)
        }
        player.numberOfLoops = loop ? -1 : 0
        player.volume = volume
        guard player.prepareToPlay() else {
            throw SceneAudioError.decode("AVFoundation could not prepare the decoded stream")
        }
    }

    public var duration: TimeInterval { player.duration }
    public var playbackState: SceneAudioPlaybackState {
        if player.isPlaying {
            lastKnownState = .playing
        } else if lastKnownState == .playing {
            // AVAudioPlayer exposes no public ended state. Falling out of an
            // active one-shot is natural completion; an infinite loop that
            // stops is an interrupted runtime.
            lastKnownState = loops ? .stopped : .ended
        }
        return lastKnownState
    }
    public var isPlaying: Bool { playbackState == .playing }
    public var loops: Bool { player.numberOfLoops == -1 }
    public var volume: Float { player.volume }
    public var position: TimeInterval { player.currentTime }
    public var currentTime: TimeInterval { position }

    public func play() throws {
        guard playbackState != .playing else { return }
        guard player.play() else { throw SceneAudioError.playbackFailed(nil) }
        lastKnownState = .playing
    }

    public func pause() {
        guard playbackState == .playing else { return }
        player.pause()
        lastKnownState = .paused
    }

    public func stop() {
        player.stop()
        player.currentTime = 0
        lastKnownState = .stopped
    }

    public func setVolume(_ volume: Float) throws {
        guard volume.isFinite, (0...1).contains(volume) else {
            throw SceneAudioError.invalidVolume(volume)
        }
        player.volume = volume
    }

    func restore(_ snapshot: SceneAudioPlayerState) throws {
        try setVolume(snapshot.volume)
        player.stop()
        player.currentTime = min(max(snapshot.position, 0), player.duration)
        switch snapshot.state {
        case .stopped:
            lastKnownState = .stopped
        case .paused:
            lastKnownState = .paused
        case .ended:
            lastKnownState = .ended
        case .playing:
            guard player.play() else { throw SceneAudioError.playbackFailed(nil) }
            lastKnownState = .playing
        }
    }
}

@MainActor
public final class SceneAudioController {
    private struct ScenePolicy: Equatable {
        let objectId: Int
        var sources: [SceneSoundSourceSnapshot]
        var playbackMode: SceneSoundPlaybackMode
        var startSilent: Bool
        var minimumTime: Double
        var maximumTime: Double
    }

    private struct AuthoringPolicy {
        var scene: ScenePolicy?
        var visible: Bool
        var startsAutomatically: Bool
        var currentSourceOffset: Int?
    }

    private struct RuntimeControl {
        var automaticStartConsumed = false
        var resumeAfterHostPause = false
        var resumeAfterVisibility = false
        var deferredPlayGeneration: UInt64?
        var lastAppliedCommand: SceneSoundPlaybackCommand?
        var waitingDeadline: TimeInterval?
        var waitingRemaining: TimeInterval?
    }

    private struct Entry {
        var player: any SceneAudioPlayback
        var resource: String?
        var authoring: AuthoringPolicy
        var command: SceneSoundPlaybackCommand?
        var runtime: RuntimeControl
    }

    private struct Desired {
        let identifier: String
        let policy: ScenePolicy
        let visible: Bool
        let volume: Float
        let command: SceneSoundPlaybackCommand?
    }

    private struct PlayerSelection {
        let player: any SceneAudioPlayback
        let resource: String
        let sourceOffset: Int
    }

    private var players: [String: Entry] = [:]
    private var isPaused = false
    private let makePlayer: (Data, Bool, Float) throws -> any SceneAudioPlayback
    private let currentTime: () -> TimeInterval
    private let selectRandomSource: (Int) -> Int
    private let randomUnit: () -> Double

    public convenience init() {
        self.init { data, loop, volume in
            try SceneAudioPlayer(data: data, loop: loop, volume: volume)
        }
    }

    init(
        makePlayer: @escaping (Data, Bool, Float) throws -> any SceneAudioPlayback,
        currentTime: @escaping () -> TimeInterval = {
            ProcessInfo.processInfo.systemUptime
        },
        selectRandomSource: @escaping (Int) -> Int = {
            Int.random(in: 0..<$0)
        },
        randomUnit: @escaping () -> Double = {
            Double.random(in: 0...1)
        }
    ) {
        self.makePlayer = makePlayer
        self.currentTime = currentTime
        self.selectRandomSource = selectRandomSource
        self.randomUnit = randomUnit
    }

    public var playerCount: Int { players.count }

    public static func identifier(objectId: Int) -> String {
        "sound:\(objectId)"
    }

    public func reconcile(
        _ sounds: [SceneSoundSnapshot],
        masterVolume: Float,
        audioOutput: Float,
        loadAsset: (String) throws -> Data
    ) throws {
        try reconcile(
            sounds,
            masterVolume: masterVolume,
            audioOutput: audioOutput,
            loadAsset: loadAsset,
            shouldRemoveMissing: { $0.authoring.scene != nil }
        )
    }

    private func reconcile(
        _ sounds: [SceneSoundSnapshot],
        masterVolume: Float,
        audioOutput: Float,
        loadAsset: (String) throws -> Data,
        shouldRemoveMissing: (Entry) -> Bool
    ) throws {
        try validateVolume(masterVolume)
        try validateVolume(audioOutput)

        var desired: [String: Desired] = [:]
        var desiredOrder: [String] = []
        for sound in sounds {
            try validateTimingBounds(sound)
            try validateVolume(sound.volume)
            guard !sound.sources.isEmpty else {
                throw SceneAudioError.allCandidateAssetsFailed(
                    objectId: sound.objectId,
                    failures: ["the candidate list is empty"]
                )
            }
            if let command = sound.playbackCommand, command.generation == 0 {
                throw SceneAudioError.invalidPlaybackCommand(
                    "sound object \(sound.objectId) uses reserved generation 0"
                )
            }
            var sourceIndices: Set<Int> = []
            for source in sound.sources {
                guard sourceIndices.insert(source.sourceIndex).inserted else {
                    throw SceneAudioError.duplicateSource(
                        "sound:\(sound.objectId):\(source.sourceIndex)"
                    )
                }
            }
            let identifier = Self.identifier(objectId: sound.objectId)
            guard desired[identifier] == nil else {
                throw SceneAudioError.duplicateSource(identifier)
            }
            let volume = sound.volume * masterVolume * audioOutput
            try validateVolume(volume)
            let policy = ScenePolicy(
                objectId: sound.objectId,
                sources: sound.sources,
                playbackMode: sound.playbackMode,
                startSilent: sound.startSilent,
                minimumTime: sound.minimumTime,
                maximumTime: sound.maximumTime
            )
            desired[identifier] = Desired(
                identifier: identifier,
                policy: policy,
                visible: sound.visible,
                volume: volume,
                command: sound.playbackCommand
            )
            desiredOrder.append(identifier)
        }

        for identifier in desiredOrder {
            guard let item = desired[identifier],
                  let command = item.command,
                  let previous = players[identifier]?.runtime.lastAppliedCommand else {
                continue
            }
            if command.generation < previous.generation {
                throw SceneAudioError.stalePlaybackCommand(
                    identifier: identifier,
                    current: previous.generation,
                    received: command.generation
                )
            }
            if command.generation == previous.generation,
               command.action != previous.action {
                throw SceneAudioError.conflictingPlaybackCommand(
                    identifier: identifier,
                    generation: command.generation
                )
            }
        }

        let originalEntries = players
        let originalStates = players.mapValues(playerState)
        var next = players

        do {
            for identifier in desiredOrder {
                guard let item = desired[identifier] else { continue }
                let existing = players[identifier]
                let existingPolicy = existing?.authoring.scene
                let requiresReplacement = existingPolicy == nil ||
                    existingPolicy?.sources != item.policy.sources ||
                    existingPolicy?.playbackMode != item.policy.playbackMode
                if requiresReplacement {
                    let selection = try makeSelection(
                        policy: item.policy,
                        preferredOffset: initialSourceOffset(for: item.policy),
                        volume: item.volume,
                        loadAsset: loadAsset
                    )
                    var runtime = existing?.runtime ?? RuntimeControl()
                    runtime.automaticStartConsumed = false
                    runtime.resumeAfterHostPause = false
                    runtime.resumeAfterVisibility = false
                    runtime.deferredPlayGeneration = nil
                    runtime.waitingDeadline = nil
                    runtime.waitingRemaining = nil
                    next[identifier] = Entry(
                        player: selection.player,
                        resource: selection.resource,
                        authoring: AuthoringPolicy(
                            scene: item.policy,
                            visible: item.visible,
                            startsAutomatically: !item.policy.startSilent,
                            currentSourceOffset: selection.sourceOffset
                        ),
                        command: item.command,
                        runtime: runtime
                    )
                }
            }
        } catch {
            stopPlayersCreated(in: next, excluding: originalEntries)
            throw error
        }

        for (identifier, entry) in originalEntries
        where desired[identifier] == nil && shouldRemoveMissing(entry) {
            next.removeValue(forKey: identifier)
        }

        do {
            for identifier in desiredOrder {
                guard let item = desired[identifier], var entry = next[identifier] else {
                    preconditionFailure("Prepared Scene audio entry is missing")
                }
                let oldPolicy = entry.authoring.scene
                try entry.player.setVolume(item.volume)
                entry.authoring.visible = item.visible
                entry.authoring.startsAutomatically = !item.policy.startSilent
                entry.authoring.scene = item.policy
                entry.command = item.command

                if let oldPolicy,
                   (oldPolicy.minimumTime != item.policy.minimumTime ||
                    oldPolicy.maximumTime != item.policy.maximumTime),
                   entry.runtime.waitingDeadline != nil ||
                    entry.runtime.waitingRemaining != nil {
                    clearWaiting(&entry)
                    scheduleRandomWait(&entry)
                }

                if let command = item.command,
                   command != entry.runtime.lastAppliedCommand {
                    try apply(
                        command,
                        to: &entry,
                        identifier: identifier,
                        volume: item.volume,
                        loadAsset: loadAsset
                    )
                    entry.runtime.lastAppliedCommand = command
                }
                try enforcePlaybackPolicy(
                    for: &entry,
                    identifier: identifier,
                    volume: item.volume,
                    loadAsset: loadAsset
                )
                next[identifier] = entry
            }
        } catch {
            do {
                stopPlayersCreated(in: next, excluding: originalEntries)
                for (identifier, state) in originalStates {
                    guard let entry = originalEntries[identifier] else { continue }
                    try entry.player.restore(state)
                }
                players = originalEntries
            } catch let rollbackError {
                throw SceneAudioError.rollbackFailed(rollbackError.localizedDescription)
            }
            throw error
        }

        for (identifier, entry) in originalEntries
        where next[identifier]?.player !== entry.player {
            entry.player.stop()
        }
        players = next
    }

    @discardableResult
    public func synchronize(
        _ sounds: [SceneSoundSnapshot],
        masterVolume: Float,
        audioOutput: Float,
        loadAsset: (String) throws -> Data
    ) -> [SceneAudioSynchronizationIssue] {
        do {
            try validateVolume(masterVolume)
            try validateVolume(audioOutput)
        } catch {
            stopAll()
            return [SceneAudioSynchronizationIssue(message: error.localizedDescription)]
        }

        var objectOrder: [Int] = []
        var soundsByObjectId: [Int: [SceneSoundSnapshot]] = [:]
        for sound in sounds {
            if soundsByObjectId[sound.objectId] == nil {
                objectOrder.append(sound.objectId)
            }
            soundsByObjectId[sound.objectId, default: []].append(sound)
        }

        var issues: [SceneAudioSynchronizationIssue] = []
        for objectId in objectOrder {
            guard let objectSounds = soundsByObjectId[objectId] else { continue }
            do {
                try reconcile(
                    objectSounds,
                    masterVolume: masterVolume,
                    audioOutput: audioOutput,
                    loadAsset: loadAsset,
                    shouldRemoveMissing: {
                        $0.authoring.scene?.objectId == objectId
                    }
                )
            } catch {
                stopPlayers(forObjectId: objectId)
                issues.append(SceneAudioSynchronizationIssue(
                    objectId: objectId,
                    message: error.localizedDescription
                ))
            }
        }

        removePlayersForMissingObjects(Set(soundsByObjectId.keys))
        return issues
    }

    private func stopPlayers(forObjectId objectId: Int) {
        let identifier = Self.identifier(objectId: objectId)
        players.removeValue(forKey: identifier)?.player.stop()
    }

    private func removePlayersForMissingObjects(_ objectIds: Set<Int>) {
        let identifiers: [String] = players.compactMap { identifier, entry in
            guard let objectId = entry.authoring.scene?.objectId,
                  !objectIds.contains(objectId) else { return nil }
            return identifier
        }
        for identifier in identifiers {
            players.removeValue(forKey: identifier)?.player.stop()
        }
    }

    public func load(
        identifier: String,
        data: Data,
        loop: Bool,
        volume: Float,
        autoplay: Bool
    ) throws {
        try validateVolume(volume)
        let replacement = try makePlayer(data, loop, volume)
        var runtime = RuntimeControl()
        do {
            if autoplay && !isPaused {
                try replacement.play()
                runtime.automaticStartConsumed = true
            } else if autoplay {
                runtime.resumeAfterHostPause = true
            }
        } catch {
            throw SceneAudioError.playbackFailed(identifier)
        }
        let previous = players.removeValue(forKey: identifier)
        previous?.player.stop()
        players[identifier] = Entry(
            player: replacement,
            resource: nil,
            authoring: AuthoringPolicy(
                scene: nil,
                visible: true,
                startsAutomatically: autoplay,
                currentSourceOffset: nil
            ),
            command: nil,
            runtime: runtime
        )
    }

    public func setVolume(_ volume: Float, for identifier: String) throws {
        try validateVolume(volume)
        try player(identifier).setVolume(volume)
    }

    public func pauseAll() {
        guard !isPaused else { return }
        isPaused = true
        for identifier in players.keys.sorted() {
            guard var entry = players[identifier] else { continue }
            freezeWaiting(&entry)
            if entry.player.playbackState == .playing {
                entry.player.pause()
            }
            entry.runtime.resumeAfterHostPause = canStartOrResume(entry)
            players[identifier] = entry
        }
    }

    public func resumeAll() throws {
        guard isPaused else { return }
        let previousEntries = players
        let previousStates = players.mapValues(playerState)
        isPaused = false
        do {
            for identifier in players.keys.sorted() {
                guard var entry = players[identifier],
                      entry.runtime.resumeAfterHostPause else { continue }
                guard entry.authoring.visible, playbackRequested(entry) else {
                    entry.runtime.resumeAfterHostPause = false
                    entry.runtime.resumeAfterVisibility = playbackRequested(entry)
                    players[identifier] = entry
                    continue
                }
                let requiresLoadedTransition =
                    entry.runtime.deferredPlayGeneration != nil &&
                    entry.player.playbackState == .ended &&
                    (entry.authoring.scene?.sources.count ?? 0) > 1
                if entry.runtime.waitingRemaining != nil {
                    resumeWaiting(&entry)
                } else if !requiresLoadedTransition &&
                            (entry.player.playbackState == .paused ||
                             entry.player.playbackState == .stopped ||
                             entry.runtime.deferredPlayGeneration != nil) {
                    try startCurrentPlayer(
                        for: &entry,
                        identifier: identifier,
                        restartEnded: entry.runtime.deferredPlayGeneration != nil
                    )
                    entry.runtime.deferredPlayGeneration = nil
                }
                entry.runtime.resumeAfterHostPause = false
                players[identifier] = entry
            }
        } catch {
            let playbackError = error
            isPaused = true
            do {
                for (identifier, state) in previousStates {
                    guard let entry = players[identifier] else { continue }
                    try entry.player.restore(state)
                }
                players = previousEntries
            } catch let rollbackError {
                throw SceneAudioError.rollbackFailed(rollbackError.localizedDescription)
            }
            throw playbackError
        }
    }

    public func stop(identifier: String) throws {
        guard let entry = players.removeValue(forKey: identifier) else {
            throw SceneAudioError.unknownPlayer(identifier)
        }
        entry.player.stop()
    }

    public func stopAll() {
        for entry in players.values { entry.player.stop() }
        players.removeAll()
        isPaused = false
    }

    public func playerState(identifier: String) throws -> SceneAudioPlayerState {
        playerState(try entry(identifier))
    }

    public func playbackIntent(identifier: String) throws -> Bool {
        let entry = try entry(identifier)
        return entry.authoring.visible && playbackRequested(entry)
    }

    public func soundRuntimeSnapshots() -> [SceneSoundRuntimeSnapshot] {
        players.values.compactMap { entry in
            guard let objectId = entry.authoring.scene?.objectId else {
                return nil
            }
            return SceneSoundRuntimeSnapshot(
                objectId: objectId,
                state: entry.player.playbackState,
                position: entry.player.position
            )
        }.sorted { $0.objectId < $1.objectId }
    }

    private func apply(
        _ command: SceneSoundPlaybackCommand,
        to entry: inout Entry,
        identifier: String,
        volume: Float,
        loadAsset: (String) throws -> Data
    ) throws {
        switch command.action {
        case .play:
            clearWaiting(&entry)
            if !entry.authoring.visible {
                entry.runtime.deferredPlayGeneration = command.generation
                entry.runtime.resumeAfterVisibility = true
                return
            }
            if isPaused {
                entry.runtime.deferredPlayGeneration = command.generation
                entry.runtime.resumeAfterHostPause = true
                return
            }
            if entry.player.playbackState == .ended,
               let policy = entry.authoring.scene,
               policy.sources.count > 1 {
                try transitionAndStart(
                    entry: &entry,
                    identifier: identifier,
                    volume: volume,
                    loadAsset: loadAsset
                )
            } else {
                try startCurrentPlayer(
                    for: &entry,
                    identifier: identifier,
                    restartEnded: true
                )
            }
            entry.runtime.deferredPlayGeneration = nil
            entry.runtime.resumeAfterHostPause = false
            entry.runtime.resumeAfterVisibility = false
        case .pause:
            clearResumeState(&entry)
            freezeWaiting(&entry)
            if entry.player.playbackState == .playing {
                entry.player.pause()
            }
        case .stop:
            clearResumeState(&entry)
            clearWaiting(&entry)
            if entry.player.playbackState != .stopped {
                entry.player.stop()
            }
            entry.runtime.automaticStartConsumed = true
        }
    }

    private func enforcePlaybackPolicy(
        for entry: inout Entry,
        identifier: String,
        volume: Float,
        loadAsset: (String) throws -> Data
    ) throws {
        let requested = playbackRequested(entry)
        if !entry.authoring.visible {
            freezeWaiting(&entry)
            if entry.player.playbackState == .playing {
                entry.player.pause()
            }
            entry.runtime.resumeAfterVisibility = requested
            entry.runtime.resumeAfterHostPause = false
            if !requested {
                entry.runtime.deferredPlayGeneration = nil
            }
            return
        }

        if isPaused {
            freezeWaiting(&entry)
            if entry.player.playbackState == .playing {
                entry.player.pause()
            }
            entry.runtime.resumeAfterHostPause = canStartOrResume(entry)
            return
        }

        if entry.runtime.deferredPlayGeneration != nil {
            if entry.player.playbackState == .ended,
               let policy = entry.authoring.scene,
               policy.sources.count > 1 {
                try transitionAndStart(
                    entry: &entry,
                    identifier: identifier,
                    volume: volume,
                    loadAsset: loadAsset
                )
            } else {
                try startCurrentPlayer(
                    for: &entry,
                    identifier: identifier,
                    restartEnded: true
                )
            }
            entry.runtime.deferredPlayGeneration = nil
            entry.runtime.resumeAfterHostPause = false
            entry.runtime.resumeAfterVisibility = false
            return
        }

        if entry.runtime.resumeAfterVisibility || entry.runtime.resumeAfterHostPause {
            entry.runtime.resumeAfterVisibility = false
            entry.runtime.resumeAfterHostPause = false
            if entry.runtime.waitingRemaining != nil {
                resumeWaiting(&entry)
            } else if requested && entry.player.playbackState == .paused {
                try startCurrentPlayer(
                    for: &entry,
                    identifier: identifier,
                    restartEnded: false
                )
                return
            }
        }

        guard requested else {
            freezeWaiting(&entry)
            if entry.player.playbackState == .playing {
                entry.player.pause()
            }
            return
        }
        if entry.command?.action == .pause || entry.command?.action == .stop {
            return
        }

        if entry.runtime.waitingRemaining != nil {
            resumeWaiting(&entry)
        }
        if let deadline = entry.runtime.waitingDeadline {
            guard currentTime() >= deadline else { return }
            clearWaiting(&entry)
            if entry.player.playbackState == .ended {
                try transitionAndStart(
                    entry: &entry,
                    identifier: identifier,
                    volume: volume,
                    loadAsset: loadAsset
                )
            } else {
                try startCurrentPlayer(
                    for: &entry,
                    identifier: identifier,
                    restartEnded: false
                )
            }
            return
        }

        switch entry.player.playbackState {
        case .playing:
            return
        case .paused:
            try startCurrentPlayer(
                for: &entry,
                identifier: identifier,
                restartEnded: false
            )
        case .stopped:
            if !entry.runtime.automaticStartConsumed {
                entry.runtime.automaticStartConsumed = true
                if let policy = entry.authoring.scene,
                   policy.playbackMode == .random,
                   policy.startSilent,
                   entry.command == nil {
                    scheduleRandomWait(&entry)
                    if waitingDeadlineHasElapsed(entry) {
                        clearWaiting(&entry)
                        try startCurrentPlayer(
                            for: &entry,
                            identifier: identifier,
                            restartEnded: false
                        )
                    }
                } else {
                    try startCurrentPlayer(
                        for: &entry,
                        identifier: identifier,
                        restartEnded: false
                    )
                }
            } else if entry.player.loops {
                try startCurrentPlayer(
                    for: &entry,
                    identifier: identifier,
                    restartEnded: false
                )
            }
        case .ended:
            guard let policy = entry.authoring.scene else { return }
            switch policy.playbackMode {
            case .single:
                return
            case .loop:
                try transitionAndStart(
                    entry: &entry,
                    identifier: identifier,
                    volume: volume,
                    loadAsset: loadAsset
                )
            case .random:
                scheduleRandomWait(&entry)
                if waitingDeadlineHasElapsed(entry) {
                    clearWaiting(&entry)
                    try transitionAndStart(
                        entry: &entry,
                        identifier: identifier,
                        volume: volume,
                        loadAsset: loadAsset
                    )
                }
            }
        }
    }

    private func transitionAndStart(
        entry: inout Entry,
        identifier: String,
        volume: Float,
        loadAsset: (String) throws -> Data
    ) throws {
        guard let policy = entry.authoring.scene else {
            try startCurrentPlayer(
                for: &entry,
                identifier: identifier,
                restartEnded: true
            )
            return
        }
        let preferredOffset = nextSourceOffset(for: entry, policy: policy)
        if preferredOffset == entry.authoring.currentSourceOffset {
            try startCurrentPlayer(
                for: &entry,
                identifier: identifier,
                restartEnded: true
            )
            return
        }
        let selection = try makeSelection(
            policy: policy,
            preferredOffset: preferredOffset,
            volume: volume,
            loadAsset: loadAsset
        )
        do {
            try selection.player.play()
        } catch {
            selection.player.stop()
            throw SceneAudioError.playbackFailed(identifier)
        }
        entry.player = selection.player
        entry.resource = selection.resource
        entry.authoring.currentSourceOffset = selection.sourceOffset
        entry.runtime.automaticStartConsumed = true
    }

    private func makeSelection(
        policy: ScenePolicy,
        preferredOffset: Int,
        volume: Float,
        loadAsset: (String) throws -> Data
    ) throws -> PlayerSelection {
        precondition(policy.sources.indices.contains(preferredOffset))
        var offsets = [preferredOffset]
        offsets.append(contentsOf: policy.sources.indices.filter {
            $0 != preferredOffset
        })
        var failures: [String] = []
        var onlyFailure: Error?
        for offset in offsets {
            let source = policy.sources[offset]
            do {
                let data = try loadAsset(source.resource)
                let loops = policy.playbackMode == .loop &&
                    policy.sources.count == 1
                return PlayerSelection(
                    player: try makePlayer(data, loops, volume),
                    resource: source.resource,
                    sourceOffset: offset
                )
            } catch {
                onlyFailure = error
                failures.append("\(source.resource): \(error.localizedDescription)")
            }
        }
        if policy.sources.count == 1, let onlyFailure {
            throw onlyFailure
        }
        throw SceneAudioError.allCandidateAssetsFailed(
            objectId: policy.objectId,
            failures: failures
        )
    }

    private func initialSourceOffset(for policy: ScenePolicy) -> Int {
        switch policy.playbackMode {
        case .loop:
            return 0
        case .random, .single:
            return randomSourceOffset(count: policy.sources.count)
        }
    }

    private func nextSourceOffset(for entry: Entry, policy: ScenePolicy) -> Int {
        switch policy.playbackMode {
        case .loop:
            return ((entry.authoring.currentSourceOffset ?? -1) + 1) %
                policy.sources.count
        case .random, .single:
            return randomSourceOffset(count: policy.sources.count)
        }
    }

    private func randomSourceOffset(count: Int) -> Int {
        let result = selectRandomSource(count)
        precondition((0..<count).contains(result))
        return result
    }

    private func scheduleRandomWait(_ entry: inout Entry) {
        guard let policy = entry.authoring.scene,
              policy.playbackMode == .random else { return }
        let unit = randomUnit()
        precondition(unit.isFinite && (0...1).contains(unit))
        let delay = policy.minimumTime +
            (policy.maximumTime - policy.minimumTime) * unit
        if entry.authoring.visible && !isPaused && playbackRequested(entry) {
            entry.runtime.waitingDeadline = currentTime() + delay
            entry.runtime.waitingRemaining = nil
        } else {
            entry.runtime.waitingDeadline = nil
            entry.runtime.waitingRemaining = delay
        }
    }

    private func freezeWaiting(_ entry: inout Entry) {
        guard let deadline = entry.runtime.waitingDeadline else { return }
        entry.runtime.waitingRemaining = max(0, deadline - currentTime())
        entry.runtime.waitingDeadline = nil
    }

    private func resumeWaiting(_ entry: inout Entry) {
        guard let remaining = entry.runtime.waitingRemaining,
              entry.authoring.visible,
              !isPaused,
              playbackRequested(entry) else { return }
        entry.runtime.waitingDeadline = currentTime() + remaining
        entry.runtime.waitingRemaining = nil
    }

    private func clearWaiting(_ entry: inout Entry) {
        entry.runtime.waitingDeadline = nil
        entry.runtime.waitingRemaining = nil
    }

    private func waitingDeadlineHasElapsed(_ entry: Entry) -> Bool {
        entry.runtime.waitingDeadline.map { $0 <= currentTime() } ?? false
    }

    private func startCurrentPlayer(
        for entry: inout Entry,
        identifier: String,
        restartEnded: Bool
    ) throws {
        let state = entry.player.playbackState
        guard state != .playing else { return }
        if state == .ended {
            guard restartEnded else { return }
            entry.player.stop()
        }
        do {
            try entry.player.play()
        } catch {
            throw SceneAudioError.playbackFailed(identifier)
        }
        entry.runtime.automaticStartConsumed = true
    }

    private func clearResumeState(_ entry: inout Entry) {
        entry.runtime.resumeAfterHostPause = false
        entry.runtime.resumeAfterVisibility = false
        entry.runtime.deferredPlayGeneration = nil
    }

    private func playbackRequested(_ entry: Entry) -> Bool {
        switch entry.command?.action {
        case .play:
            return true
        case .pause, .stop:
            return false
        case nil:
            if entry.authoring.scene?.playbackMode == .random {
                return true
            }
            return entry.authoring.startsAutomatically
        }
    }

    private func canStartOrResume(_ entry: Entry) -> Bool {
        guard playbackRequested(entry) else { return false }
        if entry.runtime.deferredPlayGeneration != nil ||
            entry.runtime.waitingDeadline != nil ||
            entry.runtime.waitingRemaining != nil {
            return true
        }
        switch entry.player.playbackState {
        case .playing, .paused:
            return true
        case .stopped:
            if entry.command?.action == .play { return true }
            return !entry.runtime.automaticStartConsumed || entry.player.loops
        case .ended:
            guard let mode = entry.authoring.scene?.playbackMode else {
                return false
            }
            return mode == .loop || mode == .random
        }
    }

    private func entry(_ identifier: String) throws -> Entry {
        guard let entry = players[identifier] else {
            throw SceneAudioError.unknownPlayer(identifier)
        }
        return entry
    }

    private func player(_ identifier: String) throws -> any SceneAudioPlayback {
        try entry(identifier).player
    }

    private func playerState(_ entry: Entry) -> SceneAudioPlayerState {
        SceneAudioPlayerState(
            state: entry.player.playbackState,
            position: entry.player.position,
            volume: entry.player.volume,
            loops: entry.player.loops
        )
    }

    private func stopPlayersCreated(
        in next: [String: Entry],
        excluding original: [String: Entry]
    ) {
        for entry in next.values where !original.values.contains(where: {
            $0.player === entry.player
        }) {
            entry.player.stop()
        }
    }

    private func validateVolume(_ volume: Float) throws {
        guard volume.isFinite, (0...1).contains(volume) else {
            throw SceneAudioError.invalidVolume(volume)
        }
    }

    private func validateTimingBounds(_ sound: SceneSoundSnapshot) throws {
        guard sound.minimumTime.isFinite,
              sound.maximumTime.isFinite,
              sound.minimumTime >= 0,
              sound.maximumTime >= 0,
              sound.minimumTime <= sound.maximumTime else {
            throw SceneAudioError.invalidTimingBounds(
                objectId: sound.objectId,
                minimumTime: sound.minimumTime,
                maximumTime: sound.maximumTime
            )
        }
    }
}
