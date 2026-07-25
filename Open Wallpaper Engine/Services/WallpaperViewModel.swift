//
//  WallpaperViewModel.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/8/14.
//

import SwiftUI
import ColorSync

/// Provide Wallpaper Database for WallpaperView and ContentView etc.
@MainActor
class WallpaperViewModel: ObservableObject {
    private static let scenePropertyPersistenceKey = "ScenePropertyPersistenceV1"

    @Published var nextCurrentWallpaper: WEWallpaper =
    WEWallpaper(using: .invalid, where: Bundle.main.url(forResource: "WallpaperNotFound", withExtension: "mp4")!) {
        willSet {
            if ["web", "application"].contains(newValue.project.type) {
                if let trustedWallpapers = UserDefaults.standard.array(forKey: "TrustedWallpapers") as? [String],
                   trustedWallpapers.contains(newValue.wallpaperDirectory.path(percentEncoded: false)) {
                    self.setWallpaper(newValue, for: selectedScreenId)
                } else {
                    AppDelegate.shared.contentViewModel.warningUnsafeWallpaperModal(which: newValue)
                }
            } else {
                self.setWallpaper(newValue, for: selectedScreenId)
            }
        }
    }

    /// Per-screen wallpaper assignments, keyed by CGDirectDisplayID as String.
    @Published var wallpapers: [String: WEWallpaper] = [:] {
        didSet { saveWallpapers() }
    }

    /// Screens where wallpaper display is enabled.
    @Published var enabledScreens: Set<String> = [] {
        didSet {
            UserDefaults.standard.set(Array(enabledScreens), forKey: "EnabledScreens")
        }
    }

    /// The screen currently selected in the UI for configuration.
    @Published var selectedScreenId: String = ""

    /// Runtime-owned property descriptors copied for the App UI. They are not
    /// persisted because the Scene package remains authoritative.
    @Published private(set) var scenePropertyCatalogs: [String: [String: ScenePropertyCatalog]] = [:]
    @Published private(set) var sceneRuntimeErrors: [String: [String: String]] = [:]

    /// The only persisted source of user overrides. Values are isolated by
    /// screen and canonical Scene identity.
    @Published private(set) var scenePropertyPersistence = ScenePropertyPersistence()
    @Published private(set) var scenePropertyPersistenceError: String?

    static let defaultWallpaper = WEWallpaper(using: .invalid, where: Bundle.main.url(forResource: "WallpaperNotFound", withExtension: "mp4")!)

    // MARK: - Recent wallpapers

    private static let maxRecents = 10
    private static let recentsKey = "RecentWallpapers"

    @Published var recentWallpapers: [WEWallpaper] = []

    private func loadRecents() {
        guard let data = UserDefaults.standard.data(forKey: Self.recentsKey),
              let saved = try? JSONDecoder().decode([WEWallpaper].self, from: data) else { return }
        recentWallpapers = saved.filter { $0.project != .invalid }
    }

    private func saveRecents() {
        if let data = try? JSONEncoder().encode(recentWallpapers) {
            UserDefaults.standard.set(data, forKey: Self.recentsKey)
        }
    }

    func addToRecents(_ wallpaper: WEWallpaper) {
        guard wallpaper.project != .invalid else { return }
        recentWallpapers.removeAll { $0.wallpaperDirectory == wallpaper.wallpaperDirectory }
        recentWallpapers.insert(wallpaper, at: 0)
        if recentWallpapers.count > Self.maxRecents {
            recentWallpapers = Array(recentWallpapers.prefix(Self.maxRecents))
        }
        saveRecents()
    }

    // MARK: - Wallpaper access

    /// Convenience: wallpaper for the currently selected screen in the UI.
    var currentWallpaper: WEWallpaper {
        get {
            wallpapers[selectedScreenId] ?? Self.defaultWallpaper
        }
        set {
            setWallpaper(newValue, for: selectedScreenId)
        }
    }

    /// Get wallpaper for a specific screen.
    func wallpaper(for screenId: String) -> WEWallpaper {
        wallpapers[screenId] ?? Self.defaultWallpaper
    }

    /// Set wallpaper for a specific screen.
    func setWallpaper(_ wallpaper: WEWallpaper, for screenId: String) {
        wallpapers[screenId] = wallpaper
        addToRecents(wallpaper)
    }

