//
//  GlobalSettingsService.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/9/2.
//

import Cocoa
import Combine
import SceneAudio
import SwiftUI
import ServiceManagement

enum GSQuality {
    case low, medium, high, ultra
}

enum GSAntiAliasingQuality: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case none, msaa_x2, msaa_x4, msaa_x8
}

enum GSPostProcessingQuality: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case disabled, enabled, ultra
}

enum GSTextureResolutionQuality: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case highQuality, highPerformance, automatic
}

enum GSAppearance: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case light, dark, followSystem
}

enum GSLocalization: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case en_US, zh_CN, followSystem
}

enum GSVideoFramework: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case avkit
}

enum GSScenePresentationScaling: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case stretch
    case aspectFit
    case aspectFill
}

enum GSProcessPiority: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case normal, belowNormal
}

enum GSLogLevel: String, CaseIterable, Identifiable, Codable {
    var id: Self { self }
    case error, verbose, none
}

struct GlobalSettings: Codable, Equatable {
    private static let currentScenePresentationSettingsVersion = 1
    
    // MARK: Playback
    var otherApplicationFocused = GSPlayback.keepRunning
    // Keep the persisted key for compatibility with existing settings. The
    // detector now treats both native fullscreen and desktop maximize as active.
    var otherApplicationFullscreen = GSPlayback.keepRunning
    var otherApplicationPlayingAudio = GSPlayback.keepRunning
    var laptopOnBattery = GSPlayback.keepRunning
    
    // MARK: Quality
    var antiAliasing = GSAntiAliasingQuality.msaa_x2
    var postProcessing = GSPostProcessingQuality.disabled
    var textureResolution = GSTextureResolutionQuality.automatic
    var reflections = false
    var fps: Double = 30

    // MARK: Scene presentation
    // Per-screen rendering remains the default. Span mode opts into a shared
    // virtual canvas whose slices are presented by each display window.
    var scenePresentationScaling = GSScenePresentationScaling.stretch
    var sceneSpanAcrossScreens = false
    private var scenePresentationSettingsVersion =
        Self.currentScenePresentationSettingsVersion
    
    // MARK: Automatic Setup
    var autoStart = false
    var safeMode = false
    
    // MARK: Basic Setup
    var language = GSLocalization.followSystem
    
    // MARK: macOS
    var adjustMenuBarTint = true
    
    // MARK: Appearance
    var appearance = GSAppearance.followSystem
    
    // MARK: Audio
    var audioOutput = true
    var systemAudioCaptureEnabled = false
    var reloadWhenChangingOutputDevice = true // Not putting in use
    
    // MARK: Video
    var videoFramework = GSVideoFramework.avkit
    
    // MARK: Advanced
    var wallpaperEngineAssetsDirectory: String?
    var processPiority = GSProcessPiority.normal // Not putting in use
    var pauseOnVRAMExhausted = false // Not putting in use
    var restartAfterCrashing = false // Not putting in use
    
    // MARK: Developer
    var logLevel = GSLogLevel.none
    
    // MARK: Misc
    var autoRefresh = true

    private enum CodingKeys: String, CodingKey {
        case otherApplicationFocused, otherApplicationFullscreen,
             otherApplicationPlayingAudio, laptopOnBattery
        case antiAliasing, postProcessing, textureResolution, reflections, fps
        case scenePresentationScaling, sceneSpanAcrossScreens,
             scenePresentationSettingsVersion
        case autoStart, safeMode, language
        case adjustMenuBarTint, appearance
        case audioOutput, systemAudioCaptureEnabled,
             reloadWhenChangingOutputDevice
        case videoFramework
        case wallpaperEngineAssetsDirectory, processPiority,
             pauseOnVRAMExhausted, restartAfterCrashing
        case logLevel, autoRefresh
    }

