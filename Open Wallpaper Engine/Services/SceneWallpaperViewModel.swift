import Foundation
import OpenGL
import SceneAudio
import SceneRuntimeBridge

private struct SceneExecutorIssue: Hashable {
    let severity: String
    let objectIndex: Int
    let objectId: Int32
    let operationIndex: Int
    let message: String

    var logMessage: String {
        "Scene runtime issue [\(severity)] objectIndex=\(objectIndex) "
            + "objectId=\(objectId) operationIndex=\(operationIndex): \(message)"
    }
}

enum SceneRuntimeSessionError: LocalizedError {
    case configuration(String)
    case runtime(String)

    var errorDescription: String? {
        switch self {
        case .configuration(let message), .runtime(let message): return message
        }
    }
}

/// Owns one complete SceneRuntime pipeline for one screen and one CGL context.
/// Handles are destroyed in strict reverse dependency order.
@MainActor
final class SceneRuntimeSession {
    private var runtime: WESceneRuntimeRef?
    private var model: WESceneModelRef?
    private var graph: WESceneGraphRef?
    private var frameGraph: WESceneFrameGraphRef?
    private var executor: WESceneFrameExecutorRef?
    private let audioController = SceneAudioController()
    private var lastSounds: [SceneSoundSnapshot] = []
    private var previousExecutorIssues: Set<SceneExecutorIssue> = []
    private var previousExecutorIssueReadFailure: String?
    private var isPaused = false
    private var lastMediaSnapshot: SceneMediaProviderSnapshot?
    private(set) var audioPlaybackIssue: String?
    private(set) var requiresAudioSpectrum = false
    private(set) var properties: [ScenePropertyDefinition] = []

    init(
        wallpaper: WEWallpaper,
        assetsDirectory: String?,
        cglContext: CGLContextObj,
        isScreensaver: Bool
    ) throws {
        guard let assetsPath = assetsDirectory, !assetsPath.isEmpty else {
            throw SceneRuntimeSessionError.configuration(
                "Wallpaper Engine assets directory is not configured"
            )
        }

        let sceneFile = wallpaper.project.file
        let packageName = (sceneFile as NSString).deletingPathExtension + ".pkg"
        let packageURL = wallpaper.wallpaperDirectory.appending(path: packageName)
        guard FileManager.default.fileExists(atPath: packageURL.path) else {
            throw SceneRuntimeSessionError.configuration(
                "Scene package is missing: \(packageURL.path)"
            )
        }

        do {
            var error: WESceneRuntimeErrorRef?
            runtime = assetsPath.withCString { assets in
                packageURL.path.withCString { package in
                    var configuration = WESceneRuntimeConfiguration(
                        assets_directory: assets,
                        scene_package_path: package
                    )
                    return we_scene_runtime_create(&configuration, &error)
                }
            }
            guard let runtime else { throw bridgeError("Creating Scene runtime", error) }

            model = "project.json".withCString {
                we_scene_runtime_model_create(runtime, $0, &error)
            }
            guard let model else { throw bridgeError("Loading Scene model", error) }
            var projectInfo = WESceneProjectInfo()
            guard we_scene_model_project_info(model, &projectInfo, &error) == 1 else {
                throw bridgeError("Reading Scene project metadata", error)
            }
            requiresAudioSpectrum = projectInfo.supports_audio_processing == 1
            properties = try loadProperties(from: model)

            graph = we_scene_model_graph_create(model, &error)
            guard let graph else { throw bridgeError("Creating Scene graph", error) }

            frameGraph = we_scene_graph_frame_graph_create(graph, &error)
            guard let frameGraph else { throw bridgeError("Creating Scene frame graph", error) }

            executor = we_scene_frame_executor_create_with_cgl_context(
                frameGraph, UnsafeMutableRawPointer(cglContext), &error
            )
            guard executor != nil else {
                throw bridgeError("Creating Scene OpenGL executor", error)
            }
            guard we_scene_frame_executor_set_screensaver_state(
                executor,
                isScreensaver ? 1 : 0,
                &error
            ) == 1 else {
                throw bridgeError("Setting Scene screensaver mode", error)
            }
        } catch {
            unload()
            throw error
        }
    }

