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
        }
    }
}

public struct SceneSoundSourceSnapshot: Equatable, Sendable {
    public let sourceIndex: Int
    public let resource: String
    public let loop: Bool
    public let volume: Float
    public let startSilent: Bool

    public init(sourceIndex: Int, resource: String, loop: Bool, volume: Float, startSilent: Bool) {
        self.sourceIndex = sourceIndex
        self.resource = resource
        self.loop = loop
        self.volume = volume
        self.startSilent = startSilent
    }
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
    public let playbackCommand: SceneSoundPlaybackCommand?
    public let minimumTime: Double
    public let maximumTime: Double

    public init(
        objectId: Int,
        visible: Bool,
        sources: [SceneSoundSourceSnapshot],
        playbackCommand: SceneSoundPlaybackCommand? = nil,
        minimumTime: Double = 0,
        maximumTime: Double = 0
    ) {
        self.objectId = objectId
        self.visible = visible
        self.sources = sources
        self.playbackCommand = playbackCommand
        self.minimumTime = minimumTime
        self.maximumTime = maximumTime
    }
}

public struct SceneAudioSynchronizationIssue: Equatable, Sendable {
    public let message: String

    public init(message: String) {
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

    private var activeScreenIdSet: Set<String> = []
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

    public var activeScreenIds: [String] { activeScreenIdSet.sorted() }

    public func register(screenId: String) {
        guard activeScreenIdSet.insert(screenId).inserted else { return }
        reconcileOwner()
    }

    public func unregister(screenId: String) {
        guard activeScreenIdSet.remove(screenId) != nil else { return }
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
        if let mainScreenId, activeScreenIdSet.contains(mainScreenId) {
            nextOwner = mainScreenId
        } else {
            nextOwner = activeScreenIdSet.min()
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
    private struct AuthoringPolicy {
        let objectId: Int?
        let sourceIndex: Int?
        let loop: Bool
        var visible: Bool
        var startsAutomatically: Bool
    }

    private struct RuntimeControl {
        var automaticStartConsumed = false
        var resumeAfterHostPause = false
        var resumeAfterVisibility = false
        var deferredPlayGeneration: UInt64?
        var lastAppliedCommand: SceneSoundPlaybackCommand?
    }

    private struct Entry {
        let player: any SceneAudioPlayback
        let resource: String?
        var authoring: AuthoringPolicy
        var command: SceneSoundPlaybackCommand?
        var runtime: RuntimeControl
    }

    private struct Desired {
        let identifier: String
        let source: SceneSoundSourceSnapshot
        let objectId: Int
        let visible: Bool
        let volume: Float
        let command: SceneSoundPlaybackCommand?
    }

    private var players: [String: Entry] = [:]
    private var isPaused = false
    private let makePlayer: (Data, Bool, Float) throws -> any SceneAudioPlayback

    public convenience init() {
        self.init { data, loop, volume in
            try SceneAudioPlayer(data: data, loop: loop, volume: volume)
        }
    }

    init(makePlayer: @escaping (Data, Bool, Float) throws -> any SceneAudioPlayback) {
        self.makePlayer = makePlayer
    }

    public var playerCount: Int { players.count }

    public static func identifier(objectId: Int, sourceIndex: Int) -> String {
        "sound:\(objectId):\(sourceIndex)"
    }

    public func reconcile(
        _ sounds: [SceneSoundSnapshot],
        masterVolume: Float,
        audioOutput: Float,
        loadAsset: (String) throws -> Data
    ) throws {
        try validateVolume(masterVolume)
        try validateVolume(audioOutput)

        var desired: [String: Desired] = [:]
        var desiredOrder: [String] = []
        for sound in sounds {
            try validateTimingBounds(sound)
            if let command = sound.playbackCommand, command.generation == 0 {
                throw SceneAudioError.invalidPlaybackCommand(
                    "sound object \(sound.objectId) uses reserved generation 0"
                )
            }
            for source in sound.sources {
                let identifier = Self.identifier(objectId: sound.objectId, sourceIndex: source.sourceIndex)
                guard desired[identifier] == nil else { throw SceneAudioError.duplicateSource(identifier) }
                try validateVolume(source.volume)
                let volume = source.volume * masterVolume * audioOutput
                try validateVolume(volume)
                desired[identifier] = Desired(
                    identifier: identifier,
                    source: source,
                    objectId: sound.objectId,
                    visible: sound.visible,
                    volume: volume,
                    command: sound.playbackCommand
                )
                desiredOrder.append(identifier)
            }
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

        // Decode every replacement before mutating the live set. A bad asset cannot leave a
        // partially reconciled frame behind.
        var replacements: [String: Entry] = [:]
        for identifier in desiredOrder {
            guard let item = desired[identifier] else { continue }
            let existing = players[item.identifier]
            if existing?.resource == item.source.resource,
               existing?.authoring.loop == item.source.loop {
                continue
            }
            let data = try loadAsset(item.source.resource)
            let player = try makePlayer(data, item.source.loop, item.volume)
            replacements[item.identifier] = Entry(
                player: player,
                resource: item.source.resource,
                authoring: AuthoringPolicy(
                    objectId: item.objectId,
                    sourceIndex: item.source.sourceIndex,
                    loop: item.source.loop,
                    visible: item.visible,
                    startsAutomatically: !item.source.startSilent
                ),
                command: item.command,
                runtime: RuntimeControl()
            )
        }

        var next = players
        for (identifier, replacement) in replacements { next[identifier] = replacement }

        // Volume validation and decoding are complete, so the remaining mutations cannot fail
        // except playback. Start replacements before stopping old players to preserve rollback.
        let previousStates = next.mapValues {
            SceneAudioPlayerState(
                state: $0.player.playbackState,
                position: $0.player.position,
                volume: $0.player.volume,
                loops: $0.player.loops
            )
        }
        do {
            for identifier in desiredOrder {
                guard let item = desired[identifier] else { continue }
                guard var entry = next[item.identifier] else {
                    preconditionFailure("Decoded Scene audio replacement is missing")
                }
                try entry.player.setVolume(item.volume)
                entry.authoring.visible = item.visible
                entry.authoring.startsAutomatically = !item.source.startSilent
                entry.command = item.command
                if let command = item.command,
                   command != entry.runtime.lastAppliedCommand {
                    try apply(command, to: &entry, identifier: item.identifier)
                    entry.runtime.lastAppliedCommand = command
                }
                try enforcePlaybackPolicy(for: &entry, identifier: item.identifier)
                next[item.identifier] = entry
            }
        } catch {
            do {
                for (identifier, state) in previousStates {
                    guard let entry = next[identifier] else { continue }
                    try entry.player.restore(state)
                }
            } catch let rollbackError {
                throw SceneAudioError.rollbackFailed(rollbackError.localizedDescription)
            }
            throw error
        }

        for (identifier, entry) in players where next[identifier]?.player !== entry.player {
            entry.player.stop()
        }
        for (identifier, entry) in players where desired[identifier] == nil {
            entry.player.stop()
            next.removeValue(forKey: identifier)
        }
        players = next
    }

    /// Synchronizes the audio sidecar without allowing an audio failure to
    /// invalidate its owning visual Scene session. The strict `reconcile`
    /// path remains available to callers that need transactional errors.
    /// This boundary reports the failure explicitly and clears all players so
    /// stale audio cannot survive a failed snapshot.
    @discardableResult
    public func synchronize(
        _ sounds: [SceneSoundSnapshot],
        masterVolume: Float,
        audioOutput: Float,
        loadAsset: (String) throws -> Data
    ) -> SceneAudioSynchronizationIssue? {
        do {
            try reconcile(
                sounds,
                masterVolume: masterVolume,
                audioOutput: audioOutput,
                loadAsset: loadAsset
            )
            return nil
        } catch {
            stopAll()
            return SceneAudioSynchronizationIssue(
                message: error.localizedDescription
            )
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
                objectId: nil,
                sourceIndex: nil,
                loop: loop,
                visible: true,
                startsAutomatically: autoplay
            ),
            command: nil,
            runtime: runtime
        )
    }

    public func setVolume(_ volume: Float, for identifier: String) throws {
        try player(identifier).setVolume(volume)
    }

    public func pauseAll() {
        guard !isPaused else { return }
        isPaused = true
        for identifier in players.keys.sorted() {
            guard var entry = players[identifier] else { continue }
            let state = entry.player.playbackState
            if state == .playing {
                entry.player.pause()
                entry.runtime.resumeAfterHostPause = playbackRequested(entry)
            } else {
                entry.runtime.resumeAfterHostPause = canStartOrResume(entry)
            }
            players[identifier] = entry
        }
    }

    public func resumeAll() throws {
        guard isPaused else { return }
        let previousEntries = players
        let previousStates = players.mapValues {
            SceneAudioPlayerState(
                state: $0.player.playbackState,
                position: $0.player.position,
                volume: $0.player.volume,
                loops: $0.player.loops
            )
        }
        do {
            for identifier in players.keys.sorted() {
                guard var entry = players[identifier],
                      entry.runtime.resumeAfterHostPause else { continue }
                guard entry.authoring.visible, playbackRequested(entry) else {
                    entry.runtime.resumeAfterHostPause = false
                    if playbackRequested(entry) {
                        entry.runtime.resumeAfterVisibility = true
                    }
                    players[identifier] = entry
                    continue
                }
                let restartEnded = entry.runtime.deferredPlayGeneration != nil
                try startPlayback(
                    for: &entry,
                    identifier: identifier,
                    restartEnded: restartEnded
                )
                entry.runtime.resumeAfterHostPause = false
                entry.runtime.deferredPlayGeneration = nil
                players[identifier] = entry
            }
        } catch {
            let playbackError = error
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
        isPaused = false
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
        let player = try player(identifier)
        return SceneAudioPlayerState(
            state: player.playbackState,
            position: player.position,
            volume: player.volume,
            loops: player.loops
        )
    }

    public func playbackIntent(identifier: String) throws -> Bool {
        guard let entry = players[identifier] else {
            throw SceneAudioError.unknownPlayer(identifier)
        }
        return entry.authoring.visible && playbackRequested(entry)
    }

    public func soundRuntimeSnapshots() -> [SceneSoundRuntimeSnapshot] {
        var grouped: [Int: [(sourceIndex: Int, state: SceneAudioPlaybackState, position: TimeInterval)]] = [:]
        for entry in players.values {
            guard let objectId = entry.authoring.objectId,
                  let sourceIndex = entry.authoring.sourceIndex else { continue }
            grouped[objectId, default: []].append((
                sourceIndex,
                entry.player.playbackState,
                entry.player.position
            ))
        }
        return grouped.keys.sorted().compactMap { objectId in
            guard let sources = grouped[objectId]?.sorted(by: { $0.sourceIndex < $1.sourceIndex }),
                  var selected = sources.first else { return nil }
            for source in sources.dropFirst()
            where playbackStatePriority(source.state) > playbackStatePriority(selected.state) {
                selected = source
            }
            return SceneSoundRuntimeSnapshot(
                objectId: objectId,
                state: selected.state,
                position: selected.position
            )
        }
    }

    private func apply(
        _ command: SceneSoundPlaybackCommand,
        to entry: inout Entry,
        identifier: String
    ) throws {
        switch command.action {
        case .play:
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
            try startPlayback(for: &entry, identifier: identifier, restartEnded: true)
            entry.runtime.deferredPlayGeneration = nil
            entry.runtime.resumeAfterHostPause = false
            entry.runtime.resumeAfterVisibility = false
        case .pause:
            clearResumeState(&entry)
            if entry.player.playbackState == .playing {
                entry.player.pause()
            }
        case .stop:
            clearResumeState(&entry)
            if entry.player.playbackState != .stopped {
                entry.player.stop()
            }
        }
    }

    private func enforcePlaybackPolicy(
        for entry: inout Entry,
        identifier: String
    ) throws {
        let requested = playbackRequested(entry)
        if !entry.authoring.visible {
            if entry.player.playbackState == .playing {
                entry.player.pause()
                entry.runtime.resumeAfterVisibility = requested
            }
            if entry.runtime.resumeAfterHostPause {
                entry.runtime.resumeAfterHostPause = false
                entry.runtime.resumeAfterVisibility = requested
            }
            if !requested {
                entry.runtime.resumeAfterVisibility = false
                entry.runtime.deferredPlayGeneration = nil
            }
            return
        }

        if isPaused {
            if entry.player.playbackState == .playing {
                entry.player.pause()
            }
            entry.runtime.resumeAfterHostPause = canStartOrResume(entry)
            return
        }

        if entry.runtime.deferredPlayGeneration != nil {
            try startPlayback(for: &entry, identifier: identifier, restartEnded: true)
            entry.runtime.deferredPlayGeneration = nil
            entry.runtime.resumeAfterHostPause = false
            entry.runtime.resumeAfterVisibility = false
            return
        }

        if entry.runtime.resumeAfterVisibility || entry.runtime.resumeAfterHostPause {
            if requested {
                try startPlayback(for: &entry, identifier: identifier, restartEnded: false)
            }
            entry.runtime.resumeAfterVisibility = false
            entry.runtime.resumeAfterHostPause = false
            return
        }

        guard requested else {
            if entry.player.playbackState == .playing {
                entry.player.pause()
            }
            return
        }

        switch entry.command?.action {
        case .play:
            switch entry.player.playbackState {
            case .stopped, .paused:
                try startPlayback(for: &entry, identifier: identifier, restartEnded: false)
            case .playing, .ended:
                break
            }
        case .pause, .stop:
            break
        case nil:
            switch entry.player.playbackState {
            case .stopped:
                if !entry.runtime.automaticStartConsumed || entry.authoring.loop {
                    try startPlayback(for: &entry, identifier: identifier, restartEnded: false)
                }
            case .ended:
                if entry.authoring.loop {
                    try startPlayback(for: &entry, identifier: identifier, restartEnded: true)
                }
            case .playing, .paused:
                break
            }
        }
    }

    private func startPlayback(
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
        case .play: return true
        case .pause, .stop: return false
        case nil: return entry.authoring.startsAutomatically
        }
    }

    private func canStartOrResume(_ entry: Entry) -> Bool {
        guard playbackRequested(entry) else { return false }
        if entry.runtime.deferredPlayGeneration != nil { return true }
        switch entry.player.playbackState {
        case .playing, .paused:
            return true
        case .stopped:
            if entry.command?.action == .play { return true }
            return !entry.runtime.automaticStartConsumed || entry.authoring.loop
        case .ended:
            return entry.authoring.loop
        }
    }

    private func playbackStatePriority(_ state: SceneAudioPlaybackState) -> Int {
        switch state {
        case .playing: return 3
        case .paused: return 2
        case .ended: return 1
        case .stopped: return 0
        }
    }

    private func player(_ identifier: String) throws -> any SceneAudioPlayback {
        guard let entry = players[identifier] else {
            throw SceneAudioError.unknownPlayer(identifier)
        }
        return entry.player
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
        // linux-wallpaperengine accepts these authoring fields but does not
        // schedule playback from them. Preserve that behavior: valid bounds
        // remain metadata and the ordinary once/loop policy starts the source.
    }
}
