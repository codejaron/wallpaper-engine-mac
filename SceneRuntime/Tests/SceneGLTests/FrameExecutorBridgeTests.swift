import Foundation
import SceneRuntimeBridge
import XCTest

final class FrameExecutorBridgeTests: XCTestCase {
    func testRenderableMayUseGroupAsRuntimeParent() throws {
        let loaded = try loadEmptyFrameGraph(sceneObjects: [
            [
                "id": 139,
                "name": "Transform group",
                "origin": "2 3 0",
                "scale": "2 2 1",
                "visible": true,
            ],
            [
                "id": 8,
                "image": "models/image.json",
                "name": "Grouped image",
                "origin": "1 1 0",
                "parent": 139,
                "size": "2 2",
                "visible": true,
            ],
        ])
        defer {
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }

        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(
            loaded.frameGraph, &error
        ) else {
            throw failure("executor", error)
        }
        defer { we_scene_frame_executor_destroy(executor) }

        var inputs = WESceneFrameInputs(
            pointer_x: 0.5,
            pointer_y: 0.5,
            time_seconds: 0,
            frame_time_seconds: 1.0 / 60.0
        )
        XCTAssertEqual(
            we_scene_frame_executor_render(executor, &inputs, &error),
            1,
            errorMessage(error)
        )
    }

    func testRandomAndSingleSoundModesReachFrameSnapshotWithoutRuntimeIssue() throws {
        let modes: [(String, WESceneFrameSoundPlaybackMode)] = [
            ("random", WE_SCENE_FRAME_SOUND_PLAYBACK_RANDOM),
            ("single", WE_SCENE_FRAME_SOUND_PLAYBACK_SINGLE),
        ]
        for (mode, expectedMode) in modes {
            let loaded = try loadEmptyFrameGraph(
                soundOnly: true,
                playbackMode: mode
            )
            defer {
                we_scene_frame_graph_destroy(loaded.frameGraph)
                we_scene_graph_destroy(loaded.graph)
                we_scene_model_destroy(loaded.model)
                we_scene_runtime_destroy(loaded.runtime)
                try? FileManager.default.removeItem(at: loaded.root)
            }

            var error: WESceneRuntimeErrorRef?
            guard let executor = we_scene_frame_executor_create(
                loaded.frameGraph, &error
            ) else {
                throw failure("executor", error)
            }
            defer { we_scene_frame_executor_destroy(executor) }

            var inputs = WESceneFrameInputs(
                pointer_x: 0.5,
                pointer_y: 0.5,
                time_seconds: 0,
                frame_time_seconds: 1.0 / 60.0
            )
            XCTAssertEqual(
                we_scene_frame_executor_render(executor, &inputs, &error),
                1,
                "mode: \(mode), error: \(errorMessage(error))"
            )

            var soundCount = 0
            XCTAssertEqual(
                we_scene_frame_executor_sound_count(
                    executor, &soundCount, &error
                ),
                1,
                errorMessage(error)
            )
            XCTAssertEqual(soundCount, 1)

            var sound = WESceneFrameSoundInfo()
            XCTAssertEqual(
                we_scene_frame_executor_sound_info(
                    executor, 0, &sound, &error
                ),
                1,
                errorMessage(error)
            )
            XCTAssertEqual(
                sound.playback_mode,
                expectedMode,
                "mode \(mode) must cross the frame bridge unchanged"
            )

            var issueCount = 99
            XCTAssertEqual(
                we_scene_frame_executor_issue_count(
                    executor, &issueCount, &error
                ),
                1,
                errorMessage(error)
            )
            XCTAssertEqual(issueCount, 0, "mode: \(mode)")
        }
    }

    func testUnknownSoundPlaybackModeFailsAtModelBoundary() {
        XCTAssertThrowsError(try loadEmptyFrameGraph(
            soundOnly: true,
            playbackMode: "once"
        )) { error in
            guard case let ExecutorBridgeFailure.creation(message) = error else {
                return XCTFail("Unexpected error: \(error)")
            }
            XCTAssertTrue(message.contains("model:"))
            XCTAssertTrue(message.contains("/objects/0/playbackmode"))
            XCTAssertTrue(message.contains("loop, random, or single"))
        }
    }

