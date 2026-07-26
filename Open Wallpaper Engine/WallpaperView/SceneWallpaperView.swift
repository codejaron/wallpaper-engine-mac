import Cocoa
import OpenGL.GL3
import SceneAudio
import SceneRuntimeBridge
import SwiftUI

enum ScenePresentationLayoutError: LocalizedError {
    case invalid(String)

    var errorDescription: String? {
        switch self {
        case .invalid(let message): return message
        }
    }
}

/// Presentation geometry shared by all Scene windows participating in one
/// virtual desktop. Coordinates are bottom-left-origin pixels at a common
/// reference scale; each window supplies its actual drawable backing size.
struct ScenePresentationLayout: Equatable {
    let spanAcrossScreens: Bool
    let scaling: GSScenePresentationScaling
    let canvasWidth: UInt32
    let canvasHeight: UInt32
    let viewportX: UInt32
    let viewportY: UInt32
    let viewportWidth: UInt32
    let viewportHeight: UInt32
    let validationError: String?

    init(
        spanAcrossScreens: Bool,
        scaling: GSScenePresentationScaling,
        canvasWidth: UInt32,
        canvasHeight: UInt32,
        viewportX: UInt32,
        viewportY: UInt32,
        viewportWidth: UInt32,
        viewportHeight: UInt32,
        validationError: String? = nil
    ) {
        self.spanAcrossScreens = spanAcrossScreens
        self.scaling = scaling
        self.canvasWidth = canvasWidth
        self.canvasHeight = canvasHeight
        self.viewportX = viewportX
        self.viewportY = viewportY
        self.viewportWidth = viewportWidth
        self.viewportHeight = viewportHeight
        self.validationError = validationError
    }

    @MainActor
    static func forScreen(
        _ screenId: String,
        wallpaper: WEWallpaper,
        spanAcrossScreens: Bool,
        scaling: GSScenePresentationScaling,
        wallpaperViewModel: WallpaperViewModel
    ) -> ScenePresentationLayout {
        guard spanAcrossScreens else {
            return ScenePresentationLayout(
                spanAcrossScreens: false,
                scaling: scaling,
                canvasWidth: 0,
                canvasHeight: 0,
                viewportX: 0,
                viewportY: 0,
                viewportWidth: 0,
                viewportHeight: 0
            )
        }
        guard let screen = NSScreen.screens.first(where: {
            WallpaperViewModel.screenId(for: $0) == screenId
        }) else {
            return invalidSpan(
                scaling: scaling,
                message: "Scene span layout cannot find display \(screenId)"
            )
        }

        let identity = wallpaper.scenePropertyIdentity
        let enabledIds = wallpaperViewModel.enabledScreens
        let screens = NSScreen.screens.filter { candidate in
            let candidateId = WallpaperViewModel.screenId(for: candidate)
            guard enabledIds.contains(candidateId),
                  wallpaperViewModel.wallpaper(for: candidateId)
                      .scenePropertyIdentity == identity else { return false }
            return true
        }
        guard !screens.isEmpty else {
            return invalidSpan(
                scaling: scaling,
                message: "Scene span layout has no enabled display for the current Scene"
            )
        }
        let referenceScale = screens
            .map(\.backingScaleFactor)
            .max() ?? screen.backingScaleFactor
        guard referenceScale.isFinite, referenceScale > 0 else {
            return invalidSpan(
                scaling: scaling,
                message: "Scene span layout has an invalid display scale"
            )
        }
        let union = screens
            .map(\.frame)
            .reduce(screens[0].frame) { $0.union($1) }

        let canvasMinX = floor(union.minX * referenceScale)
        let canvasMinY = floor(union.minY * referenceScale)
        let canvasMaxX = ceil(union.maxX * referenceScale)
        let canvasMaxY = ceil(union.maxY * referenceScale)
        let screenMinX = floor(screen.frame.minX * referenceScale)
        let screenMinY = floor(screen.frame.minY * referenceScale)
        let screenMaxX = ceil(screen.frame.maxX * referenceScale)
        let screenMaxY = ceil(screen.frame.maxY * referenceScale)

        do {
            let canvasWidth = try checkedDimension(
                canvasMaxX - canvasMinX,
                field: "virtual canvas width"
            )
            let canvasHeight = try checkedDimension(
                canvasMaxY - canvasMinY,
                field: "virtual canvas height"
            )
            let viewportX = try checkedOffset(
                screenMinX - canvasMinX,
                field: "viewport x"
            )
            let viewportY = try checkedOffset(
                screenMinY - canvasMinY,
                field: "viewport y"
            )
            let viewportWidth = try checkedDimension(
                screenMaxX - screenMinX,
                field: "viewport width"
            )
            let viewportHeight = try checkedDimension(
                screenMaxY - screenMinY,
                field: "viewport height"
            )
            guard UInt64(viewportX) + UInt64(viewportWidth) <= UInt64(canvasWidth),
                  UInt64(viewportY) + UInt64(viewportHeight) <= UInt64(canvasHeight) else {
                throw ScenePresentationLayoutError.invalid(
                    "Scene span viewport lies outside the virtual canvas"
                )
            }
            return ScenePresentationLayout(
                spanAcrossScreens: true,
                scaling: scaling,
                canvasWidth: canvasWidth,
                canvasHeight: canvasHeight,
                viewportX: viewportX,
                viewportY: viewportY,
                viewportWidth: viewportWidth,
                viewportHeight: viewportHeight
            )
        } catch {
            return invalidSpan(
                scaling: scaling,
                message: error.localizedDescription
            )
        }
    }