    func isScreenEnabled(_ screenId: String) -> Bool {
        enabledScreens.contains(screenId)
    }

    func toggleScreen(_ screenId: String) {
        if enabledScreens.contains(screenId) {
            enabledScreens.remove(screenId)
        } else {
            enabledScreens.insert(screenId)
        }
        AppDelegate.shared.rebuildWallpaperWindows()
    }

    func scenePropertyCatalog(
        for screenId: String,
        wallpaper: WEWallpaper
    ) -> ScenePropertyCatalog? {
        scenePropertyCatalogs[screenId]?[wallpaper.scenePropertyIdentity]
    }

    func scenePropertyOverrides(
        for screenId: String,
        wallpaper: WEWallpaper
    ) -> [String: ScenePropertyValue] {
        scenePropertyPersistence.values(
            screenId: Self.scenePropertyScreenIdentity(screenId),
            wallpaperIdentity: wallpaper.scenePropertyIdentity
        )
    }

    func registerScenePropertyCatalog(
        _ properties: [ScenePropertyDefinition],
        for screenId: String,
        wallpaper: WEWallpaper
    ) {
        let identity = wallpaper.scenePropertyIdentity
        let catalog = ScenePropertyCatalog(
            wallpaperIdentity: identity,
            properties: properties
        )
        guard scenePropertyCatalogs[screenId]?[identity] != catalog else { return }
        var catalogs = scenePropertyCatalogs
        var screenCatalogs = catalogs[screenId] ?? [:]
        screenCatalogs[identity] = catalog
        catalogs[screenId] = screenCatalogs
        scenePropertyCatalogs = catalogs
    }

    func clearScenePropertyCatalog(for screenId: String, wallpaper: WEWallpaper) {
        let identity = wallpaper.scenePropertyIdentity
        guard scenePropertyCatalogs[screenId]?[identity] != nil else { return }
        var catalogs = scenePropertyCatalogs
        catalogs[screenId]?[identity] = nil
        if catalogs[screenId]?.isEmpty == true {
            catalogs[screenId] = nil
        }
        scenePropertyCatalogs = catalogs
    }

    func sceneRuntimeError(for screenId: String, wallpaper: WEWallpaper) -> String? {
        sceneRuntimeErrors[screenId]?[wallpaper.scenePropertyIdentity]
    }

    func setSceneRuntimeError(
        _ message: String?,
        for screenId: String,
        wallpaper: WEWallpaper
    ) {
        let identity = wallpaper.scenePropertyIdentity
        guard sceneRuntimeErrors[screenId]?[identity] != message else { return }
        var errors = sceneRuntimeErrors
        errors[screenId, default: [:]][identity] = message
        if errors[screenId]?.isEmpty == true {
            errors[screenId] = nil
        }
        sceneRuntimeErrors = errors
    }

    func setSceneProperty(
        _ value: ScenePropertyValue,
        key: String,
        for screenId: String,
        wallpaper: WEWallpaper
    ) {
        var persistence = scenePropertyPersistence
        guard persistence.set(
            value,
            key: key,
            screenId: Self.scenePropertyScreenIdentity(screenId),
            wallpaperIdentity: wallpaper.scenePropertyIdentity
        ) else { return }
        commitScenePropertyPersistence(persistence)
    }

    func resetSceneProperties(for screenId: String, wallpaper: WEWallpaper) {
        var persistence = scenePropertyPersistence
        guard persistence.reset(
            screenId: Self.scenePropertyScreenIdentity(screenId),
            wallpaperIdentity: wallpaper.scenePropertyIdentity
        ) else { return }
        commitScenePropertyPersistence(persistence)
    }

    func discardScenePropertyPersistence() {
        UserDefaults.standard.removeObject(forKey: Self.scenePropertyPersistenceKey)
        scenePropertyPersistence = ScenePropertyPersistence()
        scenePropertyPersistenceError = nil
    }