    func testLightCookieSamplerMetadataResolvesTheLinuxRuntimeAlias() throws {
        let loaded = try loadLightCookieFrameGraph()
        defer {
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }

        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(
            loaded.frameGraph, &error
        ) else {
            throw failure("executor", error)
        }
        defer { we_scene_frame_executor_destroy(executor) }

        var inputs = WESceneFrameInputs(
            pointer_x: 0.5,
            pointer_y: 0.5,
            time_seconds: 0.0,
            frame_time_seconds: 1.0 / 60.0
        )
        guard we_scene_frame_executor_render(executor, &inputs, &error) == 1 else {
            throw failure("render", error)
        }
        XCTAssertNil(error)

        var issueCount = 99
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(executor, &issueCount, &error),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(issueCount, 0)
        XCTAssertNil(error)

        var pixels = [UInt8](
            repeating: 0x7f,
            count: we_scene_frame_executor_rgba8_byte_count(executor)
        )
        guard pixels.withUnsafeMutableBytes({ bytes in
            we_scene_frame_executor_read_rgba8(
                executor,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count,
                &error
            )
        }) == 1 else {
            throw failure("readback", error)
        }
        XCTAssertNil(error)
        XCTAssertTrue(
            pixels.withUnsafeBytes { raw in
                stride(from: 0, to: raw.count, by: 4).allSatisfy { offset in
                    raw[offset] == 0 &&
                        raw[offset + 1] == 0 &&
                        raw[offset + 2] == 0 &&
                        raw[offset + 3] == 255
                }
            }
        )
    }

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

