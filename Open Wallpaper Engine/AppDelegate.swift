//
//  AppDelegate.swift
//  Open Wallpaper Engine
//
//  Created by Haren on 2023/6/6.
//

import Cocoa
import SwiftUI
import AVKit
import SceneAudio
import WebKit

private final class WallpaperScrollEvent: NSEvent {
    private let source: NSEvent
    private let targetWindow: NSWindow
    private let targetLocation: NSPoint

    init(source: NSEvent, targetWindow: NSWindow, targetLocation: NSPoint) {
        precondition(source.type == .scrollWheel)
        self.source = source
        self.targetWindow = targetWindow
        self.targetLocation = targetLocation
        super.init()
    }

    required init?(coder: NSCoder) {
        return nil
    }

    override var type: NSEvent.EventType { source.type }
    override var window: NSWindow? { targetWindow }
    override var windowNumber: Int { targetWindow.windowNumber }
    override var locationInWindow: NSPoint { targetLocation }
    override var modifierFlags: NSEvent.ModifierFlags { source.modifierFlags }
    override var timestamp: TimeInterval { source.timestamp }
    override var eventNumber: Int { source.eventNumber }
    override var deltaX: CGFloat { source.deltaX }
    override var deltaY: CGFloat { source.deltaY }
    override var deltaZ: CGFloat { source.deltaZ }
    override var scrollingDeltaX: CGFloat { source.scrollingDeltaX }
    override var scrollingDeltaY: CGFloat { source.scrollingDeltaY }
    override var hasPreciseScrollingDeltas: Bool {
        source.hasPreciseScrollingDeltas
    }
    override var phase: NSEvent.Phase { source.phase }
    override var momentumPhase: NSEvent.Phase { source.momentumPhase }
    override var isDirectionInvertedFromDevice: Bool {
        source.isDirectionInvertedFromDevice
    }
    override var cgEvent: CGEvent? { source.cgEvent }
}

@MainActor
class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    
    var statusItem: NSStatusItem!
    var settingsWindow: NSWindow!
    
    var mainWindowController: MainWindowController!
    
    var wallpaperWindows: [String: NSWindow] = [:]
    private(set) var playbackSuppressesWallpaperWindows = false
    
    var contentViewModel = ContentViewModel()
    var wallpaperViewModel = WallpaperViewModel()
    var globalSettingsViewModel = GlobalSettingsViewModel()
    let sceneMediaSnapshotProvider = SceneMediaSnapshotProvider()
    let sceneAudioCaptureLifecycleMonitor = SceneAudioCaptureLifecycleMonitor()
    lazy var sceneAudioOwnerCoordinator = MainActor.assumeIsolated {
        SceneAudioOwnerCoordinator(mainScreenId: WallpaperViewModel.mainScreenId())
    }
    
    var importOpenPanel: NSOpenPanel!
    
    var eventHandler: Any?
    var localEventHandler: Any?
    
    static var shared = AppDelegate()
    
    func applicationWillFinishLaunching(_ notification: Notification) {
        sceneAudioCaptureLifecycleMonitor.start()

        // 创建设置视窗
        setSettingsWindow()
        
        // 创建桌面壁纸视窗
        setWallpaperWindows()

        // 监听显示器连接/断开
        NotificationCenter.default.addObserver(
            self, selector: #selector(screensChanged),
            name: NSApplication.didChangeScreenParametersNotification, object: nil
        )
        
        // 创建化左上角菜单栏
        setMainMenu()
        
        // 创建化右上角常驻菜单栏
        setStatusMenu()
        
        // 创建主视窗
        self.mainWindowController = MainWindowController()
        
        // 将外部输入传递到壁纸窗口
        AppDelegate.shared.setEventHandler()
    }
    
    func applicationDockMenu(_ sender: NSApplication) -> NSMenu? {
        let dockMenu = self.statusItem.menu?.copy() as! NSMenu?
        dockMenu?.items.removeLast() // Remove `Quit` menu item
        return dockMenu
    }
    
