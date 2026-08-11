import Foundation
import SceneRuntimeBridge
import XCTest

final class SceneMetalBridgeTests: XCTestCase {
    func testOffscreenRendererDrawsAndReadsRGBA8() throws {
        var error: WESceneMetalErrorRef?
        guard let renderer = we_scene_metal_renderer_create(4, 3, &error) else {
            XCTFail("SceneMetal context creation failed: \(errorMessage(error))")
            we_scene_metal_error_destroy(error)
            return
        }
        defer { we_scene_metal_renderer_destroy(renderer) }
        XCTAssertNil(error)
        XCTAssertEqual(we_scene_metal_renderer_rgba8_byte_count(renderer), 48)

        let vertex = """
        #version 410 core
        void main() {
            const vec2 positions[3] = vec2[3](
                vec2(-1.0, -1.0),
                vec2( 3.0, -1.0),
                vec2(-1.0,  3.0)
            );
            gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
        }
        """
        let fragment = """
        #version 410 core
        layout(location = 0) out vec4 outputColor;
        void main() {
            outputColor = gl_FragCoord.y < 1.0
                ? vec4(1.0, 0.0, 1.0, 1.0)
                : vec4(0.0, 1.0, 0.0, 1.0);
        }
        """

        let compiled = vertex.withCString { vertexSource in
            fragment.withCString { fragmentSource in
                we_scene_metal_renderer_compile_program(
                    renderer,
                    vertexSource,
                    fragmentSource,
                    &error
                )
            }
        }
        XCTAssertEqual(compiled, 1, errorMessage(error))
        XCTAssertNil(error)
        XCTAssertEqual(we_scene_metal_renderer_draw(renderer, &error), 1, errorMessage(error))
        XCTAssertNil(error)

        var pixels = [UInt8](repeating: 0, count: 48)
        let read = pixels.withUnsafeMutableBytes { bytes in
            we_scene_metal_renderer_read_rgba8(
                renderer,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count,
                &error
            )
        }
        XCTAssertEqual(read, 1, errorMessage(error))
        XCTAssertNil(error)
        let greenPixel: [UInt8] = [0, 255, 0, 255]
        let magentaPixel: [UInt8] = [255, 0, 255, 255]
        let greenRow = Array(repeating: greenPixel, count: 4).flatMap { $0 }
        let magentaRow = Array(repeating: magentaPixel, count: 4).flatMap { $0 }
        XCTAssertEqual(pixels, magentaRow + greenRow + greenRow)
    }

    func testOffscreenRendererLinksWallpaperEngineVaryingsBySymbol() throws {
        let renderer = try createRenderer(width: 1, height: 1)
        defer { we_scene_metal_renderer_destroy(renderer) }
        let vertex = """
        #version 330
        varying vec4 v_Color;
        varying vec2 v_TexCoord;
        varying vec4 v_TexCoordRipple;
        void main() {
            const vec2 positions[3] = vec2[3](
                vec2(-1.0, -1.0),
                vec2( 3.0, -1.0),
                vec2(-1.0,  3.0)
            );
            gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
            v_Color = vec4(0.25, 0.5, 0.75, 1.0);
            v_TexCoord = vec2(0.125, 0.875);
            v_TexCoordRipple = vec4(0.0625);
        }
        """
        let fragment = """
        #version 330
        varying vec2 v_TexCoord;
        varying vec2 v_Scroll;
        varying vec4 v_TexCoordRipple;
        varying vec4 v_Color;
        void main() {
            gl_FragColor = v_Color
                + vec4(v_TexCoord, 0.0, 0.0) * 0.0
                + v_TexCoordRipple * 0.0;
        }
        """

        var error: WESceneMetalErrorRef?
        XCTAssertEqual(
            vertex.withCString { vertexSource in
                fragment.withCString { fragmentSource in
                    we_scene_metal_renderer_compile_program(
                        renderer,
                        vertexSource,
                        fragmentSource,
                        &error
                    )
                }
            },
            1,
            errorMessage(error)
        )
        XCTAssertNil(error)
        XCTAssertEqual(
            we_scene_metal_renderer_draw(renderer, &error),
            1,
            errorMessage(error)
        )
        XCTAssertNil(error)
    }

