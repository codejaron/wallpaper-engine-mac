import Foundation
import SceneRuntimeBridge
import XCTest

final class SceneProjectIntegrationTests: XCTestCase {
    func testConfiguredProjectRendersCompleteMetalFrameChain() throws {
        let environment = ProcessInfo.processInfo.environment
        let variant = environment["WE_PROJECT_VARIANT"]
        let particleBranch = environment["WE_PARTICLE_BRANCH"]
        guard let assetsDirectory = environment["WE_ASSETS_DIR"],
              !assetsDirectory.isEmpty,
              let projectDirectory = environment["WE_PROJECT_DIR"],
              !projectDirectory.isEmpty else {
            throw XCTSkip("WE_ASSETS_DIR and WE_PROJECT_DIR are required")
        }

        let packagePath = URL(
            fileURLWithPath: projectDirectory,
            isDirectory: true
        ).appendingPathComponent("scene.pkg").path
        var error: WESceneRuntimeErrorRef?
        let failure = { (operation: String) -> NSError in
            let message = we_scene_runtime_error_message(error)
                .map(String.init(cString:)) ?? "No Scene runtime error"
            we_scene_runtime_error_destroy(error)
            error = nil
            return NSError(
                domain: "SceneProjectIntegrationTests",
                code: 1,
                userInfo: [
                    NSLocalizedDescriptionKey: "\(operation) failed: \(message)",
                ]
            )
        }

        guard let runtime = assetsDirectory.withCString({ assets in
            packagePath.withCString { package in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assets,
                    scene_package_path: package
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }) else {
            throw failure("Runtime creation")
        }
        defer { we_scene_runtime_destroy(runtime) }

        guard let model = "project.json".withCString({
            we_scene_runtime_model_create(runtime, $0, &error)
        }) else {
            throw failure("Model loading")
        }
        defer { we_scene_model_destroy(model) }

        let graph = "integration-wallpaper".withCString { wallpaperIdentity in
            "integration-screen".withCString { screenIdentity in
                var configuration = WESceneLocalStorageConfiguration(
                    wallpaper_identity: wallpaperIdentity,
                    screen_identity: screenIdentity
                )
                return we_scene_model_graph_create_with_local_storage(
                    model,
                    &configuration,
                    &error
                )
            }
        }
        guard let graph else {
            throw failure("Graph creation")
        }
        defer { we_scene_graph_destroy(graph) }

        guard let frameGraph = we_scene_graph_frame_graph_create(graph, &error) else {
            throw failure("Frame graph creation")
        }
        defer { we_scene_frame_graph_destroy(frameGraph) }

        guard let executor = we_scene_frame_executor_create(frameGraph, &error) else {
            throw failure("Metal executor creation")
        }
        defer { we_scene_frame_executor_destroy(executor) }

        let setBoolean = { (key: String, value: Bool) throws in
            var property = WEScenePropertyValue(
                type: WE_SCENE_VALUE_BOOLEAN,
                boolean_value: value ? 1 : 0,
                integer_value: 0,
                number_value: 0,
                string_value: nil,
                component_count: 0,
                vector_value: WESceneVector4()
            )
            guard key.withCString({
                we_scene_model_set_property_value(
                    model, $0, &property, &error
                )
            }) == 1 else {
                throw failure("Property \(key) setup")
            }
        }
        let setString = { (key: String, value: String) throws in
            var property = WEScenePropertyValue(
                type: WE_SCENE_VALUE_STRING,
                boolean_value: 0,
                integer_value: 0,
                number_value: 0,
                string_value: nil,
                component_count: 0,
                vector_value: WESceneVector4()
            )
            let result = value.withCString { string in
                property.string_value = string
                return key.withCString {
                    we_scene_model_set_property_value(
                        model, $0, &property, &error
                    )
                }
            }
            guard result == 1 else {
                throw failure("Property \(key) setup")
            }
        }
        let setMediaSnapshot = {
            (title: String, artist: String, hasThumbnail: Bool) throws in
            let values = [title, artist, "music", "", "", "", ""]
            var strings: [UnsafeMutablePointer<CChar>] = []
            defer { strings.forEach { free($0) } }
            for value in values {
                guard let result = strdup(value) else {
                    throw NSError(
                        domain: "SceneProjectIntegrationTests",
                        code: 3,
                        userInfo: [
                            NSLocalizedDescriptionKey:
                                "Allocating media snapshot input failed",
                        ]
                    )
                }
                strings.append(result)
            }
            var snapshot = WESceneMediaSnapshot(
                status_revision: 1,
                metadata_revision: 1,
                playback_revision: 1,
                timeline_revision: 1,
                thumbnail_revision: 1,
                available: 1,
                playback_state: WE_SCENE_MEDIA_PLAYING,
                title: UnsafePointer(strings[0]),
                artist: UnsafePointer(strings[1]),
                content_type: UnsafePointer(strings[2]),
                album_title: UnsafePointer(strings[3]),
                sub_title: UnsafePointer(strings[4]),
                album_artist: UnsafePointer(strings[5]),
                genres: UnsafePointer(strings[6]),
                position: 30,
                duration: 240,
                has_thumbnail: hasThumbnail ? 1 : 0,
                primary_color: (0.1, 0.2, 0.3),
                secondary_color: (0.2, 0.3, 0.4),
                tertiary_color: (0.3, 0.4, 0.5),
                text_color: (0.05, 0.05, 0.05),
                high_contrast_color: (1, 1, 1)
            )
            guard we_scene_frame_executor_set_media_snapshot(
                executor, &snapshot, &error
            ) == 1 else {
                throw failure("Media snapshot setup")
            }
        }
        let setMediaThumbnail = { () throws in
            var pixels: [UInt8] = [
                48, 72, 88, 255, 96, 112, 120, 255,
                144, 152, 152, 255, 208, 208, 192, 255,
            ]
            let result = pixels.withUnsafeMutableBufferPointer { storage in
                var thumbnail = WESceneMediaThumbnailRGBA8(
                    revision: 1,
                    width: 2,
                    height: 2,
                    bytes_per_row: 8,
                    pixels: storage.baseAddress,
                    pixel_length: storage.count
                )
                return we_scene_frame_executor_set_media_thumbnail_rgba8(
                    executor, &thumbnail, &error
                )
            }
            guard result == 1 else {
                throw failure("Media thumbnail setup")
            }
        }
        if variant == "reported" {
            let particleProperties = [
                "birds", "blinkingstars", "shootingstar", "wind", "dust",
            ]
            for key in particleProperties {
                try setBoolean(
                    key,
                    particleBranch.map { $0 == key } ?? true
                )
            }
            for (key, value) in [
                "soundbar": false,
                "daydatetime": true,
                "mediainfo": false,
            ] {
                try setBoolean(key, value)
            }
            try setString("wall", "2")
            try setString("clock", "0")
        } else if variant == "media" {
            try setBoolean("mediainfo", true)
            try setMediaSnapshot("南山南", "马頔", true)
            try setMediaThumbnail()
        } else if variant == "player" {
            try setString("newproperty2", "3")
            try setString("newproperty12", "南山南")
            try setString("newproperty14", "马頔")
        }

        let drawableWidth = UInt32(environment["WE_DRAWABLE_WIDTH"] ?? "")
            ?? 1920
        let drawableHeight = UInt32(environment["WE_DRAWABLE_HEIGHT"] ?? "")
            ?? 1080

        var spectrum16Left = [Float](repeating: 0, count: 16)
        var spectrum16Right = [Float](repeating: 0, count: 16)
        var spectrum32Left = [Float](repeating: 0, count: 32)
        var spectrum32Right = [Float](repeating: 0, count: 32)
        var spectrum64Left = [Float](repeating: 0, count: 64)
        var spectrum64Right = [Float](repeating: 0, count: 64)

        let renderFrame = {
            (frame: Int, time: Double, delta: Double) throws in
            guard we_scene_frame_executor_set_pointer_state(
                executor,
                1,
                (4...7).contains(frame) ? 1 : 0,
                &error
            ) == 1 else {
                throw failure("Frame \(frame) pointer state")
            }
            var inputs = WESceneFrameInputs(
                pointer_x: Double(frame % 12) / 11.0,
                pointer_y: 1.0 - Double(frame % 12) / 11.0,
                time_seconds: time,
                frame_time_seconds: delta
            )
            let renderResult = spectrum16Left.withUnsafeMutableBufferPointer { left16 in
                spectrum16Right.withUnsafeMutableBufferPointer { right16 in
                    spectrum32Left.withUnsafeMutableBufferPointer { left32 in
                        spectrum32Right.withUnsafeMutableBufferPointer { right32 in
                            spectrum64Left.withUnsafeMutableBufferPointer { left64 in
                                spectrum64Right.withUnsafeMutableBufferPointer { right64 in
                                    var spectrum = WESceneAudioSpectrumInputs(
                                        spectrum_16_left: left16.baseAddress,
                                        spectrum_16_right: right16.baseAddress,
                                        spectrum_32_left: left32.baseAddress,
                                        spectrum_32_right: right32.baseAddress,
                                        spectrum_64_left: left64.baseAddress,
                                        spectrum_64_right: right64.baseAddress
                                    )
                                    return we_scene_frame_executor_render_for_drawable_with_audio_spectrum(
                                        executor,
                                        &inputs,
                                        &spectrum,
                                        drawableWidth,
                                        drawableHeight,
                                        WE_SCENE_PRESENTATION_ASPECT_FILL,
                                        &error
                                    )
                                }
                            }
                        }
                    }
                }
            }
            guard renderResult == 1 else {
                throw failure("Frame \(frame) render")
            }

            var issueCount = 0
            guard we_scene_frame_executor_issue_count(
                executor,
                &issueCount,
                &error
            ) == 1 else {
                throw failure("Frame \(frame) issue inspection")
            }
            if issueCount != 0 {
                var messages: [String] = []
                for index in 0..<issueCount {
                    var issue = WESceneFrameExecutorIssueInfo()
                    guard we_scene_frame_executor_issue_info(
                        executor,
                        index,
                        &issue,
                        &error
                    ) == 1 else {
                        throw failure("Frame \(frame) issue reading")
                    }
                    messages.append(
                        issue.message.map(String.init(cString:)) ?? "<no message>"
                    )
                }
                throw NSError(
                    domain: "SceneProjectIntegrationTests",
                    code: 2,
                    userInfo: [
                        NSLocalizedDescriptionKey:
                            "Frame \(frame) completed with executor issues: " +
                            messages.joined(separator: " | "),
                    ]
                )
            }
        }
        let readPixels = { () throws -> [UInt8] in
            var pixels = [UInt8](
                repeating: 0,
                count: we_scene_frame_executor_rgba8_byte_count(executor)
            )
            let result = pixels.withUnsafeMutableBufferPointer { buffer in
                we_scene_frame_executor_read_rgba8(
                    executor,
                    buffer.baseAddress,
                    buffer.count,
                    &error
                )
            }
            guard result == 1 else {
                throw failure("Frame readback")
            }
            return pixels
        }

        for frame in 0..<12 {
            try renderFrame(
                frame,
                Double(frame) / 60.0,
                1.0 / 60.0
            )
        }

        if variant == "reported" {
            try renderFrame(12, 10, 10 - 11.0 / 60.0)
        }
        if variant == "reported" && particleBranch == nil {
            for key in ["birds", "blinkingstars", "shootingstar"] {
                let enabledPixels = try readPixels()
                try setBoolean(key, false)
                try renderFrame(12, 10, 0)
                let disabledPixels = try readPixels()
                XCTAssertTrue(
                    enabledPixels != disabledPixels,
                    "Enabled project particle branch '\(key)' produced no visible pixels"
                )
                try setBoolean(key, true)
                try renderFrame(12, 10, 0)
            }
            for key in ["wind", "dust", "soundbar", "daydatetime"] {
                try setBoolean(key, false)
                try renderFrame(12, 10, 0)
                try setBoolean(key, true)
                try renderFrame(12, 10, 0)
            }
            for value in ["0", "1", "2"] {
                try setString("wall", value)
                try renderFrame(12, 10, 0)
                try setString("clock", value)
                try renderFrame(12, 10, 0)
            }
            try setBoolean("soundbar", false)
            try setString("wall", "2")
            try setString("clock", "0")
            try renderFrame(12, 10, 0)
        }
        if let dumpPath = environment["WE_FRAME_DUMP_PATH"],
           !dumpPath.isEmpty {
            try Data(try readPixels()).write(
                to: URL(fileURLWithPath: dumpPath),
                options: .atomic
            )
        }
    }
}
