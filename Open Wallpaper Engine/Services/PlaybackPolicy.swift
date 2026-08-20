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

    /// `stop` is the only policy that tears down the wallpaper runtime.
    /// Pause and mute preserve the runtime so playback can resume in place.
    var keepsRuntimeLoaded: Bool { self != .stop }

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
    var otherApplicationFullscreenOrMaximized: GSPlayback = .keepRunning
    var otherApplicationPlayingAudio: GSPlayback = .keepRunning
    var laptopOnBattery: GSPlayback = .keepRunning
    var screenInactive: GSPlayback = .stop
}

/// Current host conditions. A value is true only while that condition is
/// active; the reducer combines all active conditions instead of letting the
/// last detector event win.
struct PlaybackPolicyConditions: Equatable, Sendable {
    var otherApplicationFocused = false
    var otherApplicationFullscreenOrMaximized = false
    var otherApplicationPlayingAudio = false
    var laptopOnBattery = false
    var screenInactive = false
}

/// Explicit inputs accepted from settings changes and platform detectors.
enum PlaybackPolicyEvent: Equatable, Sendable {
    case configurationChanged(PlaybackPolicyConfiguration)
    case conditionsChanged(PlaybackPolicyConditions)
    case otherApplicationFocused(Bool)
    case otherApplicationFullscreenOrMaximized(Bool)
    case otherApplicationPlayingAudio(Bool)
    case laptopOnBattery(Bool)
    case screenInactive(Bool)
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

/// Pure reducer for playback policy. Lock, screen-saver, and display-sleep
/// signals are folded into one configurable screen-inactive condition. If
/// multiple conditions are active, the most restrictive action wins:
/// stop > pause > mute > keepRunning.
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
        case .otherApplicationFullscreenOrMaximized(let active):
            state.conditions.otherApplicationFullscreenOrMaximized = active
        case .otherApplicationPlayingAudio(let active):
            state.conditions.otherApplicationPlayingAudio = active
        case .laptopOnBattery(let active):
            state.conditions.laptopOnBattery = active
        case .screenInactive(let active):
            state.conditions.screenInactive = active
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
            conditions.otherApplicationFullscreenOrMaximized
                ? configuration.otherApplicationFullscreenOrMaximized : .keepRunning,
            conditions.otherApplicationPlayingAudio
                ? configuration.otherApplicationPlayingAudio : .keepRunning,
            conditions.laptopOnBattery
                ? configuration.laptopOnBattery : .keepRunning,
            conditions.screenInactive
                ? configuration.screenInactive : .keepRunning,
        ]

        return candidates.max {
            $0.policyPriority < $1.policyPriority
        } ?? .keepRunning
    }
}