// MARK: - delegate methods
    func applicationDidFinishLaunching(_ notification: Notification) {
        saveCurrentWallpaper()
        AppDelegate.shared.setPlacehoderWallpaper(with: wallpaperViewModel.currentWallpaper)

        reconcileWallpaperWindowVisibility()
        
        if globalSettingsViewModel.isFirstLaunch {
            self.mainWindowController.window.center()
            self.mainWindowController.window.makeKeyAndOrderFront(nil)
        }

        // Apply the real frontmost-application condition after wallpaper
        // windows have reached their intended initial visibility.
        globalSettingsViewModel.activateApplicationDidChange()
    }
    
    func applicationDidBecomeActive(_ notification: Notification) {
        NSApp.activate(ignoringOtherApps: true)
    }
    
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !self.mainWindowController.window.isVisible && !settingsWindow.isVisible {
            self.mainWindowController.window?.makeKeyAndOrderFront(nil)
        }
        
        return true
    }
    
    func applicationWillTerminate(_ notification: Notification) {
        sceneAudioCaptureLifecycleMonitor.stop()
        globalSettingsViewModel.stopPlaybackPolicyMonitoring(restorePlayback: false)
        if let eventHandler { NSEvent.removeMonitor(eventHandler) }
        if let localEventHandler { NSEvent.removeMonitor(localEventHandler) }
        eventHandler = nil
        localEventHandler = nil
        if let wallpaper = UserDefaults.standard.url(forKey: "OSWallpaper") {
            for screen in NSScreen.screens {
                try? NSWorkspace.shared.setDesktopImageURL(wallpaper, for: screen)
            }
        }
        
        let cacheDirectory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        do {
            let filesURL = try FileManager.default.contentsOfDirectory(at: cacheDirectory,
                                                                       includingPropertiesForKeys: nil,
                                                                       options: .skipsHiddenFiles)
            for url in filesURL {
                if url.lastPathComponent.contains("staticWP") {
                    try FileManager.default.removeItem(at: url)
                }
            }
        } catch {
            print(error)
        }
    }
    
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return false
    }

// MARK: - misc methods
    @objc func openSettingsWindow() {
        NSApp.activate(ignoringOtherApps: true)
        self.settingsWindow.center()
        self.settingsWindow.makeKeyAndOrderFront(nil)
    }
    
    @objc func openMainWindow() {
        self.mainWindowController.window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
    
    @MainActor @objc func toggleFilter() {
        self.contentViewModel.isFilterReveal.toggle()
    }
    
// MARK: Set Settings Window
    func setSettingsWindow() {
        self.settingsWindow = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 480, height: 300),
            styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
            backing: .buffered, defer: false)
        self.settingsWindow.title = "Settings"
        self.settingsWindow.isReleasedWhenClosed = false
        self.settingsWindow.toolbarStyle = .preference
        
        self.settingsWindow.delegate = self
        
        let toolbar = NSToolbar(identifier: "SettingsToolbar")
        toolbar.delegate = self
        
        toolbar.selectedItemIdentifier = SettingsToolbarIdentifiers.performance
        
        self.settingsWindow.toolbar = toolbar
        self.settingsWindow.contentView = NSHostingView(rootView: SettingsView().environmentObject(self.globalSettingsViewModel))
    }
    