    deinit {
        MainActor.assumeIsolated { close() }
    }

    func render(
        runtimeSeconds: Double,
        frameTimeSeconds: Double,
        pointerX: Double,
        pointerY: Double,
        pointerActive: Bool,
        pointerLeftDown: Bool,
        mediaSnapshot: SceneMediaProviderSnapshot,
        drawableWidth: UInt32,
        drawableHeight: UInt32,
        presentation: ScenePresentationLayout,
        masterVolume: Float,
        audioOutputEnabled: Bool,
        isAudibleOwner: Bool
    ) throws {
        guard let executor else {
            throw SceneRuntimeSessionError.runtime("Scene executor is not available")
        }
        var inputs = WESceneFrameInputs(
            pointer_x: pointerX,
            pointer_y: pointerY,
            time_seconds: runtimeSeconds,
            frame_time_seconds: frameTimeSeconds
        )
        var error: WESceneRuntimeErrorRef?
        guard we_scene_frame_executor_set_pointer_state(
            executor,
            pointerActive ? 1 : 0,
            pointerLeftDown ? 1 : 0,
            &error
        ) == 1 else {
            throw bridgeError("Updating Scene pointer state", error)
        }
        var soundRuntimeStates = audioController.soundRuntimeSnapshots().map {
            snapshot -> WESceneSoundRuntimeStateInput in
            let state: WESceneSoundRuntimeState
            switch snapshot.state {
            case .stopped: state = WE_SCENE_SOUND_RUNTIME_STOPPED
            case .playing: state = WE_SCENE_SOUND_RUNTIME_PLAYING
            case .paused: state = WE_SCENE_SOUND_RUNTIME_PAUSED
            case .ended: state = WE_SCENE_SOUND_RUNTIME_ENDED
            }
            return WESceneSoundRuntimeStateInput(
                object_id: Int32(snapshot.objectId),
                state: state,
                position: snapshot.position
            )
        }
        let soundStateResult = soundRuntimeStates.withUnsafeBufferPointer {
            we_scene_frame_executor_set_sound_runtime_states(
                executor, $0.baseAddress, $0.count, &error
            )
        }
        guard soundStateResult == 1 else {
            throw bridgeError("Updating Scene sound runtime state", error)
        }
        if lastMediaSnapshot != mediaSnapshot {
            try applyMediaSnapshot(mediaSnapshot, to: executor)
            lastMediaSnapshot = mediaSnapshot
        }
        // A zero-filled spectrum is not equivalent to an unavailable capture:
        // scripts use registerAudioBuffers() during module evaluation and the
        // Linux contract reports an explicit unavailable error until a real
        // recorder frame exists. Only pass the audio ABI when ScreenCaptureKit
        // has actually reached the running state.
        let renderResult: Int32
        if var viewport = try presentation.viewport(
            drawableWidth: drawableWidth,
            drawableHeight: drawableHeight
        ) {
            if case .running = SceneSystemAudioSpectrumProvider.shared.status {
                let spectrum = SceneSystemAudioSpectrumProvider.shared.latestFrame
                renderResult = withAudioSpectrumInputs(spectrum) { audioInputs in
                    we_scene_frame_executor_render_for_viewport_with_audio_spectrum(
                        executor,
                        &inputs,
                        &audioInputs,
                        &viewport,
                        presentation.bridgeScaling,
                        &error
                    )
                }
            } else {
                renderResult = we_scene_frame_executor_render_for_viewport(
                    executor,
                    &inputs,
                    &viewport,
                    presentation.bridgeScaling,
                    &error
                )
            }
        } else if case .running = SceneSystemAudioSpectrumProvider.shared.status {
            let spectrum = SceneSystemAudioSpectrumProvider.shared.latestFrame
            renderResult = withAudioSpectrumInputs(spectrum) { audioInputs in
                we_scene_frame_executor_render_for_drawable_with_audio_spectrum(
                    executor,
                    &inputs,
                    &audioInputs,
                    drawableWidth,
                    drawableHeight,
                    presentation.bridgeScaling,
                    &error
                )
            }
        } else {
            renderResult = we_scene_frame_executor_render_for_drawable(
                executor,
                &inputs,
                drawableWidth,
                drawableHeight,
                presentation.bridgeScaling,
                &error
            )
        }
        guard renderResult == 1 else {
            invalidateExecutorIssueSnapshot()
            throw bridgeError("Rendering Scene frame", error)
        }
        reportExecutorIssues(from: executor)
        try present(
            drawableWidth: drawableWidth,
            drawableHeight: drawableHeight,
            presentation: presentation
        )
        synchronizeAudio(
            from: executor,
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
    }

    private func withAudioSpectrumInputs<T>(
        _ frame: SceneAudioSpectrumFrame,
        _ body: (inout WESceneAudioSpectrumInputs) -> T
    ) -> T {
        frame.spectrum16Left.withUnsafeBufferPointer { left16 in
            frame.spectrum16Right.withUnsafeBufferPointer { right16 in
                frame.spectrum32Left.withUnsafeBufferPointer { left32 in
                    frame.spectrum32Right.withUnsafeBufferPointer { right32 in
                        frame.spectrum64Left.withUnsafeBufferPointer { left64 in
                            frame.spectrum64Right.withUnsafeBufferPointer { right64 in
                                var inputs = WESceneAudioSpectrumInputs(
                                    spectrum_16_left: left16.baseAddress,
                                    spectrum_16_right: right16.baseAddress,
                                    spectrum_32_left: left32.baseAddress,
                                    spectrum_32_right: right32.baseAddress,
                                    spectrum_64_left: left64.baseAddress,
                                    spectrum_64_right: right64.baseAddress
                                )
                                return body(&inputs)
                            }
                        }
                    }
                }
            }
        }
    }

    private func applyMediaSnapshot(
        _ snapshot: SceneMediaProviderSnapshot,
        to executor: WESceneFrameExecutorRef
    ) throws {
        var allocatedStrings: [UnsafeMutablePointer<CChar>] = []
        defer {
            for pointer in allocatedStrings { free(pointer) }
        }

        func copiedCString(_ value: String) throws -> UnsafePointer<CChar> {
            guard let pointer = strdup(value) else {
                throw SceneRuntimeSessionError.runtime(
                    "Allocating Scene media bridge input failed"
                )
            }
            allocatedStrings.append(pointer)
            return UnsafePointer(pointer)
        }

        var bridgeSnapshot: WESceneMediaSnapshot
        switch snapshot {
        case .unavailable(let revision):
            bridgeSnapshot = WESceneMediaSnapshot(
                revision: revision,
                available: 0,
                playback_state: WE_SCENE_MEDIA_STOPPED,
                title: nil,
                artist: nil,
                content_type: nil,
                album_title: nil,
                sub_title: nil,
                album_artist: nil,
                genres: nil,
                position: 0,
                duration: 0,
                has_thumbnail: 0,
                primary_color: (0, 0, 0),
                secondary_color: (0, 0, 0),
                tertiary_color: (0, 0, 0),
                text_color: (0, 0, 0),
                high_contrast_color: (0, 0, 0)
            )
        case .available(let revision, let content):
            let playbackState: WESceneMediaPlaybackState
            switch content.playbackState {
            case .stopped: playbackState = WE_SCENE_MEDIA_STOPPED
            case .playing: playbackState = WE_SCENE_MEDIA_PLAYING
            case .paused: playbackState = WE_SCENE_MEDIA_PAUSED
            }
            bridgeSnapshot = WESceneMediaSnapshot(
                revision: revision,
                available: 1,
                playback_state: playbackState,
                title: try copiedCString(content.title),
                artist: try copiedCString(content.artist),
                content_type: try copiedCString(content.contentType),
                album_title: try copiedCString(content.albumTitle),
                sub_title: try copiedCString(content.subTitle),
                album_artist: try copiedCString(content.albumArtist),
                genres: try copiedCString(content.genres),
                position: content.position,
                duration: content.duration,
                has_thumbnail: content.hasThumbnail ? 1 : 0,
                primary_color: (
                    content.primaryColor.red,
                    content.primaryColor.green,
                    content.primaryColor.blue
                ),
                secondary_color: (
                    content.secondaryColor.red,
                    content.secondaryColor.green,
                    content.secondaryColor.blue
                ),
                tertiary_color: (
                    content.tertiaryColor.red,
                    content.tertiaryColor.green,
                    content.tertiaryColor.blue
                ),
                text_color: (
                    content.textColor.red,
                    content.textColor.green,
                    content.textColor.blue
                ),
                high_contrast_color: (
                    content.highContrastColor.red,
                    content.highContrastColor.green,
                    content.highContrastColor.blue
                )
            )
        }

        var error: WESceneRuntimeErrorRef?
        guard we_scene_frame_executor_set_media_snapshot(
            executor, &bridgeSnapshot, &error
        ) == 1 else {
            throw bridgeError("Updating Scene media snapshot", error)
        }
    }

    func updateAudioConfiguration(
        masterVolume: Float,
        audioOutputEnabled: Bool,
        isAudibleOwner: Bool
    ) {
        synchronizeAudio(
            lastSounds,
            masterVolume: masterVolume,
            audioOutputEnabled: audioOutputEnabled,
            isAudibleOwner: isAudibleOwner
        )
    }

    func replayLastEvaluatedFrame(
        drawableWidth: UInt32,
        drawableHeight: UInt32,
        presentation: ScenePresentationLayout
    ) throws {
        guard let executor else {
            throw SceneRuntimeSessionError.runtime("Scene executor is not available")
        }
        var error: WESceneRuntimeErrorRef?
        let replayResult: Int32
        if var viewport = try presentation.viewport(
            drawableWidth: drawableWidth,
            drawableHeight: drawableHeight
        ) {
            replayResult = we_scene_frame_executor_replay_for_viewport(
                executor, &viewport, &error
            )
        } else {
            replayResult = we_scene_frame_executor_replay_for_drawable(
                executor, drawableWidth, drawableHeight, &error
            )
        }
        guard replayResult == 1 else {
            invalidateExecutorIssueSnapshot()
            throw bridgeError("Reprojecting paused Scene frame", error)
        }
        reportExecutorIssues(from: executor)
        try present(
            drawableWidth: drawableWidth,
            drawableHeight: drawableHeight,
            presentation: presentation
        )
    }

    func setPaused(_ value: Bool) {
        guard isPaused != value else { return }
        if value {
            audioController.pauseAll()
        } else {
            do {
                try audioController.resumeAll()
            } catch {
                recordAudioFailure(error)
            }
        }
        isPaused = value
    }

    func present(
        drawableWidth: UInt32,
        drawableHeight: UInt32,
        presentation: ScenePresentationLayout
    ) throws {
        guard let executor else {
            throw SceneRuntimeSessionError.runtime("Scene executor is not available")
        }
        var error: WESceneRuntimeErrorRef?
        let presentResult: Int32
        if var viewport = try presentation.viewport(
            drawableWidth: drawableWidth,
            drawableHeight: drawableHeight
        ) {
            presentResult = we_scene_frame_executor_present_for_viewport(
                executor, &viewport, presentation.bridgeScaling, &error
            )
        } else {
            presentResult = we_scene_frame_executor_present(
                executor,
                drawableWidth,
                drawableHeight,
                presentation.bridgeScaling,
                &error
            )
        }
        guard presentResult == 1 else {
            throw bridgeError("Presenting Scene frame", error)
        }
    }

    func applyPropertyOverrides(_ overrides: [String: ScenePropertyValue]) throws {
        guard let model else {
            throw SceneRuntimeSessionError.runtime("Scene model is not available")
        }

        let definitions = Dictionary(uniqueKeysWithValues: properties.map { ($0.key, $0) })
        var values: [(String, ScenePropertyValue)] = properties.compactMap { property in
            guard !property.isReadOnly,
                  let value = overrides[property.key] ?? property.defaultValue else {
                return nil
            }
            return (property.key, value)
        }
        for key in overrides.keys.sorted() where definitions[key] == nil || definitions[key]?.isReadOnly == true {
            values.append((key, overrides[key]!))
        }
        guard !values.isEmpty else { return }

        var allocatedPointers: [UnsafeMutablePointer<CChar>] = []
        defer {
            for pointer in allocatedPointers {
                free(pointer)
            }
        }
        func copyCString(_ string: String) throws -> UnsafePointer<CChar> {
            guard let pointer = strdup(string) else {
                throw SceneRuntimeSessionError.runtime(
                    "Allocating Scene property bridge input failed"
                )
            }
            allocatedPointers.append(pointer)
            return UnsafePointer(pointer)
        }

        var updates: [WEScenePropertyUpdate] = []
        updates.reserveCapacity(values.count)
        for (key, value) in values {
            let keyPointer = try copyCString(key)
            let bridgeValue: WEScenePropertyValue
            switch value {
            case .boolean(let boolean):
                bridgeValue = WEScenePropertyValue(
                    type: WE_SCENE_VALUE_BOOLEAN,
                    boolean_value: boolean ? 1 : 0,
                    integer_value: 0,
                    number_value: 0,
                    string_value: nil,
                    component_count: 0,
                    vector_value: WESceneVector4()
                )
            case .integer(let integer):
                bridgeValue = WEScenePropertyValue(
                    type: WE_SCENE_VALUE_INTEGER,
                    boolean_value: 0,
                    integer_value: integer,
                    number_value: 0,
                    string_value: nil,
                    component_count: 0,
                    vector_value: WESceneVector4()
                )
            case .number(let number):
                bridgeValue = WEScenePropertyValue(
                    type: WE_SCENE_VALUE_NUMBER,
                    boolean_value: 0,
                    integer_value: 0,
                    number_value: number,
                    string_value: nil,
                    component_count: 0,
                    vector_value: WESceneVector4()
                )
            case .string(let string):
                bridgeValue = WEScenePropertyValue(
                    type: WE_SCENE_VALUE_STRING,
                    boolean_value: 0,
                    integer_value: 0,
                    number_value: 0,
                    string_value: try copyCString(string),
                    component_count: 0,
                    vector_value: WESceneVector4()
                )
            }
            updates.append(WEScenePropertyUpdate(key: keyPointer, value: bridgeValue))
        }

        var error: WESceneRuntimeErrorRef?
        let result = updates.withUnsafeBufferPointer { buffer in
            we_scene_model_set_property_values(
                model,
                buffer.baseAddress,
                buffer.count,
                &error
            )
        }
        guard result == 1 else {
            throw bridgeError("Applying Scene properties", error)
        }
    }

    func close() {
        audioController.stopAll()
        lastSounds = []
        audioPlaybackIssue = nil
        invalidateExecutorIssueSnapshot()
        isPaused = false
        unload()
    }

    private func unload() {
        if let executor { we_scene_frame_executor_destroy(executor) }
        if let frameGraph { we_scene_frame_graph_destroy(frameGraph) }
        if let graph { we_scene_graph_destroy(graph) }
        if let model { we_scene_model_destroy(model) }
        if let runtime { we_scene_runtime_destroy(runtime) }
        executor = nil
        frameGraph = nil
        graph = nil
        model = nil
        runtime = nil
    }

    private func loadSoundSnapshot(
        from executor: WESceneFrameExecutorRef
    ) throws -> [SceneSoundSnapshot] {
        var error: WESceneRuntimeErrorRef?
        var count = 0
        guard we_scene_frame_executor_sound_count(executor, &count, &error) == 1 else {
            throw bridgeError("Reading Scene sound count", error)
        }
        return try (0..<count).map { soundIndex in
            var info = WESceneFrameSoundInfo()
            guard we_scene_frame_executor_sound_info(
                executor, soundIndex, &info, &error
            ) == 1 else {
                throw bridgeError("Reading Scene sound metadata", error)
            }
            let volume = Float(info.volume)
            guard volume.isFinite, (0...1).contains(volume) else {
                throw SceneRuntimeSessionError.runtime(
                    "Scene sound object \(info.object_id) has invalid volume \(info.volume)"
                )
            }
            let loop: Bool
            switch info.playback_mode {
            case WE_SCENE_FRAME_SOUND_PLAYBACK_ONCE: loop = false
            case WE_SCENE_FRAME_SOUND_PLAYBACK_LOOP: loop = true
            default:
                throw SceneRuntimeSessionError.runtime(
                    "Scene sound object \(info.object_id) has an unknown playback mode"
                )
            }
            let playbackCommand: SceneSoundPlaybackCommand?
            switch info.playback_command {
            case WE_SCENE_FRAME_SOUND_COMMAND_NONE:
                guard info.playback_command_generation == 0 else {
                    throw SceneRuntimeSessionError.runtime(
                        "Scene sound object \(info.object_id) has a generation without a command"
                    )
                }
                playbackCommand = nil
            case WE_SCENE_FRAME_SOUND_COMMAND_PLAY:
                playbackCommand = SceneSoundPlaybackCommand(
                    action: .play,
                    generation: info.playback_command_generation
                )
            case WE_SCENE_FRAME_SOUND_COMMAND_PAUSE:
                playbackCommand = SceneSoundPlaybackCommand(
                    action: .pause,
                    generation: info.playback_command_generation
                )
            case WE_SCENE_FRAME_SOUND_COMMAND_STOP:
                playbackCommand = SceneSoundPlaybackCommand(
                    action: .stop,
                    generation: info.playback_command_generation
                )
            default:
                throw SceneRuntimeSessionError.runtime(
                    "Scene sound object \(info.object_id) has an unknown playback command"
                )
            }
            if playbackCommand != nil && info.playback_command_generation == 0 {
                throw SceneRuntimeSessionError.runtime(
                    "Scene sound object \(info.object_id) uses reserved command generation 0"
                )
            }
            let sources = try (0..<info.source_count).map { sourceIndex in
                var source: UnsafePointer<CChar>?
                guard we_scene_frame_executor_sound_source(
                    executor, soundIndex, sourceIndex, &source, &error
                ) == 1 else {
                    throw bridgeError("Reading Scene sound source", error)
                }
                guard let source else {
                    throw SceneRuntimeSessionError.runtime(
                        "Scene sound object \(info.object_id) contains a null source"
                    )
                }
                return SceneSoundSourceSnapshot(
                    sourceIndex: sourceIndex,
                    resource: String(cString: source),
                    loop: loop,
                    volume: volume,
                    startSilent: info.start_silent == 1
                )
            }
            return SceneSoundSnapshot(
                objectId: Int(info.object_id),
                visible: info.visible == 1,
                sources: sources,
                playbackCommand: playbackCommand,
                minimumTime: info.minimum_time,
                maximumTime: info.maximum_time
            )
        }
    }

    private func reportExecutorIssues(from executor: WESceneFrameExecutorRef) {
        do {
            let issues = try loadExecutorIssues(from: executor)
            previousExecutorIssueReadFailure = nil

            let currentIssues = Set(issues)
            var loggedThisFrame: Set<SceneExecutorIssue> = []
            for issue in issues {
                guard !previousExecutorIssues.contains(issue),
                      loggedThisFrame.insert(issue).inserted else { continue }
                NSLog("[Scene] %@", issue.logMessage)
            }
            previousExecutorIssues = currentIssues
        } catch {
            let message = error.localizedDescription
            if previousExecutorIssueReadFailure != message {
                NSLog("[Scene] %@", message)
            }
            previousExecutorIssues.removeAll()
            previousExecutorIssueReadFailure = message
        }
    }

    private func invalidateExecutorIssueSnapshot() {
        previousExecutorIssues.removeAll()
        previousExecutorIssueReadFailure = nil
    }

    private func loadExecutorIssues(
        from executor: WESceneFrameExecutorRef
    ) throws -> [SceneExecutorIssue] {
        var error: WESceneRuntimeErrorRef?
        var count = 0
        guard we_scene_frame_executor_issue_count(executor, &count, &error) == 1 else {
            throw bridgeError("Reading Scene executor issue count", error)
        }

        var issues: [SceneExecutorIssue] = []
        issues.reserveCapacity(count)
        for issueIndex in 0..<count {
            var info = WESceneFrameExecutorIssueInfo()
            guard we_scene_frame_executor_issue_info(
                executor, issueIndex, &info, &error
            ) == 1 else {
                throw bridgeError("Reading Scene executor issue metadata", error)
            }
            guard let message = info.message else {
                throw SceneRuntimeSessionError.runtime(
                    "Reading Scene executor issue metadata failed: "
                        + "issue \(issueIndex) contains a null message"
                )
            }
            issues.append(SceneExecutorIssue(
                severity: executorIssueSeverityLabel(info.severity),
                objectIndex: info.object_index,
                objectId: info.object_id,
                operationIndex: info.operation_index,
                message: String(cString: message)
            ))
        }
        return issues
    }

    private func executorIssueSeverityLabel(
        _ severity: WESceneFramePlanIssueSeverity
    ) -> String {
        switch severity {
        case WE_SCENE_FRAME_ISSUE_WARNING: return "warning"
        case WE_SCENE_FRAME_ISSUE_SKIP_PASS: return "skip-pass"
        case WE_SCENE_FRAME_ISSUE_SKIP_OBJECT: return "skip-object"
        case WE_SCENE_FRAME_ISSUE_FRAME_FATAL: return "frame-fatal"
        default: return "unknown(\(severity.rawValue))"
        }
    }

    private func synchronizeAudio(
        from executor: WESceneFrameExecutorRef,
        masterVolume: Float,
        audioOutputEnabled: Bool,
        isAudibleOwner: Bool
    ) {
        do {
            let sounds = try loadSoundSnapshot(from: executor)
            lastSounds = sounds
            synchronizeAudio(
                sounds,
                masterVolume: masterVolume,
                audioOutputEnabled: audioOutputEnabled,
                isAudibleOwner: isAudibleOwner
            )
        } catch {
            lastSounds = []
            recordAudioFailure(error)
        }
    }

    private func synchronizeAudio(
        _ sounds: [SceneSoundSnapshot],
        masterVolume: Float,
        audioOutputEnabled: Bool,
        isAudibleOwner: Bool
    ) {
        guard let runtime else {
            recordAudioFailure(
                SceneRuntimeSessionError.runtime(
                    "Scene runtime is not available while synchronizing audio"
                )
            )
            return
        }
        let issue = audioController.synchronize(
            sounds,
            masterVolume: masterVolume,
            audioOutput: audioOutputEnabled && isAudibleOwner ? 1 : 0
        ) { path in
            try SceneAssetDataLoader.load(runtime: runtime, path: path)
        }
        audioPlaybackIssue = issue.map {
            "Scene audio unavailable: \($0.message)"
        }
    }

    private func recordAudioFailure(_ error: Error) {
        audioController.stopAll()
        audioPlaybackIssue = "Scene audio unavailable: \(error.localizedDescription)"
    }

    private func loadProperties(
        from model: WESceneModelRef
    ) throws -> [ScenePropertyDefinition] {
        var error: WESceneRuntimeErrorRef?
        var count = 0
        guard we_scene_model_property_count(model, &count, &error) == 1 else {
            throw bridgeError("Reading Scene property count", error)
        }
        return try (0..<count).map { propertyIndex in
            var info = WEScenePropertyInfo()
            guard we_scene_model_property_info(
                model, propertyIndex, &info, &error
            ) == 1 else {
                throw bridgeError("Reading Scene property metadata", error)
            }
            guard let keyPointer = info.key, let textPointer = info.text else {
                throw SceneRuntimeSessionError.runtime(
                    "Scene property metadata contains a null key or label"
                )
            }
            let key = String(cString: keyPointer)
            let text = String(cString: textPointer)
            let kind = try propertyKind(info.type)

            var optionCount = 0
            guard we_scene_model_property_option_count(
                model, propertyIndex, &optionCount, &error
            ) == 1 else {
                throw bridgeError("Reading Scene property option count", error)
            }
            let options = try (0..<optionCount).map { optionIndex in
                var option = WEScenePropertyOptionInfo()
                guard we_scene_model_property_option_info(
                    model, propertyIndex, optionIndex, &option, &error
                ) == 1 else {
                    throw bridgeError("Reading Scene property option", error)
                }
                guard let value = option.value, let label = option.label else {
                    throw SceneRuntimeSessionError.runtime(
                        "Scene property option contains a null value or label"
                    )
                }
                return ScenePropertyOption(
                    value: String(cString: value),
                    label: String(cString: label)
                )
            }

            var current = WEScenePropertyValue()
            guard we_scene_model_property_value(
                model, propertyIndex, &current, &error
            ) == 1 else {
                throw bridgeError("Reading Scene property default", error)
            }
            let defaultValue = try propertyValue(current, key: key)
            return ScenePropertyDefinition(
                key: key,
                text: text,
                kind: kind,
                index: info.has_index == 1 ? Int(info.index) : nil,
                order: info.has_order == 1 ? Int(info.order) : nil,
                minimum: info.has_minimum == 1 ? info.minimum : nil,
                maximum: info.has_maximum == 1 ? info.maximum : nil,
                step: info.has_step == 1 ? info.step : nil,
                precision: info.has_precision == 1 ? Int(info.precision) : nil,
                fraction: info.has_fraction == 1 ? info.fraction == 1 : nil,
                isReadOnly: info.is_read_only == 1,
                options: options,
                defaultValue: defaultValue
            )
        }
    }

    private func propertyKind(_ type: WEScenePropertyType) throws -> ScenePropertyKind {
        switch type {
        case WE_SCENE_PROPERTY_BOOLEAN: return .boolean
        case WE_SCENE_PROPERTY_SLIDER: return .slider
        case WE_SCENE_PROPERTY_COMBO: return .combo
        case WE_SCENE_PROPERTY_COLOR: return .color
        case WE_SCENE_PROPERTY_TEXT: return .text
        case WE_SCENE_PROPERTY_SCENE_TEXTURE: return .sceneTexture
        case WE_SCENE_PROPERTY_FILE: return .file
        case WE_SCENE_PROPERTY_DIRECTORY: return .directory
        case WE_SCENE_PROPERTY_TEXT_INPUT: return .textInput
        case WE_SCENE_PROPERTY_USER_SHORTCUT: return .userShortcut
        case WE_SCENE_PROPERTY_GROUP: return .group
        default:
            throw SceneRuntimeSessionError.runtime(
                "Scene property metadata contains an unknown type"
            )
        }
    }

    private func propertyValue(
        _ value: WEScenePropertyValue,
        key: String
    ) throws -> ScenePropertyValue? {
        switch value.type {
        case WE_SCENE_VALUE_NULL: return nil
        case WE_SCENE_VALUE_BOOLEAN: return .boolean(value.boolean_value == 1)
        case WE_SCENE_VALUE_INTEGER: return .integer(value.integer_value)
        case WE_SCENE_VALUE_NUMBER: return .number(value.number_value)
        case WE_SCENE_VALUE_STRING:
            guard let pointer = value.string_value else {
                throw SceneRuntimeSessionError.runtime(
                    "Scene property '\(key)' contains a null string"
                )
            }
            return .string(String(cString: pointer))
        case WE_SCENE_VALUE_ARRAY, WE_SCENE_VALUE_OBJECT:
            throw SceneRuntimeSessionError.runtime(
                "Scene property '\(key)' uses an unsupported structured value"
            )
        default:
            throw SceneRuntimeSessionError.runtime(
                "Scene property '\(key)' contains an unknown value type"
            )
        }
    }

    private func bridgeError(
        _ operation: String,
        _ error: WESceneRuntimeErrorRef?
    ) -> SceneRuntimeSessionError {
        let code = we_scene_runtime_error_code(error).rawValue
        let detail = we_scene_runtime_error_message(error).map(String.init(cString:))
            ?? "Unknown SceneRuntime failure"
        we_scene_runtime_error_destroy(error)
        return .runtime("\(operation) failed [\(code)]: \(detail)")
    }
}
