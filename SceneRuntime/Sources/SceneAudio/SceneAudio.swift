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

public struct SceneSoundSnapshot: Equatable, Sendable {
    public let objectId: Int
    public let visible: Bool
    public let sources: [SceneSoundSourceSnapshot]

    public init(objectId: Int, visible: Bool, sources: [SceneSoundSourceSnapshot]) {
        self.objectId = objectId
        self.visible = visible
        self.sources = sources
    }
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
    var isPlaying: Bool { get }
    var loops: Bool { get }
    var volume: Float { get }
    func play() throws
    func pause()
    func stop()
    func setVolume(_ volume: Float) throws
}

@MainActor
public final class SceneAudioPlayer: SceneAudioPlayback {
    private let player: AVAudioPlayer

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
    public var isPlaying: Bool { player.isPlaying }
    public var loops: Bool { player.numberOfLoops == -1 }
    public var volume: Float { player.volume }
    public var currentTime: TimeInterval { player.currentTime }

    public func play() throws {
        guard player.play() else { throw SceneAudioError.playbackFailed(nil) }
    }

    public func pause() { player.pause() }

    public func stop() {
        player.stop()
        player.currentTime = 0
    }

    public func setVolume(_ volume: Float) throws {
        guard volume.isFinite, (0...1).contains(volume) else {
            throw SceneAudioError.invalidVolume(volume)
        }
        player.volume = volume
    }
}

@MainActor
public final class SceneAudioController {
    private struct Entry {
        let player: any SceneAudioPlayback
        let resource: String?
        let loop: Bool
        var startsAutomatically: Bool
        var desiredPlayback: Bool
        var hasStarted: Bool
        var resumeAfterPause: Bool
        var resumeAfterVisibility: Bool
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

        struct Desired {
            let identifier: String
            let source: SceneSoundSourceSnapshot
            let visible: Bool
            let volume: Float
        }

        var desired: [String: Desired] = [:]
        var desiredOrder: [String] = []
        for sound in sounds {
            for source in sound.sources {
                let identifier = Self.identifier(objectId: sound.objectId, sourceIndex: source.sourceIndex)
                guard desired[identifier] == nil else { throw SceneAudioError.duplicateSource(identifier) }
                try validateVolume(source.volume)
                let volume = source.volume * masterVolume * audioOutput
                try validateVolume(volume)
                desired[identifier] = Desired(
                    identifier: identifier,
                    source: source,
                    visible: sound.visible,
                    volume: volume
                )
                desiredOrder.append(identifier)
            }
        }

        // Decode every replacement before mutating the live set. A bad asset cannot leave a
        // partially reconciled frame behind.
        var replacements: [String: Entry] = [:]
        for identifier in desiredOrder {
            guard let item = desired[identifier] else { continue }
            let existing = players[item.identifier]
            if existing?.resource == item.source.resource, existing?.loop == item.source.loop {
                continue
            }
            let data = try loadAsset(item.source.resource)
            let player = try makePlayer(data, item.source.loop, item.volume)
            replacements[item.identifier] = Entry(
                player: player,
                resource: item.source.resource,
                loop: item.source.loop,
                startsAutomatically: !item.source.startSilent,
                desiredPlayback: false,
                hasStarted: false,
                resumeAfterPause: false,
                resumeAfterVisibility: false
            )
        }

        var next = players
        for (identifier, replacement) in replacements { next[identifier] = replacement }

        // Volume validation and decoding are complete, so the remaining mutations cannot fail
        // except playback. Start replacements before stopping old players to preserve rollback.
        var newlyStarted: [any SceneAudioPlayback] = []
        let previousStates = next.mapValues { ($0.player.volume, $0.player.isPlaying) }
        do {
            for identifier in desiredOrder {
                guard let item = desired[identifier] else { continue }
                guard var entry = next[item.identifier] else {
                    preconditionFailure("Decoded Scene audio replacement is missing")
                }
                try entry.player.setVolume(item.volume)
                entry.startsAutomatically = !item.source.startSilent
                let shouldPlay = item.visible && entry.startsAutomatically
                entry.desiredPlayback = shouldPlay
                if !shouldPlay {
                    if !item.visible && entry.player.isPlaying {
                        entry.player.pause()
                        entry.resumeAfterVisibility = true
                    } else if item.visible && entry.player.isPlaying {
                        entry.player.pause()
                        entry.resumeAfterVisibility = false
                    }
                    entry.resumeAfterPause = false
                } else if isPaused {
                    if !entry.hasStarted || entry.loop || entry.resumeAfterVisibility {
                        entry.resumeAfterPause = true
                        entry.resumeAfterVisibility = false
                    }
                } else if !entry.player.isPlaying {
                    let shouldStart = !entry.hasStarted || entry.loop || entry.resumeAfterVisibility
                    if shouldStart {
                        do {
                            try entry.player.play()
                        } catch {
                            throw SceneAudioError.playbackFailed(item.identifier)
                        }
                        entry.hasStarted = true
                        entry.resumeAfterVisibility = false
                        if replacements[item.identifier] != nil { newlyStarted.append(entry.player) }
                    }
                } else {
                    entry.hasStarted = true
                }
                next[item.identifier] = entry
            }
        } catch {
            for player in newlyStarted { player.stop() }
            do {
                for (identifier, state) in previousStates {
                    guard let entry = next[identifier] else { continue }
                    try entry.player.setVolume(state.0)
                    if state.1, !entry.player.isPlaying {
                        try entry.player.play()
                    } else if !state.1, entry.player.isPlaying {
                        entry.player.pause()
                    }
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

    public func load(
        identifier: String,
        data: Data,
        loop: Bool,
        volume: Float,
        autoplay: Bool
    ) throws {
        let replacement = try makePlayer(data, loop, volume)
        do {
            if autoplay && !isPaused { try replacement.play() }
        } catch {
            throw SceneAudioError.playbackFailed(identifier)
        }
        let previous = players.removeValue(forKey: identifier)
        previous?.player.stop()
        players[identifier] = Entry(
            player: replacement,
            resource: nil,
            loop: loop,
            startsAutomatically: autoplay,
            desiredPlayback: autoplay,
            hasStarted: autoplay && !isPaused,
            resumeAfterPause: autoplay && isPaused,
            resumeAfterVisibility: false
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
            entry.resumeAfterPause = entry.desiredPlayback && entry.player.isPlaying
            if entry.player.isPlaying { entry.player.pause() }
            players[identifier] = entry
        }
    }

    public func resumeAll() throws {
        guard isPaused else { return }
        var resumed: [String] = []
        for identifier in players.keys.sorted() {
            guard let entry = players[identifier], entry.resumeAfterPause else { continue }
            do {
                try entry.player.play()
            } catch {
                for resumedIdentifier in resumed { players[resumedIdentifier]?.player.pause() }
                throw SceneAudioError.playbackFailed(identifier)
            }
            resumed.append(identifier)
            players[identifier] = entry
        }
        for identifier in resumed {
            guard var entry = players[identifier] else { continue }
            entry.hasStarted = true
            entry.resumeAfterPause = false
            players[identifier] = entry
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

    public func playerState(identifier: String) throws -> (isPlaying: Bool, volume: Float, loops: Bool) {
        let player = try player(identifier)
        return (player.isPlaying, player.volume, player.loops)
    }

    public func playbackIntent(identifier: String) throws -> Bool {
        guard let entry = players[identifier] else {
            throw SceneAudioError.unknownPlayer(identifier)
        }
        return entry.desiredPlayback
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
}
