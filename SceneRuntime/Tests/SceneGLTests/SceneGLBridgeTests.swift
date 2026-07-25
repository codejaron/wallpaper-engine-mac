import Foundation
import SceneRuntimeBridge
import OpenGL
import XCTest

final class SceneGLBridgeTests: XCTestCase {
    func testOffscreenRendererDrawsAndReadsRGBA8() throws {
        var error: WESceneGLErrorRef?
        guard let renderer = we_scene_gl_renderer_create(4, 3, &error) else {
            XCTFail("SceneGL context creation failed: \(errorMessage(error))")
            we_scene_gl_error_destroy(error)
            return
        }
        defer { we_scene_gl_renderer_destroy(renderer) }
        XCTAssertNil(error)
        XCTAssertEqual(we_scene_gl_renderer_rgba8_byte_count(renderer), 48)

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
                we_scene_gl_renderer_compile_program(
                    renderer,
                    vertexSource,
                    fragmentSource,
                    &error
                )
            }
        }
        XCTAssertEqual(compiled, 1, errorMessage(error))
        XCTAssertNil(error)
        XCTAssertEqual(we_scene_gl_renderer_draw(renderer, &error), 1, errorMessage(error))
        XCTAssertNil(error)

        var pixels = [UInt8](repeating: 0, count: 48)
        let read = pixels.withUnsafeMutableBytes { bytes in
            we_scene_gl_renderer_read_rgba8(
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
        XCTAssertEqual(pixels, greenRow + greenRow + magentaRow)
    }

    func testInvalidShaderReportsCompilationPhase() throws {
        let renderer = try createRenderer(width: 1, height: 1)
        defer { we_scene_gl_renderer_destroy(renderer) }
        var error: WESceneGLErrorRef?
        let validVertex = "#version 410 core\nvoid main() { gl_Position = vec4(0.0); }"
        let validFragment = "#version 410 core\nout vec4 color;\nvoid main() { color = vec4(1.0); }"
        XCTAssertEqual(
            validVertex.withCString { vertexSource in
                validFragment.withCString { fragmentSource in
                    we_scene_gl_renderer_compile_program(
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
        XCTAssertEqual(we_scene_gl_renderer_draw(renderer, &error), 1, errorMessage(error))
        XCTAssertNil(error)

        let vertex = "#version 410 core\nvoid main() { this is invalid; }"
        let fragment = "#version 410 core\nout vec4 color;\nvoid main() { color = vec4(1.0); }"

        let result = vertex.withCString { vertexSource in
            fragment.withCString { fragmentSource in
                we_scene_gl_renderer_compile_program(
                    renderer,
                    vertexSource,
                    fragmentSource,
                    &error
                )
            }
        }

        XCTAssertEqual(result, 0)
        XCTAssertEqual(we_scene_gl_error_code(error), WE_SCENE_GL_ERROR_SHADER_COMPILATION)
        XCTAssertTrue(errorMessage(error).contains("Vertex shader compilation failed"))
        we_scene_gl_error_destroy(error)

        var drawError: WESceneGLErrorRef?
        XCTAssertEqual(we_scene_gl_renderer_draw(renderer, &drawError), 0)
        XCTAssertEqual(we_scene_gl_error_code(drawError), WE_SCENE_GL_ERROR_DRAW)
        XCTAssertTrue(errorMessage(drawError).contains("linked shader program"))
        we_scene_gl_error_destroy(drawError)

        var clearedPixels = [UInt8](repeating: 0x7f, count: 4)
        var readError: WESceneGLErrorRef?
        XCTAssertEqual(
            clearedPixels.withUnsafeMutableBytes { bytes in
                we_scene_gl_renderer_read_rgba8(
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
        var creationError: WESceneGLErrorRef?
        XCTAssertNil(we_scene_gl_renderer_create(0, 1, &creationError))
        XCTAssertEqual(
            we_scene_gl_error_code(creationError),
            WE_SCENE_GL_ERROR_INVALID_ARGUMENT
        )
        we_scene_gl_error_destroy(creationError)

        var allocationError: WESceneGLErrorRef?
        XCTAssertNil(we_scene_gl_renderer_create(16_384, 16_384, &allocationError))
        XCTAssertEqual(
            we_scene_gl_error_code(allocationError),
            WE_SCENE_GL_ERROR_INVALID_ARGUMENT
        )
        XCTAssertTrue(errorMessage(allocationError).contains("256 MiB"))
        we_scene_gl_error_destroy(allocationError)

        let renderer = try createRenderer(width: 2, height: 2)
        defer { we_scene_gl_renderer_destroy(renderer) }
        var readError: WESceneGLErrorRef?
        var pixels = [UInt8](repeating: 0, count: 15)
        let result = pixels.withUnsafeMutableBytes { bytes in
            we_scene_gl_renderer_read_rgba8(
                renderer,
                bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                bytes.count,
                &readError
            )
        }
        XCTAssertEqual(result, 0)
        XCTAssertEqual(
            we_scene_gl_error_code(readError),
            WE_SCENE_GL_ERROR_INVALID_ARGUMENT
        )
        we_scene_gl_error_destroy(readError)
    }

    func testOperationsRestoreTheCallersCurrentContext() throws {
        var attributes: [CGLPixelFormatAttribute] = [
            kCGLPFAOpenGLProfile,
            CGLPixelFormatAttribute(rawValue: kCGLOGLPVersion_GL4_Core.rawValue),
            CGLPixelFormatAttribute(rawValue: 0),
        ]
        var pixelFormat: CGLPixelFormatObj?
        var pixelFormatCount: GLint = 0
        XCTAssertEqual(
            CGLChoosePixelFormat(&attributes, &pixelFormat, &pixelFormatCount),
            kCGLNoError
        )
        guard let pixelFormat else {
            XCTFail("CGL did not return a pixel format")
            return
        }
        defer { CGLReleasePixelFormat(pixelFormat) }

        var callerContext: CGLContextObj?
        XCTAssertEqual(
            CGLCreateContext(pixelFormat, nil, &callerContext),
            kCGLNoError
        )
        guard let callerContext else {
            XCTFail("CGL did not return a context")
            return
        }
        let originalContext = CGLGetCurrentContext()
        XCTAssertEqual(CGLSetCurrentContext(callerContext), kCGLNoError)
        defer {
            CGLSetCurrentContext(originalContext)
            CGLDestroyContext(callerContext)
        }

        let renderer = try createRenderer(width: 1, height: 1)
        XCTAssertEqual(CGLGetCurrentContext(), callerContext)

        var error: WESceneGLErrorRef?
        let vertex = "#version 410 core\nvoid main() { gl_Position = vec4(0.0); }"
        let fragment = "#version 410 core\nout vec4 color;\nvoid main() { color = vec4(1.0); }"
        XCTAssertEqual(
            vertex.withCString { vertexSource in
                fragment.withCString { fragmentSource in
                    we_scene_gl_renderer_compile_program(
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
        XCTAssertEqual(CGLGetCurrentContext(), callerContext)
        XCTAssertEqual(we_scene_gl_renderer_draw(renderer, &error), 1, errorMessage(error))
        XCTAssertEqual(CGLGetCurrentContext(), callerContext)
        var pixels = [UInt8](repeating: 0, count: 4)
        XCTAssertEqual(
            pixels.withUnsafeMutableBytes { bytes in
                we_scene_gl_renderer_read_rgba8(
                    renderer,
                    bytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                    bytes.count,
                    &error
                )
            },
            1,
            errorMessage(error)
        )
        XCTAssertEqual(CGLGetCurrentContext(), callerContext)
        we_scene_gl_renderer_destroy(renderer)
        XCTAssertEqual(CGLGetCurrentContext(), callerContext)
    }

    func testOfficialTranslatedShaderCompilesOnAppleOpenGL() throws {
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
            throw SceneGLTestFailure.creationFailed
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
            throw SceneGLTestFailure.creationFailed
        }
        defer { we_scene_shader_translation_destroy(translation) }

        let vertex = runtimeString(
            we_scene_shader_translation_vertex_source(translation)
        )
        let fragment = runtimeString(
            we_scene_shader_translation_fragment_source(translation)
        )
        var glError: WESceneGLErrorRef?
        guard let renderer = we_scene_gl_renderer_create(32, 32, &glError) else {
            let message = errorMessage(glError)
            we_scene_gl_error_destroy(glError)
            XCTFail("SceneGL creation failed: \(message)")
            throw SceneGLTestFailure.creationFailed
        }
        defer { we_scene_gl_renderer_destroy(renderer) }

        let compiled = vertex.withCString { vertexSource in
            fragment.withCString { fragmentSource in
                we_scene_gl_renderer_compile_program(
                    renderer,
                    vertexSource,
                    fragmentSource,
                    &glError
                )
            }
        }
        XCTAssertEqual(compiled, 1, errorMessage(glError))
        XCTAssertNil(glError)
    }

    private func createRenderer(
        width: UInt32,
        height: UInt32
    ) throws -> WESceneGLRendererRef {
        var error: WESceneGLErrorRef?
        guard let renderer = we_scene_gl_renderer_create(width, height, &error) else {
            let message = errorMessage(error)
            we_scene_gl_error_destroy(error)
            XCTFail("SceneGL context creation failed: \(message)")
            throw SceneGLTestFailure.creationFailed
        }
        XCTAssertNil(error)
        return renderer
    }

    private func errorMessage(_ error: WESceneGLErrorRef?) -> String {
        guard let message = we_scene_gl_error_message(error) else {
            return "No SceneGL error"
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

private enum SceneGLTestFailure: Error {
    case creationFailed
}
