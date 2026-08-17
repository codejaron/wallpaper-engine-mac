import Cocoa
import MetalKit
import QuartzCore
import SceneAudio
import SceneRuntimeBridge
import SwiftUI

enum SceneFrameTrace {
    static let enabled = ProcessInfo.processInfo.environment["WE_SCENE_FRAME_TRACE"] == "1"

    static func log(_ message: @autoclosure () -> String) {
        guard enabled else { return }
        NSLog("[SceneFrameTrace] %@", message())
    }
}

private struct SceneDrawFrameStats {
    private static let enabled =
        ProcessInfo.processInfo.environment["WE_SCENE_FRAME_STATS"] == "1"

    private var frames: UInt64 = 0
    private var deltaSum = 0.0
    private var renderSum = 0.0
    private var renderMaximum = 0.0
    private var flushSum = 0.0
    private var flushMaximum = 0.0
    private var totalSum = 0.0
    private var totalMaximum = 0.0
    private var targetLagMaximum = -Double.infinity
    private var totalOver16: UInt64 = 0
    private var totalOver25: UInt64 = 0
    private var lastDisplaySequence: UInt64?
    private var skippedDisplayCallbacks: UInt64 = 0

    mutating func record(
        displaySequence: UInt64?,
        deltaSeconds: Double,
        renderMilliseconds: Double,
        flushMilliseconds: Double,
        totalMilliseconds: Double,
        targetLagMilliseconds: Double
    ) {
        guard Self.enabled else { return }
        frames &+= 1
        deltaSum += deltaSeconds
        renderSum += renderMilliseconds
        renderMaximum = max(renderMaximum, renderMilliseconds)
        flushSum += flushMilliseconds
        flushMaximum = max(flushMaximum, flushMilliseconds)
        totalSum += totalMilliseconds
        totalMaximum = max(totalMaximum, totalMilliseconds)
        targetLagMaximum = max(targetLagMaximum, targetLagMilliseconds)
        if totalMilliseconds > 16.6667 { totalOver16 &+= 1 }
        if totalMilliseconds > 25 { totalOver25 &+= 1 }
        if let displaySequence {
            if let lastDisplaySequence,
               displaySequence > lastDisplaySequence + 1 {
                skippedDisplayCallbacks &+=
                    displaySequence - lastDisplaySequence - 1
            }
            lastDisplaySequence = displaySequence
        }
        guard deltaSum >= 2, frames > 0 else { return }

        let message = String(
            format: "[SceneFrameStats] draw frames=%llu deltaAvgMs=%.3f "
                + "renderAvgMs=%.3f renderMaxMs=%.3f flushAvgMs=%.3f "
                + "flushMaxMs=%.3f totalAvgMs=%.3f totalMaxMs=%.3f "
                + "totalOver16=%llu totalOver25=%llu displaySkips=%llu "
                + "targetLagMaxMs=%.3f",
            CUnsignedLongLong(frames),
            deltaSum * 1_000 / Double(frames),
            renderSum / Double(frames),
            renderMaximum,
            flushSum / Double(frames),
            flushMaximum,
            totalSum / Double(frames),
            totalMaximum,
            CUnsignedLongLong(totalOver16),
            CUnsignedLongLong(totalOver25),
            CUnsignedLongLong(skippedDisplayCallbacks),
            targetLagMaximum
        )
        NSLog("%@", message)
        self = SceneDrawFrameStats()
    }
}

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
              integer <= CGFloat(UInt32.max) else {
            throw ScenePresentationLayoutError.invalid(
                "Scene span \(field) is outside the supported Metal range: \(value)"
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
        case .automatic: return WE_SCENE_PRESENTATION_AUTOMATIC
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

    /// Returns the fixed backing-pixel budget for Scene framebuffer rendering.
    /// The runtime applies the cross-display 1080p ceiling for Balanced.
    /// The caller supplies NSView.convertToBacking(bounds), never point sizes.
    /// Span participants use the same virtual-canvas dimensions so separate
    /// display windows cannot choose divergent full-scene render sizes.
    func physicalRenderTarget(
        drawableWidth: UInt32,
        drawableHeight: UInt32,
        quality: GSSceneRenderQuality
    ) throws -> WEScenePhysicalRenderTarget? {
        let bridgeQuality: WEScenePhysicalRenderQuality
        switch quality {
        case .high:
            return nil
        case .ultra:
            bridgeQuality = WE_SCENE_PHYSICAL_RENDER_ULTRA
        case .balanced:
            bridgeQuality = WE_SCENE_PHYSICAL_RENDER_BALANCED
        case .powerSaving:
            bridgeQuality = WE_SCENE_PHYSICAL_RENDER_POWER_SAVING
        }
        if let validationError {
            throw ScenePresentationLayoutError.invalid(validationError)
        }
        let backingWidth = spanAcrossScreens ? canvasWidth : drawableWidth
        let backingHeight = spanAcrossScreens ? canvasHeight : drawableHeight
        guard backingWidth > 0, backingHeight > 0 else {
            throw ScenePresentationLayoutError.invalid(
                "Scene render target backing dimensions must be non-zero"
            )
        }
        return WEScenePhysicalRenderTarget(
            backing_width: backingWidth,
            backing_height: backingHeight,
            quality: bridgeQuality
        )
    }
}

/// Span-mode Scene views use one monotonic origin and one FPS-aligned time grid.
/// Per-screen presentation retains its independent accumulated runtime.
final class ScenePresentationClock {
    static let shared = ScenePresentationClock()
    private let origin = CACurrentMediaTime()

    private init() {}

    func seconds(
        at uptime: TimeInterval = CACurrentMediaTime(),
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
            SceneMetalRepresentable(
                wallpaper: wallpaper,
                localStorageScreenIdentity:
                    WallpaperViewModel.scenePropertyScreenIdentity(screenId),
                presentation: presentation,
                renderQuality: globalSettingsViewModel.settings.sceneRenderQuality,
                playbackPaused: wallpaperViewModel.effectivePlayRate == 0,
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

private struct SceneMetalRepresentable: NSViewRepresentable {
    let wallpaper: WEWallpaper
    let localStorageScreenIdentity: String
    let presentation: ScenePresentationLayout
    let renderQuality: GSSceneRenderQuality
    let playbackPaused: Bool
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

    func makeNSView(context: Context) -> SceneMetalContainerView {
        let view = SceneMetalContainerView(frame: .zero)
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
            localStorageScreenIdentity: localStorageScreenIdentity,
            propertyOverrides: propertyOverrides
        )
        view.setPresentation(presentation)
        view.setRenderQuality(renderQuality)
        view.setPaused(playbackPaused)
        view.setAudioConfiguration(
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
        view.setMediaSnapshot(mediaSnapshot)
        view.setFramesPerSecond(framesPerSecond)
        return view
    }

    func updateNSView(_ view: SceneMetalContainerView, context: Context) {
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
            localStorageScreenIdentity: localStorageScreenIdentity,
            propertyOverrides: propertyOverrides
        )
        view.setPresentation(presentation)
        view.setRenderQuality(renderQuality)
        view.setPaused(playbackPaused)
        view.setAudioConfiguration(
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
        view.setMediaSnapshot(mediaSnapshot)
        view.setFramesPerSecond(framesPerSecond)
    }

    static func dismantleNSView(_ view: SceneMetalContainerView, coordinator: ()) {
        view.shutdown()
    }
}

final class SceneMetalContainerView: NSView {
    var onError: ((String?) -> Void)? {
        didSet { metalView?.onError = onError }
    }
    var onPropertiesLoaded: (([ScenePropertyDefinition]?) -> Void)? {
        didSet { metalView?.onPropertiesLoaded = onPropertiesLoaded }
    }

    private let metalView: SceneMetalView?

    override init(frame frameRect: NSRect) {
        if let device = MTLCreateSystemDefaultDevice() {
            metalView = SceneMetalView(frame: frameRect, device: device)
        } else {
            metalView = nil
        }
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.clear.cgColor
        if let metalView {
            metalView.autoresizingMask = [.width, .height]
            addSubview(metalView)
        } else {
            DispatchQueue.main.async { [weak self] in
                self?.report("macOS did not provide a Metal device")
            }
        }
    }

    required init?(coder: NSCoder) { nil }

    func configure(
        wallpaper: WEWallpaper,
        assetsDirectory: String?,
        localStorageScreenIdentity: String,
        propertyOverrides: [String: ScenePropertyValue]
    ) {
        metalView?.configure(
            wallpaper: wallpaper,
            assetsDirectory: assetsDirectory,
            localStorageScreenIdentity: localStorageScreenIdentity,
            propertyOverrides: propertyOverrides
        )
    }

    func setPaused(_ value: Bool) { metalView?.setPaused(value) }

    func setPresentation(_ value: ScenePresentationLayout) {
        metalView?.setPresentation(value)
    }

    func setRenderQuality(_ value: GSSceneRenderQuality) {
        metalView?.setRenderQuality(value)
    }

    func setAudioConfiguration(
        masterVolume: Float,
        audioOutputEnabled: Bool,
        isAudibleOwner: Bool
    ) {
        metalView?.setAudioConfiguration(
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
    }

    func setSystemAudioCaptureEnabled(_ enabled: Bool) {
        metalView?.setSystemAudioCaptureEnabled(enabled)
    }

    func setMediaSnapshot(_ value: SceneMediaProviderSnapshot) {
        metalView?.setMediaSnapshot(value)
    }

    func setFramesPerSecond(_ value: Double) { metalView?.setFramesPerSecond(value) }

    func forwardDesktopLeftMouseButton(isDown: Bool) {
        metalView?.setPointerState(active: true, leftDown: isDown)
    }

    func shutdown() { metalView?.shutdown() }

    private func report(_ message: String) {
        NSLog("[Scene] %@", message)
        onError?(message)
    }
}

private final class SceneMetalView: MTKView, MTKViewDelegate {
    private static let maximumMetalTextureDimension: CGFloat = 16_384
    private struct SessionIdentity: Equatable {
        let scene: URL
        let assetsDirectory: String?
        let localStorageScreenIdentity: String
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
        scaling: .automatic,
        canvasWidth: 0,
        canvasHeight: 0,
        viewportX: 0,
        viewportY: 0,
        viewportWidth: 0,
        viewportHeight: 0
    )
    private var propertyConfigurationValid = false
    private var frameDisplayLink: CADisplayLink?
    private var isMetalPrepared = false
    private var hasRenderedFrame = false
    private var playbackPaused = false
    private var renderQuality = GSSceneRenderQuality.high
    private var framesPerSecond = 60.0
    private var masterVolume: Float = 1
    private var audioOutputEnabled = true
    private var systemAudioCaptureEnabled = false
    private var isAudibleOwner = false
    private var mediaSnapshot: SceneMediaProviderSnapshot =
        .unavailable(revisions: .zero)
    private var runtimeSeconds = 0.0
    private var previousTimestamp: TimeInterval?
    private var pendingDisplayTargetTimestamp: TimeInterval?
    private var displayLinkSequence: UInt64 = 0
    private var pendingDisplayLinkSequence: UInt64?
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
    private var frameStats = SceneDrawFrameStats()

    private lazy var clearCommandQueue = device?.makeCommandQueue()

    override init(frame frameRect: NSRect, device: MTLDevice?) {
        super.init(frame: frameRect, device: device)
        colorPixelFormat = .bgra8Unorm
        framebufferOnly = true
        sampleCount = 1
        isPaused = true
        enableSetNeedsDisplay = true
        autoResizeDrawable = true
        clearColor = MTLClearColorMake(0, 0, 0, 0)
        delegate = self
        isMetalPrepared = true
    }

    required init(coder: NSCoder) { fatalError("init(coder:) is unsupported") }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil { reconcileSession() }
        updateDisplayLink()
    }

    func configure(
        wallpaper: WEWallpaper,
        assetsDirectory: String?,
        localStorageScreenIdentity: String,
        propertyOverrides: [String: ScenePropertyValue]
    ) {
        let identity = SessionIdentity(
            scene: wallpaper.wallpaperDirectory.appending(path: wallpaper.project.file),
            assetsDirectory: assetsDirectory,
            localStorageScreenIdentity: localStorageScreenIdentity
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
        pendingDisplayTargetTimestamp = nil
        previousSpanRuntimeSeconds = nil
        audioCaptureIssue = nil
        clearDrawableIfAvailable()
        reconcileSession()
        updateDisplayLink()
    }

    func setPaused(_ value: Bool) {
        guard playbackPaused != value else { return }
        session?.setPaused(value)
        playbackPaused = value
        previousTimestamp = nil
        pendingDisplayTargetTimestamp = nil
        previousSpanRuntimeSeconds = nil
        reportAudioIssue()
        updateDisplayLink()
    }

    func setPresentation(_ value: ScenePresentationLayout) {
        guard presentation != value else { return }
        if presentation.spanAcrossScreens != value.spanAcrossScreens {
            previousTimestamp = nil
            pendingDisplayTargetTimestamp = nil
            previousSpanRuntimeSeconds = nil
        }
        presentation = value
        needsDisplay = true
    }

    func setRenderQuality(_ value: GSSceneRenderQuality) {
        guard renderQuality != value else { return }
        renderQuality = value
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
        // frame even while playback is paused; replay alone cannot deliver
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
            updateDisplayLink()
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
            pendingDisplayTargetTimestamp = nil
            previousSpanRuntimeSeconds = nil
            reportFatalIssue(nil)
            reportAudioIssue()
            needsDisplay = true
            updateDisplayLink()
        } catch {
            propertyConfigurationValid = false
            hasRenderedFrame = false
            updateDisplayLink()
            clearDrawableIfAvailable()
            reportFatalIssue(error.localizedDescription)
        }
    }

    func setFramesPerSecond(_ value: Double) {
        let clamped = min(max(value, 1), 240)
        guard framesPerSecond != clamped else { return }
        framesPerSecond = clamped
        previousTimestamp = nil
        pendingDisplayTargetTimestamp = nil
        previousSpanRuntimeSeconds = nil
        updateDisplayLink()
    }

    func setPointerState(active: Bool, leftDown: Bool) {
        guard let session else { return }
        do {
            try session.setPointerState(active: active, leftDown: leftDown)
            needsDisplay = true
        } catch {
            failSession(error.localizedDescription)
        }
    }

    func draw(in view: MTKView) {
        guard propertyConfigurationValid,
              let session,
              let drawable = currentDrawable else {
            clearDrawableIfAvailable()
            return
        }
        let backing = drawableSize
        guard backing.width >= 1, backing.height >= 1,
              backing.width <= Self.maximumMetalTextureDimension,
              backing.height <= Self.maximumMetalTextureDimension else { return }
        if let validationError = presentation.validationError {
            clearDrawableIfAvailable()
            reportFatalIssue(validationError)
            return
        }
        do {
            let drawStarted = CACurrentMediaTime()
            let displaySequence = pendingDisplayLinkSequence
            pendingDisplayLinkSequence = nil
            // A synchronous display-link draw can still be followed by
            // AppKit's ordinary dirty-view pass. That pass has no display
            // sequence and must not evaluate and submit the same scene frame
            // a second time. Initial, paused, and manual draws remain valid.
            if frameDisplayLink != nil,
               displaySequence == nil,
               hasRenderedFrame {
                return
            }
            var frameTimeForStats = 0.0
            var renderMillisecondsForStats = 0.0
            var targetLagMillisecondsForStats = 0.0
            let width = UInt32(backing.width.rounded())
            let height = UInt32(backing.height.rounded())
            if playbackPaused && hasRenderedFrame {
                try session.replayLastEvaluatedFrame(
                    drawable: drawable,
                    drawableWidth: width,
                    drawableHeight: height,
                    presentation: presentation,
                    renderQuality: renderQuality
                )
            } else {
                // Drive animation from the presentation timestamp supplied by
                // CADisplayLink. Wall-clock sampling here introduces jitter
                // between the display callback and AppKit's deferred draw,
                // which is especially visible on short particle trails.
                let now = pendingDisplayTargetTimestamp ?? CACurrentMediaTime()
                pendingDisplayTargetTimestamp = nil
                let frameTime: Double
                let presentedRuntimeSeconds: Double
                if presentation.spanAcrossScreens {
                    let sharedRuntimeSeconds = ScenePresentationClock.shared.seconds(
                        at: now,
                        framesPerSecond: framesPerSecond
                    )
                    frameTime = playbackPaused ? 0 : min(max(
                        previousSpanRuntimeSeconds.map {
                            sharedRuntimeSeconds - $0
                        } ?? 0,
                        0
                    ), 0.25)
                    previousSpanRuntimeSeconds = playbackPaused ? nil : sharedRuntimeSeconds
                    presentedRuntimeSeconds = sharedRuntimeSeconds
                } else {
                    frameTime = playbackPaused ? 0 : min(max(
                        previousTimestamp.map { now - $0 } ?? 0,
                        0
                    ), 0.25)
                    previousTimestamp = playbackPaused ? nil : now
                    runtimeSeconds += frameTime
                    presentedRuntimeSeconds = runtimeSeconds
                }
                let displayLabel = displaySequence.map(String.init) ?? "none"
                let targetLagMilliseconds = (drawStarted - now) * 1_000
                frameTimeForStats = frameTime
                targetLagMillisecondsForStats = targetLagMilliseconds
                SceneFrameTrace.log(
                    "draw.begin display=\(displayLabel) "
                        + "target=\(String(format: "%.6f", now)) "
                        + "runtime=\(String(format: "%.6f", presentedRuntimeSeconds)) "
                        + "delta=\(String(format: "%.6f", frameTime)) "
                        + "targetLagMs=\(String(format: "%.3f", targetLagMilliseconds))"
                )
                let pointer = normalizedDrawablePointer()
                let pointerState = sampledDesktopPointerState()
                let renderStarted = CACurrentMediaTime()
                try session.render(
                    runtimeSeconds: presentedRuntimeSeconds,
                    frameTimeSeconds: frameTime,
                    pointerX: pointer.x,
                    pointerY: pointer.y,
                    pointerActive: pointerState.active,
                    pointerLeftDown: pointerState.leftDown,
                    mediaSnapshot: mediaSnapshot,
                    drawable: drawable,
                    drawableWidth: width,
                    drawableHeight: height,
                    presentation: presentation,
                    renderQuality: renderQuality,
                    masterVolume: masterVolume,
                    audioOutputEnabled: audioOutputEnabled,
                    systemAudioCaptureEnabled: systemAudioCaptureEnabled,
                    isAudibleOwner: isAudibleOwner
                )
                let renderMilliseconds =
                    (CACurrentMediaTime() - renderStarted) * 1_000
                renderMillisecondsForStats = renderMilliseconds
                SceneFrameTrace.log(
                    "draw.render display=\(displayLabel) "
                        + "ms=\(String(format: "%.3f", renderMilliseconds))"
                )
                hasRenderedFrame = true
            }
            let finished = CACurrentMediaTime()
            let displayLabel = displaySequence.map(String.init) ?? "none"
            let flushMilliseconds = 0.0
            let totalMilliseconds = (finished - drawStarted) * 1_000
            frameStats.record(
                displaySequence: displaySequence,
                deltaSeconds: frameTimeForStats,
                renderMilliseconds: renderMillisecondsForStats,
                flushMilliseconds: flushMilliseconds,
                totalMilliseconds: totalMilliseconds,
                targetLagMilliseconds: targetLagMillisecondsForStats
            )
            SceneFrameTrace.log(
                "draw.submit display=\(displayLabel) "
                    + "totalMs=\(String(format: "%.3f", totalMilliseconds))"
            )
            sessionRetryAttempt = 0
            reportFatalIssue(nil)
            reportAudioIssue()
        } catch {
            failSession(error.localizedDescription)
        }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        needsDisplay = true
    }

    func shutdown() {
        frameDisplayLink?.invalidate()
        frameDisplayLink = nil
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
        pendingDisplayTargetTimestamp = nil
        previousSpanRuntimeSeconds = nil
        audioCaptureIssue = nil
        clearDrawableIfAvailable()
        onPropertiesLoaded?(nil)
    }

    deinit {
        frameDisplayLink?.invalidate()
        closeSession()
    }

    private func updateDisplayLink() {
        frameDisplayLink?.invalidate()
        frameDisplayLink = nil
        pendingDisplayTargetTimestamp = nil
        guard isMetalPrepared,
              window != nil,
              session != nil,
              propertyConfigurationValid,
              !playbackPaused else { return }
        let displayLink = displayLink(
            target: self,
            selector: #selector(displayLinkDidFire(_:))
        )
        let requestedRate = Float(framesPerSecond)
        displayLink.preferredFrameRateRange = CAFrameRateRange(
            minimum: requestedRate,
            maximum: requestedRate,
            preferred: requestedRate
        )
        displayLink.add(to: .main, forMode: .common)
        frameDisplayLink = displayLink
    }

    @objc
    private func displayLinkDidFire(_ displayLink: CADisplayLink) {
        displayLinkSequence &+= 1
        pendingDisplayLinkSequence = displayLinkSequence
        pendingDisplayTargetTimestamp = displayLink.targetTimestamp
        SceneFrameTrace.log(
            "display.tick display=\(displayLinkSequence) "
                + "timestamp=\(String(format: "%.6f", displayLink.timestamp)) "
                + "target=\(String(format: "%.6f", displayLink.targetTimestamp)) "
                + "duration=\(String(format: "%.6f", displayLink.duration))"
        )
        needsDisplay = true
        displayIfNeeded()
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
        guard isMetalPrepared,
              window != nil,
              let wallpaper = requestedWallpaper,
              let identity = requestedIdentity,
              identity != activeIdentity,
              identity != attemptedIdentity,
              let device else { return }
        attemptedIdentity = identity
        let newSession: SceneRuntimeSession
        do {
            newSession = try SceneRuntimeSession(
                wallpaper: wallpaper,
                assetsDirectory: identity.assetsDirectory,
                localStorageScreenIdentity: identity.localStorageScreenIdentity,
                metalDevice: device,
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

        newSession.setPaused(playbackPaused)
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
        guard isMetalPrepared,
              window != nil,
              let drawable = currentDrawable,
              let commandBuffer = clearCommandQueue?.makeCommandBuffer()
        else { return }
        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture = drawable.texture
        descriptor.colorAttachments[0].loadAction = .clear
        descriptor.colorAttachments[0].storeAction = .store
        descriptor.colorAttachments[0].clearColor = clearColor
        guard let encoder = commandBuffer.makeRenderCommandEncoder(
            descriptor: descriptor
        ) else { return }
        encoder.endEncoding()
        commandBuffer.present(drawable)
        commandBuffer.commit()
    }

    private func closeSession() {
        sessionRetryTask?.cancel()
        sessionRetryTask = nil
        cancelAudioCapture()
        guard let session else { return }
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
        updateDisplayLink()
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
        guard isMetalPrepared,
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