    init() {
        otherApplicationFocused = .keepRunning
        otherApplicationFullscreen = .keepRunning
        otherApplicationPlayingAudio = .keepRunning
        laptopOnBattery = .keepRunning
        antiAliasing = .msaa_x2
        postProcessing = .disabled
        textureResolution = .automatic
        reflections = false
        fps = 30
        scenePresentationScaling = .stretch
        sceneSpanAcrossScreens = false
        scenePresentationSettingsVersion =
            Self.currentScenePresentationSettingsVersion
        autoStart = false
        safeMode = false
        language = .followSystem
        adjustMenuBarTint = true
        appearance = .followSystem
        audioOutput = true
        systemAudioCaptureEnabled = false
        reloadWhenChangingOutputDevice = true
        videoFramework = .avkit
        wallpaperEngineAssetsDirectory = nil
        processPiority = .normal
        pauseOnVRAMExhausted = false
        restartAfterCrashing = false
        logLevel = .none
        autoRefresh = true
    }

    // Settings persisted by older builds predate the Scene presentation keys.
    // Decode those keys opportunistically so adding a setting never resets the
    // user's unrelated preferences.
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        otherApplicationFocused = try container.decodeIfPresent(
            GSPlayback.self, forKey: .otherApplicationFocused
        ) ?? .keepRunning
        otherApplicationFullscreen = try container.decodeIfPresent(
            GSPlayback.self, forKey: .otherApplicationFullscreen
        ) ?? .keepRunning
        otherApplicationPlayingAudio = try container.decodeIfPresent(
            GSPlayback.self, forKey: .otherApplicationPlayingAudio
        ) ?? .keepRunning
        laptopOnBattery = try container.decodeIfPresent(
            GSPlayback.self, forKey: .laptopOnBattery
        ) ?? .keepRunning
        antiAliasing = try container.decodeIfPresent(
            GSAntiAliasingQuality.self, forKey: .antiAliasing
        ) ?? .msaa_x2
        postProcessing = try container.decodeIfPresent(
            GSPostProcessingQuality.self, forKey: .postProcessing
        ) ?? .disabled
        textureResolution = try container.decodeIfPresent(
            GSTextureResolutionQuality.self, forKey: .textureResolution
        ) ?? .automatic
        reflections = try container.decodeIfPresent(
            Bool.self, forKey: .reflections
        ) ?? false
        fps = try container.decodeIfPresent(Double.self, forKey: .fps) ?? 30
        let storedScenePresentationVersion = try container.decodeIfPresent(
            Int.self, forKey: .scenePresentationSettingsVersion
        ) ?? 0
        let storedScenePresentationScaling = try container.decodeIfPresent(
            GSScenePresentationScaling.self,
            forKey: .scenePresentationScaling
        )
        // Builds predating this version persisted aspectFill as the implicit
        // default, which cropped authored content at narrower display ratios.
        // Migrate that unversioned state once; versioned user selections remain
        // authoritative, including an explicit aspectFill choice.
        if storedScenePresentationVersion == 0 &&
            storedScenePresentationScaling == .aspectFill {
            scenePresentationScaling = .stretch
        } else {
            scenePresentationScaling = storedScenePresentationScaling ?? .stretch
        }
        sceneSpanAcrossScreens = try container.decodeIfPresent(
            Bool.self, forKey: .sceneSpanAcrossScreens
        ) ?? false
        scenePresentationSettingsVersion =
            Self.currentScenePresentationSettingsVersion
        autoStart = try container.decodeIfPresent(Bool.self, forKey: .autoStart) ?? false
        safeMode = try container.decodeIfPresent(Bool.self, forKey: .safeMode) ?? false
        language = try container.decodeIfPresent(
            GSLocalization.self, forKey: .language
        ) ?? .followSystem
        adjustMenuBarTint = try container.decodeIfPresent(
            Bool.self, forKey: .adjustMenuBarTint
        ) ?? true
        appearance = try container.decodeIfPresent(
            GSAppearance.self, forKey: .appearance
        ) ?? .followSystem
        audioOutput = try container.decodeIfPresent(Bool.self, forKey: .audioOutput) ?? true
        systemAudioCaptureEnabled = try container.decodeIfPresent(
            Bool.self, forKey: .systemAudioCaptureEnabled
        ) ?? false
        reloadWhenChangingOutputDevice = try container.decodeIfPresent(
            Bool.self, forKey: .reloadWhenChangingOutputDevice
        ) ?? true
        videoFramework = try container.decodeIfPresent(
            GSVideoFramework.self, forKey: .videoFramework
        ) ?? .avkit
        wallpaperEngineAssetsDirectory = try container.decodeIfPresent(
            String.self, forKey: .wallpaperEngineAssetsDirectory
        )
        processPiority = try container.decodeIfPresent(
            GSProcessPiority.self, forKey: .processPiority
        ) ?? .normal
        pauseOnVRAMExhausted = try container.decodeIfPresent(
            Bool.self, forKey: .pauseOnVRAMExhausted
        ) ?? false
        restartAfterCrashing = try container.decodeIfPresent(
            Bool.self, forKey: .restartAfterCrashing
        ) ?? false
        logLevel = try container.decodeIfPresent(
            GSLogLevel.self, forKey: .logLevel
        ) ?? .none
        autoRefresh = try container.decodeIfPresent(Bool.self, forKey: .autoRefresh) ?? true
    }

    var playbackPolicyConfiguration: PlaybackPolicyConfiguration {
        PlaybackPolicyConfiguration(
            otherApplicationFocused: otherApplicationFocused,
            otherApplicationFullscreenOrMaximized: otherApplicationFullscreen,
            otherApplicationPlayingAudio: otherApplicationPlayingAudio,
            laptopOnBattery: laptopOnBattery
        )
    }

    var requiresSystemAudioCaptureForAudioRule: Bool {
        otherApplicationPlayingAudio != .keepRunning &&
            !systemAudioCaptureEnabled
    }
}

