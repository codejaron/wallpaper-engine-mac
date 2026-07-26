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

struct SceneMediaThumbnailRGBA8: Equatable, Sendable {
    let width: UInt32
    let height: UInt32
    let bytesPerRow: UInt32
    let pixels: Data
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
    let thumbnail: SceneMediaThumbnailRGBA8?
    let primaryColor: SceneMediaColor
    let secondaryColor: SceneMediaColor
    let tertiaryColor: SceneMediaColor
    let textColor: SceneMediaColor
    let highContrastColor: SceneMediaColor

    var hasThumbnail: Bool { thumbnail != nil }
}

struct SceneMediaRevisions: Equatable, Sendable {
    var status: UInt64
    var metadata: UInt64
    var playback: UInt64
    var timeline: UInt64
    var thumbnail: UInt64

    static let zero = SceneMediaRevisions(
        status: 0,
        metadata: 0,
        playback: 0,
        timeline: 0,
        thumbnail: 0
    )
}

private struct SceneMediaTimelineIdentity: Equatable {
    let position: Double
    let duration: Double
}

private struct SceneMediaThumbnailIdentity: Equatable {
    let thumbnail: SceneMediaThumbnailRGBA8?
    let primaryColor: SceneMediaColor
    let secondaryColor: SceneMediaColor
    let tertiaryColor: SceneMediaColor
    let textColor: SceneMediaColor
    let highContrastColor: SceneMediaColor
}

private extension SceneMediaContent {
    var metadataIdentity: [String] {
        [title, artist, contentType, albumTitle, subTitle, albumArtist, genres]
    }

    var timelineIdentity: SceneMediaTimelineIdentity {
        SceneMediaTimelineIdentity(position: position, duration: duration)
    }

    var thumbnailIdentity: SceneMediaThumbnailIdentity {
        SceneMediaThumbnailIdentity(
            thumbnail: thumbnail,
            primaryColor: primaryColor,
            secondaryColor: secondaryColor,
            tertiaryColor: tertiaryColor,
            textColor: textColor,
            highContrastColor: highContrastColor
        )
    }
}

enum SceneMediaProviderSnapshot: Equatable, Sendable {
    case unavailable(revisions: SceneMediaRevisions)
    case available(revisions: SceneMediaRevisions, content: SceneMediaContent)

    var revisions: SceneMediaRevisions {
        switch self {
        case .unavailable(let revisions), .available(let revisions, _):
            return revisions
        }
    }

    func hasThumbnailUpdate(since previous: Self?) -> Bool {
        previous?.revisions.thumbnail != revisions.thumbnail
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
        .unavailable(revisions: .zero)

    private var revisions = SceneMediaRevisions.zero

    func publish(_ content: SceneMediaContent) throws {
        try validate(content)
        let previous: SceneMediaContent?
        let wasAvailable: Bool
        switch snapshot {
        case .unavailable:
            previous = nil
            wasAvailable = false
        case .available(_, let content):
            previous = content
            wasAvailable = true
        }

        var next = revisions
        if !wasAvailable {
            next.status = try increment(next.status)
        }
        if previous?.metadataIdentity != content.metadataIdentity {
            next.metadata = try increment(next.metadata)
        }
        if previous?.playbackState != content.playbackState {
            next.playback = try increment(next.playback)
        }
        if previous?.timelineIdentity != content.timelineIdentity {
            next.timeline = try increment(next.timeline)
        }
        if previous?.thumbnailIdentity != content.thumbnailIdentity {
            next.thumbnail = try increment(next.thumbnail)
        }
        guard !wasAvailable || previous != content else { return }
        revisions = next
        snapshot = .available(
            revisions: next,
            content: content
        )
    }

    func markUnavailable() throws {
        if case .unavailable = snapshot { return }
        var next = revisions
        next.status = try increment(next.status)
        revisions = next
        snapshot = .unavailable(revisions: next)
    }

    private func increment(_ revision: UInt64) throws -> UInt64 {
        let result = revision.addingReportingOverflow(1)
        guard !result.overflow else {
            throw SceneMediaSnapshotProviderError.revisionExhausted
        }
        return result.partialValue
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
        if let thumbnail = content.thumbnail {
            let minimumRowBytes = UInt64(thumbnail.width) * 4
            let storageLength = UInt64(thumbnail.bytesPerRow) *
                UInt64(thumbnail.height)
            guard thumbnail.width > 0,
                  thumbnail.height > 0,
                  minimumRowBytes <= UInt32.max,
                  UInt64(thumbnail.bytesPerRow) >= minimumRowBytes,
                  storageLength == UInt64(thumbnail.pixels.count),
                  minimumRowBytes * UInt64(thumbnail.height) <=
                    256 * 1024 * 1024 else {
                throw SceneMediaSnapshotProviderError.invalid(
                    "Scene media thumbnail has an invalid RGBA8 row layout"
                )
            }
        }
    }
}