    private static func checkedDimension(
        _ value: CGFloat,
        field: String
    ) throws -> UInt32 {
        try checkedInteger(value, field: field, minimum: 1)
    }

    private static func checkedOffset(
        _ value: CGFloat,
        field: String
    ) throws -> UInt32 {
        try checkedInteger(value, field: field, minimum: 0)
    }

    private static func checkedInteger(
        _ value: CGFloat,
        field: String,
        minimum: CGFloat
    ) throws -> UInt32 {
        let integer = value.rounded(.towardZero)
        guard value.isFinite,
              integer >= minimum,
              integer <= CGFloat(Int32.max) else {
            throw ScenePresentationLayoutError.invalid(
                "Scene span \(field) is outside the supported OpenGL range: \(value)"
            )
        }
        return UInt32(integer)
    }

    private static func invalidSpan(
        scaling: GSScenePresentationScaling,
        message: String
    ) -> ScenePresentationLayout {
        ScenePresentationLayout(
            spanAcrossScreens: true,
            scaling: scaling,
            canvasWidth: 0,
            canvasHeight: 0,
            viewportX: 0,
            viewportY: 0,
            viewportWidth: 0,
            viewportHeight: 0,
            validationError: message
        )
    }

    var bridgeScaling: WEScenePresentationScaling {
        switch scaling {
        case .stretch: return WE_SCENE_PRESENTATION_STRETCH
        case .aspectFit: return WE_SCENE_PRESENTATION_ASPECT_FIT
        case .aspectFill: return WE_SCENE_PRESENTATION_ASPECT_FILL
        }
    }

    func viewport(
        drawableWidth: UInt32,
        drawableHeight: UInt32
    ) throws -> WEScenePresentationViewport? {
        if let validationError {
            throw ScenePresentationLayoutError.invalid(validationError)
        }
        guard spanAcrossScreens else { return nil }
        return WEScenePresentationViewport(
            virtual_canvas_width: canvasWidth,
            virtual_canvas_height: canvasHeight,
            viewport_x: viewportX,
            viewport_y: viewportY,
            viewport_width: viewportWidth,
            viewport_height: viewportHeight,
            drawable_width: drawableWidth,
            drawable_height: drawableHeight
        )
    }
}

/// Span-mode Scene views use one monotonic origin and one FPS-aligned time grid.
/// Per-screen presentation retains its independent accumulated runtime.
final class ScenePresentationClock {
    static let shared = ScenePresentationClock()
    private let origin = ProcessInfo.processInfo.systemUptime

    private init() {}

