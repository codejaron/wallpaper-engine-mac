import Cocoa
import OpenGL.GL3
import SceneAudio
import SwiftUI

struct SceneWallpaperView: View {
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    @ObservedObject private var globalSettingsViewModel: GlobalSettingsViewModel
    @ObservedObject private var audioOwnerCoordinator: SceneAudioOwnerCoordinator
    let screenId: String
    @State private var errorMessage: String?

    init(wallpaperViewModel: WallpaperViewModel, screenId: String) {
        self.wallpaperViewModel = wallpaperViewModel
        self.screenId = screenId
        globalSettingsViewModel = AppDelegate.shared.globalSettingsViewModel
        audioOwnerCoordinator = AppDelegate.shared.sceneAudioOwnerCoordinator
    }

    var body: some View {
        let wallpaper = wallpaperViewModel.wallpaper(for: screenId)
        ZStack {
            SceneOpenGLRepresentable(
                wallpaper: wallpaper,
                paused: wallpaperViewModel.playRate == 0,
                framesPerSecond: globalSettingsViewModel.settings.fps,
                masterVolume: wallpaperViewModel.playVolume,
                audioOutputEnabled: globalSettingsViewModel.settings.audioOutput,
                isAudibleOwner: audioOwnerCoordinator.isAudible(screenId: screenId),
                assetsDirectory: resolvedAssetsDirectory,
                propertyOverrides: wallpaperViewModel.scenePropertyOverrides(
                    for: screenId,
                    wallpaper: wallpaper
                ),
                onPropertiesLoaded: { properties in
                    DispatchQueue.main.async {
                        if let properties {
                            wallpaperViewModel.registerScenePropertyCatalog(
                                properties,
                                for: screenId,
                                wallpaper: wallpaper
                            )
                        } else {
                            wallpaperViewModel.clearScenePropertyCatalog(
                                for: screenId,
                                wallpaper: wallpaper
                            )
                        }
                    }
                },
                onRuntimeError: { message in
                    DispatchQueue.main.async {
                        wallpaperViewModel.setSceneRuntimeError(
                            message,
                            for: screenId,
                            wallpaper: wallpaper
                        )
                    }
                },
                errorMessage: $errorMessage
            )
            if let errorMessage {
                Color.black
                Text(errorMessage)
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
                    .padding(24)
            }
        }
        .onAppear { audioOwnerCoordinator.register(screenId: screenId) }
        .onDisappear { audioOwnerCoordinator.unregister(screenId: screenId) }
    }

    private var resolvedAssetsDirectory: String? {
        let configured = globalSettingsViewModel.settings.wallpaperEngineAssetsDirectory?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if let configured, !configured.isEmpty { return configured }
        let environment = ProcessInfo.processInfo.environment["WE_ASSETS_DIR"]?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return environment.flatMap { $0.isEmpty ? nil : $0 }
    }
}

private struct SceneOpenGLRepresentable: NSViewRepresentable {
    let wallpaper: WEWallpaper
    let paused: Bool
    let framesPerSecond: Double
    let masterVolume: Float
    let audioOutputEnabled: Bool
    let isAudibleOwner: Bool
    let assetsDirectory: String?
    let propertyOverrides: [String: ScenePropertyValue]
    let onPropertiesLoaded: ([ScenePropertyDefinition]?) -> Void
    let onRuntimeError: (String?) -> Void
    @Binding var errorMessage: String?

