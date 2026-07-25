import Darwin
import Foundation
import SceneRuntimeBridge
import XCTest

final class SceneRuntimeBridgeTests: XCTestCase {
    func testWallpaperEngineShaderForkTranslatesRelaxedGLSL() throws {
        let vertex = """
        #version 330
        layout(location = 0) in vec2 aPosition;
        out vec2 vUV;
        void main() {
            int scale = 1;
            float promoted = scale;
            vUV = aPosition * promoted;
            gl_Position = vec4(aPosition, 0.0, 1.0);
        }
        """
        let fragment = """
        #version 330
        in vec2 vUV;
        out vec4 outColor;
        uniform float values[2];
        void main() {
            float index = 1.0;
            uint amount = 1u;
            float value = values[index] + amount;
            outColor = vec4(vUV, value, 1.0);
        }
        """

        var error: WESceneRuntimeErrorRef?
        let translation = vertex.withCString { vertexSource in
            fragment.withCString { fragmentSource in
                "relaxed.vert".withCString { vertexName in
                    "relaxed.frag".withCString { fragmentName in
                        var sources = WESceneShaderSources(
                            vertex_source: vertexSource,
                            fragment_source: fragmentSource,
                            vertex_name: vertexName,
                            fragment_name: fragmentName
                        )
                        return we_scene_shader_translate(&sources, &error)
                    }
                }
            }
        }
        guard let translation else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Shader translation failed: \(message)")
            throw TestFailure.shaderTranslation
        }
        defer { we_scene_shader_translation_destroy(translation) }
        XCTAssertNil(error)