// MARK: Set Wallpaper Windows - One per screen
    func setWallpaperWindowsSuppressedForPlayback(_ suppressed: Bool) {
        playbackSuppressesWallpaperWindows = suppressed
        reconcileWallpaperWindowVisibility()
    }

    func reconcileWallpaperWindowVisibility() {
        for window in wallpaperWindows.values {
            if playbackSuppressesWallpaperWindows {
                window.orderOut(nil)
            } else {
                window.orderFront(nil)
            }
        }
    }

    func setWallpaperWindows() {
        updateSceneAudioMainScreen()
        for screen in NSScreen.screens {
            let screenId = WallpaperViewModel.screenId(for: screen)
            guard wallpaperViewModel.isScreenEnabled(screenId) else { continue }

            let window = WallpaperWindow()
            window.styleMask = [.borderless, .fullSizeContentView]
            window.level = NSWindow.Level(Int(CGWindowLevelForKey(.desktopWindow)))
            window.collectionBehavior = [.stationary, .canJoinAllSpaces]
            window.setFrame(screen.frame, display: true)
            window.isMovable = false
            window.titlebarAppearsTransparent = true
            window.titleVisibility = .hidden
            window.canHide = false
            window.canBecomeVisibleWithoutLogin = true
            window.isReleasedWhenClosed = false
            window.ignoresMouseEvents = true
            window.setWallpaperContent(
                WallpaperView(viewModel: self.wallpaperViewModel, screenId: screenId)
            )
            wallpaperWindows[screenId] = window
        }
    }

    /// Rebuild wallpaper windows without changing enabled state.
    func rebuildWallpaperWindows() {
        updateSceneAudioMainScreen()
        for (_, window) in wallpaperWindows { window.close() }
        wallpaperWindows.removeAll()
        setWallpaperWindows()
        reconcileWallpaperWindowVisibility()
    }

    /// Called when monitors connect/disconnect — auto-enables newly connected screens.
    @objc func screensChanged() {
        updateSceneAudioMainScreen()
        let connectedIds = Set(NSScreen.screens.map { WallpaperViewModel.screenId(for: $0) })
        for id in connectedIds where !wallpaperViewModel.enabledScreens.contains(id) {
            wallpaperViewModel.enabledScreens.insert(id)
        }
        rebuildWallpaperWindows()
    }

    private func updateSceneAudioMainScreen() {
        MainActor.assumeIsolated {
            sceneAudioOwnerCoordinator.updateMainScreenId(
                WallpaperViewModel.mainScreenId()
            )
        }
    }
    
    func windowWillClose(_ notification: Notification) {
        globalSettingsViewModel.reset()
    }
    
    func setEventHandler() {
        // Only monitor event types we actually handle — .any causes main thread starvation
        let relevantEvents: NSEvent.EventTypeMask = [
            .scrollWheel, .mouseMoved, .mouseEntered, .mouseExited,
            .leftMouseUp, .rightMouseUp, .leftMouseDown, .rightMouseDown,
            .leftMouseDragged, .rightMouseDragged
        ]
        self.eventHandler = NSEvent.addGlobalMonitorForEvents(matching: relevantEvents) { [weak self] event in
            guard let self = self,
                  let frontmostApplication = NSWorkspace.shared.frontmostApplication,
                  frontmostApplication.bundleIdentifier == "com.apple.finder" else { return }
            self.forwardWallpaperEvent(event)
        }
        self.localEventHandler = NSEvent.addLocalMonitorForEvents(matching: relevantEvents) { [weak self] event in
            self?.forwardWallpaperEvent(event)
            return event
        }
    }

    private func forwardWallpaperEvent(_ event: NSEvent) {
        guard NSWorkspace.shared.frontmostApplication?.bundleIdentifier == "com.apple.finder" else {
            return
        }
        let mouseLocation = NSEvent.mouseLocation
        guard let targetWindow = wallpaperWindows.values.first(where: {
              $0.isVisible && $0.frame.contains(mouseLocation)
        }),
              let webview = findWebView(in: targetWindow.contentView) else { return }

        // Global-monitor events carry the originating application's window
        // number and location. Passing that event directly to a wallpaper
        // WKWebView makes hit testing use the wrong coordinate space (and is
        // especially visible on non-main displays). Rebuild mouse events in
        // the target wallpaper window's local coordinates.
        let localLocation = targetWindow.convertPoint(fromScreen: mouseLocation)
        let forwardedMouseEvent = makeWallpaperMouseEvent(
            event,
            location: localLocation,
            window: targetWindow
        )

        switch event.type {
        case .scrollWheel:
            // AppKit has no public scroll-wheel factory. Wrap the original
            // deltas/phases while rebinding the event to the wallpaper window
            // so WKWebView receives the correct location on secondary screens.
            webview.scrollWheel(with: WallpaperScrollEvent(
                source: event,
                targetWindow: targetWindow,
                targetLocation: localLocation
            ))
        case .mouseMoved:
            if let forwardedMouseEvent { webview.mouseMoved(with: forwardedMouseEvent) }
        case .mouseEntered:
            if let forwardedMouseEvent { webview.mouseEntered(with: forwardedMouseEvent) }
        case .mouseExited:
            if let forwardedMouseEvent { webview.mouseExited(with: forwardedMouseEvent) }
        case .leftMouseUp:
            if let forwardedMouseEvent { webview.mouseUp(with: forwardedMouseEvent) }
        case .rightMouseUp:
            if let forwardedMouseEvent { webview.rightMouseUp(with: forwardedMouseEvent) }
        case .leftMouseDown:
            if let forwardedMouseEvent { webview.mouseDown(with: forwardedMouseEvent) }
        case .rightMouseDown:
            if let forwardedMouseEvent { webview.rightMouseDown(with: forwardedMouseEvent) }
        case .leftMouseDragged:
            if let forwardedMouseEvent { webview.mouseDragged(with: forwardedMouseEvent) }
        case .rightMouseDragged:
            if let forwardedMouseEvent { webview.rightMouseDragged(with: forwardedMouseEvent) }
        default: break
        }
    }

    private func makeWallpaperMouseEvent(
        _ event: NSEvent,
        location: NSPoint,
        window: NSWindow
    ) -> NSEvent? {
        switch event.type {
        case .mouseMoved, .mouseEntered, .mouseExited,
             .leftMouseDown, .leftMouseUp, .leftMouseDragged,
             .rightMouseDown, .rightMouseUp, .rightMouseDragged:
            break
        default:
            return nil
        }
        return NSEvent.mouseEvent(
            with: event.type,
            location: location,
            modifierFlags: event.modifierFlags,
            timestamp: event.timestamp,
            windowNumber: window.windowNumber,
            context: nil,
            eventNumber: event.eventNumber,
            clickCount: event.clickCount,
            pressure: event.pressure
        )
    }

    private func findWebView(in view: NSView?) -> WKWebView? {
        guard let view else { return nil }
        if let webView = view as? WKWebView { return webView }
        for child in view.subviews {
            if let webView = findWebView(in: child) { return webView }
        }
        return nil
    }
    
    func saveCurrentWallpaper() {
        guard let mainScreen = NSScreen.main else { return }
        var wallpaper: URL {
            var osWallpaper: URL { NSWorkspace.shared.desktopImageURL(for: mainScreen)! }
            if let wallpaper = UserDefaults.standard.url(forKey: "OSWallpaper") {
                if wallpaper != osWallpaper {
                    if !wallpaper.lastPathComponent.contains("staticWP") {
                        return wallpaper
                    }
                }
            }
            return osWallpaper
        }
        UserDefaults.standard.set(wallpaper, forKey: "OSWallpaper")
    }
    
    func setPlacehoderWallpaper(with wallpaper: WEWallpaper) {
        switch wallpaper.project.type {
        case "video":
            let asset = AVAsset(url: wallpaper.wallpaperDirectory.appending(component: wallpaper.project.file))
            let imageGenerator = AVAssetImageGenerator(asset: asset)
            imageGenerator.appliesPreferredTrackTransform = true
            
            let time = CMTimeMake(value: 1, timescale: 1) // 第一帧的时间
            imageGenerator.generateCGImagesAsynchronously(forTimes: [NSValue(time: time)]) { _, cgImage, _, _, error in
                if let error = error {
                    print(error)
                } else if let cgImage = cgImage {
                    let nsImage = NSImage(cgImage: cgImage, size: .zero)
                    if let data = nsImage.tiffRepresentation {
                        do {
                            let url = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appending(path: "staticWP_\(wallpaper.wallpaperDirectory.hashValue).tiff")
                            try data.write(to: url, options: .atomic)
                            for screen in NSScreen.screens {
                                try NSWorkspace.shared.setDesktopImageURL(url, for: screen)
                            }
                        } catch {
                            print(error)
                        }
                    }
                }
            }
        default:
            return
        }
    }
}

/// Non-interactive window that stays behind all other windows.
class WallpaperWindow: NSWindow {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }

    func setWallpaperContent<Content: View>(_ rootView: Content) {
        let hostingView = NSHostingView(rootView: rootView)
        // Screen geometry owns the wallpaper window size. SwiftUI can
        // become empty during Stop, but must not collapse the window.
        hostingView.sizingOptions = []
        contentView = hostingView
    }
}

enum SettingsToolbarIdentifiers {
    static let performance = NSToolbarItem.Identifier(rawValue: "performance")
    static let general = NSToolbarItem.Identifier(rawValue: "general")
    static let plugins = NSToolbarItem.Identifier(rawValue: "plugins")
    static let about = NSToolbarItem.Identifier(rawValue: "about")
}