@MainActor
class GlobalSettingsViewModel: ObservableObject {
    @Published var settings: GlobalSettings
    {
        didSet {
            save()
            validate()
            SceneSystemAudioSpectrumProvider.shared.setCaptureAllowed(
                settings.systemAudioCaptureEnabled
            )
        }
    }
    
    @Published var selection = 0

    @Published var isFirstLaunch = UserDefaults.standard.value(forKey: "IsFirstLaunch") as? Bool ?? true

    /// A host-condition detector can require privacy permission or a live
    /// capture stream. Keep that failure visible instead of treating the
    /// condition as if it were being monitored successfully.
    @Published private(set) var playbackPolicyIssue: String?
    
    var didFinishLaunchingNotificationCancellable: Cancellable?
    var didActivateApplicationNotificationCancellable: Cancellable?
    var didCurrentWallpaperChangeCancellable: Cancellable?
    var didAddToLoginItemCancellable: Cancellable?
    var didChangeAdjustMenuBarTintCancellable: Cancellable?
    var didChangePlaybackPolicyConfigurationCancellable: Cancellable?
    private var playbackConditionMonitor: PlaybackConditionMonitor?

    private var playbackPolicyState = PlaybackPolicyState()
    private var appliedPlaybackPolicyAction = GSPlayback.keepRunning
    private var hasStartedPlaybackPolicy = false
    
    init() {
        if let data = UserDefaults.standard.data(forKey: "GlobalSettings"),
           let settings = try? JSONDecoder().decode(GlobalSettings.self, from: data) {
            self.settings = settings
        } else {
            self.settings = GlobalSettings()
        }
        self.playbackPolicyState = PlaybackPolicyState(
            configuration: self.settings.playbackPolicyConfiguration
        )
        SceneSystemAudioSpectrumProvider.shared.setCaptureAllowed(
            self.settings.systemAudioCaptureEnabled
        )
        
        // Add observers
        self.didFinishLaunchingNotificationCancellable =
        NotificationCenter.default.publisher(for: NSApplication.didFinishLaunchingNotification)
            .sink { [weak self] _ in self?.didFinishLaunchingNotification() }
    }
    
    deinit {
        MainActor.assumeIsolated {
            playbackConditionMonitor?.stop()
        }
        didActivateApplicationNotificationCancellable?.cancel()
        didFinishLaunchingNotificationCancellable?.cancel()
        didCurrentWallpaperChangeCancellable?.cancel()
        didAddToLoginItemCancellable?.cancel()
        didChangeAdjustMenuBarTintCancellable?.cancel()
        didChangePlaybackPolicyConfigurationCancellable?.cancel()
    }
    