    func testMediaSnapshotSetterValidatesAndCopiesHostState() throws {
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

        XCTAssertEqual(
            we_scene_frame_executor_set_media_snapshot(executor, nil, &error),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        var unavailable = WESceneMediaSnapshot(
            status_revision: 1,
            metadata_revision: 1,
            playback_revision: 1,
            timeline_revision: 1,
            thumbnail_revision: 1,
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
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_set_media_snapshot(
                executor, &unavailable, &error
            ),
            1,
            errorMessage(error)
        )

        unavailable.available = 2
        XCTAssertEqual(
            we_scene_frame_executor_set_media_snapshot(
                executor, &unavailable, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_clear_media_snapshot(executor, &error),
            1,
            errorMessage(error)
        )
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

    func testViewportEntryPointsUseCanvasProjectionAndInvalidateOnBadLayout() throws {
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
            pointer_x: 0.5, pointer_y: 0.5, time_seconds: 0,
            frame_time_seconds: 1.0 / 60.0
        )
        var viewport = WEScenePresentationViewport(
            virtual_canvas_width: 20,
            virtual_canvas_height: 10,
            viewport_x: 10,
            viewport_y: 0,
            viewport_width: 10,
            viewport_height: 10,
            drawable_width: 8,
            drawable_height: 8
        )
        XCTAssertEqual(
            we_scene_frame_executor_render_for_viewport(
                executor, &inputs, &viewport,
                WE_SCENE_PRESENTATION_STRETCH, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_frame_executor_width(executor), 20)
        XCTAssertEqual(we_scene_frame_executor_height(executor), 10)

        var soundCount = 0
        XCTAssertEqual(
            we_scene_frame_executor_sound_count(executor, &soundCount, &error),
            1
        )
        XCTAssertEqual(soundCount, 1)

        viewport.viewport_x = 15
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_render_for_viewport(
                executor, &inputs, &viewport,
                WE_SCENE_PRESENTATION_STRETCH, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        XCTAssertTrue(errorMessage(error).contains("viewport"))
        we_scene_runtime_error_destroy(error)

        error = nil
        soundCount = 99
        XCTAssertEqual(
            we_scene_frame_executor_sound_count(executor, &soundCount, &error),
            0
        )
        XCTAssertEqual(soundCount, 99)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE
        )
        we_scene_runtime_error_destroy(error)

        viewport.viewport_x = 10
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_render_for_viewport(
                executor, &inputs, &viewport,
                WE_SCENE_PRESENTATION_STRETCH, &error
            ),
            1,
            errorMessage(error)
        )
        viewport.viewport_x = 15
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_replay_for_viewport(
                executor, &viewport, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        viewport.viewport_x = 10
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_render_for_viewport(
                executor, &inputs, &viewport,
                WE_SCENE_PRESENTATION_STRETCH, &error
            ),
            1,
            errorMessage(error)
        )

        // Present validates the viewport at the C boundary but preserves the
        // published frame when presentation itself is unavailable.
        viewport.viewport_x = 15
        error = nil
        XCTAssertEqual(
            we_scene_frame_executor_present_for_viewport(
                executor, &viewport,
                WE_SCENE_PRESENTATION_STRETCH, &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        we_scene_runtime_error_destroy(error)

        error = nil
        soundCount = 0
        XCTAssertEqual(
            we_scene_frame_executor_sound_count(executor, &soundCount, &error),
            1
        )
        XCTAssertEqual(soundCount, 1)
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

    func testStartSilentSoundIsSupportedWithoutRuntimeIssue() throws {
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
        var issueCount = 99
        XCTAssertEqual(
            we_scene_frame_executor_issue_count(
                executor, &issueCount, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(issueCount, 0)
    }

    func testSoundScriptCommandsUseGenerationsAndIsPlayingUsesHostTruth() throws {
        let loaded = try loadEmptyFrameGraph(
            soundOnly: true,
            startSilent: true,
            soundScript: """
            export function update(value) {
                if (engine.runtime < 1) thisLayer.play();
                else if (engine.runtime < 2) thisLayer.pause();
                else if (engine.runtime < 3) thisLayer.play();
                else thisLayer.stop();
                return thisLayer.isPlaying();
            }
            """
        )
        defer {
            we_scene_frame_graph_destroy(loaded.frameGraph)
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }
        var error: WESceneRuntimeErrorRef?
        guard let executor = we_scene_frame_executor_create(
            loaded.frameGraph, &error
        ) else {
            throw failure("executor", error)
        }
        defer { we_scene_frame_executor_destroy(executor) }
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5,
            pointer_y: 0.5,
            time_seconds: 0,
            frame_time_seconds: 1.0 / 60.0
        )

        func render(_ time: Double) throws -> WESceneFrameSoundInfo {
            inputs.time_seconds = time
            guard we_scene_frame_executor_render(
                executor, &inputs, &error
            ) == 1 else {
                throw failure("render", error)
            }
            var info = WESceneFrameSoundInfo()
            guard we_scene_frame_executor_sound_info(
                executor, 0, &info, &error
            ) == 1 else {
                throw failure("sound info", error)
            }
            return info
        }

        func setRuntimeState(
            _ state: WESceneSoundRuntimeState,
            position: Double
        ) throws {
            var input = WESceneSoundRuntimeStateInput(
                object_id: 42,
                state: state,
                position: position
            )
            guard we_scene_frame_executor_set_sound_runtime_states(
                executor, &input, 1, &error
            ) == 1 else {
                throw failure("sound runtime state", error)
            }
        }

        var sound = try render(0)
        XCTAssertEqual(sound.visible, 0)
        XCTAssertEqual(sound.playback_command, WE_SCENE_FRAME_SOUND_COMMAND_PLAY)
        XCTAssertEqual(sound.playback_command_generation, 1)

        try setRuntimeState(WE_SCENE_SOUND_RUNTIME_PLAYING, position: 0.5)
        sound = try render(0.5)
        XCTAssertEqual(sound.visible, 1)
        XCTAssertEqual(sound.playback_command, WE_SCENE_FRAME_SOUND_COMMAND_PLAY)
        XCTAssertEqual(sound.playback_command_generation, 1)

        try setRuntimeState(WE_SCENE_SOUND_RUNTIME_PLAYING, position: 0.75)
        sound = try render(1)
        XCTAssertEqual(sound.visible, 1)
        XCTAssertEqual(sound.playback_command, WE_SCENE_FRAME_SOUND_COMMAND_PAUSE)
        XCTAssertEqual(sound.playback_command_generation, 2)

        try setRuntimeState(WE_SCENE_SOUND_RUNTIME_PAUSED, position: 0.75)
        sound = try render(1.5)
        XCTAssertEqual(sound.visible, 0)
        XCTAssertEqual(sound.playback_command, WE_SCENE_FRAME_SOUND_COMMAND_PAUSE)
        XCTAssertEqual(sound.playback_command_generation, 2)

        try setRuntimeState(WE_SCENE_SOUND_RUNTIME_PAUSED, position: 0.75)
        sound = try render(2)
        XCTAssertEqual(sound.visible, 0)
        XCTAssertEqual(sound.playback_command, WE_SCENE_FRAME_SOUND_COMMAND_PLAY)
        XCTAssertEqual(sound.playback_command_generation, 3)

        try setRuntimeState(WE_SCENE_SOUND_RUNTIME_PLAYING, position: 1)
        sound = try render(3)
        XCTAssertEqual(sound.visible, 1)
        XCTAssertEqual(sound.playback_command, WE_SCENE_FRAME_SOUND_COMMAND_STOP)
        XCTAssertEqual(sound.playback_command_generation, 4)
    }

    private struct LoadedFrameGraph {
        let root: URL
        let runtime: WESceneRuntimeRef
        let model: WESceneModelRef
        let graph: WESceneGraphRef
        let frameGraph: WESceneFrameGraphRef
    }

    private func loadLightCookieFrameGraph() throws -> LoadedFrameGraph {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: shaders,
            withIntermediateDirectories: true
        )
        try Data("""
        attribute vec3 a_Position;
        attribute vec2 a_TexCoord;
        uniform mat4 g_ModelViewProjectionMatrix;
        void main() {
            gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);
        }
        """.utf8).write(to: shaders.appendingPathComponent("fixture.vert"))
        try Data("""
        uniform sampler2D g_Texture1; // {"default":"_alias_lightCookie"}
        void main() {
            vec4 cookie = texSample2D(g_Texture1, vec2(0.5, 0.5));
            gl_FragColor = vec4(cookie.rgb, 1.0);
        }
        """.utf8).write(to: shaders.appendingPathComponent("fixture.frag"))

        let project: [String: Any] = [
            "file": "scene.json",
            "general": ["properties": [String: Any]()],
            "title": "Light cookie alias fixture",
            "type": "scene",
            "version": 2,
        ]
        let scene: [String: Any] = [
            "camera": [
                "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0",
            ],
            "general": [
                "clearcolor": "0 0 0 0",
                "orthogonalprojection": ["height": 8, "width": 8],
            ],
            "objects": [[
                "id": 1,
                "image": "models/image.json",
                "name": "Image",
                "origin": "4 4 0",
                "size": "8 8",
                "visible": true,
            ]],
            "version": 1,
        ]
        let model: [String: Any] = [
            "material": "materials/image.json",
            "solidlayer": true,
        ]
        let material: [String: Any] = [
            "passes": [[
                "blending": "normal",
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": "fixture",
            ]],
        ]
        let jsonData: (Any) throws -> Data = {
            try JSONSerialization.data(withJSONObject: $0, options: [.sortedKeys])
        }
        let entries = try [
            ("materials/image.json", jsonData(material)),
            ("models/image.json", jsonData(model)),
            ("project.json", jsonData(project)),
            ("scene.json", jsonData(scene)),
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
        guard let modelHandle = "project.json".withCString({
            we_scene_runtime_model_create(runtime, $0, &error)
        }) else {
            we_scene_runtime_destroy(runtime)
            throw failure("model", error)
        }
        guard let graph = we_scene_model_graph_create(modelHandle, &error) else {
            we_scene_model_destroy(modelHandle)
            we_scene_runtime_destroy(runtime)
            throw failure("graph", error)
        }
        guard let frameGraph = we_scene_graph_frame_graph_create(graph, &error) else {
            we_scene_graph_destroy(graph)
            we_scene_model_destroy(modelHandle)
            we_scene_runtime_destroy(runtime)
            throw failure("frame graph", error)
        }
        return LoadedFrameGraph(
            root: root,
            runtime: runtime,
            model: modelHandle,
            graph: graph,
            frameGraph: frameGraph
        )
    }

    private func loadEmptyFrameGraph(
        soundOnly: Bool = false,
        startSilent: Bool = false,
        projectionAuto: Bool = false,
        soundScript: String? = nil,
        playbackMode: String = "loop",
        sceneObjects: [[String: Any]]? = nil
    ) throws -> LoadedFrameGraph {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        var keepFixture = false
        defer {
            if !keepFixture {
                try? FileManager.default.removeItem(at: root)
            }
        }
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
        let objects: [[String: Any]]
        if let sceneObjects {
            objects = sceneObjects
        } else if soundOnly {
            var soundObject: [String: Any] = [
                "id": 42,
                "name": "Sound",
                "sound": ["sounds/first.mp3", "sounds/second.ogg"],
                "playbackmode": playbackMode,
                "startsilent": startSilent,
                "visible": true,
                "volume": ["user": "volume", "value": 0.25],
            ]
            if let soundScript {
                soundObject["visible"] = [
                    "value": true,
                    "script": soundScript,
                ]
            }
            objects = [soundObject]
        } else {
            objects = [[
                "id": 1,
                "image": "models/image.json",
                "name": "Image",
                "origin": "4 4 0",
                "size": "8 8",
                "visible": true,
            ]]
        }
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
        let loaded = LoadedFrameGraph(
            root: root,
            runtime: runtime,
            model: model,
            graph: graph,
            frameGraph: frameGraph
        )
        keepFixture = true
        return loaded
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