    func testOffscreenRendererPreservesUniformAddressSpaceInShaderHelpers() throws {
        let renderer = try createRenderer(width: 1, height: 1)
        defer { we_scene_metal_renderer_destroy(renderer) }
        let vertex = """
        #version 330
        uniform vec3 g_Offset;
        void offsetPosition(in vec3 inputPosition, out vec3 outputPosition) {
            outputPosition = inputPosition + g_Offset;
        }
        void main() {
            vec3 position;
            offsetPosition(vec3(0.0), position);
            gl_Position = vec4(position, 1.0);
        }
        """
        let fragment = """
        #version 330
        uniform vec2 g_Scale;
        vec2 scaleCoordinates(vec2 coordinates) {
            return coordinates * g_Scale;
        }
        void main() {
            gl_FragColor = vec4(scaleCoordinates(vec2(1.0)), 0.0, 1.0);
        }
        """

        var error: WESceneMetalErrorRef?
        XCTAssertEqual(
            vertex.withCString { vertexSource in
                fragment.withCString { fragmentSource in
                    we_scene_metal_renderer_compile_program(
                        renderer,
                        vertexSource,
                        fragmentSource,
                        &error
                    )
                }
            },
            1,
            errorMessage(error)
        )
        XCTAssertNil(error)
    }

    func testInvalidShaderReportsCompilationPhase() throws {
        let renderer = try createRenderer(width: 1, height: 1)
        defer { we_scene_metal_renderer_destroy(renderer) }
        var error: WESceneMetalErrorRef?
        let validVertex = "#version 410 core\nvoid main() { gl_Position = vec4(0.0); }"
        let validFragment = "#version 410 core\nout vec4 color;\nvoid main() { color = vec4(1.0); }"
        XCTAssertEqual(
            validVertex.withCString { vertexSource in
                validFragment.withCString { fragmentSource in
                    we_scene_metal_renderer_compile_program(
                        renderer,
                        vertexSource,
                        fragmentSource,
                        &error
                    )
                }
            },
            1,
            errorMessage(error)
        )
        XCTAssertNil(error)
        XCTAssertEqual(we_scene_metal_renderer_draw(renderer, &error), 1, errorMessage(error))
        XCTAssertNil(error)

        let vertex = "#version 410 core\nvoid main() { this is invalid; }"
        let fragment = "#version 410 core\nout vec4 color;\nvoid main() { color = vec4(1.0); }"

        let result = vertex.withCString { vertexSource in
            fragment.withCString { fragmentSource in
                we_scene_metal_renderer_compile_program(
                    renderer,
                    vertexSource,
                    fragmentSource,
                    &error
                )
            }
        }

        XCTAssertEqual(result, 0)
        XCTAssertEqual(we_scene_metal_error_code(error), WE_SCENE_METAL_ERROR_SHADER_COMPILATION)
        XCTAssertTrue(errorMessage(error).contains("Vertex shader compilation failed"))
        we_scene_metal_error_destroy(error)

        var drawError: WESceneMetalErrorRef?
        XCTAssertEqual(we_scene_metal_renderer_draw(renderer, &drawError), 0)
        XCTAssertEqual(we_scene_metal_error_code(drawError), WE_SCENE_METAL_ERROR_DRAW)
        XCTAssertTrue(errorMessage(drawError).contains("compiled Metal program"))
        we_scene_metal_error_destroy(drawError)

        var clearedPixels = [UInt8](repeating: 0x7f, count: 4)
        var readError: WESceneMetalErrorRef?
        XCTAssertEqual(
            clearedPixels.withUnsafeMutableBytes { bytes in
                we_scene_metal_renderer_read_rgba8(
                    renderer,
                    bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                    bytes.count,
                    &readError
                )
            },
            1,
            errorMessage(readError)
        )
        XCTAssertEqual(clearedPixels, [0, 0, 0, 0])
        XCTAssertNil(readError)
    }

    func testInvalidDimensionsAndShortReadBufferAreRejected() throws {
        var creationError: WESceneMetalErrorRef?
        XCTAssertNil(we_scene_metal_renderer_create(0, 1, &creationError))
        XCTAssertEqual(
            we_scene_metal_error_code(creationError),
            WE_SCENE_METAL_ERROR_INVALID_ARGUMENT
        )
        we_scene_metal_error_destroy(creationError)

        var allocationError: WESceneMetalErrorRef?
        XCTAssertNil(we_scene_metal_renderer_create(16_384, 16_384, &allocationError))
        XCTAssertEqual(
            we_scene_metal_error_code(allocationError),
            WE_SCENE_METAL_ERROR_INVALID_ARGUMENT
        )
        XCTAssertTrue(errorMessage(allocationError).contains("256 MiB"))
        we_scene_metal_error_destroy(allocationError)

        let renderer = try createRenderer(width: 2, height: 2)
        defer { we_scene_metal_renderer_destroy(renderer) }
        var readError: WESceneMetalErrorRef?
        var pixels = [UInt8](repeating: 0, count: 15)
        let result = pixels.withUnsafeMutableBytes { bytes in
            we_scene_metal_renderer_read_rgba8(
                renderer,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count,
                &readError
            )
        }
        XCTAssertEqual(result, 0)
        XCTAssertEqual(
            we_scene_metal_error_code(readError),
            WE_SCENE_METAL_ERROR_INVALID_ARGUMENT
        )
        we_scene_metal_error_destroy(readError)
    }


