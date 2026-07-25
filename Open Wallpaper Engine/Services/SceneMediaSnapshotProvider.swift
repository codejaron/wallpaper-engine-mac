import Combine
import Foundation

enum SceneMediaPlaybackState: Int32, Sendable {
    case stopped = 0
    case playing = 1
    case paused = 2
}

struct SceneMediaColor: Equatable, Sendable {
    let red: Double
    let green: Double
    let blue: Double

    static let black = SceneMediaColor(red: 0, green: 0, blue: 0)

    fileprivate var components: [Double] { [red, green, blue] }
}

struct SceneMediaContent: Equatable, Sendable {
    let playbackState: SceneMediaPlaybackState
    let title: String
    let artist: String
    let contentType: String
    let albumTitle: String
    let subTitle: String
    let albumArtist: String
    let genres: String
    let position: Double
    let duration: Double
    let hasThumbnail: Bool
    let primaryColor: SceneMediaColor
    let secondaryColor: SceneMediaColor
    let tertiaryColor: SceneMediaColor
    let textColor: SceneMediaColor
    let highContrastColor: SceneMediaColor
}

enum SceneMediaProviderSnapshot: Equatable, Sendable {
    case unavailable(revision: UInt64)
    case available(revision: UInt64, content: SceneMediaContent)

    var revision: UInt64 {
        switch self {
        case .unavailable(let revision), .available(let revision, _):
            return revision
        }
    }
}

enum SceneMediaSnapshotProviderError: LocalizedError {
    case invalid(String)
    case revisionExhausted

    var errorDescription: String? {
        switch self {
        case .invalid(let message): return message
        case .revisionExhausted:
            return "Scene media snapshot revision counter is exhausted"
        }
    }
}

/// Explicit host injection point for SceneScript media events. macOS has no
/// supported public API for observing arbitrary applications' global Now
/// Playing state, so this provider deliberately starts unavailable and never
/// manufactures track metadata.
@MainActor
final class SceneMediaSnapshotProvider: ObservableObject {
    @Published private(set) var snapshot: SceneMediaProviderSnapshot =
        .unavailable(revision: 0)

    private var revision: UInt64 = 0

    func publish(_ content: SceneMediaContent) throws {
        try validate(content)
        snapshot = .available(
            revision: try nextRevision(),
            content: content
        )
    }

    func markUnavailable() throws {
        if case .unavailable = snapshot { return }
        snapshot = .unavailable(revision: try nextRevision())
    }

    private func nextRevision() throws -> UInt64 {
        let result = revision.addingReportingOverflow(1)
        guard !result.overflow else {
            throw SceneMediaSnapshotProviderError.revisionExhausted
        }
        revision = result.partialValue
        return revision
    }

    private func validate(_ content: SceneMediaContent) throws {
        guard content.position.isFinite, content.position >= 0,
              content.duration.isFinite, content.duration >= 0 else {
            throw SceneMediaSnapshotProviderError.invalid(
                "Scene media timeline values must be finite and non-negative"
            )
        }
        let colors = [
            content.primaryColor,
            content.secondaryColor,
            content.tertiaryColor,
            content.textColor,
            content.highContrastColor,
        ]
        guard colors.allSatisfy({ color in
            color.components.allSatisfy { $0.isFinite && (0...1).contains($0) }
        }) else {
            throw SceneMediaSnapshotProviderError.invalid(
                "Scene media colors must contain finite values in [0, 1]"
            )
        }
    }
}