    /// Remove a wallpaper from all screens (e.g., when unsubscribing).
    func removeWallpaperFromAllScreens(directory: URL) {
        for (key, wp) in wallpapers {
            if wp.wallpaperDirectory == directory {
                wallpapers[key] = Self.defaultWallpaper
            }
        }
        recentWallpapers.removeAll { $0.wallpaperDirectory == directory }
        saveRecents()

        var catalogs = scenePropertyCatalogs
        var runtimeErrors = sceneRuntimeErrors
        var persistence = scenePropertyPersistence
        let canonicalDirectory = directory.standardizedFileURL.resolvingSymlinksInPath()
        for screenId in Array(catalogs.keys) {
            let identities = catalogs[screenId].map { Array($0.keys) } ?? []
            for identity in identities {
                let sceneDirectory = URL(fileURLWithPath: identity)
                    .deletingLastPathComponent()
                    .standardizedFileURL
                    .resolvingSymlinksInPath()
                if sceneDirectory == canonicalDirectory {
                    catalogs[screenId]?[identity] = nil
                }
            }
            if catalogs[screenId]?.isEmpty == true {
                catalogs[screenId] = nil
            }
        }
        for screenId in Array(runtimeErrors.keys) {
            let identities = runtimeErrors[screenId].map { Array($0.keys) } ?? []
            for identity in identities {
                let sceneDirectory = URL(fileURLWithPath: identity)
                    .deletingLastPathComponent()
                    .standardizedFileURL
                    .resolvingSymlinksInPath()
                if sceneDirectory == canonicalDirectory {
                    runtimeErrors[screenId]?[identity] = nil
                }
            }
            if runtimeErrors[screenId]?.isEmpty == true {
                runtimeErrors[screenId] = nil
            }
        }
        _ = persistence.remove(wallpaperDirectory: directory)
        scenePropertyCatalogs = catalogs
        sceneRuntimeErrors = runtimeErrors
        if persistence != scenePropertyPersistence {
            commitScenePropertyPersistence(persistence)
        }
    }

    private(set) var playbackPolicyAction = GSPlayback.keepRunning
    @Published private(set) var effectivePlayRate: Float = 1.0
    @Published private(set) var effectivePlayVolume: Float = 1.0