    func seconds(
        at uptime: TimeInterval = ProcessInfo.processInfo.systemUptime,
        framesPerSecond: Double
    ) -> Double {
        let elapsed = max(0, uptime - origin)
        return floor(elapsed * framesPerSecond) / framesPerSecond
    }
}

struct SceneWallpaperView: View {
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    @ObservedObject private var globalSettingsViewModel: GlobalSettingsViewModel
    @ObservedObject private var audioOwnerCoordinator: SceneAudioOwnerCoordinator
    @ObservedObject private var mediaSnapshotProvider: SceneMediaSnapshotProvider
    let screenId: String
    @State private var errorMessage: String?

    init(wallpaperViewModel: WallpaperViewModel, screenId: String) {
        self.wallpaperViewModel = wallpaperViewModel
        self.screenId = screenId
        globalSettingsViewModel = AppDelegate.shared.globalSettingsViewModel
        audioOwnerCoordinator = AppDelegate.shared.sceneAudioOwnerCoordinator
        mediaSnapshotProvider = AppDelegate.shared.sceneMediaSnapshotProvider
    }

    var body: some View {
        let wallpaper = wallpaperViewModel.wallpaper(for: screenId)
        let presentation = ScenePresentationLayout.forScreen(
            screenId,
            wallpaper: wallpaper,
            spanAcrossScreens: globalSettingsViewModel.settings.sceneSpanAcrossScreens,
            scaling: globalSettingsViewModel.settings.scenePresentationScaling,
            wallpaperViewModel: wallpaperViewModel
        )
        ZStack {
            SceneOpenGLRepresentable(
                wallpaper: wallpaper,
                presentation: presentation,
                paused: wallpaperViewModel.effectivePlayRate == 0,
                framesPerSecond: globalSettingsViewModel.settings.fps,
                masterVolume: wallpaperViewModel.effectivePlayVolume,
                audioOutputEnabled: globalSettingsViewModel.settings.audioOutput,
                systemAudioCaptureEnabled:
                    globalSettingsViewModel.settings.systemAudioCaptureEnabled,
                isAudibleOwner: audioOwnerCoordinator.isAudible(screenId: screenId),
                mediaSnapshot: mediaSnapshotProvider.snapshot,
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
    }

    private var resolvedAssetsDirectory: String? {
        let configured = globalSettingsViewModel.settings.wallpaperEngineAssetsDirectory?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return configured.flatMap { $0.isEmpty ? nil : $0 }
    }
}

private struct SceneOpenGLRepresentable: NSViewRepresentable {
    let wallpaper: WEWallpaper
    let presentation: ScenePresentationLayout
    let paused: Bool
    let framesPerSecond: Double
    let masterVolume: Float
    let audioOutputEnabled: Bool
    let systemAudioCaptureEnabled: Bool
    let isAudibleOwner: Bool
    let mediaSnapshot: SceneMediaProviderSnapshot
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
        view.setSystemAudioCaptureEnabled(systemAudioCaptureEnabled)
        view.configure(
            wallpaper: wallpaper,
            assetsDirectory: assetsDirectory,
            propertyOverrides: propertyOverrides
        )
        view.setPresentation(presentation)
        view.setPaused(paused)
        view.setAudioConfiguration(
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
        view.setMediaSnapshot(mediaSnapshot)
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
        view.setSystemAudioCaptureEnabled(systemAudioCaptureEnabled)
        view.configure(
            wallpaper: wallpaper,
            assetsDirectory: assetsDirectory,
            propertyOverrides: propertyOverrides
        )
        view.setPresentation(presentation)
        view.setPaused(paused)
        view.setAudioConfiguration(
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
        view.setMediaSnapshot(mediaSnapshot)
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

    func setPresentation(_ value: ScenePresentationLayout) {
        openGLView?.setPresentation(value)
    }

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

    func setSystemAudioCaptureEnabled(_ enabled: Bool) {
        openGLView?.setSystemAudioCaptureEnabled(enabled)
    }

    func setMediaSnapshot(_ value: SceneMediaProviderSnapshot) {
        openGLView?.setMediaSnapshot(value)
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
    private var presentation = ScenePresentationLayout(
        spanAcrossScreens: false,
        scaling: .aspectFill,
        canvasWidth: 0,
        canvasHeight: 0,
        viewportX: 0,
        viewportY: 0,
        viewportWidth: 0,
        viewportHeight: 0
    )
    private var propertyConfigurationValid = false
    private var timer: Timer?
    private var isOpenGLPrepared = false
    private var hasRenderedFrame = false
    private var paused = false
    private var framesPerSecond = 30.0
    private var masterVolume: Float = 1
    private var audioOutputEnabled = true
    private var systemAudioCaptureEnabled = false
    private var isAudibleOwner = false
    private var mediaSnapshot: SceneMediaProviderSnapshot =
        .unavailable(revisions: .zero)
    private var runtimeSeconds = 0.0
    private var previousTimestamp: TimeInterval?
    private var previousSpanRuntimeSeconds: Double?
    private var audioCaptureLease: SceneAudioCaptureLease?
    private var audioCaptureTask: Task<Void, Never>?
    private var audioCaptureGeneration = 0
    private var audioCaptureIssue: String?
    private var sessionRetryTask: Task<Void, Never>?
    private var sessionRetryAttempt = 0
    private var hasReportedFatalIssue = false
    private var lastReportedFatalIssue: String?
    private var lastReportedAudioIssue: String?

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
        hasReportedFatalIssue = false
        lastReportedFatalIssue = nil
        lastReportedAudioIssue = nil
        onPropertiesLoaded?(nil)
        sessionRetryAttempt = 0
        attemptedIdentity = nil
        activeIdentity = nil
        closeSession()
        propertyConfigurationValid = false
        hasRenderedFrame = false
        runtimeSeconds = 0
        previousTimestamp = nil
        previousSpanRuntimeSeconds = nil
        audioCaptureIssue = nil
        clearDrawableIfAvailable()
        reconcileSession()
        updateTimer()
    }

    func setPaused(_ value: Bool) {
        guard paused != value else { return }
        session?.setPaused(value)
        paused = value
        previousTimestamp = nil
        previousSpanRuntimeSeconds = nil
        reportAudioIssue()
        updateTimer()
    }

    func setPresentation(_ value: ScenePresentationLayout) {
        guard presentation != value else { return }
        if presentation.spanAcrossScreens != value.spanAcrossScreens {
            previousTimestamp = nil
            previousSpanRuntimeSeconds = nil
        }
        presentation = value
        needsDisplay = true
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
        session?.updateAudioConfiguration(
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
        reportAudioIssue()
    }

    func setSystemAudioCaptureEnabled(_ enabled: Bool) {
        guard systemAudioCaptureEnabled != enabled else { return }
        systemAudioCaptureEnabled = enabled
        audioCaptureIssue = nil
        if !enabled {
            cancelAudioCapture()
        } else if let session, session.requiresAudioSpectrum {
            beginAudioCapture(for: session)
        }
        reportAudioIssue()
        needsDisplay = true
    }

    func setMediaSnapshot(_ value: SceneMediaProviderSnapshot) {
        guard mediaSnapshot != value else { return }
        mediaSnapshot = value
        // Media callbacks are part of script evaluation. Force one evaluated
        // frame even while presentation is paused; replay alone cannot deliver
        // a new revision.
        hasRenderedFrame = false
        needsDisplay = true
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
            previousSpanRuntimeSeconds = nil
            reportFatalIssue(nil)
            reportAudioIssue()
            needsDisplay = true
            updateTimer()
        } catch {
            propertyConfigurationValid = false
            hasRenderedFrame = false
            updateTimer()
            clearDrawableIfAvailable()
            reportFatalIssue(error.localizedDescription)
        }
    }

    func setFramesPerSecond(_ value: Double) {
        let clamped = min(max(value, 1), 240)
        guard framesPerSecond != clamped else { return }
        framesPerSecond = clamped
        previousSpanRuntimeSeconds = nil
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
        if let validationError = presentation.validationError {
            clearDrawableIfAvailable()
            reportFatalIssue(validationError)
            return
        }
        do {
            let width = UInt32(backing.width.rounded())
            let height = UInt32(backing.height.rounded())
            if paused && hasRenderedFrame {
                try session.replayLastEvaluatedFrame(
                    drawableWidth: width,
                    drawableHeight: height,
                    presentation: presentation
                )
            } else {
                let now = ProcessInfo.processInfo.systemUptime
                let frameTime: Double
                let presentedRuntimeSeconds: Double
                if presentation.spanAcrossScreens {
                    let sharedRuntimeSeconds = ScenePresentationClock.shared.seconds(
                        at: now,
                        framesPerSecond: framesPerSecond
                    )
                    frameTime = paused ? 0 : min(max(
                        previousSpanRuntimeSeconds.map {
                            sharedRuntimeSeconds - $0
                        } ?? 0,
                        0
                    ), 0.25)
                    previousSpanRuntimeSeconds = paused ? nil : sharedRuntimeSeconds
                    presentedRuntimeSeconds = sharedRuntimeSeconds
                } else {
                    frameTime = paused ? 0 : min(max(
                        previousTimestamp.map { now - $0 } ?? 0,
                        0
                    ), 0.25)
                    previousTimestamp = paused ? nil : now
                    runtimeSeconds += frameTime
                    presentedRuntimeSeconds = runtimeSeconds
                }
                let pointer = normalizedDrawablePointer()
                let pointerState = sampledDesktopPointerState()
                try session.render(
                    runtimeSeconds: presentedRuntimeSeconds,
                    frameTimeSeconds: frameTime,
                    pointerX: pointer.x,
                    pointerY: pointer.y,
                    pointerActive: pointerState.active,
                    pointerLeftDown: pointerState.leftDown,
                    mediaSnapshot: mediaSnapshot,
                    drawableWidth: width,
                    drawableHeight: height,
                    presentation: presentation,
                    masterVolume: masterVolume,
                    audioOutputEnabled: audioOutputEnabled,
                    systemAudioCaptureEnabled: systemAudioCaptureEnabled,
                    isAudibleOwner: isAudibleOwner
                )
                hasRenderedFrame = true
            }
            context.flushBuffer()
            sessionRetryAttempt = 0
            reportFatalIssue(nil)
            reportAudioIssue()
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
        runtimeSeconds = 0
        previousTimestamp = nil
        previousSpanRuntimeSeconds = nil
        audioCaptureIssue = nil
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
        previousSpanRuntimeSeconds = nil
        audioCaptureIssue = nil
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

    private func sampledDesktopPointerState() -> (active: Bool, leftDown: Bool) {
        let leftDown = (NSEvent.pressedMouseButtons & 1) != 0
        guard NSWorkspace.shared.frontmostApplication?.bundleIdentifier == "com.apple.finder",
              let window,
              window.isVisible,
              window.frame.contains(NSEvent.mouseLocation) else {
            return (false, leftDown)
        }
        return (true, leftDown)
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
                cglContext: context,
                // This view is the desktop-wallpaper host. A future native
                // screensaver host passes true through the same session/API;
                // the script runtime never guesses the mode.
                isScreensaver: false
            )
        } catch {
            closeSession()
            activeIdentity = nil
            attemptedIdentity = nil
            propertyConfigurationValid = false
            hasRenderedFrame = false
            reportFatalIssue(error.localizedDescription)
            onPropertiesLoaded?(nil)
            clearDrawableIfAvailable()
            scheduleSessionRetry()
            return
        }

        newSession.setPaused(paused)
        session = newSession
        activeIdentity = identity
        hasRenderedFrame = false
        propertyConfigurationValid = false
        audioCaptureIssue = nil
        onPropertiesLoaded?(newSession.properties)
        if systemAudioCaptureEnabled && newSession.requiresAudioSpectrum {
            beginAudioCapture(for: newSession)
        }
        do {
            try newSession.applyPropertyOverrides(requestedPropertyOverrides)
            propertyConfigurationValid = true
            NSLog("[Scene] Runtime session ready: %@", identity.scene.path)
            reportFatalIssue(nil)
            reportAudioIssue()
            needsDisplay = true
        } catch {
            propertyConfigurationValid = false
            hasRenderedFrame = false
            reportFatalIssue(error.localizedDescription)
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
        sessionRetryTask?.cancel()
        sessionRetryTask = nil
        cancelAudioCapture()
        guard let session else { return }
        openGLContext?.makeCurrentContext()
        session.close()
        self.session = nil
    }

    private func failSession(_ message: String) {
        closeSession()
        activeIdentity = nil
        attemptedIdentity = nil
        propertyConfigurationValid = false
        hasRenderedFrame = false
        audioCaptureIssue = nil
        onPropertiesLoaded?(nil)
        updateTimer()
        clearDrawableIfAvailable()
        reportFatalIssue(message)
        scheduleSessionRetry()
    }

    private func beginAudioCapture(for targetSession: SceneRuntimeSession) {
        cancelAudioCapture()
        audioCaptureGeneration &+= 1
        let generation = audioCaptureGeneration
        audioCaptureTask = Task { @MainActor [weak self, targetSession] in
            guard let self,
                  self.session === targetSession,
                  self.audioCaptureGeneration == generation,
                  self.systemAudioCaptureEnabled else { return }
            do {
                let lease = try await SceneSystemAudioSpectrumProvider.shared.acquire()
                guard self.session === targetSession,
                      self.audioCaptureGeneration == generation,
                      self.systemAudioCaptureEnabled,
                      !Task.isCancelled else {
                    lease.cancel()
                    return
                }
                self.audioCaptureLease?.cancel()
                self.audioCaptureLease = lease
                self.audioCaptureTask = nil
                self.audioCaptureIssue = nil
                self.reportAudioIssue()
                self.needsDisplay = true
            } catch is CancellationError {
                return
            } catch {
                guard self.session === targetSession,
                      self.audioCaptureGeneration == generation,
                      self.systemAudioCaptureEnabled else { return }
                self.audioCaptureTask = nil
                self.audioCaptureIssue =
                    "System audio capture unavailable: \(error.localizedDescription)"
                self.reportAudioIssue()
                self.needsDisplay = true
            }
        }
    }

    private func cancelAudioCapture() {
        audioCaptureGeneration &+= 1
        audioCaptureTask?.cancel()
        audioCaptureTask = nil
        audioCaptureLease?.cancel()
        audioCaptureLease = nil
    }

    private func scheduleSessionRetry() {
        guard isOpenGLPrepared,
              window != nil,
              requestedIdentity != nil,
              activeIdentity == nil else { return }
        sessionRetryTask?.cancel()
        sessionRetryAttempt &+= 1
        let shift = min(max(sessionRetryAttempt - 1, 0), 5)
        let delay = min(UInt64(250_000_000) << UInt64(shift), 8_000_000_000)
        let identity = requestedIdentity
        sessionRetryTask = Task { @MainActor [weak self] in
            do {
                try await Task.sleep(nanoseconds: delay)
            } catch {
                return
            }
            guard let self,
                  self.requestedIdentity == identity,
                  self.activeIdentity == nil else { return }
            self.sessionRetryTask = nil
            self.reconcileSession()
        }
    }

    private func reportFatalIssue(_ message: String?) {
        guard !hasReportedFatalIssue || lastReportedFatalIssue != message else { return }
        hasReportedFatalIssue = true
        lastReportedFatalIssue = message
        if let message { NSLog("[Scene] %@", message) }
        onError?(message)
    }

    private func reportAudioIssue() {
        let providerIssue: String?
        if systemAudioCaptureEnabled,
           session?.requiresAudioSpectrum == true,
           case .unavailable(let detail) =
               SceneSystemAudioSpectrumProvider.shared.status {
            providerIssue = "System audio capture unavailable: \(detail)"
        } else {
            providerIssue = nil
        }
        let message = [
            session?.audioPlaybackIssue,
            audioCaptureIssue ?? providerIssue,
        ]
            .compactMap { $0 }
            .joined(separator: "\n")
        let effectiveMessage = message.isEmpty ? nil : message
        guard lastReportedAudioIssue != effectiveMessage else { return }
        lastReportedAudioIssue = effectiveMessage
        if let effectiveMessage { NSLog("[Scene] %@", effectiveMessage) }
    }
}