    func testOfficialTranslatedShaderProducesMetalSource() throws {
        guard let assetsPath = ProcessInfo.processInfo.environment["WE_ASSETS_DIR"],
              !assetsPath.isEmpty else {
            throw XCTSkip("WE_ASSETS_DIR is required for the driver integration contract")
        }

        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: root,
            withIntermediateDirectories: true
        )
        try makeEmptyPackage().write(to: package)
        defer { try? FileManager.default.removeItem(at: root) }

        var runtimeError: WESceneRuntimeErrorRef?
        let runtime = assetsPath.withCString { officialAssets in
            package.path.withCString { packagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: officialAssets,
                    scene_package_path: packagePath
                )
                return we_scene_runtime_create(&configuration, &runtimeError)
            }
        }
        guard let runtime else {
            let message = runtimeErrorMessage(runtimeError)
            we_scene_runtime_error_destroy(runtimeError)
            XCTFail("Runtime creation failed: \(message)")
            throw SceneMetalTestFailure.creationFailed
        }
        defer { we_scene_runtime_destroy(runtime) }

        var translationError: WESceneRuntimeErrorRef?
        let translation = "shaders/generic.vert".withCString { vertexPath in
            "shaders/generic.frag".withCString { fragmentPath in
                we_scene_runtime_shader_translate_files(
                    runtime,
                    vertexPath,
                    fragmentPath,
                    &translationError
                )
            }
        }
        guard let translation else {
            let message = runtimeErrorMessage(translationError)
            we_scene_runtime_error_destroy(translationError)
            XCTFail("Official shader translation failed: \(message)")
            throw SceneMetalTestFailure.creationFailed
        }
        defer { we_scene_shader_translation_destroy(translation) }

        let vertex = runtimeString(
            we_scene_shader_translation_vertex_source(translation)
        )
        let fragment = runtimeString(
            we_scene_shader_translation_fragment_source(translation)
        )
        XCTAssertTrue(vertex.contains("#include <metal_stdlib>"))
        XCTAssertTrue(vertex.contains("we_scene_vertex_main"))
        XCTAssertTrue(fragment.contains("#include <metal_stdlib>"))
        XCTAssertTrue(fragment.contains("we_scene_fragment_main"))
    }

    private func createRenderer(
        width: UInt32,
        height: UInt32
    ) throws -> WESceneMetalRendererRef {
        var error: WESceneMetalErrorRef?
        guard let renderer = we_scene_metal_renderer_create(width, height, &error) else {
            let message = errorMessage(error)
            we_scene_metal_error_destroy(error)
            XCTFail("SceneMetal context creation failed: \(message)")
            throw SceneMetalTestFailure.creationFailed
        }
        XCTAssertNil(error)
        return renderer
    }

    private func errorMessage(_ error: WESceneMetalErrorRef?) -> String {
        guard let message = we_scene_metal_error_message(error) else {
            return "No SceneMetal error"
        }
        return String(cString: message)
    }

    private func runtimeErrorMessage(_ error: WESceneRuntimeErrorRef?) -> String {
        guard let message = we_scene_runtime_error_message(error) else {
            return "No Scene runtime error"
        }
        return String(cString: message)
    }

    private func runtimeString(_ value: UnsafePointer<CChar>?) -> String {
        guard let value else { return "" }
        return String(cString: value)
    }

    private func makeEmptyPackage() -> Data {
        var result = Data()
        appendUInt32(8, to: &result)
        result.append(contentsOf: Array("PKGV0001".utf8))
        appendUInt32(0, to: &result)
        return result
    }

    private func appendUInt32(_ value: UInt32, to data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
        data.append(UInt8(truncatingIfNeeded: value >> 16))
        data.append(UInt8(truncatingIfNeeded: value >> 24))
    }
}

private enum SceneMetalTestFailure: Error {
    case creationFailed
}