        let translatedVertex = runtimeString(
            we_scene_shader_translation_vertex_source(translation)
        )
        let translatedFragment = runtimeString(
            we_scene_shader_translation_fragment_source(translation)
        )
        XCTAssertTrue(translatedVertex.contains("#version 330"))
        XCTAssertTrue(translatedFragment.contains("#version 330"))
        XCTAssertTrue(translatedFragment.contains("[int("), translatedFragment)
        XCTAssertEqual(
            runtimeString(we_scene_shader_glslang_revision()),
            "b775500a153f5ceb0e4b6f366b79c4c57521bb62"
        )
        XCTAssertEqual(
            runtimeString(we_scene_shader_spirv_cross_revision()),
            "ad4d02220b01c1800e5a4e6671d6d8ca8ab07783"
        )
    }

    func testShaderTranslationNarrowsWideVertexVaryingToFragmentContract() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let packageURL = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders", isDirectory: true),
            withIntermediateDirectories: true
        )
        defer { try? FileManager.default.removeItem(at: root) }
        let vertex = """
        uniform mat4 g_ModelViewProjectionMatrix;
        attribute vec3 a_Position;
        attribute vec2 a_TexCoord;
        varying vec4 v_TexCoord;
        void main() {
            gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);
            v_TexCoord.xy = a_TexCoord;
        }
        """
        let fragment = """
        varying vec2 v_TexCoord;
        void main() {
            vec2 fragmentPosition = v_TexCoord;
            gl_FragColor = vec4(fragmentPosition, 0.0, 1.0);
        }
        """
        try makePackage([
            ("shaders/wide-varying.vert", Data(vertex.utf8)),
            ("shaders/narrow-varying.frag", Data(fragment.utf8)),
        ]).write(to: packageURL)
        let runtime = try createRuntime(assets: assets, package: packageURL)
        defer { we_scene_runtime_destroy(runtime) }

        var error: WESceneRuntimeErrorRef?
        let translation = "shaders/wide-varying.vert".withCString { vertexPath in
            "shaders/narrow-varying.frag".withCString { fragmentPath in
                we_scene_runtime_shader_translate_files(
                    runtime,
                    vertexPath,
                    fragmentPath,
                    &error
                )
            }
        }
        guard let translation else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Shader translation failed: \(message)")
            throw TestFailure.shaderTranslation
        }
        defer { we_scene_shader_translation_destroy(translation) }
        XCTAssertNil(error)
        XCTAssertTrue(
            runtimeString(
                we_scene_shader_translation_preprocessed_vertex_source(translation)
            ).contains("varying vec2 v_TexCoord")
        )
    }

    func testShaderParseFailurePreservesStageAndSourceName() {
        let vertex = """
        #version 330
        void main() { gl_Position = vec4(0.0); }
        """
        let fragment = """
        #version 330
        this is not valid GLSL
        """

        var error: WESceneRuntimeErrorRef?
        let translation = vertex.withCString { vertexSource in
            fragment.withCString { fragmentSource in
                "valid.vert".withCString { vertexName in
                    "broken.frag".withCString { fragmentName in
                        var sources = WESceneShaderSources(
                            vertex_source: vertexSource,
                            fragment_source: fragmentSource,
                            vertex_name: vertexName,
                            fragment_name: fragmentName
                        )
                        return we_scene_shader_translate(&sources, &error)
                    }
                }
            }
        }

        XCTAssertNil(translation)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SHADER_PARSE_FAILURE
        )
        let message = errorMessage(error)
        XCTAssertTrue(message.contains("broken.frag"), message)
        XCTAssertTrue(message.contains("parsing failed"), message)
        we_scene_runtime_error_destroy(error)
    }

    func testOfficialShaderPreprocessingExpandsIncludesAndRequires() throws {
        guard let assetsPath = ProcessInfo.processInfo.environment["WE_ASSETS_DIR"],
              !assetsPath.isEmpty else {
            throw XCTSkip("WE_ASSETS_DIR is required for the shader contract")
        }
        let fixtureRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let packageURL = fixtureRoot.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: fixtureRoot.appendingPathComponent("assets/shaders"),
            withIntermediateDirectories: true
        )
        try makePackage([]).write(to: packageURL)
        defer { try? FileManager.default.removeItem(at: fixtureRoot) }

        let runtime = try createRuntime(
            assets: URL(fileURLWithPath: assetsPath, isDirectory: true),
            package: packageURL
        )
        defer { we_scene_runtime_destroy(runtime) }

        var error: WESceneRuntimeErrorRef?
        let translation = "shaders/generic.vert".withCString { vertexPath in
            "shaders/generic.frag".withCString { fragmentPath in
                we_scene_runtime_shader_translate_files(
                    runtime,
                    vertexPath,
                    fragmentPath,
                    &error
                )
            }
        }
        guard let translation else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Official shader preprocessing failed: \(message)")
            throw TestFailure.shaderTranslation
        }
        defer { we_scene_shader_translation_destroy(translation) }
        XCTAssertNil(error)
        let translatedVertex = runtimeString(
            we_scene_shader_translation_vertex_source(translation)
        )
        let translatedFragment = runtimeString(
            we_scene_shader_translation_fragment_source(translation)
        )
        let preprocessedVertex = runtimeString(
            we_scene_shader_translation_preprocessed_vertex_source(translation)
        )
        let preprocessedFragment = runtimeString(
            we_scene_shader_translation_preprocessed_fragment_source(translation)
        )
        XCTAssertTrue(preprocessedVertex.contains("BuildTangentSpace"), preprocessedVertex)
        XCTAssertTrue(preprocessedFragment.contains("ComputeLightSpecular"), preprocessedFragment)
        XCTAssertTrue(preprocessedVertex.contains("#define GLSL 1"))
        XCTAssertTrue(translatedVertex.contains("#version 330"), translatedVertex)
        XCTAssertTrue(translatedFragment.contains("#version 330"), translatedFragment)
        XCTAssertTrue(translatedFragment.contains("out_FragColor"), translatedFragment)

        var foliageError: WESceneRuntimeErrorRef?
        let foliage = "shaders/foliage4.vert".withCString { vertexPath in
            "shaders/foliage4.frag".withCString { fragmentPath in
                we_scene_runtime_shader_translate_files(
                    runtime,
                    vertexPath,
                    fragmentPath,
                    &foliageError
                )
            }
        }
        guard let foliage else {
            let message = errorMessage(foliageError)
            we_scene_runtime_error_destroy(foliageError)
            XCTFail("LightingV1 shader preprocessing failed: \(message)")
            throw TestFailure.shaderTranslation
        }
        defer { we_scene_shader_translation_destroy(foliage) }
        XCTAssertNil(foliageError)
        XCTAssertTrue(
            runtimeString(we_scene_shader_translation_fragment_source(foliage))
                .contains("PerformLighting_V1")
        )
    }

    func testOfficialShaderMetadataExposesMaterialAndTypedDefaults() throws {
        guard let assetsPath = ProcessInfo.processInfo.environment["WE_ASSETS_DIR"],
              !assetsPath.isEmpty else {
            throw XCTSkip("WE_ASSETS_DIR is required for the shader metadata contract")
        }
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: root.appendingPathComponent("assets/shaders"),
            withIntermediateDirectories: true
        )
        try makePackage([]).write(to: package)
        defer { try? FileManager.default.removeItem(at: root) }

        let runtime = try createRuntime(
            assets: URL(fileURLWithPath: assetsPath, isDirectory: true),
            package: package
        )
        defer { we_scene_runtime_destroy(runtime) }

        let xray = try translateShaderFiles(
            runtime,
            vertex: "effects/xray/shaders/effects/xray.vert",
            fragment: "effects/xray/shaders/effects/xray.frag"
        )
        defer { we_scene_shader_translation_destroy(xray) }

        let size = try shaderParameter(
            xray,
            stage: WE_SCENE_SHADER_STAGE_VERTEX,
            named: "g_PointerScale"
        )
        XCTAssertEqual(runtimeString(size.type), "float")
        XCTAssertEqual(runtimeString(size.material), "size")
        XCTAssertEqual(size.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_NUMBER)
        XCTAssertEqual(size.default_number, 0.2, accuracy: 0.000_001)

        let multiply = try shaderParameter(
            xray,
            stage: WE_SCENE_SHADER_STAGE_FRAGMENT,
            named: "g_Multiply"
        )
        XCTAssertEqual(runtimeString(multiply.material), "multiply")
        XCTAssertEqual(multiply.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_INTEGER)
        XCTAssertEqual(multiply.default_integer, 1)

        let blendTexture = try shaderParameter(
            xray,
            stage: WE_SCENE_SHADER_STAGE_FRAGMENT,
            named: "g_Texture1"
        )
        XCTAssertNil(blendTexture.material)
        XCTAssertEqual(blendTexture.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_STRING)
        XCTAssertEqual(runtimeString(blendTexture.default_string), "util/white")

        let genericImage = try translateShaderFiles(
            runtime,
            vertex: "shaders/genericimage2.vert",
            fragment: "shaders/genericimage2.frag"
        )
        defer { we_scene_shader_translation_destroy(genericImage) }

        let brightness = try shaderParameter(
            genericImage,
            stage: WE_SCENE_SHADER_STAGE_FRAGMENT,
            named: "g_Brightness"
        )
        XCTAssertEqual(runtimeString(brightness.material), "Brightness")
        XCTAssertEqual(brightness.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_INTEGER)
        XCTAssertEqual(brightness.default_integer, 1)

        let alpha = try shaderParameter(
            genericImage,
            stage: WE_SCENE_SHADER_STAGE_FRAGMENT,
            named: "g_UserAlpha"
        )
        XCTAssertEqual(runtimeString(alpha.material), "Alpha")
        XCTAssertEqual(alpha.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_INTEGER)
        XCTAssertEqual(alpha.default_integer, 1)
        XCTAssertTrue(runtimeString(alpha.metadata_json).contains("\"material\":\"Alpha\""))

        let albedo = try shaderParameter(
            genericImage,
            stage: WE_SCENE_SHADER_STAGE_FRAGMENT,
            named: "g_Texture0"
        )
        XCTAssertNil(albedo.material)
        XCTAssertEqual(albedo.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_NONE)
    }

    func testShaderMetadataParsesEverySupportedDefaultKind() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = root.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders"),
            withIntermediateDirectories: true
        )
        try FileManager.default.createDirectory(at: shaders, withIntermediateDirectories: true)
        try makePackage([]).write(to: package)
        defer { try? FileManager.default.removeItem(at: root) }

        let vertex = """
        uniform bool g_Enabled; // {"material":"enabled","default":true}
        uniform int g_Count; // {"material":"count","default":-3}
        uniform vec3 g_Tint; // {"material":"tint","default":"1 -0.5 2.25"}
        void main() { gl_Position = vec4(g_Tint, g_Enabled ? float(g_Count) : 1.0); }
        """
        let fragment = """
        uniform float g_Amount; // {"material":"amount","default":0.125}
        uniform sampler2D g_Texture0; // {"label":"source","default":"util/white"}
        void main() { gl_FragColor = texture(g_Texture0, vec2(g_Amount)); }
        """
        try Data(vertex.utf8).write(to: shaders.appendingPathComponent("typed.vert"))
        try Data(fragment.utf8).write(to: shaders.appendingPathComponent("typed.frag"))

        let runtime = try createRuntime(assets: assets, package: package)
        defer { we_scene_runtime_destroy(runtime) }
        let translation = try translateShaderFiles(
            runtime,
            vertex: "shaders/typed.vert",
            fragment: "shaders/typed.frag"
        )
        defer { we_scene_shader_translation_destroy(translation) }

        let enabled = try shaderParameter(
            translation,
            stage: WE_SCENE_SHADER_STAGE_VERTEX,
            named: "g_Enabled"
        )
        XCTAssertEqual(enabled.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_BOOLEAN)
        XCTAssertEqual(enabled.default_boolean, 1)

        let count = try shaderParameter(
            translation,
            stage: WE_SCENE_SHADER_STAGE_VERTEX,
            named: "g_Count"
        )
        XCTAssertEqual(count.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_INTEGER)
        XCTAssertEqual(count.default_integer, -3)

        let amount = try shaderParameter(
            translation,
            stage: WE_SCENE_SHADER_STAGE_FRAGMENT,
            named: "g_Amount"
        )
        XCTAssertEqual(amount.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_NUMBER)
        XCTAssertEqual(amount.default_number, 0.125, accuracy: 0.000_001)

        let texture = try shaderParameter(
            translation,
            stage: WE_SCENE_SHADER_STAGE_FRAGMENT,
            named: "g_Texture0"
        )
        XCTAssertNil(texture.material)
        XCTAssertEqual(texture.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_STRING)
        XCTAssertEqual(runtimeString(texture.default_string), "util/white")

        let tint = try shaderParameter(
            translation,
            stage: WE_SCENE_SHADER_STAGE_VERTEX,
            named: "g_Tint"
        )
        XCTAssertEqual(tint.default_type, WE_SCENE_SHADER_PARAMETER_DEFAULT_VECTOR)
        XCTAssertEqual(tint.default_vector_count, 3)
        guard let vector = tint.default_vector else {
            XCTFail("Typed vector metadata has no component storage")
            throw TestFailure.shaderMetadataQuery("Missing vector storage")
        }
        XCTAssertEqual(vector[0], 1.0, accuracy: 0.000_001)
        XCTAssertEqual(vector[1], -0.5, accuracy: 0.000_001)
        XCTAssertEqual(vector[2], 2.25, accuracy: 0.000_001)
    }

    func testShaderMetadataRejectsMalformedTypedFields() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = root.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders"),
            withIntermediateDirectories: true
        )
        try FileManager.default.createDirectory(at: shaders, withIntermediateDirectories: true)
        try makePackage([]).write(to: package)
        try Data("void main() { gl_FragColor = vec4(1.0); }".utf8)
            .write(to: shaders.appendingPathComponent("valid.frag"))
        defer { try? FileManager.default.removeItem(at: root) }

        let runtime = try createRuntime(assets: assets, package: package)
        defer { we_scene_runtime_destroy(runtime) }
        let cases = [
            (
                "g_BadObject",
                "uniform float g_BadObject; // []",
                "must be a JSON object"
            ),
            (
                "g_BadMaterial",
                "uniform float g_BadMaterial; // {\"material\":7,\"default\":1}",
                "material"
            ),
            (
                "g_BadDefault",
                "uniform float g_BadDefault; // {\"default\":[1]}",
                "boolean, integer, number, or string"
            ),
            (
                "g_BadVector",
                "uniform vec3 g_BadVector; // {\"default\":\"1 2\"}",
                "exactly 3 numbers"
            ),
        ]

        for (name, declaration, expectedMessage) in cases {
            let vertex = "\(declaration)\nvoid main() { gl_Position = vec4(0.0); }\n"
            try Data(vertex.utf8).write(to: shaders.appendingPathComponent("invalid.vert"))
            var error: WESceneRuntimeErrorRef?
            let translation = "shaders/invalid.vert".withCString { vertexPath in
                "shaders/valid.frag".withCString { fragmentPath in
                    we_scene_runtime_shader_translate_files(
                        runtime,
                        vertexPath,
                        fragmentPath,
                        &error
                    )
                }
            }
            XCTAssertNil(translation, name)
            XCTAssertEqual(
                we_scene_runtime_error_code(error),
                WE_SCENE_RUNTIME_ERROR_SHADER_INPUT_INVALID,
                name
            )
            let message = errorMessage(error)
            XCTAssertTrue(message.contains(name), message)
            XCTAssertTrue(message.contains(expectedMessage), message)
            we_scene_runtime_error_destroy(error)
        }
    }

    func testShaderPreprocessingReportsMissingAndCyclicIncludes() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let projectShaders = root.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders"),
            withIntermediateDirectories: true
        )
        try FileManager.default.createDirectory(at: projectShaders, withIntermediateDirectories: true)
        try makePackage([]).write(to: package)
        defer { try? FileManager.default.removeItem(at: root) }

        let vertex = "#version 330\n#include \"missing.h\"\nvoid main() { gl_Position = vec4(0.0); }\n"
        let fragment = "#version 330\nout vec4 color;\nvoid main() { color = vec4(1.0); }\n"
        try Data(vertex.utf8).write(to: projectShaders.appendingPathComponent("vertex.vert"))
        try Data(fragment.utf8).write(to: projectShaders.appendingPathComponent("fragment.frag"))
        let runtime = try createRuntime(assets: assets, package: package)
        defer { we_scene_runtime_destroy(runtime) }

        var missingError: WESceneRuntimeErrorRef?
        let missing = "shaders/vertex.vert".withCString { vertexPath in
            "shaders/fragment.frag".withCString { fragmentPath in
                we_scene_runtime_shader_translate_files(runtime, vertexPath, fragmentPath, &missingError)
            }
        }
        XCTAssertNil(missing)
        XCTAssertEqual(we_scene_runtime_error_code(missingError), WE_SCENE_RUNTIME_ERROR_SHADER_INPUT_INVALID)
        XCTAssertTrue(errorMessage(missingError).contains("missing.h"))
        we_scene_runtime_error_destroy(missingError)

        try Data("#include \"cycle-b.h\"\n".utf8)
            .write(to: projectShaders.appendingPathComponent("cycle-a.h"))
        try Data("#include \"cycle-a.h\"\n".utf8)
            .write(to: projectShaders.appendingPathComponent("cycle-b.h"))
        try Data("#version 330\n#include \"cycle-a.h\"\nvoid main() { gl_Position = vec4(0.0); }\n".utf8)
            .write(to: projectShaders.appendingPathComponent("cycle.vert"))
        var cycleError: WESceneRuntimeErrorRef?
        let cycle = "shaders/cycle.vert".withCString { vertexPath in
            "shaders/fragment.frag".withCString { fragmentPath in
                we_scene_runtime_shader_translate_files(runtime, vertexPath, fragmentPath, &cycleError)
            }
        }
        XCTAssertNil(cycle)
        XCTAssertEqual(we_scene_runtime_error_code(cycleError), WE_SCENE_RUNTIME_ERROR_SHADER_INPUT_INVALID)
        XCTAssertTrue(errorMessage(cycleError).contains("cycle-a.h"))
        we_scene_runtime_error_destroy(cycleError)
    }

    func testNullConfigurationReturnsExplicitError() {
        var error: WESceneRuntimeErrorRef?

        let runtime = we_scene_runtime_create(nil, &error)

        XCTAssertNil(runtime)
        XCTAssertNotNil(error)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT
        )
        XCTAssertEqual(
            errorMessage(error),
            "Scene runtime configuration is required"
        )

        we_scene_runtime_error_destroy(error)
    }

    func testMissingAssetsDirectoryReturnsPathSpecificError() {
        let missingRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let scenePackage = missingRoot.appendingPathComponent("scene.pkg")
        var error: WESceneRuntimeErrorRef?

        let runtime = missingRoot.path.withCString { assetsDirectory in
            scenePackage.path.withCString { scenePackagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsDirectory,
                    scene_package_path: scenePackagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }

        XCTAssertNil(runtime)
        XCTAssertNotNil(error)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_ASSETS_DIRECTORY_NOT_FOUND
        )
        XCTAssertTrue(errorMessage(error).contains(missingRoot.path))

        we_scene_runtime_error_destroy(error)
    }

    func testValidatedConfigurationOwnsCanonicalPathsUntilDestroyed() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assetsDirectory = root.appendingPathComponent("assets", isDirectory: true)
        let shadersDirectory = assetsDirectory
            .appendingPathComponent("shaders", isDirectory: true)
        let scenePackage = root.appendingPathComponent("scene.pkg")

        try FileManager.default.createDirectory(
            at: shadersDirectory,
            withIntermediateDirectories: true
        )
        var package = Data()
        appendUInt32(8, to: &package)
        package.append(contentsOf: Array("PKGV0001".utf8))
        appendUInt32(0, to: &package)
        XCTAssertTrue(FileManager.default.createFile(
            atPath: scenePackage.path,
            contents: package
        ))
        defer { try? FileManager.default.removeItem(at: root) }

        var error: WESceneRuntimeErrorRef?
        let runtime = assetsDirectory.path.withCString { assetsPath in
            scenePackage.path.withCString { scenePackagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsPath,
                    scene_package_path: scenePackagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }

        XCTAssertNotNil(runtime)
        XCTAssertNil(error)
        XCTAssertEqual(
            runtimeString(we_scene_runtime_assets_directory(runtime)),
            try canonicalPath(assetsDirectory)
        )
        XCTAssertEqual(
            runtimeString(we_scene_runtime_scene_package_path(runtime)),
            try canonicalPath(scenePackage)
        )

        we_scene_runtime_destroy(runtime)
    }

    func testPackageEntriesAndResolverSourcesUseRealBytes() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let packageURL = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: shaders,
            withIntermediateDirectories: true
        )
        try Data("official".utf8).write(
            to: assets.appendingPathComponent("official.bin")
        )
        let package = makePackage([
            ("package-only.bin", Data("package".utf8)),
            ("directory-collision.bin", Data("package-wins".utf8)),
            ("scene.json", Data("{\"ok\":true}".utf8)),
        ])
        try package.write(to: packageURL)
        try FileManager.default.createDirectory(
            at: root.appendingPathComponent("directory-collision.bin"),
            withIntermediateDirectories: false
        )
        defer { try? FileManager.default.removeItem(at: root) }

        let runtime = try createRuntime(assets: assets, package: packageURL)
        defer { we_scene_runtime_destroy(runtime) }

        XCTAssertEqual(runtimeString(we_scene_runtime_package_version(runtime)), "PKGV0001")
        XCTAssertEqual(try packageEntryCount(runtime), 3)

        var entry = WEScenePackageEntryInfo()
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_runtime_package_entry(runtime, 0, &entry, &error),
            1
        )
        XCTAssertNil(error)
        XCTAssertEqual(runtimeString(entry.path), "package-only.bin")

        let packageAsset = try createAsset(runtime: runtime, path: "package-only.bin")
        defer { we_scene_runtime_asset_destroy(packageAsset) }
        XCTAssertEqual(
            we_scene_runtime_asset_source(packageAsset),
            WE_SCENE_ASSET_SOURCE_SCENE_PACKAGE
        )
        XCTAssertEqual(assetData(packageAsset), Data("package".utf8))

        let collision = try createAsset(
            runtime: runtime,
            path: "directory-collision.bin"
        )
        defer { we_scene_runtime_asset_destroy(collision) }
        XCTAssertEqual(
            we_scene_runtime_asset_source(collision),
            WE_SCENE_ASSET_SOURCE_SCENE_PACKAGE
        )
        XCTAssertEqual(assetData(collision), Data("package-wins".utf8))

        let officialAsset = try createAsset(runtime: runtime, path: "official.bin")
        defer { we_scene_runtime_asset_destroy(officialAsset) }
        XCTAssertEqual(
            we_scene_runtime_asset_source(officialAsset),
            WE_SCENE_ASSET_SOURCE_OFFICIAL_ASSETS
        )
        XCTAssertEqual(assetData(officialAsset), Data("official".utf8))

        var missingError: WESceneRuntimeErrorRef?
        let missing = "missing.bin".withCString {
            we_scene_runtime_asset_create(runtime, $0, &missingError)
        }
        XCTAssertNil(missing)
        XCTAssertEqual(
            we_scene_runtime_error_code(missingError),
            WE_SCENE_RUNTIME_ERROR_ASSET_NOT_FOUND
        )
        we_scene_runtime_error_destroy(missingError)
    }

    func testMalformedPackageFailsWithExplicitFormatError() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let packageURL = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: shaders,
            withIntermediateDirectories: true
        )
        var malformed = Data()
        appendUInt32(8, to: &malformed)
        malformed.append(contentsOf: Array("PKGV0001".utf8))
        appendUInt32(1, to: &malformed)
        try malformed.write(to: packageURL)
        defer { try? FileManager.default.removeItem(at: root) }

        var error: WESceneRuntimeErrorRef?
        let runtime = assets.path.withCString { assetsPath in
            packageURL.path.withCString { packagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsPath,
                    scene_package_path: packagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }
        XCTAssertNil(runtime)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_INVALID
        )
        XCTAssertTrue(errorMessage(error).contains("Unexpected end"))
        we_scene_runtime_error_destroy(error)
    }

    func testUnsafePackageEntryFailsAtItsTableOffset() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let packageURL = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders"),
            withIntermediateDirectories: true
        )
        try makePackage([("../escape.bin", Data([1]))]).write(to: packageURL)
        defer { try? FileManager.default.removeItem(at: root) }

        var error: WESceneRuntimeErrorRef?
        let runtime = assets.path.withCString { assetsPath in
            packageURL.path.withCString { packagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsPath,
                    scene_package_path: packagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }
        XCTAssertNil(runtime)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_INVALID
        )
        XCTAssertTrue(errorMessage(error).contains("../escape.bin"))
        XCTAssertTrue(errorMessage(error).contains("offset"))
        we_scene_runtime_error_destroy(error)
    }

    func testCorruptGifPackageHasDistinctErrorCode() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let packageURL = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders"),
            withIntermediateDirectories: true
        )
        try makePackage([]).write(to: packageURL)
        try Data("not-a-package".utf8).write(
            to: root.appendingPathComponent("gifscene.pkg")
        )
        defer { try? FileManager.default.removeItem(at: root) }

        var error: WESceneRuntimeErrorRef?
        let runtime = assets.path.withCString { assetsPath in
            packageURL.path.withCString { packagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsPath,
                    scene_package_path: packagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }
        XCTAssertNil(runtime)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_GIF_SCENE_PACKAGE_INVALID
        )
        XCTAssertTrue(errorMessage(error).contains("gifscene.pkg"))
        we_scene_runtime_error_destroy(error)
    }

    func testGifPackageSymlinkOutsideProjectIsRejected() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let externalRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let packageURL = root.appendingPathComponent("scene.pkg")
        let externalGifPackage = externalRoot.appendingPathComponent("payload.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders"),
            withIntermediateDirectories: true
        )
        try FileManager.default.createDirectory(
            at: externalRoot,
            withIntermediateDirectories: true
        )
        try makePackage([]).write(to: packageURL)
        try makePackage([]).write(to: externalGifPackage)
        try FileManager.default.createSymbolicLink(
            at: root.appendingPathComponent("gifscene.pkg"),
            withDestinationURL: externalGifPackage
        )
        defer {
            try? FileManager.default.removeItem(at: root)
            try? FileManager.default.removeItem(at: externalRoot)
        }

        var error: WESceneRuntimeErrorRef?
        let runtime = assets.path.withCString { assetsPath in
            packageURL.path.withCString { packagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsPath,
                    scene_package_path: packagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }
        XCTAssertNil(runtime)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_GIF_SCENE_PACKAGE_INVALID
        )
        XCTAssertTrue(errorMessage(error).contains("outside"))
        if let runtime {
            we_scene_runtime_destroy(runtime)
        }
        we_scene_runtime_error_destroy(error)
    }

    func testTextureFormatErrorsAndUnknownEmbeddedImageContract() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let packageURL = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders"),
            withIntermediateDirectories: true
        )
        try makePackage([
            ("bad.tex", makeTextureWithInvalidLZ4Payload()),
            ("embedded.tex", makeUnknownEmbeddedPNGTexture()),
        ]).write(to: packageURL)
        defer { try? FileManager.default.removeItem(at: root) }

        let runtime = try createRuntime(assets: assets, package: packageURL)
        defer { we_scene_runtime_destroy(runtime) }
        var error: WESceneRuntimeErrorRef?
        let texture = "bad.tex".withCString {
            we_scene_runtime_texture_create(runtime, $0, &error)
        }
        XCTAssertNil(texture)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID
        )
        XCTAssertTrue(errorMessage(error).contains("LZ4"))
        we_scene_runtime_error_destroy(error)

        let embedded = try createTexture(runtime: runtime, path: "embedded.tex")
        defer { we_scene_runtime_texture_destroy(embedded) }
        var embeddedInfo = WESceneTextureInfo()
        var embeddedError: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_runtime_texture_info(embedded, &embeddedInfo, &embeddedError),
            1
        )
        XCTAssertNil(embeddedError)
        XCTAssertEqual(embeddedInfo.format, UInt32.max)
        XCTAssertEqual(embeddedInfo.file_format, 13)
    }

    func testOversizedTextureMipmapIsRejectedBeforeDecompression() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let packageURL = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders"),
            withIntermediateDirectories: true
        )
        try makePackage([
            ("oversized.tex", makeTextureWithOversizedMipmap()),
            ("mismatched.tex", makeTextureWithMismatchedMipmapSize())
        ]).write(to: packageURL)
        defer { try? FileManager.default.removeItem(at: root) }

        let runtime = try createRuntime(assets: assets, package: packageURL)
        defer { we_scene_runtime_destroy(runtime) }
        var error: WESceneRuntimeErrorRef?
        let texture = "oversized.tex".withCString {
            we_scene_runtime_texture_create(runtime, $0, &error)
        }
        XCTAssertNil(texture)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID
        )
        let message = errorMessage(error)
        XCTAssertTrue(message.contains("allocation limit"), message)
        if let texture {
            we_scene_runtime_texture_destroy(texture)
        }
        we_scene_runtime_error_destroy(error)

        var mismatchError: WESceneRuntimeErrorRef?
        let mismatch = "mismatched.tex".withCString {
            we_scene_runtime_texture_create(runtime, $0, &mismatchError)
        }
        XCTAssertNil(mismatch)
        XCTAssertEqual(
            we_scene_runtime_error_code(mismatchError),
            WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID
        )
        XCTAssertTrue(
            errorMessage(mismatchError).contains("declares 1024 bytes; expected 1")
        )
        if let mismatch {
            we_scene_runtime_texture_destroy(mismatch)
        }
        we_scene_runtime_error_destroy(mismatchError)
    }

    func testExtremeAnimationFrameDimensionsAreRejected() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let packageURL = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: assets.appendingPathComponent("shaders"),
            withIntermediateDirectories: true
        )
        try makePackage([
            ("extreme.tex", makeTextureWithExtremeAnimationFrame())
        ]).write(to: packageURL)
        defer { try? FileManager.default.removeItem(at: root) }

        let runtime = try createRuntime(assets: assets, package: packageURL)
        defer { we_scene_runtime_destroy(runtime) }
        var error: WESceneRuntimeErrorRef?
        let texture = "extreme.tex".withCString {
            we_scene_runtime_texture_create(runtime, $0, &error)
        }
        XCTAssertNil(texture)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID
        )
        XCTAssertTrue(errorMessage(error).contains("frame dimensions"))
        if let texture {
            we_scene_runtime_texture_destroy(texture)
        }
        we_scene_runtime_error_destroy(error)
    }

    func testOfficialTextureContractsCoverDXTLUTAndAnimation() throws {
        guard let assetsPath = ProcessInfo.processInfo.environment["WE_ASSETS_DIR"],
              !assetsPath.isEmpty else {
            throw XCTSkip("WE_ASSETS_DIR is required for the official texture contract")
        }
        let assets = URL(fileURLWithPath: assetsPath, isDirectory: true)
        let fixtureRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let packageURL = fixtureRoot.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: fixtureRoot.appendingPathComponent("assets/shaders"),
            withIntermediateDirectories: true
        )
        try makePackage([]).write(to: packageURL)
        defer { try? FileManager.default.removeItem(at: fixtureRoot) }

        let runtime = try createRuntime(assets: assets, package: packageURL)
        defer { we_scene_runtime_destroy(runtime) }

        let dxt = try createTexture(runtime: runtime, path: "materials/util/flatnormal.tex")
        defer { we_scene_runtime_texture_destroy(dxt) }
        var dxtInfo = WESceneTextureInfo()
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(we_scene_runtime_texture_info(dxt, &dxtInfo, &error), 1)
        XCTAssertNil(error)
        XCTAssertEqual(dxtInfo.format, 4)
        var dxtMip = WESceneTextureMipmapInfo()
        XCTAssertEqual(
            we_scene_runtime_texture_mipmap_info(dxt, 0, 0, &dxtMip, &error),
            1
        )
        XCTAssertEqual(dxtMip.uncompressed_size, 256)

        let lut = try createTexture(runtime: runtime, path: "materials/lut/lutx32_westernf.tex")
        defer { we_scene_runtime_texture_destroy(lut) }
        var lutInfo = WESceneTextureInfo()
        XCTAssertEqual(we_scene_runtime_texture_info(lut, &lutInfo, &error), 1)
        XCTAssertEqual(lutInfo.has_extra_texi_field, 1)

        let animation = try createTexture(
            runtime: runtime,
            path: "materials/particle/shape/sparks_sheet.tex"
        )
        defer { we_scene_runtime_texture_destroy(animation) }
        var animationInfo = WESceneTextureInfo()
        XCTAssertEqual(
            we_scene_runtime_texture_info(animation, &animationInfo, &error),
            1
        )
        XCTAssertGreaterThan(animationInfo.frame_count, 0)
        var frame = WESceneTextureFrameInfo()
        XCTAssertEqual(
            we_scene_runtime_texture_frame_info(animation, 0, &frame, &error),
            1
        )
        XCTAssertGreaterThan(frame.frame_time, 0)

        var textureCount = 0
        var argbCount = 0
        var r8Count = 0
        var rg88Count = 0
        var dxt5Count = 0
        var mipmapCount = 0
        var lz4MipmapCount = 0
        var animatedCount = 0
        let rootPath = assets.standardizedFileURL.path + "/"
        let enumerator = FileManager.default.enumerator(
            at: assets,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles]
        )
        while let file = enumerator?.nextObject() as? URL {
            guard file.pathExtension == "tex", file.path.hasPrefix(rootPath) else {
                continue
            }
            let relativePath = String(file.path.dropFirst(rootPath.count))
            let texture = try createTexture(runtime: runtime, path: relativePath)
            var info = WESceneTextureInfo()
            var textureError: WESceneRuntimeErrorRef?
            XCTAssertEqual(
                we_scene_runtime_texture_info(texture, &info, &textureError),
                1,
                relativePath
            )
            XCTAssertNil(textureError)
            textureCount += 1
            switch info.format {
            case 0: argbCount += 1
            case 4: dxt5Count += 1
            case 8: rg88Count += 1
            case 9: r8Count += 1
            default: XCTFail("Unexpected texture format \(info.format): \(relativePath)")
            }
            if info.frame_count > 0 {
                animatedCount += 1
            }
            for image in 0..<Int(info.image_count) {
                let count = try textureMipmapCount(texture, image)
                mipmapCount += count
                for mipmap in 0..<count {
                    var mipInfo = WESceneTextureMipmapInfo()
                    var mipError: WESceneRuntimeErrorRef?
                    XCTAssertEqual(
                        we_scene_runtime_texture_mipmap_info(
                            texture, image, mipmap, &mipInfo, &mipError
                        ),
                        1,
                        relativePath
                    )
                    XCTAssertNil(mipError)
                    if mipInfo.compression == 1 {
                        lz4MipmapCount += 1
                    }
                }
            }
            we_scene_runtime_texture_destroy(texture)
        }
        XCTAssertEqual(textureCount, 311)
        XCTAssertEqual(argbCount, 191)
        XCTAssertEqual(r8Count, 60)
        XCTAssertEqual(rg88Count, 51)
        XCTAssertEqual(dxt5Count, 9)
        XCTAssertEqual(mipmapCount, 1_473)
        XCTAssertEqual(lz4MipmapCount, 494)
        XCTAssertEqual(animatedCount, 52)
    }

    private func errorMessage(_ error: WESceneRuntimeErrorRef?) -> String {
        runtimeString(we_scene_runtime_error_message(error))
    }

    private func runtimeString(_ value: UnsafePointer<CChar>?) -> String {
        guard let value else { return "" }
        return String(cString: value)
    }

    private func appendUInt32(_ value: UInt32, to data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
        data.append(UInt8(truncatingIfNeeded: value >> 16))
        data.append(UInt8(truncatingIfNeeded: value >> 24))
    }

    private func canonicalPath(_ url: URL) throws -> String {
        try url.path.withCString { path in
            guard let resolvedPath = realpath(path, nil) else {
                throw POSIXError(.init(rawValue: errno) ?? .EIO)
            }
            defer { free(resolvedPath) }
            return String(cString: resolvedPath)
        }
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

    private func makeTextureWithInvalidLZ4Payload() -> Data {
        makeTextureWithLZ4Mipmap(uncompressedSize: 1)
    }

    private func makeTextureWithMismatchedMipmapSize() -> Data {
        makeTextureWithLZ4Mipmap(uncompressedSize: 1024)
    }

    private func makeTextureWithLZ4Mipmap(uncompressedSize: UInt32) -> Data {
        var texture = Data()
        appendMagic("TEXV0005", to: &texture)
        appendMagic("TEXI0001", to: &texture)
        appendUInt32(9, to: &texture)
        appendUInt32(0, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(0, to: &texture)
        appendMagic("TEXB0003", to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(UInt32.max, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(uncompressedSize, to: &texture)
        appendUInt32(1, to: &texture)
        texture.append(0xff)
        return texture
    }

    private func makeTextureWithOversizedMipmap() -> Data {
        makeTextureWithLZ4Mipmap(
            uncompressedSize: 256 * 1024 * 1024 + 1
        )
    }

    private func makeTextureWithExtremeAnimationFrame() -> Data {
        var texture = Data()
        appendMagic("TEXV0005", to: &texture)
        appendMagic("TEXI0001", to: &texture)
        appendUInt32(9, to: &texture)
        appendUInt32(4, to: &texture)
        appendUInt32(2, to: &texture)
        appendUInt32(2, to: &texture)
        appendUInt32(2, to: &texture)
        appendUInt32(2, to: &texture)
        appendUInt32(0, to: &texture)
        appendMagic("TEXB0003", to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(UInt32.max, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(2, to: &texture)
        appendUInt32(2, to: &texture)
        appendUInt32(0, to: &texture)
        appendUInt32(4, to: &texture)
        appendUInt32(4, to: &texture)
        texture.append(contentsOf: [0, 0, 0, 0])
        appendMagic("TEXS0002", to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(0, to: &texture)
        appendFloat32(0.016, to: &texture)
        appendFloat32(0, to: &texture)
        appendFloat32(0, to: &texture)
        appendFloat32(Float.greatestFiniteMagnitude, to: &texture)
        appendFloat32(0, to: &texture)
        appendFloat32(0, to: &texture)
        appendFloat32(Float.greatestFiniteMagnitude, to: &texture)
        return texture
    }

    private func makeUnknownEmbeddedPNGTexture() -> Data {
        let png = Data(base64Encoded: "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=")!
        var texture = Data()
        appendMagic("TEXV0005", to: &texture)
        appendMagic("TEXI0001", to: &texture)
        appendUInt32(UInt32.max, to: &texture)
        appendUInt32(0, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(0, to: &texture)
        appendMagic("TEXB0003", to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(13, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(1, to: &texture)
        appendUInt32(0, to: &texture)
        appendUInt32(0, to: &texture)
        appendUInt32(UInt32(png.count), to: &texture)
        texture.append(png)
        return texture
    }

    private func appendMagic(_ value: String, to data: inout Data) {
        let bytes = Array(value.utf8)
        precondition(bytes.count == 8)
        data.append(contentsOf: bytes)
        data.append(0)
    }

    private func appendFloat32(_ value: Float, to data: inout Data) {
        appendUInt32(value.bitPattern, to: &data)
    }

    private func createRuntime(assets: URL, package: URL) throws -> WESceneRuntimeRef {
        var error: WESceneRuntimeErrorRef?
        let runtime = assets.path.withCString { assetsPath in
            package.path.withCString { packagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsPath,
                    scene_package_path: packagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }
        guard let runtime else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Runtime creation failed: \(message)")
            throw TestFailure.runtimeCreation
        }
        XCTAssertNil(error)
        return runtime
    }

    private func createAsset(
        runtime: WESceneRuntimeRef,
        path: String
    ) throws -> WESceneRuntimeAssetRef {
        var error: WESceneRuntimeErrorRef?
        let asset = path.withCString {
            we_scene_runtime_asset_create(runtime, $0, &error)
        }
        guard let asset else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Asset creation failed for \(path): \(message)")
            throw TestFailure.assetCreation
        }
        return asset
    }

    private func createTexture(
        runtime: WESceneRuntimeRef,
        path: String
    ) throws -> WESceneTextureRef {
        var error: WESceneRuntimeErrorRef?
        let texture = path.withCString {
            we_scene_runtime_texture_create(runtime, $0, &error)
        }
        guard let texture else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Texture creation failed for \(path): \(message)")
            throw TestFailure.textureCreation
        }
        return texture
    }

    private func translateShaderFiles(
        _ runtime: WESceneRuntimeRef,
        vertex: String,
        fragment: String
    ) throws -> WESceneShaderTranslationRef {
        var error: WESceneRuntimeErrorRef?
        let translation = vertex.withCString { vertexPath in
            fragment.withCString { fragmentPath in
                we_scene_runtime_shader_translate_files(
                    runtime,
                    vertexPath,
                    fragmentPath,
                    &error
                )
            }
        }
        guard let translation else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Shader translation failed for \(vertex) / \(fragment): \(message)")
            throw TestFailure.shaderTranslation
        }
        XCTAssertNil(error)
        return translation
    }

    private func shaderParameter(
        _ translation: WESceneShaderTranslationRef,
        stage: WESceneShaderStage,
        named name: String
    ) throws -> WESceneShaderParameterInfo {
        var count = 0
        var error: WESceneRuntimeErrorRef?
        guard we_scene_shader_translation_parameter_count(
            translation,
            stage,
            &count,
            &error
        ) == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.shaderMetadataQuery(message)
        }

        for index in 0..<count {
            var info = WESceneShaderParameterInfo()
            guard we_scene_shader_translation_parameter_info(
                translation,
                stage,
                index,
                &info,
                &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.shaderMetadataQuery(message)
            }
            if runtimeString(info.name) == name {
                return info
            }
        }
        throw TestFailure.shaderMetadataQuery(
            "Shader parameter '\(name)' was not found"
        )
    }

    private func assetData(_ asset: WESceneRuntimeAssetRef) -> Data {
        guard let bytes = we_scene_runtime_asset_bytes(asset) else { return Data() }
        return Data(bytes: bytes, count: we_scene_runtime_asset_length(asset))
    }

    private func packageEntryCount(_ runtime: WESceneRuntimeRef) throws -> Int {
        var count = 0
        var error: WESceneRuntimeErrorRef?
        guard we_scene_runtime_package_entry_count(runtime, &count, &error) == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Package entry count failed: \(message)")
            throw TestFailure.packageQuery
        }
        return count
    }

    private func textureMipmapCount(
        _ texture: WESceneTextureRef,
        _ image: Int
    ) throws -> Int {
        var count = 0
        var error: WESceneRuntimeErrorRef?
        guard we_scene_runtime_texture_mipmap_count(
            texture, image, &count, &error
        ) == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            XCTFail("Texture mipmap count failed: \(message)")
            throw TestFailure.textureQuery
        }
        return count
    }
}

private enum TestFailure: Error {
    case runtimeCreation
    case assetCreation
    case textureCreation
    case packageQuery
    case textureQuery
    case shaderTranslation
    case shaderMetadataQuery(String)
}
