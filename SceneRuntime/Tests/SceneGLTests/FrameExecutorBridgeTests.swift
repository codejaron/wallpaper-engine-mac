import Foundation
import SceneRuntimeBridge
import XCTest

final class FrameExecutorBridgeTests: XCTestCase {
    func testCreateAndOperationsRejectMissingInputs() {
        var error: WESceneRuntimeErrorRef?
        XCTAssertNil(we_scene_frame_executor_create(nil, &error))
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertEqual(we_scene_frame_executor_render(nil, nil, &error), 0)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertEqual(we_scene_frame_executor_read_rgba8(nil, nil, 0, &error), 0)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertNil(we_scene_frame_executor_create_with_cgl_context(nil, nil, &error))
        XCTAssertEqual(we_scene_runtime_error_code(error), WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT)
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_present(
                nil, 100, 100, WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            0
        )
        XCTAssertEqual(we_scene_runtime_error_code(error), WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT)
        we_scene_runtime_error_destroy(error)

        var issueCount = 99
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(nil, &issueCount, &error),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        XCTAssertEqual(issueCount, 99)
        we_scene_runtime_error_destroy(error)

        "unchanged".withCString { sentinelMessage in
            var issue = WESceneFrameExecutorIssueInfo()
            issue.severity = WE_SCENE_FRAME_ISSUE_FRAME_FATAL
            issue.object_index = 17
            issue.object_id = -23
            issue.operation_index = 29
            issue.message = sentinelMessage
            error = nil
            XCTAssertEqual(
                we_scene_frame_executor_issue_info(nil, 0, &issue, &error),
                0
            )
            XCTAssertEqual(
                we_scene_runtime_error_code(error),
                WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
            )
            XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_FRAME_FATAL)
            XCTAssertEqual(issue.object_index, 17)
            XCTAssertEqual(issue.object_id, -23)
            XCTAssertEqual(issue.operation_index, 29)
            XCTAssertEqual(issue.message.map(String.init(cString:)), "unchanged")
            we_scene_runtime_error_destroy(error)
        }
    }

    func testExecutorRetainsFrameGraphOwnershipAndValidatesFrameInputs() throws {
        let loaded = try loadEmptyFrameGraph()
        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(loaded.frameGraph, &error) else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Executor creation failed: \(message)")
            return
        }

        we_scene_frame_graph_destroy(loaded.frameGraph)
        we_scene_graph_destroy(loaded.graph)
        we_scene_model_destroy(loaded.model)
        we_scene_runtime_destroy(loaded.runtime)
        try? FileManager.default.removeItem(at: loaded.root)
        defer { we_scene_frame_executor_destroy(executor) }

        XCTAssertEqual(we_scene_frame_executor_width(executor), 8)
        XCTAssertEqual(we_scene_frame_executor_height(executor), 8)
        XCTAssertEqual(we_scene_frame_executor_rgba8_byte_count(executor), 256)

        var revision: UInt64 = .max
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_last_model_revision(executor, &revision, &error),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        XCTAssertEqual(revision, .max)
        we_scene_runtime_error_destroy(error)

        var issueCount = 99
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(executor, &issueCount, &error),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        XCTAssertEqual(issueCount, 99)
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(executor, nil, &error),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_issue_info(executor, 0, nil, &error),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        "unchanged".withCString { sentinelMessage in
            var issue = WESceneFrameExecutorIssueInfo()
            issue.severity = WE_SCENE_FRAME_ISSUE_FRAME_FATAL
            issue.object_index = 17
            issue.object_id = -23
            issue.operation_index = 29
            issue.message = sentinelMessage
            error = nil
            XCTAssertEqual(
                we_scene_frame_executor_issue_info(executor, 0, &issue, &error),
                0
            )
            XCTAssertEqual(
                we_scene_runtime_error_code(error),
                WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
            )
            XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_FRAME_FATAL)
            XCTAssertEqual(issue.object_index, 17)
            XCTAssertEqual(issue.object_id, -23)
            XCTAssertEqual(issue.operation_index, 29)
            XCTAssertEqual(issue.message.map(String.init(cString:)), "unchanged")
            we_scene_runtime_error_destroy(error)
        }

        error = nil
        XCTAssertEqual(we_scene_frame_executor_render(executor, nil, &error), 0)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        var invalid = WESceneFrameInputs(
            pointer_x: .nan,
            pointer_y: 0,
            time_seconds: 0,
            frame_time_seconds: 0
        )
        error = nil
        XCTAssertEqual(we_scene_frame_executor_render(executor, &invalid, &error), 0)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        XCTAssertTrue(errorMessage(error).contains("finite"))
        we_scene_runtime_error_destroy(error)

        var negative = WESceneFrameInputs(
            pointer_x: 0, pointer_y: 0, time_seconds: -1, frame_time_seconds: 0
        )
        error = nil
        XCTAssertEqual(we_scene_frame_executor_render(executor, &negative, &error), 0)
        XCTAssertEqual(we_scene_runtime_error_code(error), WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT)
        we_scene_runtime_error_destroy(error)

        var valid = WESceneFrameInputs(
            pointer_x: 0.5, pointer_y: 0.5,
            time_seconds: 0, frame_time_seconds: 0
        )
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                executor, &valid, 100, 100,
                WEScenePresentationScaling(rawValue: 99), &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        XCTAssertTrue(errorMessage(error).contains("scaling"))
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_present(
                executor, 100, 100, WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            0
        )
        XCTAssertEqual(we_scene_runtime_error_code(error), WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE)
        XCTAssertTrue(errorMessage(error).contains("borrowed"))
        we_scene_runtime_error_destroy(error)

        for (width, height) in [(UInt32(0), UInt32(100)), (100, 0), (.max, 100)] {
            error = nil
            XCTAssertEqual(
                we_scene_frame_executor_present(
                    executor, width, height, WE_SCENE_PRESENTATION_ASPECT_FILL, &error
                ),
                0
            )
            XCTAssertEqual(we_scene_runtime_error_code(error), WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT)
            we_scene_runtime_error_destroy(error)
        }
    }

    func testSuccessfulRenderPublishesCoherentDynamicSoundSnapshotAndFailureClearsIt() throws {
        let loaded = try loadEmptyFrameGraph(soundOnly: true)
        defer {
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }
        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(loaded.frameGraph, &error) else {
            throw failure("executor", error)
        }
        defer { we_scene_frame_executor_destroy(executor) }

        var count = 99
        XCTAssertEqual(we_scene_frame_executor_sound_count(executor, &count, &error), 0)
        XCTAssertEqual(we_scene_runtime_error_code(error), WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE)
        XCTAssertEqual(count, 99)
        we_scene_runtime_error_destroy(error)

        var inputs = WESceneFrameInputs(
            pointer_x: 0.5, pointer_y: 0.5, time_seconds: 1,
            frame_time_seconds: 1.0 / 60.0
        )
        error = nil
        XCTAssertEqual(we_scene_frame_executor_render(executor, &inputs, &error), 1, errorMessage(error))
        XCTAssertEqual(we_scene_frame_executor_sound_count(executor, &count, &error), 1)
        XCTAssertEqual(count, 1)
        var issueCount = 99
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(executor, &issueCount, &error),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(issueCount, 0)

        "unchanged".withCString { sentinelMessage in
            var issue = WESceneFrameExecutorIssueInfo()
            issue.severity = WE_SCENE_FRAME_ISSUE_FRAME_FATAL
            issue.object_index = 17
            issue.object_id = -23
            issue.operation_index = 29
            issue.message = sentinelMessage
            error = nil
            XCTAssertEqual(
                we_scene_frame_executor_issue_info(executor, 0, &issue, &error),
                0
            )
            XCTAssertEqual(
                we_scene_runtime_error_code(error),
                WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE
            )
            XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_FRAME_FATAL)
            XCTAssertEqual(issue.object_index, 17)
            XCTAssertEqual(issue.object_id, -23)
            XCTAssertEqual(issue.operation_index, 29)
            XCTAssertEqual(issue.message.map(String.init(cString:)), "unchanged")
            we_scene_runtime_error_destroy(error)
        }

        var info = WESceneFrameSoundInfo()
        XCTAssertEqual(we_scene_frame_executor_sound_info(executor, 0, &info, &error), 1)
        XCTAssertEqual(info.object_index, 0)
        XCTAssertEqual(info.object_id, 42)
        XCTAssertEqual(info.visible, 1)
        XCTAssertEqual(info.source_count, 2)
        XCTAssertEqual(info.playback_mode, WE_SCENE_FRAME_SOUND_PLAYBACK_LOOP)
        XCTAssertEqual(info.volume, 0.25, accuracy: 1e-6)
        XCTAssertEqual(info.start_silent, 0)
        XCTAssertEqual(info.mute_in_editor, 0)
        XCTAssertEqual(info.minimum_time, 0)
        XCTAssertEqual(info.maximum_time, 0)
        var source: UnsafePointer<CChar>?
        XCTAssertEqual(we_scene_frame_executor_sound_source(executor, 0, 1, &source, &error), 1)
        XCTAssertEqual(source.map(String.init(cString:)), "sounds/second.ogg")

        var property = WEScenePropertyValue(
            type: WE_SCENE_VALUE_NUMBER, boolean_value: 0, integer_value: 0,
            number_value: 0.75, string_value: nil, component_count: 0,
            vector_value: WESceneVector4()
        )
        XCTAssertEqual("volume".withCString {
            we_scene_model_set_property_value(loaded.model, $0, &property, &error)
        }, 1, errorMessage(error))
        XCTAssertEqual(we_scene_frame_executor_render(executor, &inputs, &error), 1, errorMessage(error))
        XCTAssertEqual(we_scene_frame_executor_sound_info(executor, 0, &info, &error), 1)
        XCTAssertEqual(info.volume, 0.75, accuracy: 1e-6)

        inputs.pointer_x = .nan
        XCTAssertEqual(we_scene_frame_executor_render(executor, &inputs, &error), 0)
        we_scene_runtime_error_destroy(error)
        error = nil
        XCTAssertEqual(we_scene_frame_executor_sound_count(executor, &count, &error), 0)
        XCTAssertEqual(we_scene_runtime_error_code(error), WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE)
        we_scene_runtime_error_destroy(error)

        issueCount = 99
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(executor, &issueCount, &error),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        XCTAssertEqual(issueCount, 99)
        we_scene_runtime_error_destroy(error)
    }

    func testAutomaticProjectionWithoutImageExtentRequiresAndTracksDrawableSize() throws {
        let loaded = try loadEmptyFrameGraph(soundOnly: true, projectionAuto: true)
        defer {
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }
        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(loaded.frameGraph, &error) else {
            throw failure("executor", error)
        }
        defer { we_scene_frame_executor_destroy(executor) }
        XCTAssertEqual(we_scene_frame_executor_width(executor), 0)
        XCTAssertEqual(we_scene_frame_executor_height(executor), 0)

        var inputs = WESceneFrameInputs(
            pointer_x: 0.5, pointer_y: 0.5, time_seconds: 0,
            frame_time_seconds: 1.0 / 60.0
        )
        XCTAssertEqual(we_scene_frame_executor_render(executor, &inputs, &error), 0)
        XCTAssertTrue(errorMessage(error).contains("requires host drawable pixel dimensions"))
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                executor, &inputs, 10, 6,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(executor), 10)
        XCTAssertEqual(we_scene_frame_executor_height(executor), 6)
        XCTAssertEqual(we_scene_frame_executor_rgba8_byte_count(executor), 240)

        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                executor, &inputs, 20, 5,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(executor), 20)
        XCTAssertEqual(we_scene_frame_executor_height(executor), 5)
        XCTAssertEqual(we_scene_frame_executor_rgba8_byte_count(executor), 400)
    }

    func testReplayRequiresAPreviouslySuccessfulFrame() throws {
        let loaded = try loadEmptyFrameGraph(soundOnly: true, projectionAuto: true)
        defer {
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }
        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(loaded.frameGraph, &error) else {
            throw failure("executor", error)
        }
        defer { we_scene_frame_executor_destroy(executor) }

        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(executor, 10, 6, &error),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        XCTAssertTrue(errorMessage(error).contains("successful"))
        we_scene_runtime_error_destroy(error)
    }

    func testAutomaticProjectionReplayResizesWithoutRefreshingSoundSnapshot() throws {
        let loaded = try loadEmptyFrameGraph(soundOnly: true, projectionAuto: true)
        defer {
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }
        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(loaded.frameGraph, &error) else {
            throw failure("executor", error)
        }
        defer { we_scene_frame_executor_destroy(executor) }
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5, pointer_y: 0.5, time_seconds: 1,
            frame_time_seconds: 1.0 / 60.0
        )

        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                executor, &inputs, 10, 6,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(executor), 10)
        XCTAssertEqual(we_scene_frame_executor_height(executor), 6)

        var soundCount = 0
        var sound = WESceneFrameSoundInfo()
        var source: UnsafePointer<CChar>?
        var renderedRevision: UInt64 = .max
        XCTAssertEqual(we_scene_frame_executor_sound_count(executor, &soundCount, &error), 1)
        XCTAssertEqual(soundCount, 1)
        XCTAssertEqual(we_scene_frame_executor_sound_info(executor, 0, &sound, &error), 1)
        XCTAssertEqual(sound.object_index, 0)
        XCTAssertEqual(sound.object_id, 42)
        XCTAssertEqual(sound.visible, 1)
        XCTAssertEqual(sound.source_count, 2)
        XCTAssertEqual(sound.playback_mode, WE_SCENE_FRAME_SOUND_PLAYBACK_LOOP)
        XCTAssertEqual(sound.volume, 0.25, accuracy: 1e-6)
        XCTAssertEqual(sound.start_silent, 0)
        XCTAssertEqual(sound.mute_in_editor, 0)
        XCTAssertEqual(sound.minimum_time, 0)
        XCTAssertEqual(sound.maximum_time, 0)
        XCTAssertEqual(we_scene_frame_executor_sound_source(executor, 0, 1, &source, &error), 1)
        XCTAssertEqual(source.map(String.init(cString:)), "sounds/second.ogg")
        XCTAssertEqual(
            we_scene_frame_executor_last_model_revision(executor, &renderedRevision, &error),
            1
        )
        XCTAssertEqual(renderedRevision, 0)

        var changedVolume = WEScenePropertyValue(
            type: WE_SCENE_VALUE_NUMBER, boolean_value: 0, integer_value: 0,
            number_value: 0.75, string_value: nil, component_count: 0,
            vector_value: WESceneVector4()
        )
        XCTAssertEqual("volume".withCString {
            we_scene_model_set_property_value(loaded.model, $0, &changedVolume, &error)
        }, 1, errorMessage(error))

        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(executor, 20, 5, &error),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(executor), 20)
        XCTAssertEqual(we_scene_frame_executor_height(executor), 5)
        XCTAssertEqual(we_scene_frame_executor_rgba8_byte_count(executor), 400)
        XCTAssertEqual(we_scene_frame_executor_sound_count(executor, &soundCount, &error), 1)
        XCTAssertEqual(soundCount, 1)
        XCTAssertEqual(we_scene_frame_executor_sound_info(executor, 0, &sound, &error), 1)
        XCTAssertEqual(sound.object_index, 0)
        XCTAssertEqual(sound.object_id, 42)
        XCTAssertEqual(sound.visible, 1)
        XCTAssertEqual(sound.source_count, 2)
        XCTAssertEqual(sound.playback_mode, WE_SCENE_FRAME_SOUND_PLAYBACK_LOOP)
        XCTAssertEqual(sound.volume, 0.25, accuracy: 1e-6)
        XCTAssertEqual(sound.start_silent, 0)
        XCTAssertEqual(sound.mute_in_editor, 0)
        XCTAssertEqual(sound.minimum_time, 0)
        XCTAssertEqual(sound.maximum_time, 0)
        XCTAssertEqual(we_scene_frame_executor_sound_source(executor, 0, 1, &source, &error), 1)
        XCTAssertEqual(source.map(String.init(cString:)), "sounds/second.ogg")
        XCTAssertEqual(
            we_scene_frame_executor_last_model_revision(executor, &renderedRevision, &error),
            1
        )
        XCTAssertEqual(renderedRevision, 0)

        inputs.time_seconds = 2
        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                executor, &inputs, 20, 5,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_sound_info(executor, 0, &sound, &error), 1)
        XCTAssertEqual(sound.volume, 0.75, accuracy: 1e-6)
        XCTAssertEqual(
            we_scene_frame_executor_last_model_revision(executor, &renderedRevision, &error),
            1
        )
        XCTAssertEqual(renderedRevision, 1)
    }

    func testFixedProjectionIgnoresDrawableSizeDuringRender() throws {
        let loaded = try loadEmptyFrameGraph(soundOnly: true)
        defer {
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }
        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(loaded.frameGraph, &error) else {
            throw failure("executor", error)
        }
        defer { we_scene_frame_executor_destroy(executor) }
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5, pointer_y: 0.5, time_seconds: 0,
            frame_time_seconds: 1.0 / 60.0
        )

        XCTAssertEqual(
            we_scene_frame_executor_render_for_drawable(
                executor, &inputs, 100, 50,
                WE_SCENE_PRESENTATION_ASPECT_FILL, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(executor), 8)
        XCTAssertEqual(we_scene_frame_executor_height(executor), 8)
        XCTAssertEqual(we_scene_frame_executor_rgba8_byte_count(executor), 256)
        XCTAssertEqual(
            we_scene_frame_executor_replay_for_drawable(executor, 200, 40, &error),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(executor), 8)
        XCTAssertEqual(we_scene_frame_executor_height(executor), 8)
    }

    func testStartSilentSoundWarningDoesNotBlockFramePublication() throws {
        let loaded = try loadEmptyFrameGraph(soundOnly: true, startSilent: true)
        defer {
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }
        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(loaded.frameGraph, &error) else {
            throw failure("executor", error)
        }
        defer { we_scene_frame_executor_destroy(executor) }
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5, pointer_y: 0.5, time_seconds: 0,
            frame_time_seconds: 1.0 / 60.0
        )
        guard we_scene_frame_executor_render(executor, &inputs, &error) == 1 else {
            throw failure("render", error)
        }
        XCTAssertNil(error)
        var count = 0
        guard we_scene_frame_executor_sound_count(executor, &count, &error) == 1 else {
            throw failure("sound count", error)
        }
        XCTAssertEqual(count, 1)
        var sound = WESceneFrameSoundInfo()
        guard we_scene_frame_executor_sound_info(
            executor, 0, &sound, &error
        ) == 1 else {
            throw failure("sound info", error)
        }
        XCTAssertEqual(sound.start_silent, 1)
    }

    private struct LoadedFrameGraph {
        let root: URL
        let runtime: WESceneRuntimeRef
        let model: WESceneModelRef
        let graph: WESceneGraphRef
        let frameGraph: WESceneFrameGraphRef
    }

    private func loadEmptyFrameGraph(
        soundOnly: Bool = false,
        startSilent: Bool = false,
        projectionAuto: Bool = false
    ) throws -> LoadedFrameGraph {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(at: shaders, withIntermediateDirectories: true)

        let properties: [String: Any] = soundOnly ? [
            "volume": [
                "fraction": true, "max": 1.0, "min": 0.0, "step": 0.05,
                "text": "Volume", "type": "slider", "value": 0.25,
            ],
        ] : [:]
        let project: [String: Any] = [
            "file": "scene.json",
            "general": ["properties": properties],
            "title": "Empty executor fixture",
            "type": "scene",
            "version": 2,
        ]
        let objects: [[String: Any]] = soundOnly ? [[
            "id": 42,
            "name": "Sound",
            "sound": ["sounds/first.mp3", "sounds/second.ogg"],
            "playbackmode": "loop",
            "startsilent": startSilent,
            "visible": true,
            "volume": ["user": "volume", "value": 0.25],
        ]] : [[
            "id": 1,
            "image": "models/image.json",
            "name": "Image",
            "origin": "4 4 0",
            "size": "8 8",
            "visible": true,
        ]]
        let scene: [String: Any] = [
            "camera": ["center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"],
            "general": [
                "clearcolor": "0 0 0 0",
                "orthogonalprojection": projectionAuto
                    ? ["auto": true]
                    : ["height": 8, "width": 8],
            ],
            "objects": objects,
            "version": 1,
        ]
        let model: [String: Any] = ["material": "materials/image.json"]
        let material: [String: Any] = [
            "passes": [["shader": "fixture", "textures": ["fixture"]]],
        ]
        let entries = try [
            ("materials/image.json", JSONSerialization.data(withJSONObject: material, options: [.sortedKeys])),
            ("models/image.json", JSONSerialization.data(withJSONObject: model, options: [.sortedKeys])),
            ("project.json", JSONSerialization.data(withJSONObject: project, options: [.sortedKeys])),
            ("scene.json", JSONSerialization.data(withJSONObject: scene, options: [.sortedKeys])),
        ]
        try makePackage(entries).write(to: package)

        var error: WESceneRuntimeErrorRef?
        guard let runtime = assets.path.withCString({ assetsPath in
            package.path.withCString { packagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsPath,
                    scene_package_path: packagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }) else {
            throw failure("runtime", error)
        }
        guard let model = "project.json".withCString({
            we_scene_runtime_model_create(runtime, $0, &error)
        }) else {
            we_scene_runtime_destroy(runtime)
            throw failure("model", error)
        }
        guard let graph = we_scene_model_graph_create(model, &error) else {
            we_scene_model_destroy(model)
            we_scene_runtime_destroy(runtime)
            throw failure("graph", error)
        }
        guard let frameGraph = we_scene_graph_frame_graph_create(graph, &error) else {
            we_scene_graph_destroy(graph)
            we_scene_model_destroy(model)
            we_scene_runtime_destroy(runtime)
            throw failure("frame graph", error)
        }
        return LoadedFrameGraph(
            root: root,
            runtime: runtime,
            model: model,
            graph: graph,
            frameGraph: frameGraph
        )
    }

    private func failure(_ phase: String, _ error: WESceneRuntimeErrorRef?) -> Error {
        let message = errorMessage(error)
        we_scene_runtime_error_destroy(error)
        return ExecutorBridgeFailure.creation("\(phase): \(message)")
    }

    private func errorMessage(_ error: WESceneRuntimeErrorRef?) -> String {
        we_scene_runtime_error_message(error).map(String.init(cString:)) ?? "No error"
    }

    private func makePackage(_ entries: [(String, Data)]) -> Data {
        var table = Data()
        var payload = Data()
        appendUInt32(8, to: &table)
        table.append(contentsOf: Array("PKGV0001".utf8))
        appendUInt32(UInt32(entries.count), to: &table)
        for (path, bytes) in entries {
            let pathBytes = Array(path.utf8)
            appendUInt32(UInt32(pathBytes.count), to: &table)
            table.append(contentsOf: pathBytes)
            appendUInt32(UInt32(payload.count), to: &table)
            appendUInt32(UInt32(bytes.count), to: &table)
            payload.append(bytes)
        }
        table.append(payload)
        return table
    }

    private func appendUInt32(_ value: UInt32, to data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
        data.append(UInt8(truncatingIfNeeded: value >> 16))
        data.append(UInt8(truncatingIfNeeded: value >> 24))
    }
}

private enum ExecutorBridgeFailure: Error {
    case creation(String)
}