    var lastPlayRate: Float = 1.0
    @Published public var playRate: Float = 1.0 {
        willSet {
            if newValue == 0.0 {
                for (index, item) in AppDelegate.shared.statusItem.menu!.items.enumerated() {
                    if item.title == "Pause" {
                        AppDelegate.shared.statusItem.menu!.items[index] =
                            .init(title: "Resume", systemImage: "play.fill", action: #selector(AppDelegate.shared.resume), keyEquivalent: "")
                    }
                }
            } else {
                for (index, item) in AppDelegate.shared.statusItem.menu!.items.enumerated() {
                    if item.title == "Resume" {
                        AppDelegate.shared.statusItem.menu!.items[index] =
                            .init(title: "Pause", systemImage: "pause.fill", action: #selector(AppDelegate.shared.pause), keyEquivalent: "")
                    }
                }
            }
        }
        didSet {
            self.lastPlayRate = oldValue
            refreshEffectivePlaybackState()
        }
    }

    var lastPlayVolume: Float = 1.0
    @Published public var playVolume: Float = 1.0 {
        willSet {
            if newValue == 0.0 {
                for (index, item) in AppDelegate.shared.statusItem.menu!.items.enumerated() {
                    if item.title == "Mute" {
                        AppDelegate.shared.statusItem.menu!.items[index] =
                            .init(title: String(localized: "Unmute"), systemImage: "speaker.fill", action: #selector(AppDelegate.shared.unmute), keyEquivalent: "")
                    }
                }
            } else {
                for (index, item) in AppDelegate.shared.statusItem.menu!.items.enumerated() {
                    if item.title == "Unmute" {
                        AppDelegate.shared.statusItem.menu!.items[index] =
                            .init(title: String(localized: "Mute"), systemImage: "speaker.slash.fill", action: #selector(AppDelegate.shared.mute), keyEquivalent: "")
                    }
                }
            }
        }
        didSet {
            self.lastPlayVolume = oldValue
            refreshEffectivePlaybackState()
        }
    }

    /// Applies a host-policy override without overwriting the user's playback
    /// intent. Views consume the derived effective values, while sliders and
    /// menu actions continue to edit `playRate` and `playVolume` directly.
    func setPlaybackPolicyAction(_ action: GSPlayback) {
        guard playbackPolicyAction != action else { return }
        playbackPolicyAction = action
        refreshEffectivePlaybackState()
    }

    private func refreshEffectivePlaybackState() {
        let nextRate: Float
        let nextVolume: Float
        switch playbackPolicyAction {
        case .keepRunning:
            nextRate = playRate
            nextVolume = playVolume
        case .mute:
            nextRate = playRate
            nextVolume = 0
        case .pause, .stop:
            nextRate = 0
            nextVolume = playVolume
        }
        if effectivePlayRate != nextRate {
            effectivePlayRate = nextRate
        }
        if effectivePlayVolume != nextVolume {
            effectivePlayVolume = nextVolume
        }
    }

    init() {
        // Load per-screen wallpapers
        if let data = UserDefaults.standard.data(forKey: "ScreenWallpapers"),
           let saved = try? JSONDecoder().decode([String: WEWallpaper].self, from: data) {
            // Filter out any compound keys (screenId_spaceId) from previous per-space experiment
            self.wallpapers = saved.filter { !$0.key.contains("_") }
        }
        // Migrate legacy single wallpaper
        else if let json = UserDefaults.standard.data(forKey: "CurrentWallpaper"),
                let wallpaper = try? JSONDecoder().decode(WEWallpaper.self, from: json) {
            let mainId = Self.mainScreenId()
            self.wallpapers = [mainId: wallpaper]
        }

        // Load enabled screens (default: all connected screens enabled)
        if let saved = UserDefaults.standard.array(forKey: "EnabledScreens") as? [String] {
            self.enabledScreens = Set(saved)
        } else {
            self.enabledScreens = Set(NSScreen.screens.map { Self.screenId(for: $0) })
        }

        // Default selected screen to main
        self.selectedScreenId = Self.mainScreenId()

        // Load recent wallpapers
        loadRecents()
        loadScenePropertyPersistence()
    }

    // MARK: - Screen ID helpers

    static func screenId(for screen: NSScreen) -> String {
        let displayId = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID ?? 0
        return String(displayId)
    }

    static func mainScreenId() -> String {
        guard let main = NSScreen.main else { return "0" }
        return screenId(for: main)
    }

    static func screenName(for screen: NSScreen) -> String {
        screen.localizedName
    }

    static func scenePropertyScreenIdentity(_ screenId: String) -> String {
        if let rawDisplayId = UInt32(screenId),
           let unmanagedUUID = CGDisplayCreateUUIDFromDisplayID(rawDisplayId) {
            let uuid = unmanagedUUID.takeRetainedValue()
            return "display:\(CFUUIDCreateString(nil, uuid) as String)"
        }
        let legacy = "legacy-display-id:\(screenId)"
        NSLog("[Scene] Unable to resolve a persistent display UUID for %@; using %@", screenId, legacy)
        return legacy
    }

    // MARK: - Persistence

    private func saveWallpapers() {
        if let data = try? JSONEncoder().encode(wallpapers) {
            UserDefaults.standard.set(data, forKey: "ScreenWallpapers")
        }
        // Keep legacy key updated for backward compat
        if let data = try? JSONEncoder().encode(currentWallpaper) {
            UserDefaults.standard.set(data, forKey: "CurrentWallpaper")
        }
    }

    private func loadScenePropertyPersistence() {
        guard let data = UserDefaults.standard.data(
            forKey: Self.scenePropertyPersistenceKey
        ) else { return }
        do {
            let persistence = try JSONDecoder().decode(
                ScenePropertyPersistence.self,
                from: data
            )
            guard persistence.version == ScenePropertyPersistence.currentVersion else {
                let message = "Unsupported Scene property persistence version: \(persistence.version)"
                scenePropertyPersistenceError = message
                NSLog("[Scene] %@", message)
                return
            }
            scenePropertyPersistence = persistence
            scenePropertyPersistenceError = nil
        } catch {
            let message = "Scene property settings could not be loaded: \(error.localizedDescription)"
            scenePropertyPersistenceError = message
            NSLog("[Scene] %@", message)
        }
    }

    @discardableResult
    private func commitScenePropertyPersistence(
        _ persistence: ScenePropertyPersistence
    ) -> Bool {
        do {
            let data = try JSONEncoder().encode(persistence)
            UserDefaults.standard.set(data, forKey: Self.scenePropertyPersistenceKey)
            scenePropertyPersistence = persistence
            scenePropertyPersistenceError = nil
            return true
        } catch {
            let message = "Scene property settings could not be saved: \(error.localizedDescription)"
            scenePropertyPersistenceError = message
            NSLog("[Scene] %@", message)
            return false
        }
    }
}