    func didFinishLaunchingNotification() {
        guard !hasStartedPlaybackPolicy else { return }
        hasStartedPlaybackPolicy = true

        self.didActivateApplicationNotificationCancellable =
        NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didActivateApplicationNotification)
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in self?.activateApplicationDidChange() }

        let conditionMonitor = PlaybackConditionMonitor(
            eventHandler: { [weak self] event in
                self?.updatePlaybackPolicy(event)
            },
            issueHandler: { [weak self] issue in
                self?.playbackPolicyIssue = issue
            }
        )
        playbackConditionMonitor = conditionMonitor
        conditionMonitor.start()
        // NSWorkspace may already have delivered the activation notification
        // before this observer was installed. Sample the current frontmost
        // application explicitly at startup so the initial policy is real.
        activateApplicationDidChange()
        
        self.didCurrentWallpaperChangeCancellable =
        AppDelegate.shared.wallpaperViewModel.$wallpapers
            .sink { [weak self] wallpapers in
                let mainId = WallpaperViewModel.mainScreenId()
                if let wp = wallpapers[mainId] {
                    self?.didCurrentWallpaperChange(wp)
                }
            }
        
        self.didAddToLoginItemCancellable =
        self.$settings
            .removeDuplicates { $0.autoStart == $1.autoStart }
            .map { $0.autoStart }
            .sink { [weak self] in self?.didAddToLoginItem($0) }
        
        self.didChangeAdjustMenuBarTintCancellable =
        self.$settings
            .removeDuplicates { $0.adjustMenuBarTint == $1.adjustMenuBarTint }
            .map { $0.adjustMenuBarTint }
            .sink { [weak self] in self?.didChangeAdjustMenuBarTint($0) }

        self.didChangePlaybackPolicyConfigurationCancellable =
        self.$settings
            .removeDuplicates { previous, current in
                previous.playbackPolicyConfiguration == current.playbackPolicyConfiguration &&
                    previous.systemAudioCaptureEnabled == current.systemAudioCaptureEnabled
            }
            .receive(on: RunLoop.main)
            .sink { [weak self] settings in
                guard let self else { return }
                let configuration = settings.playbackPolicyConfiguration
                self.playbackConditionMonitor?.setAudioDetectionEnabled(
                    settings.systemAudioCaptureEnabled &&
                        configuration.otherApplicationPlayingAudio != .keepRunning
                )
                self.updatePlaybackPolicy(.configurationChanged(configuration))
            }
            
        self.validate()
    }

    func stopPlaybackPolicyMonitoring(restorePlayback: Bool = true) {
        playbackConditionMonitor?.stop()
        playbackConditionMonitor = nil
        playbackPolicyIssue = nil
        didActivateApplicationNotificationCancellable?.cancel()
        didActivateApplicationNotificationCancellable = nil
        didChangePlaybackPolicyConfigurationCancellable?.cancel()
        didChangePlaybackPolicyConfigurationCancellable = nil
        playbackPolicyState = PlaybackPolicyState(
            configuration: settings.playbackPolicyConfiguration
        )
        if restorePlayback {
            let wasStopped = appliedPlaybackPolicyAction == .stop
            AppDelegate.shared.wallpaperViewModel.setPlaybackPolicyAction(.keepRunning)
            appliedPlaybackPolicyAction = .keepRunning
            if wasStopped {
                scheduleWallpaperWindowRestoration()
            }
        }
        hasStartedPlaybackPolicy = false
    }
    
    func didAddToLoginItem(_ added: Bool) {
        let appService = SMAppService.mainApp
        do {
            if added {
                try appService.register()
            } else {
                try appService.unregister()
            }
        } catch {
            print(error)
        }
    }
    
    func didChangeAdjustMenuBarTint(_ newValue: Bool) {
        if newValue != true {
            if let wallpaper = UserDefaults.standard.url(forKey: "OSWallpaper") {
                try? NSWorkspace.shared.setDesktopImageURL(wallpaper, for: .main!)
            }
        } else {
            do {
                let url = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appending(path: "staticWP_\(AppDelegate.shared.wallpaperViewModel.currentWallpaper.wallpaperDirectory.hashValue).tiff")
                try NSWorkspace.shared.setDesktopImageURL(url, for: .main!)
            } catch {
                print(error)
            }
        }
    }
    
    func didCurrentWallpaperChange(_ newValue: WEWallpaper) {
        AppDelegate.shared.setPlacehoderWallpaper(with: newValue)
    }
    
    func reset() {
        settings = (try? JSONDecoder()
            .decode(GlobalSettings.self,
                from: UserDefaults.standard.data(forKey: "GlobalSettings")
            ?? Data()))
        ?? GlobalSettings()
    }
    
    func save() {
        let data = try! JSONEncoder().encode(settings)
        UserDefaults.standard.set(data, forKey: "GlobalSettings")
    }
    
    func setQuality(_ quality: GSQuality) {
        switch quality {
        case .low:
            self.settings.antiAliasing = .none
            self.settings.postProcessing = .disabled
            self.settings.textureResolution = .highQuality
            self.settings.fps = 10
            self.settings.reflections = false
        case .medium:
            self.settings.antiAliasing = .none
            self.settings.postProcessing = .enabled
            self.settings.textureResolution = .highQuality
            self.settings.fps = 15
            self.settings.reflections = true
        case .high:
            self.settings.antiAliasing = .msaa_x2
            self.settings.postProcessing = .enabled
            self.settings.textureResolution = .highQuality
            self.settings.fps = 25
            self.settings.reflections = true
        case .ultra:
            self.settings.antiAliasing = .msaa_x2
            self.settings.postProcessing = .ultra
            self.settings.textureResolution = .highQuality
            self.settings.fps = 30
            self.settings.reflections = true
        }
    }
    
    private func validate() {
        switch settings.appearance {
        case .light:
            NSApp.appearance = NSAppearance(named: .aqua)
        case .dark:
            NSApp.appearance = NSAppearance(named: .darkAqua)
        case .followSystem:
            NSApp.appearance = nil
        }
    }
    
    func activateApplicationDidChange() {
        let bundleIdentifier = NSWorkspace.shared.frontmostApplication?.bundleIdentifier
        let isOtherApplication = bundleIdentifier != nil &&
            bundleIdentifier != "com.apple.finder" &&
            bundleIdentifier != Bundle.main.bundleIdentifier
        updatePlaybackPolicy(.otherApplicationFocused(isOtherApplication))
    }

    /// Feeds an explicit host condition into the policy state machine.
    /// Platform detectors should call this on the application thread; no
    /// detector is synthesized when a condition has no data source yet.
    @discardableResult
    func updatePlaybackPolicy(
        _ event: PlaybackPolicyEvent
    ) -> PlaybackPolicyTransition {
        let transition = playbackPolicyState.reduce(event)
        guard transition.changed else { return transition }
        applyPlaybackPolicyAction(transition.current)
        return transition
    }

    private func scheduleWallpaperWindowRestoration() {
        // @Published invalidation is delivered synchronously, while SwiftUI
        // mounts the replacement runtime on the following run-loop turn. Keep
        // the wallpaper windows hidden until that mount has been requested.
        DispatchQueue.main.async { [weak self] in
            guard let self,
                  self.appliedPlaybackPolicyAction != .stop else { return }
            AppDelegate.shared.setWallpaperWindowsSuppressedForPlayback(false)
        }
    }

    private func applyPlaybackPolicyAction(_ action: GSPlayback) {
        let previousAction = appliedPlaybackPolicyAction
        if action == previousAction {
            return
        }

        let wallpaperViewModel = AppDelegate.shared.wallpaperViewModel
        wallpaperViewModel.setPlaybackPolicyAction(action)
        appliedPlaybackPolicyAction = action
        if action == .stop {
            AppDelegate.shared.setWallpaperWindowsSuppressedForPlayback(true)
        } else if previousAction == .stop {
            scheduleWallpaperWindowRestoration()
        }
    }

}