    func makeNSView(context: Context) -> SceneOpenGLContainerView {
        let view = SceneOpenGLContainerView(frame: .zero)
        view.onError = { message in
            DispatchQueue.main.async {
                errorMessage = message
                onRuntimeError(message)
            }
        }
        view.onPropertiesLoaded = onPropertiesLoaded
        view.configure(
            wallpaper: wallpaper,
            assetsDirectory: assetsDirectory,
            propertyOverrides: propertyOverrides
        )
        view.setPaused(paused)
        view.setAudioConfiguration(
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
        view.setFramesPerSecond(framesPerSecond)
        return view
    }

    func updateNSView(_ view: SceneOpenGLContainerView, context: Context) {
        view.onError = { message in
            DispatchQueue.main.async {
                errorMessage = message
                onRuntimeError(message)
            }
        }
        view.onPropertiesLoaded = onPropertiesLoaded
        view.configure(
            wallpaper: wallpaper,
            assetsDirectory: assetsDirectory,
            propertyOverrides: propertyOverrides
        )
        view.setPaused(paused)
        view.setAudioConfiguration(
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
        view.setFramesPerSecond(framesPerSecond)
    }

    static func dismantleNSView(_ view: SceneOpenGLContainerView, coordinator: ()) {
        view.shutdown()
    }
}

private final class SceneOpenGLContainerView: NSView {
    var onError: ((String?) -> Void)? {
        didSet { openGLView?.onError = onError }
    }
    var onPropertiesLoaded: (([ScenePropertyDefinition]?) -> Void)? {
        didSet { openGLView?.onPropertiesLoaded = onPropertiesLoaded }
    }

    private let openGLView: SceneOpenGLView?

    override init(frame frameRect: NSRect) {
        var attributes: [NSOpenGLPixelFormatAttribute] = [
            NSOpenGLPixelFormatAttribute(NSOpenGLPFAOpenGLProfile),
            NSOpenGLPixelFormatAttribute(NSOpenGLProfileVersion4_1Core),
            NSOpenGLPixelFormatAttribute(NSOpenGLPFADoubleBuffer),
            NSOpenGLPixelFormatAttribute(NSOpenGLPFAAccelerated),
            NSOpenGLPixelFormatAttribute(NSOpenGLPFAColorSize), 24,
            NSOpenGLPixelFormatAttribute(NSOpenGLPFAAlphaSize), 8,
            0,
        ]
        if let format = NSOpenGLPixelFormat(attributes: &attributes) {
            openGLView = SceneOpenGLView(frame: frameRect, pixelFormat: format)
        } else {
            openGLView = nil
        }
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.clear.cgColor
        if let openGLView {
            openGLView.autoresizingMask = [.width, .height]
            addSubview(openGLView)
        } else {
            DispatchQueue.main.async { [weak self] in
                self?.report("macOS did not provide the required OpenGL 4.1 core pixel format")
            }
        }
    }

    required init?(coder: NSCoder) { nil }

    func configure(
        wallpaper: WEWallpaper,
        assetsDirectory: String?,
        propertyOverrides: [String: ScenePropertyValue]
    ) {
        openGLView?.configure(
            wallpaper: wallpaper,
            assetsDirectory: assetsDirectory,
            propertyOverrides: propertyOverrides
        )
    }

    func setPaused(_ value: Bool) { openGLView?.setPaused(value) }

    func setAudioConfiguration(
        masterVolume: Float,
        audioOutputEnabled: Bool,
        isAudibleOwner: Bool
    ) {
        openGLView?.setAudioConfiguration(
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
    }

    func setFramesPerSecond(_ value: Double) { openGLView?.setFramesPerSecond(value) }

    func shutdown() { openGLView?.shutdown() }

    private func report(_ message: String) {
        NSLog("[Scene] %@", message)
        onError?(message)
    }
}

private final class SceneOpenGLView: NSOpenGLView {
    private struct SessionIdentity: Equatable {
        let scene: URL
        let assetsDirectory: String?
    }

    var onError: ((String?) -> Void)?
    var onPropertiesLoaded: (([ScenePropertyDefinition]?) -> Void)?

    private var session: SceneRuntimeSession?
    private var requestedWallpaper: WEWallpaper?
    private var requestedIdentity: SessionIdentity?
    private var activeIdentity: SessionIdentity?
    private var attemptedIdentity: SessionIdentity?
    private var requestedPropertyOverrides: [String: ScenePropertyValue] = [:]
    private var propertyConfigurationValid = false
    private var timer: Timer?
    private var isOpenGLPrepared = false
    private var hasRenderedFrame = false
    private var paused = false
    private var framesPerSecond = 30.0
    private var masterVolume: Float = 1
    private var audioOutputEnabled = true
    private var isAudibleOwner = false
    private var runtimeSeconds = 0.0
    private var previousTimestamp: TimeInterval?

    init?(frame frameRect: NSRect, pixelFormat: NSOpenGLPixelFormat) {
        super.init(frame: frameRect, pixelFormat: pixelFormat)
    }

    required init?(coder: NSCoder) { super.init(coder: coder) }

    override func prepareOpenGL() {
        super.prepareOpenGL()
        openGLContext?.makeCurrentContext()
        var swapInterval: GLint = 1
        openGLContext?.setValues(&swapInterval, for: .swapInterval)
        wantsBestResolutionOpenGLSurface = true
        isOpenGLPrepared = true
        reconcileSession()
        updateTimer()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil { reconcileSession() }
        updateTimer()
    }

    func configure(
        wallpaper: WEWallpaper,
        assetsDirectory: String?,
        propertyOverrides: [String: ScenePropertyValue]
    ) {
        let identity = SessionIdentity(
            scene: wallpaper.wallpaperDirectory.appending(path: wallpaper.project.file),
            assetsDirectory: assetsDirectory
        )
        guard identity != requestedIdentity else {
            setPropertyOverrides(propertyOverrides)
            return
        }
        requestedWallpaper = wallpaper
        requestedIdentity = identity
        requestedPropertyOverrides = propertyOverrides
        onPropertiesLoaded?(nil)
        attemptedIdentity = nil
        activeIdentity = nil
        closeSession()
        propertyConfigurationValid = false
        hasRenderedFrame = false
        runtimeSeconds = 0
        previousTimestamp = nil
        clearDrawableIfAvailable()
        reconcileSession()
        updateTimer()
    }

    func setPaused(_ value: Bool) {
        guard paused != value else { return }
        do {
            try session?.setPaused(value)
            paused = value
            previousTimestamp = nil
            updateTimer()
        } catch {
            failSession(error.localizedDescription)
        }
    }

    func setAudioConfiguration(
        masterVolume: Float,
        audioOutputEnabled: Bool,
        isAudibleOwner: Bool
    ) {
        guard self.masterVolume != masterVolume ||
                self.audioOutputEnabled != audioOutputEnabled ||
                self.isAudibleOwner != isAudibleOwner else { return }
        self.masterVolume = masterVolume
        self.audioOutputEnabled = audioOutputEnabled
        self.isAudibleOwner = isAudibleOwner
        do {
            try session?.updateAudioConfiguration(
                masterVolume: masterVolume,
                audioOutputEnabled: audioOutputEnabled,
                isAudibleOwner: isAudibleOwner
            )
        } catch {
            failSession(error.localizedDescription)
        }
    }

    func setPropertyOverrides(_ values: [String: ScenePropertyValue]) {
        if requestedPropertyOverrides == values {
            return
        }
        requestedPropertyOverrides = values
        guard let session else {
            reconcileSession()
            updateTimer()
            return
        }
        applyPropertyOverrides(values, to: session)
    }

    private func applyPropertyOverrides(
        _ values: [String: ScenePropertyValue],
        to session: SceneRuntimeSession
    ) {
        do {
            try session.applyPropertyOverrides(values)
            propertyConfigurationValid = true
            hasRenderedFrame = false
            previousTimestamp = nil
            report(nil)
            needsDisplay = true
            updateTimer()
        } catch {
            propertyConfigurationValid = false
            hasRenderedFrame = false
            updateTimer()
            clearDrawableIfAvailable()
            report(error.localizedDescription)
        }
    }

    func setFramesPerSecond(_ value: Double) {
        let clamped = min(max(value, 1), 240)
        guard framesPerSecond != clamped else { return }
        framesPerSecond = clamped
        updateTimer()
    }

    override func draw(_ dirtyRect: NSRect) {
        guard propertyConfigurationValid,
              let session,
              let context = openGLContext else {
            clearDrawableIfAvailable()
            return
        }
        context.makeCurrentContext()
        let backing = convertToBacking(bounds).size
        guard backing.width >= 1, backing.height >= 1 else { return }
        do {
            let width = UInt32(backing.width.rounded())
            let height = UInt32(backing.height.rounded())
            if paused && hasRenderedFrame {
                try session.replayLastEvaluatedFrame(
                    drawableWidth: width,
                    drawableHeight: height
                )
            } else {
                let now = ProcessInfo.processInfo.systemUptime
                let frameTime = paused ? 0 : min(max(previousTimestamp.map { now - $0 } ?? 0, 0), 0.25)
                previousTimestamp = paused ? nil : now
                runtimeSeconds += frameTime
                let pointer = normalizedDrawablePointer()
                try session.render(
                    runtimeSeconds: runtimeSeconds,
                    frameTimeSeconds: frameTime,
                    pointerX: pointer.x,
                    pointerY: pointer.y,
                    drawableWidth: width,
                    drawableHeight: height,
                    masterVolume: masterVolume,
                    audioOutputEnabled: audioOutputEnabled,
                    isAudibleOwner: isAudibleOwner
                )
                hasRenderedFrame = true
            }
            context.flushBuffer()
            report(nil)
        } catch {
            failSession(error.localizedDescription)
        }
    }

    override func reshape() {
        super.reshape()
        needsDisplay = true
    }

    func shutdown() {
        timer?.invalidate()
        timer = nil
        closeSession()
        activeIdentity = nil
        attemptedIdentity = nil
        requestedIdentity = nil
        requestedWallpaper = nil
        requestedPropertyOverrides = [:]
        propertyConfigurationValid = false
        hasRenderedFrame = false
        clearDrawableIfAvailable()
        onPropertiesLoaded?(nil)
    }

    override func clearGLContext() {
        timer?.invalidate()
        timer = nil
        closeSession()
        activeIdentity = nil
        attemptedIdentity = nil
        propertyConfigurationValid = false
        hasRenderedFrame = false
        runtimeSeconds = 0
        previousTimestamp = nil
        isOpenGLPrepared = false
        super.clearGLContext()
    }

    deinit {
        timer?.invalidate()
        closeSession()
    }

    private func updateTimer() {
        timer?.invalidate()
        timer = nil
        guard isOpenGLPrepared,
              window != nil,
              session != nil,
              propertyConfigurationValid,
              !paused else { return }
        let timer = Timer(timeInterval: 1.0 / framesPerSecond, repeats: true) { [weak self] _ in
            self?.needsDisplay = true
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    private func normalizedDrawablePointer() -> CGPoint {
        guard let window else { return CGPoint(x: 0.5, y: 0.5) }
        let windowPoint = window.convertPoint(fromScreen: NSEvent.mouseLocation)
        let point = convert(windowPoint, from: nil)
        guard bounds.width > 0, bounds.height > 0 else { return CGPoint(x: 0.5, y: 0.5) }
        return CGPoint(
            x: min(max(point.x / bounds.width, 0), 1),
            y: min(max(point.y / bounds.height, 0), 1)
        )
    }

    private func reconcileSession() {
        guard isOpenGLPrepared,
              window != nil,
              let wallpaper = requestedWallpaper,
              let identity = requestedIdentity,
              identity != activeIdentity,
              identity != attemptedIdentity,
              let context = openGLContext?.cglContextObj else { return }
        openGLContext?.makeCurrentContext()
        attemptedIdentity = identity
        let newSession: SceneRuntimeSession
        do {
            newSession = try SceneRuntimeSession(
                wallpaper: wallpaper,
                assetsDirectory: identity.assetsDirectory,
                cglContext: context
            )
        } catch {
            closeSession()
            activeIdentity = nil
            propertyConfigurationValid = false
            hasRenderedFrame = false
            report(error.localizedDescription)
            onPropertiesLoaded?(nil)
            clearDrawableIfAvailable()
            return
        }

        do {
            try newSession.setPaused(paused)
        } catch {
            newSession.close()
            activeIdentity = nil
            propertyConfigurationValid = false
            hasRenderedFrame = false
            report(error.localizedDescription)
            onPropertiesLoaded?(nil)
            clearDrawableIfAvailable()
            return
        }
        session = newSession
        activeIdentity = identity
        hasRenderedFrame = false
        propertyConfigurationValid = false
        onPropertiesLoaded?(newSession.properties)
        do {
            try newSession.applyPropertyOverrides(requestedPropertyOverrides)
            propertyConfigurationValid = true
            NSLog("[Scene] Runtime session ready: %@", identity.scene.path)
            report(nil)
            needsDisplay = true
        } catch {
            propertyConfigurationValid = false
            hasRenderedFrame = false
            report(error.localizedDescription)
            clearDrawableIfAvailable()
        }
    }

    private func clearDrawableIfAvailable() {
        guard isOpenGLPrepared, window != nil, let context = openGLContext else { return }
        let backing = convertToBacking(bounds).size
        guard backing.width >= 1, backing.height >= 1 else { return }
        context.makeCurrentContext()
        glBindFramebuffer(GLenum(GL_FRAMEBUFFER), 0)
        glClearColor(0, 0, 0, 0)
        glClear(GLbitfield(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT))
        context.flushBuffer()
    }

    private func closeSession() {
        guard let session else { return }
        openGLContext?.makeCurrentContext()
        session.close()
        self.session = nil
    }

    private func failSession(_ message: String) {
        closeSession()
        activeIdentity = nil
        propertyConfigurationValid = false
        hasRenderedFrame = false
        onPropertiesLoaded?(nil)
        updateTimer()
        clearDrawableIfAvailable()
        report(message)
    }

    private func report(_ message: String?) {
        if let message { NSLog("[Scene] %@", message) }
        onError?(message)
    }
}
