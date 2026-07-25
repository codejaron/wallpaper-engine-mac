//
//  PlaybackPolicy.swift
//  Open Wallpaper Engine
//

/// The playback override selected for an active host condition.
///
/// `keepRunning` means that the policy does not impose an override. Restoring
/// a user's manual mute or pause state remains the responsibility of the host
/// that applies policy transitions.
enum GSPlayback: String, CaseIterable, Identifiable, Codable, Sendable {
    var id: Self { self }

    case keepRunning, mute, pause, stop

    fileprivate var policyPriority: Int {
        switch self {
        case .keepRunning: 0
        case .mute: 1
        case .pause: 2
        case .stop: 3
        }
    }
}

/// User-selected actions for each condition that can affect wallpaper
/// playback. Detectors live outside this type and feed condition changes into
/// `PlaybackPolicyReducer`.
struct PlaybackPolicyConfiguration: Equatable, Sendable {
    var otherApplicationFocused: GSPlayback = .keepRunning
    var otherApplicationFullscreen: GSPlayback = .keepRunning
    var otherApplicationPlayingAudio: GSPlayback = .keepRunning
    var displayAsleep: GSPlayback = .keepRunning
    var laptopOnBattery: GSPlayback = .keepRunning
}

/// Current host conditions. A value is true only while that condition is
/// active; the reducer combines all active conditions instead of letting the
/// last detector event win.
struct PlaybackPolicyConditions: Equatable, Sendable {
    var otherApplicationFocused = false
    var otherApplicationFullscreen = false
    var otherApplicationPlayingAudio = false
    var displayAsleep = false
    var laptopOnBattery = false
}

/// Explicit inputs accepted from settings changes and platform detectors.
enum PlaybackPolicyEvent: Equatable, Sendable {
    case configurationChanged(PlaybackPolicyConfiguration)
    case conditionsChanged(PlaybackPolicyConditions)
    case otherApplicationFocused(Bool)
    case otherApplicationFullscreen(Bool)
    case otherApplicationPlayingAudio(Bool)
    case displayAsleep(Bool)
    case laptopOnBattery(Bool)
}

struct PlaybackPolicyState: Equatable, Sendable {
    var configuration: PlaybackPolicyConfiguration
    var conditions: PlaybackPolicyConditions

    init(
        configuration: PlaybackPolicyConfiguration = PlaybackPolicyConfiguration(),
        conditions: PlaybackPolicyConditions = PlaybackPolicyConditions()
    ) {
        self.configuration = configuration
        self.conditions = conditions
    }

    var effectiveAction: GSPlayback {
        PlaybackPolicyReducer.effectiveAction(
            configuration: configuration,
            conditions: conditions
        )
    }

    @discardableResult
    mutating func reduce(_ event: PlaybackPolicyEvent) -> PlaybackPolicyTransition {
        PlaybackPolicyReducer.reduce(state: &self, event: event)
    }
}

/// Describes the desired policy override before and after an event. The host
/// should apply side effects only when `changed` is true.
struct PlaybackPolicyTransition: Equatable, Sendable {
    let previous: GSPlayback
    let current: GSPlayback

    var changed: Bool { previous != current }
}

/// Pure reducer for playback policy. If multiple conditions are active, the
/// most restrictive configured action wins: stop > pause > mute > keepRunning.
enum PlaybackPolicyReducer {
    @discardableResult
    static func reduce(
        state: inout PlaybackPolicyState,
        event: PlaybackPolicyEvent
    ) -> PlaybackPolicyTransition {
        let previous = state.effectiveAction

        switch event {
        case .configurationChanged(let configuration):
            state.configuration = configuration
        case .conditionsChanged(let conditions):
            state.conditions = conditions
        case .otherApplicationFocused(let active):
            state.conditions.otherApplicationFocused = active
        case .otherApplicationFullscreen(let active):
            state.conditions.otherApplicationFullscreen = active
        case .otherApplicationPlayingAudio(let active):
            state.conditions.otherApplicationPlayingAudio = active
        case .displayAsleep(let active):
            state.conditions.displayAsleep = active
        case .laptopOnBattery(let active):
            state.conditions.laptopOnBattery = active
        }

        return PlaybackPolicyTransition(
            previous: previous,
            current: state.effectiveAction
        )
    }

    static func effectiveAction(
        configuration: PlaybackPolicyConfiguration,
        conditions: PlaybackPolicyConditions
    ) -> GSPlayback {
        let candidates: [GSPlayback] = [
            conditions.otherApplicationFocused
                ? configuration.otherApplicationFocused : .keepRunning,
            conditions.otherApplicationFullscreen
                ? configuration.otherApplicationFullscreen : .keepRunning,
            conditions.otherApplicationPlayingAudio
                ? configuration.otherApplicationPlayingAudio : .keepRunning,
            conditions.displayAsleep
                ? configuration.displayAsleep : .keepRunning,
            conditions.laptopOnBattery
                ? configuration.laptopOnBattery : .keepRunning,
        ]

        return candidates.max {
            $0.policyPriority < $1.policyPriority
        } ?? .keepRunning
    }
}
