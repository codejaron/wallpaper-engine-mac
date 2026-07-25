//
//  GlobalSettingsService.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/9/2.
//

import Cocoa
import Combine
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
    
    // MARK: Playback
    var otherApplicationFocused = GSPlayback.keepRunning
    var otherApplicationFullscreen = GSPlayback.keepRunning
    var otherApplicationPlayingAudio = GSPlayback.keepRunning
    var displayAsleep = GSPlayback.keepRunning
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
    var scenePresentationScaling = GSScenePresentationScaling.aspectFill
    var sceneSpanAcrossScreens = false
    
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
             otherApplicationPlayingAudio, displayAsleep, laptopOnBattery
        case antiAliasing, postProcessing, textureResolution, reflections, fps
        case scenePresentationScaling, sceneSpanAcrossScreens
        case autoStart, safeMode, language
        case adjustMenuBarTint, appearance
        case audioOutput, reloadWhenChangingOutputDevice
        case videoFramework
        case wallpaperEngineAssetsDirectory, processPiority,
             pauseOnVRAMExhausted, restartAfterCrashing
        case logLevel, autoRefresh
    }

    init() {
        otherApplicationFocused = .keepRunning
        otherApplicationFullscreen = .keepRunning
        otherApplicationPlayingAudio = .keepRunning
        displayAsleep = .keepRunning
        laptopOnBattery = .keepRunning
        antiAliasing = .msaa_x2
        postProcessing = .disabled
        textureResolution = .automatic
        reflections = false
        fps = 30
        scenePresentationScaling = .aspectFill
        sceneSpanAcrossScreens = false
        autoStart = false
        safeMode = false
        language = .followSystem
        adjustMenuBarTint = true
        appearance = .followSystem
        audioOutput = true
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
        displayAsleep = try container.decodeIfPresent(
            GSPlayback.self, forKey: .displayAsleep
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
        scenePresentationScaling = try container.decodeIfPresent(
            GSScenePresentationScaling.self, forKey: .scenePresentationScaling
        ) ?? .aspectFill
        sceneSpanAcrossScreens = try container.decodeIfPresent(
            Bool.self, forKey: .sceneSpanAcrossScreens
        ) ?? false
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
            otherApplicationFullscreen: otherApplicationFullscreen,
            otherApplicationPlayingAudio: otherApplicationPlayingAudio,
            displayAsleep: displayAsleep,
            laptopOnBattery: laptopOnBattery
        )
    }
}

@MainActor
class GlobalSettingsViewModel: ObservableObject {
    @Published var settings: GlobalSettings
    {
        didSet { save(); validate() }
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
    private var stoppedWallpaperWindowIDs: Set<String>?
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
            .map(\.playbackPolicyConfiguration)
            .removeDuplicates()
            .receive(on: RunLoop.main)
            .sink { [weak self] configuration in
                guard let self else { return }
                self.playbackConditionMonitor?.setAudioDetectionEnabled(
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
        if restorePlayback, appliedPlaybackPolicyAction == .stop {
            restoreStoppedWindowVisibility()
        }
        stoppedWallpaperWindowIDs = nil
        appliedPlaybackPolicyAction = .keepRunning
        playbackPolicyState = PlaybackPolicyState(
            configuration: settings.playbackPolicyConfiguration
        )
        AppDelegate.shared.wallpaperViewModel.setPlaybackPolicyAction(.keepRunning)
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
        print(String(describing: String(data: data, encoding: .utf8)))
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

    /// Re-captures the normal visibility of newly-created wallpaper windows
    /// and reapplies `stop`. The windows are ordered front by AppDelegate
    /// during a rebuild, so that visibility is the correct restoration state.
    func wallpaperWindowsDidRebuild() {
        guard playbackPolicyState.effectiveAction == .stop else {
            return
        }
        stoppedWallpaperWindowIDs = Set(
            AppDelegate.shared.wallpaperWindows.compactMap { id, window in
                window.isVisible ? id : nil
            }
        )
        applyPlaybackPolicyAction(.stop, force: true)
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

    private func captureStoppedWindowVisibility() {
        stoppedWallpaperWindowIDs = Set(
            AppDelegate.shared.wallpaperWindows.compactMap { id, window in
                window.isVisible ? id : nil
            }
        )
    }

    private func restoreStoppedWindowVisibility() {
        guard let stoppedWallpaperWindowIDs else { return }
        for (id, window) in AppDelegate.shared.wallpaperWindows {
            if stoppedWallpaperWindowIDs.contains(id) {
                window.orderFront(nil)
            } else {
                window.orderOut(nil)
            }
        }
    }

    private func applyPlaybackPolicyAction(
        _ action: GSPlayback,
        force: Bool = false
    ) {
        let previousAction = appliedPlaybackPolicyAction
        if action == previousAction && !force {
            return
        }

        if action == .stop && previousAction != .stop {
            captureStoppedWindowVisibility()
        } else if previousAction == .stop && action != .stop {
            restoreStoppedWindowVisibility()
            stoppedWallpaperWindowIDs = nil
        }

        let wallpaperViewModel = AppDelegate.shared.wallpaperViewModel
        wallpaperViewModel.setPlaybackPolicyAction(action)
        if action == .stop {
            for window in AppDelegate.shared.wallpaperWindows.values {
                window.orderOut(nil)
            }
        }
        appliedPlaybackPolicyAction = action
    }

}
