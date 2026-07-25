import Foundation
import SceneModelTestSupport
import SceneRuntimeBridge
import XCTest

final class SceneModelBridgeTests: XCTestCase {
    private enum TestFailure: Error {
        case runtime(String)
        case model(String)
        case query(String)
    }

    private struct LoadedModel {
        let runtime: WESceneRuntimeRef
        let model: WESceneModelRef
    }

    private struct SyntheticModel {
        let root: URL
        let runtime: WESceneRuntimeRef
        let model: WESceneModelRef
    }

    private struct ModelErrorSnapshot {
        let code: WESceneRuntimeErrorCode
        let message: String
        let assetPath: String
        let jsonPointer: String
        let references: [String]
    }

    private struct TypedModelSnapshot {
        let handle: WESceneModelTestHandleRef
        let stats: WESceneModelTestStats
    }

    private func string(_ pointer: UnsafePointer<CChar>?) -> String {
        pointer.map(String.init(cString:)) ?? ""
    }

    private func errorMessage(_ error: WESceneRuntimeErrorRef?) -> String {
        string(we_scene_runtime_error_message(error))
    }

    private func loadTypedModel(
        assets: URL,
        package: URL
    ) throws -> TypedModelSnapshot {
        var stats = WESceneModelTestStats()
        var diagnostic = [CChar](repeating: 0, count: 4096)
        let handle = diagnostic.withUnsafeMutableBufferPointer { buffer in
            assets.path.withCString { assetsPath in
                package.path.withCString { packagePath in
                    "project.json".withCString { projectPath in
                        we_scene_model_test_load(
                            assetsPath,
                            packagePath,
                            projectPath,
                            &stats,
                            buffer.baseAddress,
                            buffer.count
                        )
                    }
                }
            }
        }
        guard let handle else {
            throw TestFailure.model(String(cString: diagnostic))
        }
        return TypedModelSnapshot(handle: handle, stats: stats)
    }

    private func checkTypedQuery(
        _ body: (UnsafeMutablePointer<CChar>?, Int) -> Int32
    ) throws {
        var diagnostic = [CChar](repeating: 0, count: 4096)
        let result = diagnostic.withUnsafeMutableBufferPointer { buffer in
            body(buffer.baseAddress, buffer.count)
        }
        guard result == 1 else {
            throw TestFailure.query(String(cString: diagnostic))
        }
    }

    private func typedTextureSlot(
        _ body: (
            UnsafeMutablePointer<WESceneModelTestTextureSlot>?,
            UnsafeMutablePointer<CChar>?,
            Int
        ) -> Int32
    ) throws -> (WESceneModelTestTextureKind, String?) {
        var slot = WESceneModelTestTextureSlot()
        try checkTypedQuery { errorBuffer, errorBufferSize in
            body(&slot, errorBuffer, errorBufferSize)
        }
        return (slot.kind, slot.name.map(String.init(cString:)))
    }

    private func typedTextInfo(
        _ handle: WESceneModelTestHandleRef,
        objectIndex: Int
    ) throws -> WESceneModelTestTextInfo {
        var info = WESceneModelTestTextInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_text_info(
                handle, objectIndex, &info, errorBuffer, size
            )
        }
        return info
    }

    private func appendUInt32(_ value: UInt32, to data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
        data.append(UInt8(truncatingIfNeeded: value >> 16))
        data.append(UInt8(truncatingIfNeeded: value >> 24))
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

    private func jsonData(_ object: Any) throws -> Data {
        try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
    }

    private func validDocuments() -> [String: Any] {
        let properties: [String: Any] = [
            "amount": [
                "fraction": true, "index": 2, "max": 1.0, "min": 0.0,
                "order": 20, "step": 0.1, "text": "Amount",
                "type": "slider", "value": 0.3,
            ],
            "count": [
                "fraction": false, "index": 3, "max": 10, "min": 0,
                "order": 20, "text": "Count", "type": "slider", "value": 2,
            ],
            "display": [
                "index": 5, "order": 30, "text": "Display only", "type": "text",
            ],
            "enabled": [
                "index": 0, "order": 10, "text": "Enabled",
                "type": "bool", "value": true,
            ],
            "mode": [
                "index": 1,
                "options": [
                    ["label": "A", "value": "a"],
                    ["label": "B", "value": "b"],
                ],
                "order": 10, "text": "Mode", "type": "combo", "value": "a",
            ],
            "tint": [
                "index": 4, "order": 20, "text": "Tint", "type": "color",
                "value": "1 0.5 0",
            ],
        ]
        let project: [String: Any] = [
            "file": "scene.json",
            "general": ["properties": properties],
            "preview": "preview.jpg",
            "title": "Typed fixture",
            "type": "scene",
            "version": 2,
        ]
        let image: [String: Any] = [
            "alpha": [
                "script": "export function update(value) { return value; }",
                "scriptproperties": [
                    "nested": ["user": "enabled", "value": true],
                    "scalar": 2,
                ],
                "value": 1.0,
            ],
            "effects": [[
                "file": "effects/test/effect.json",
                "id": 20,
                "passes": [
                    [
                        "id": 21,
                        "textures": [NSNull(), "mask", NSNull(), [
                            "keepaspect": true, "name": "object-slot",
                        ]],
                    ],
                    ["id": 22],
                ],
                "visible": true,
            ]],
            "id": 1,
            "image": "models/main.json",
            "instance": [
                "textures": [NSNull(), ["keepaspect": true, "name": "instance-slot"]],
            ],
            "name": "Image",
            "origin": "50 50 0",
            "size": "100 100",
            "visible": [
                "user": ["condition": "a", "name": "mode"],
                "value": true,
            ],
        ]
        let text: [String: Any] = [
            "dependencies": [[
                "id": 1, "index": 0, "type": "collisionmodel",
            ]],
            "font": "systemfont_arial",
            "id": 2,
            "name": "Text",
            "parent": 1,
            "padding": "1 2",
            "pointsize": 24,
            "size": "100 40",
            "spacing": "0 0",
            "text": [
                "script": "export function update(value) { return value; }",
                "scriptproperties": ["label": "nested"],
                "value": "hello",
            ],
        ]
        let sound: [String: Any] = [
            "id": 3,
            "maxtime": 1,
            "mintime": 0,
            "name": "Sound",
            "sound": ["sounds/test.mp3"],
            "volume": 0.5,
        ]
        let group: [String: Any] = [
            "dependencies": [2], "id": 4, "name": "Group",
            "origin": "0 0 0", "visible": true,
        ]
        let scene: [String: Any] = [
            "camera": [
                "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0",
                "farz": 10000, "fov": 50, "nearz": 0.01,
            ],
            "general": [
                "ambientcolor": "0.3 0.3 0.3",
                "cameraparallax": false,
                "camerapreview": true,
                "clearcolor": "0 0 0",
                "orthogonalprojection": ["height": 100, "width": 100],
            ],
            "objects": [image, text, sound, group],
            "version": 1,
        ]
        let model: [String: Any] = [
            "autosize": true,
            "cropoffset": "1 2",
            "material": "materials/main.json",
        ]
        let material: [String: Any] = [
            "passes": [[
                "blending": "translucent",
                "combos": ["VERSION": 2],
                "constantshadervalues": [
                    "array": [1, 2.5, NSNull()],
                    "object": ["value": ["flag": true]],
                ],
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": "genericimage2",
                "textures": [NSNull(), "main", NSNull(), [
                    "keepaspect": true, "name": "object-texture",
                ]],
            ]],
        ]
        let effect: [String: Any] = [
            "dependencies": ["authoring/missing.png", "authoring/missing.tex-json"],
            "fbos": [[
                "format": "r8", "height": 64, "name": "buffer",
                "uvs": "repeat", "width": 64,
            ]],
            "name": "Test effect",
            "passes": [
                [
                    "bind": [["index": 0, "name": "previous"]],
                    "compose": true,
                    "material": "materials/effect.json",
                    "target": "buffer",
                ],
                [
                    "command": "copy", "source": "buffer",
                    "target": "_rt_FullFrameBuffer",
                ],
            ],
            "version": 1,
        ]
        let effectMaterial: [String: Any] = [
            "passes": [[
                "blending": "normal", "cullmode": "normal",
                "depthtest": "enabled", "depthwrite": "enabled",
                "shader": "effects/test",
                "textures": [NSNull(), "effect-texture"],
            ]],
        ]
        return [
            "effects/test/effect.json": effect,
            "materials/effect.json": effectMaterial,
            "materials/main.json": material,
            "models/main.json": model,
            "preview.jpg": ["placeholder": true],
            "project.json": project,
            "scene.json": scene,
        ]
    }

    private func particleDocuments(inline: Bool = false) -> [String: Any] {
        var documents = validDocuments()
        let definition: [String: Any] = [
            "animationmode": NSNull(),
            "children": NSNull(),
            "controlpoint": [],
            "emitter": [
                [
                    "directions": "1 1 0", "distancemax": 64,
                    "distancemin": [4, 4, 0], "id": 10,
                    "instantaneous": 2, "name": "boxrandom", "rate": 12,
                ],
                [
                    "directions": [1, 1, 1], "distancemax": "2 2 2",
                    "distancemin": 0.5, "name": "sphererandom",
                    "sign": [1, -1, 0], "speedmax": 4, "speedmin": -2,
                ],
            ],
            "flags": NSNull(),
            "initializer": [
                ["id": 20, "max": 5, "min": 3, "name": "lifetimerandom"],
                ["exponent": 3, "max": 12, "min": 4, "name": "sizerandom"],
                ["max": "255 128 0", "min": "0.5 0.25 1.0", "name": "colorrandom"],
                ["max": 0.9, "min": 0.2, "name": "alpharandom"],
                ["max": "8 9 10", "min": "-1 -2 -3", "name": "velocityrandom"],
                ["name": "rotationrandom"],
            ],
            "material": "materials/main.json",
            "maxcount": 256,
            "operator": [
                ["drag": 0.25, "gravity": "0 -9 0", "name": "movement"],
                ["fadeintime": 0.1, "fadeouttime": 0.2, "name": "alphafade"],
            ],
            "sequencemultiplier": NSNull(),
            "starttime": 0,
        ]
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append([
            "id": 5,
            "instanceoverride": NSNull(),
            "name": "Particles",
            "origin": "25 30 0",
            "particle": inline ? definition : "particles/test.json",
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene
        if !inline {
            documents["particles/test.json"] = definition
        }
        return documents
    }

    private func makeSyntheticPackage(
        documents: [String: Any]
    ) throws -> (root: URL, assets: URL, package: URL) {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: shaders,
            withIntermediateDirectories: true
        )
        let entries = try documents.keys.sorted().map { key in
            (key, try jsonData(documents[key]!))
        }
        try makePackage(entries).write(to: package)
        return (root, assets, package)
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
            throw TestFailure.runtime(message)
        }
        return runtime
    }

    private func loadSynthetic(
        _ documents: [String: Any]
    ) throws -> SyntheticModel {
        let fixture = try makeSyntheticPackage(documents: documents)
        do {
            let runtime = try createRuntime(assets: fixture.assets, package: fixture.package)
            var error: WESceneRuntimeErrorRef?
            let model = "project.json".withCString {
                we_scene_runtime_model_create(runtime, $0, &error)
            }
            guard let model else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                we_scene_runtime_destroy(runtime)
                throw TestFailure.model(message)
            }
            return SyntheticModel(root: fixture.root, runtime: runtime, model: model)
        } catch {
            try? FileManager.default.removeItem(at: fixture.root)
            throw error
        }
    }

    private func loadSyntheticFailure(
        _ documents: [String: Any]
    ) throws -> ModelErrorSnapshot {
        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let runtime = try createRuntime(assets: fixture.assets, package: fixture.package)
        defer { we_scene_runtime_destroy(runtime) }
        var error: WESceneRuntimeErrorRef?
        let model = "project.json".withCString {
            we_scene_runtime_model_create(runtime, $0, &error)
        }
        XCTAssertNil(model, "Invalid fixture unexpectedly loaded")
        if let model {
            we_scene_model_destroy(model)
        }
        guard let error else {
            throw TestFailure.model("Invalid fixture returned no error")
        }
        defer { we_scene_runtime_error_destroy(error) }
        let count = we_scene_runtime_error_reference_count(error)
        return ModelErrorSnapshot(
            code: we_scene_runtime_error_code(error),
            message: errorMessage(error),
            assetPath: string(we_scene_runtime_error_asset_path(error)),
            jsonPointer: string(we_scene_runtime_error_json_pointer(error)),
            references: (0..<count).map {
                string(we_scene_runtime_error_reference_at(error, $0))
            }
        )
    }

    private func objectInfos(_ model: WESceneModelRef) throws -> [WESceneObjectInfo] {
        var count = 0
        var error: WESceneRuntimeErrorRef?
        guard we_scene_model_object_count(model, &count, &error) == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.query(message)
        }
        return try (0..<count).map { index in
            var info = WESceneObjectInfo()
            guard we_scene_model_object_info(model, index, &info, &error) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func propertyInfos(
        _ model: WESceneModelRef
    ) throws -> [(String, WEScenePropertyInfo, WEScenePropertyValue)] {
        var count = 0
        var error: WESceneRuntimeErrorRef?
        guard we_scene_model_property_count(model, &count, &error) == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.query(message)
        }
        return try (0..<count).map { index in
            var info = WEScenePropertyInfo()
            guard we_scene_model_property_info(model, index, &info, &error) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            let key = string(info.key)
            var value = WEScenePropertyValue()
            guard we_scene_model_property_value(model, index, &value, &error) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return (key, info, value)
        }
    }

    func testSyntheticDeepTypedModelContract() throws {
        let fixture = try makeSyntheticPackage(documents: validDocuments())
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.object_count, 4)
        XCTAssertEqual(typed.stats.image_object_count, 1)
        XCTAssertEqual(typed.stats.text_object_count, 1)
        XCTAssertEqual(typed.stats.sound_object_count, 1)
        XCTAssertEqual(typed.stats.group_object_count, 1)
        XCTAssertEqual(typed.stats.effect_instance_count, 1)
        XCTAssertEqual(typed.stats.effect_override_pass_count, 2)
        XCTAssertEqual(typed.stats.unique_effect_definition_count, 1)
        XCTAssertEqual(typed.stats.unique_effect_material_count, 1)
        XCTAssertEqual(typed.stats.unique_object_model_count, 1)
        XCTAssertEqual(typed.stats.unique_object_material_count, 1)
        XCTAssertEqual(typed.stats.total_effect_dependency_count, 2)
        XCTAssertEqual(typed.stats.dynamic_script_count, 2)
        XCTAssertEqual(typed.stats.dynamic_user_count, 2)
        XCTAssertEqual(typed.stats.dynamic_condition_count, 1)
        XCTAssertEqual(typed.stats.dynamic_script_property_count, 2)

        let instanceNull = try typedTextureSlot { outSlot, errorBuffer, size in
            we_scene_model_test_object_texture_slot(
                typed.handle,
                0,
                WE_SCENE_MODEL_TEST_TEXTURES,
                0,
                outSlot,
                errorBuffer,
                size
            )
        }
        XCTAssertEqual(instanceNull.0, WE_SCENE_MODEL_TEST_TEXTURE_NULL)
        XCTAssertNil(instanceNull.1)
        let instanceName = try typedTextureSlot { outSlot, errorBuffer, size in
            we_scene_model_test_object_texture_slot(
                typed.handle,
                0,
                WE_SCENE_MODEL_TEST_TEXTURES,
                1,
                outSlot,
                errorBuffer,
                size
            )
        }
        XCTAssertEqual(instanceName.0, WE_SCENE_MODEL_TEST_TEXTURE_NAME)
        XCTAssertEqual(instanceName.1, "instance-slot")

        let effectNull = try typedTextureSlot { outSlot, errorBuffer, size in
            we_scene_model_test_effect_texture_slot(
                typed.handle,
                0,
                0,
                0,
                WE_SCENE_MODEL_TEST_TEXTURES,
                0,
                outSlot,
                errorBuffer,
                size
            )
        }
        XCTAssertEqual(effectNull.0, WE_SCENE_MODEL_TEST_TEXTURE_NULL)
        let effectName = try typedTextureSlot { outSlot, errorBuffer, size in
            we_scene_model_test_effect_texture_slot(
                typed.handle,
                0,
                0,
                0,
                WE_SCENE_MODEL_TEST_TEXTURES,
                1,
                outSlot,
                errorBuffer,
                size
            )
        }
        XCTAssertEqual(effectName.1, "effect-texture")

        let overrideNull = try typedTextureSlot { outSlot, errorBuffer, size in
            we_scene_model_test_effect_override_texture_slot(
                typed.handle,
                0,
                0,
                0,
                WE_SCENE_MODEL_TEST_TEXTURES,
                0,
                outSlot,
                errorBuffer,
                size
            )
        }
        XCTAssertEqual(overrideNull.0, WE_SCENE_MODEL_TEST_TEXTURE_NULL)
        let overrideName = try typedTextureSlot { outSlot, errorBuffer, size in
            we_scene_model_test_effect_override_texture_slot(
                typed.handle,
                0,
                0,
                0,
                WE_SCENE_MODEL_TEST_TEXTURES,
                3,
                outSlot,
                errorBuffer,
                size
            )
        }
        XCTAssertEqual(overrideName.1, "object-slot")

        var dependency: UnsafePointer<CChar>?
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_effect_dependency(
                typed.handle,
                0,
                0,
                1,
                &dependency,
                errorBuffer,
                size
            )
        }
        XCTAssertEqual(string(dependency), "authoring/missing.tex-json")

        var soundSource: UnsafePointer<CChar>?
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_sound_source(
                typed.handle,
                2,
                0,
                &soundSource,
                errorBuffer,
                size
            )
        }
        XCTAssertEqual(string(soundSource), "sounds/test.mp3")
    }

    func testSyntheticParticleDefinitionLoadsFromVFSWithTypedDefaults() throws {
        let fixture = try makeSyntheticPackage(documents: particleDocuments())
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.object_count, 5)
        XCTAssertEqual(typed.stats.particle_object_count, 1)
        XCTAssertEqual(typed.stats.group_object_count, 1)
        XCTAssertEqual(typed.stats.unique_object_material_count, 1)

        var info = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &info, errorBuffer, size
            )
        }
        XCTAssertEqual(info.object_id, 5)
        XCTAssertEqual(string(info.asset_path), "particles/test.json")
        XCTAssertEqual(string(info.material_asset_path), "materials/main.json")
        XCTAssertEqual(info.max_count, 256)
        XCTAssertEqual(info.flags, 0)
        XCTAssertEqual(info.emitter_count, 2)
        XCTAssertEqual(info.initializer_count, 6)
        XCTAssertEqual(info.operator_count, 2)
        XCTAssertEqual(string(info.renderer_name), "sprite")
        XCTAssertEqual(string(info.renderer_orientation), "screen")

        var size = WESceneModelTestParticleInitializerInfo()
        try checkTypedQuery { errorBuffer, count in
            we_scene_model_test_particle_initializer_info(
                typed.handle, 4, 1, &size, errorBuffer, count
            )
        }
        XCTAssertEqual(size.kind, WE_SCENE_MODEL_TEST_PARTICLE_SIZE_RANDOM)
        XCTAssertEqual(size.minimum_is_number, 1)
        XCTAssertEqual(size.minimum_number, 4)
        XCTAssertEqual(size.maximum_number, 12)
        XCTAssertEqual(size.has_exponent, 1)
        XCTAssertEqual(size.exponent, 3)

        var color = WESceneModelTestParticleInitializerInfo()
        try checkTypedQuery { errorBuffer, count in
            we_scene_model_test_particle_initializer_info(
                typed.handle, 4, 2, &color, errorBuffer, count
            )
        }
        XCTAssertEqual(color.kind, WE_SCENE_MODEL_TEST_PARTICLE_COLOR_RANDOM)
        XCTAssertEqual(
            string(color.minimum_text),
            "0.500000, 0.250000, 1.000000, 1.000000"
        )
        let normalizedMaximum = string(color.maximum_text)
            .split(separator: ",")
            .compactMap {
                Double($0.trimmingCharacters(in: .whitespaces))
            }
        XCTAssertEqual(normalizedMaximum.count, 4)
        if normalizedMaximum.count == 4 {
            XCTAssertEqual(normalizedMaximum[0], 1, accuracy: 1e-6)
            XCTAssertEqual(normalizedMaximum[1], 128.0 / 255.0, accuracy: 1e-6)
            XCTAssertEqual(normalizedMaximum[2], 0, accuracy: 1e-6)
            XCTAssertEqual(normalizedMaximum[3], 1, accuracy: 1e-6)
        }
    }

    func testSyntheticInlineParticleRetainsScenePointerIdentity() throws {
        let fixture = try makeSyntheticPackage(documents: particleDocuments(inline: true))
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }
        var info = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &info, errorBuffer, size
            )
        }
        XCTAssertEqual(
            string(info.asset_path),
            "scene.json#/objects/4/particle"
        )
    }

    func testSyntheticNullImageAndModelPlaceholdersSelectParticle() throws {
        var documents = particleDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[4]["image"] = NSNull()
        objects[4]["model"] = NSNull()
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.particle_object_count, 1)
        var info = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &info, errorBuffer, size
            )
        }
        XCTAssertEqual(info.object_id, 5)
        XCTAssertEqual(string(info.asset_path), "particles/test.json")
    }

    func testSyntheticTwoActiveObjectDiscriminatorsUseUpstreamPrecedence() throws {
        var documents = particleDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[4]["image"] = "models/main.json"
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.object_count, 5)
        XCTAssertEqual(typed.stats.image_object_count, 2)
        XCTAssertEqual(typed.stats.particle_object_count, 0)
    }

    func testSyntheticUnsupportedObjectKindsRemainGenericNodes() throws {
        var documents = validDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append(contentsOf: [
            ["id": 5, "light": ["radius": 20], "name": "Light"],
            ["id": 6, "name": "Volume", "shape": "cone"],
            ["custommetadata": true, "id": 7, "name": "Unknown"],
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.object_count, 7)
        XCTAssertEqual(typed.stats.group_object_count, 4)
    }

    func testSyntheticNumericStringDynamicScalarMatchesUpstream() throws {
        var documents = validDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[1]["pointsize"] = [
            "animation": ["options": ["startpaused": true]],
            "value": "24.5",
        ]
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        let text = try typedTextInfo(typed.handle, objectIndex: 1)
        XCTAssertEqual(text.point_size, 24.5)
    }

    func testSyntheticProjectPropertiesFollowUpstreamAcceptance() throws {
        var documents = validDocuments()
        var project = documents["project.json"] as! [String: Any]
        var general = project["general"] as! [String: Any]
        var properties = general["properties"] as! [String: Any]
        properties["group"] = [
            "index": "ignored", "type": "group",
        ]
        properties["unknown"] = [
            "min": "ignored", "type": "future-property-type",
        ]
        properties["missingType"] = [
            "step": "ignored",
        ]
        properties["unboundedDefault"] = [
            "max": 0, "min": 10, "step": -1, "type": "slider", "value": 5,
        ]
        properties["emptyCombo"] = [
            "options": [], "type": "combo", "value": "missing",
        ]
        properties["emptyFile"] = [
            "type": "file",
        ]
        general["properties"] = properties
        project["general"] = general
        documents["project.json"] = project

        let loaded = try loadSynthetic(documents)
        defer {
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }

        let parsed = try propertyInfos(loaded.model)
        let keys = Set(parsed.map(\.0))
        XCTAssertFalse(keys.contains("group"))
        XCTAssertFalse(keys.contains("unknown"))
        XCTAssertFalse(keys.contains("missingType"))
        XCTAssertTrue(keys.contains("unboundedDefault"))
        XCTAssertTrue(keys.contains("emptyCombo"))

        let emptyFile = try XCTUnwrap(parsed.first { $0.0 == "emptyFile" })
        XCTAssertEqual(emptyFile.2.type, WE_SCENE_VALUE_STRING)
        XCTAssertEqual(string(emptyFile.2.string_value), "")
    }

    func testSyntheticNullOptionalFieldsUseUpstreamDefaults() throws {
        var documents = validDocuments()

        var project = documents["project.json"] as! [String: Any]
        var projectGeneral = project["general"] as! [String: Any]
        project["workshopid"] = NSNull()
        projectGeneral["supportsaudioprocessing"] = NSNull()
        project["general"] = projectGeneral
        documents["project.json"] = project

        var scene = documents["scene.json"] as! [String: Any]
        var sceneGeneral = scene["general"] as! [String: Any]
        sceneGeneral["bloom"] = NSNull()
        sceneGeneral["cameraparallaxdelay"] = NSNull()
        sceneGeneral["camerapreview"] = NSNull()
        var objects = scene["objects"] as! [[String: Any]]
        objects[0]["effects"] = NSNull()
        objects[0]["instance"] = NSNull()
        objects[0]["parent"] = NSNull()
        scene["general"] = sceneGeneral
        scene["objects"] = objects
        documents["scene.json"] = scene

        var model = documents["models/main.json"] as! [String: Any]
        model["cropoffset"] = NSNull()
        model["height"] = NSNull()
        documents["models/main.json"] = model

        var material = documents["materials/main.json"] as! [String: Any]
        var passes = material["passes"] as! [[String: Any]]
        passes[0]["combos"] = NSNull()
        passes[0]["constantshadervalues"] = NSNull()
        passes[0]["textures"] = NSNull()
        material["passes"] = passes
        documents["materials/main.json"] = material

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.object_count, 4)
        XCTAssertEqual(typed.stats.effect_instance_count, 0)
        XCTAssertEqual(typed.stats.dynamic_user_count, 2)
    }

    func testProjectionDimensionsAreParsedWithoutLoaderRangePolicy() throws {
        var documents = validDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var general = scene["general"] as! [String: Any]
        general["orthogonalprojection"] = ["height": -1, "width": 0]
        scene["general"] = general
        documents["scene.json"] = scene

        let loaded = try loadSynthetic(documents)
        defer {
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }
        var info = WESceneProjectInfo()
        var error: WESceneRuntimeErrorRef?
        guard we_scene_model_project_info(loaded.model, &info, &error) == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.query(message)
        }
        XCTAssertEqual(info.projection_width, 0)
        XCTAssertEqual(info.projection_height, -1)
    }

    func testSyntheticExpandedParticleContractsRetainTypedValues() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        var initializers = definition["initializer"] as! [[String: Any]]
        initializers.append([
            "exponent": 2,
            "max": "0 0 9",
            "min": "0 0 -7",
            "name": "angularvelocityrandom",
        ])
        initializers.append([
            "forward": "0 1 0",
            "name": "turbulentvelocityrandom",
            "offset": 3,
            "phasemax": 0.75,
            "phasemin": 0.25,
            "right": "0 0 1",
            "scale": 2,
            "speedmax": 60,
            "speedmin": 20,
            "timescale": 0.5,
        ])
        definition["initializer"] = initializers
        var operators = definition["operator"] as! [[String: Any]]
        operators.append([
            "drag": 0.2, "force": "0 0 4", "name": "angularmovement",
        ])
        operators.append([
            "frequencymax": 4, "frequencymin": 2, "mask": "1 0 1",
            "name": "oscillateposition", "phasemax": 1, "phasemin": 0,
            "scalemax": 8, "scalemin": 3,
        ])
        operators.append([
            "frequencymax": 6, "frequencymin": 3,
            "name": "oscillatealpha", "phasemax": 1, "phasemin": 0,
            "scalemax": 0.8, "scalemin": 0.2,
        ])
        operators.append([
            "controlpoint": 1, "name": "controlpointattract",
            "origin": "1 2 3", "scale": 200, "threshold": 500,
        ])
        definition["operator"] = operators
        definition["controlpoint"] = [[
            "flags": 0, "id": 1, "locktopointer": true,
            "offset": "5 6 7",
        ]]
        definition["flags"] = 4
        definition["starttime"] = 1.5
        definition["animationmode"] = "randomframe"
        definition["sequencemultiplier"] = 2
        documents["particles/test.json"] = definition

        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[4]["parallaxDepth"] = "0.1 0.2"
        objects[4]["instanceoverride"] = [
            "alpha": 0.5,
            "color": "0.8 0.7 0.6",
            "colorn": "0.9 0.8 0.7",
            "count": 2,
            "enabled": true,
            "lifetime": 1.5,
            "rate": 3,
            "size": 1.25,
            "speed": 0.75,
        ]
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        var particle = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &particle, errorBuffer, size
            )
        }
        XCTAssertEqual(particle.flags, 4)
        XCTAssertEqual(particle.initializer_count, 8)
        XCTAssertEqual(particle.operator_count, 6)

        var angular = WESceneModelTestParticleInitializerInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_initializer_info(
                typed.handle, 4, 6, &angular, errorBuffer, size
            )
        }
        XCTAssertEqual(
            angular.kind,
            WE_SCENE_MODEL_TEST_PARTICLE_ANGULAR_VELOCITY_RANDOM
        )
        XCTAssertEqual(
            string(angular.minimum_text),
            "0.000000, 0.000000, -7.000000"
        )
        XCTAssertEqual(
            string(angular.maximum_text),
            "0.000000, 0.000000, 9.000000"
        )
        XCTAssertEqual(angular.exponent, 2)

        var turbulent = WESceneModelTestParticleInitializerInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_initializer_info(
                typed.handle, 4, 7, &turbulent, errorBuffer, size
            )
        }
        XCTAssertEqual(
            turbulent.kind,
            WE_SCENE_MODEL_TEST_PARTICLE_TURBULENT_VELOCITY_RANDOM
        )
        XCTAssertEqual(turbulent.minimum_number, 20)
        XCTAssertEqual(turbulent.maximum_number, 60)
    }

    func testSyntheticParticleColorDefaultsUseNormalizedBlackAndWhite() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        var initializers = definition["initializer"] as! [[String: Any]]
        initializers[2].removeValue(forKey: "min")
        initializers[2].removeValue(forKey: "max")
        definition["initializer"] = initializers
        documents["particles/test.json"] = definition

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(
            assets: fixture.assets,
            package: fixture.package
        )
        defer { we_scene_model_test_destroy(typed.handle) }

        var color = WESceneModelTestParticleInitializerInfo()
        try checkTypedQuery { errorBuffer, count in
            we_scene_model_test_particle_initializer_info(
                typed.handle, 4, 2, &color, errorBuffer, count
            )
        }
        XCTAssertEqual(
            string(color.minimum_text),
            "0.000000, 0.000000, 0.000000, 1.000000"
        )
        XCTAssertEqual(
            string(color.maximum_text),
            "1.000000, 1.000000, 1.000000, 1.000000"
        )
    }

    func testSyntheticNonFiniteDynamicStringsFailConsistently() throws {
        for source in ["nan", "nan 0"] {
            var documents = validDocuments()
            var scene = documents["scene.json"] as! [String: Any]
            var objects = scene["objects"] as! [[String: Any]]
            objects[0]["scale"] = source
            scene["objects"] = objects
            documents["scene.json"] = scene

            let failure = try loadSyntheticFailure(documents)
            XCTAssertEqual(failure.jsonPointer, "/objects/0/scale")
            XCTAssertTrue(failure.message.contains("finite"))
        }
    }

    func testSyntheticConditionalUserBindingRequiresCondition() throws {
        var documents = validDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[0]["visible"] = [
            "user": ["name": "enabled"],
            "value": true,
        ]
        scene["objects"] = objects
        documents["scene.json"] = scene

        let failure = try loadSyntheticFailure(documents)
        XCTAssertEqual(failure.jsonPointer, "/objects/0/visible/user/condition")
        XCTAssertTrue(failure.message.contains("condition"))
    }

    func testSyntheticOpaqueParticleDefinitionFlagsAreRetained() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        definition["flags"] = 248
        documents["particles/test.json"] = definition

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        var particle = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &particle, errorBuffer, size
            )
        }
        XCTAssertEqual(particle.flags, 248)
    }

    func testSyntheticLinuxPermissiveMetadataDoesNotAbortTheModel() throws {
        var documents = validDocuments()
        var effect = documents["effects/test/effect.json"] as! [String: Any]
        effect["dependencies"] = "stale-authoring-value"
        effect["fbos"] = "stale-authoring-value"
        effect["passes"] = [
            [
                "bind": "stale-authoring-value",
                "command": "legacy-swap-command",
                "material": "materials/effect.json",
                "source": "previous",
                "target": "buffer",
            ],
            [
                "bind": [
                    ["index": -1, "name": "previous"],
                    ["index": 0, "name": "first"],
                    ["index": 0, "name": "duplicate"],
                ],
            ],
        ]
        documents["effects/test/effect.json"] = effect

        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[3]["name"] = 42
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(
            assets: fixture.assets,
            package: fixture.package
        )
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.object_count, 4)
        XCTAssertEqual(typed.stats.unique_effect_definition_count, 1)
        XCTAssertEqual(typed.stats.unique_effect_material_count, 1)
        XCTAssertEqual(typed.stats.total_effect_dependency_count, 0)
    }

    func testSyntheticParticleIgnoresOpaqueFlagsAndMalformedCollections() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        var emitters = definition["emitter"] as! [[String: Any]]
        emitters[0]["flags"] = 0x4000_0006
        emitters[0]["controlpoint"] = -2
        definition["emitter"] = emitters
        var operators = definition["operator"] as! [[String: Any]]
        operators[operators.count - 1]["controlpoint"] = -2
        definition["operator"] = operators
        definition["controlpoint"] = [
            ["flags": 0x4000_0003, "id": 0],
            ["flags": 0x4000_0003],
        ]
        definition["initializer"] = "stale-authoring-value"
        definition["renderer"] = "stale-authoring-value"
        documents["particles/test.json"] = definition

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(
            assets: fixture.assets,
            package: fixture.package
        )
        defer { we_scene_model_test_destroy(typed.handle) }

        var particle = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &particle, errorBuffer, size
            )
        }
        XCTAssertEqual(particle.emitter_count, 2)
        XCTAssertEqual(particle.initializer_count, 0)
        XCTAssertEqual(string(particle.renderer_name), "sprite")
    }

    func testSyntheticParticleUnknownAnimationModeUsesLinuxSequenceFallback() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        definition["animationmode"] = "legacy-animation-mode"
        documents["particles/test.json"] = definition

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(
            assets: fixture.assets,
            package: fixture.package
        )
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.particle_object_count, 1)
    }

    func testSyntheticParticleNonpositiveSequenceMultiplierUsesLinuxDefault() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        definition["sequencemultiplier"] = 0
        documents["particles/test.json"] = definition

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(
            assets: fixture.assets,
            package: fixture.package
        )
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.particle_object_count, 1)
    }

    func testSyntheticParticleZeroMaxCountUsesLinuxDefaultPool() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        definition["maxcount"] = 0
        documents["particles/test.json"] = definition

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(
            assets: fixture.assets,
            package: fixture.package
        )
        defer { we_scene_model_test_destroy(typed.handle) }

        var particle = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &particle, errorBuffer, size
            )
        }
        XCTAssertEqual(particle.max_count, 1_000)
    }

    func testSyntheticParticleIgnoresLinuxRendererOrientationMetadata() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        definition["renderer"] = [[
            "axis": [1, 0, 0],
            "name": "sprite",
            "orientation": "world",
        ]]
        documents["particles/test.json"] = definition

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(
            assets: fixture.assets,
            package: fixture.package
        )
        defer { we_scene_model_test_destroy(typed.handle) }

        var particle = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &particle, errorBuffer, size
            )
        }
        XCTAssertEqual(string(particle.renderer_orientation), "screen")
    }

    func testSyntheticParticleUsesLinuxVectorShapeFallbacks() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        var emitters = definition["emitter"] as! [[String: Any]]
        emitters[0]["directions"] = [1, 2, 3, 4]
        emitters[0]["origin"] = [1, 2]
        emitters[0]["distancemin"] = ["stale", "value"]
        emitters[1]["sign"] = [1, -1, 0, 9]
        definition["emitter"] = emitters
        definition["controlpoint"] = [[
            "id": 0,
            "offset": ["stale", "authoring", "value"],
        ]]
        documents["particles/test.json"] = definition

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(
            assets: fixture.assets,
            package: fixture.package
        )
        defer { we_scene_model_test_destroy(typed.handle) }

        var particle = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &particle, errorBuffer, size
            )
        }
        XCTAssertEqual(particle.emitter_count, 2)
    }

    func testSyntheticParticlePointerLinkUsesFlagsInsteadOfLegacyField() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        definition["controlpoint"] = [[
            "flags": 0,
            "id": 0,
            "locktopointer": ["stale": true],
        ]]
        documents["particles/test.json"] = definition

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(
            assets: fixture.assets,
            package: fixture.package
        )
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.particle_object_count, 1)
    }

    func testSyntheticParticleUnsupportedContractsAreStructured() throws {
        func mutateDefinition(
            _ body: (inout [String: Any]) -> Void
        ) -> [String: Any] {
            var documents = particleDocuments()
            var definition = documents["particles/test.json"] as! [String: Any]
            body(&definition)
            documents["particles/test.json"] = definition
            return documents
        }

        let unsupportedRenderer = try loadSyntheticFailure(mutateDefinition {
            $0["renderer"] = [["name": "spritetrail"]]
        })
        XCTAssertEqual(unsupportedRenderer.jsonPointer, "/renderer/0/name")

        let children = try loadSyntheticFailure(mutateDefinition {
            $0["children"] = [["particle": "particles/child.json"]]
        })
        XCTAssertEqual(children.jsonPointer, "/children")
        XCTAssertEqual(
            children.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_UNSUPPORTED_OBJECT
        )
    }

    func testSyntheticParticleMetadataAndParentFollowUpstreamAcceptance() throws {
        var documents = particleDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        definition["editoronly"] = true

        var emitters = definition["emitter"] as! [[String: Any]]
        emitters[0]["audioprocessingmode"] = 1
        emitters[0]["speedmin"] = 1
        emitters.append(["name": "point"])
        definition["emitter"] = emitters

        var initializers = definition["initializer"] as! [[String: Any]]
        initializers[0]["exponent"] = 2
        initializers.append(["name": "mapsequencearoundcontrolpoint"])
        definition["initializer"] = initializers

        var operators = definition["operator"] as! [[String: Any]]
        operators[0]["editoronly"] = true
        operators.append(["name": "turbulence"])
        definition["operator"] = operators
        definition["controlpoint"] = [["editoronly": true, "id": 0]]
        definition["renderer"] = [["editoronly": true, "name": "sprite"]]
        documents["particles/test.json"] = definition

        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[4]["image"] = ["legacy": true]
        objects[4]["model"] = 1
        objects[4]["parallaxdepth"] = "0 0"
        objects[4]["parent"] = 1
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeSyntheticPackage(documents: documents)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let typed = try loadTypedModel(assets: fixture.assets, package: fixture.package)
        defer { we_scene_model_test_destroy(typed.handle) }

        XCTAssertEqual(typed.stats.particle_object_count, 1)
        var particle = WESceneModelTestParticleInfo()
        try checkTypedQuery { errorBuffer, size in
            we_scene_model_test_particle_info(
                typed.handle, 4, &particle, errorBuffer, size
            )
        }
        XCTAssertEqual(particle.object_id, 5)
        XCTAssertEqual(string(particle.asset_path), "particles/test.json")
        XCTAssertEqual(particle.emitter_count, 2)
        XCTAssertEqual(particle.initializer_count, 6)
        XCTAssertEqual(particle.operator_count, 2)
    }

    func testSyntheticParticleReferenceAndTypeFailuresAreExplicit() throws {
        var missing = particleDocuments()
        missing.removeValue(forKey: "particles/test.json")
        let missingDefinition = try loadSyntheticFailure(missing)
        XCTAssertEqual(
            missingDefinition.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_DANGLING_REFERENCE
        )
        XCTAssertEqual(missingDefinition.assetPath, "particles/test.json")
        XCTAssertEqual(missingDefinition.jsonPointer, "")
        XCTAssertTrue(missingDefinition.references.contains(
            "scene.json#/objects/4/particle -> particles/test.json"
        ))

        var badType = particleDocuments()
        var scene = badType["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[4]["particle"] = 42
        scene["objects"] = objects
        badType["scene.json"] = scene
        let typeError = try loadSyntheticFailure(badType)
        XCTAssertEqual(
            typeError.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_TYPE_MISMATCH
        )
        XCTAssertEqual(typeError.assetPath, "scene.json")
        XCTAssertEqual(typeError.jsonPointer, "/objects/4/particle")

        var badDefinitionType = particleDocuments()
        var badDefinition = badDefinitionType["particles/test.json"] as! [String: Any]
        badDefinition["material"] = 7
        badDefinitionType["particles/test.json"] = badDefinition
        let materialType = try loadSyntheticFailure(badDefinitionType)
        XCTAssertEqual(
            materialType.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_TYPE_MISMATCH
        )
        XCTAssertEqual(materialType.assetPath, "particles/test.json")
        XCTAssertEqual(materialType.jsonPointer, "/material")

        var missingMaterial = particleDocuments()
        var definition = missingMaterial["particles/test.json"] as! [String: Any]
        definition["material"] = "materials/missing.json"
        missingMaterial["particles/test.json"] = definition
        let materialError = try loadSyntheticFailure(missingMaterial)
        XCTAssertEqual(
            materialError.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_DANGLING_REFERENCE
        )
        XCTAssertEqual(materialError.assetPath, "materials/missing.json")
        XCTAssertTrue(materialError.references.contains(
            "particles/test.json#/material -> materials/missing.json"
        ))
    }

    func testSyntheticTypedStateSetterAndRevisionContract() throws {
        let loaded = try loadSynthetic(validDocuments())
        defer {
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.root)
        }

        let properties = try propertyInfos(loaded.model)
        XCTAssertEqual(properties.map(\.0), [
            "enabled", "mode", "amount", "count", "tint", "display",
        ])

        var objectDependency = WESceneObjectDependencyInfo()
        var dependencyError: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_model_object_dependency_info(
                loaded.model,
                1,
                0,
                &objectDependency,
                &dependencyError
            ),
            1,
            errorMessage(dependencyError)
        )
        XCTAssertNil(dependencyError)
        XCTAssertEqual(objectDependency.id, 1)
        XCTAssertEqual(objectDependency.has_index, 1)
        XCTAssertEqual(objectDependency.index, 0)
        XCTAssertEqual(string(objectDependency.type), "collisionmodel")

        // The shorthand integer form and the legacy id-only bridge remain
        // source compatible while typed metadata is retained by the model.
        var dependencyID: Int32 = 0
        XCTAssertEqual(
            we_scene_model_object_dependency(
                loaded.model,
                3,
                0,
                &dependencyID,
                &dependencyError
            ),
            1,
            errorMessage(dependencyError)
        )
        XCTAssertEqual(dependencyID, 2)
        XCTAssertEqual(properties.last?.2.type, WE_SCENE_VALUE_NULL)

        var modeIndex = 0
        var enabledIndex = 0
        var amountIndex = 0
        for (index, property) in properties.enumerated() {
            switch property.0 {
                case "mode": modeIndex = index
                case "enabled": enabledIndex = index
                case "amount": amountIndex = index
                default: break
            }
        }
        var optionCount = 0
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_model_property_option_count(
                loaded.model,
                modeIndex,
                &optionCount,
                &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(optionCount, 2)
        var option = WEScenePropertyOptionInfo()
        XCTAssertEqual(
            we_scene_model_property_option_info(
                loaded.model,
                modeIndex,
                1,
                &option,
                &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(string(option.value), "b")

        var revision: UInt64 = .max
        XCTAssertEqual(we_scene_model_revision(loaded.model, &revision, &error), 1)
        XCTAssertEqual(revision, 0)

        var enabled = WEScenePropertyValue(
            type: WE_SCENE_VALUE_BOOLEAN,
            boolean_value: 0,
            integer_value: 0,
            number_value: 0,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        XCTAssertEqual(
            we_scene_model_set_property_value(
                loaded.model,
                "enabled",
                &enabled,
                &error
            ),
            1,
        )
        XCTAssertNil(error)
        XCTAssertEqual(we_scene_model_revision(loaded.model, &revision, &error), 1)
        XCTAssertEqual(revision, 1)

        // Repeating the same value is observable as a no-op.
        XCTAssertEqual(
            we_scene_model_set_property_value(
                loaded.model,
                "enabled",
                &enabled,
                &error
            ),
            1
        )
        XCTAssertEqual(we_scene_model_revision(loaded.model, &revision, &error), 1)
        XCTAssertEqual(revision, 1)

        var amount = WEScenePropertyValue(
            type: WE_SCENE_VALUE_NUMBER,
            boolean_value: 0,
            integer_value: 0,
            number_value: 0.4,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        XCTAssertEqual(
            we_scene_model_set_property_value(
                loaded.model,
                "amount",
                &amount,
                &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(we_scene_model_revision(loaded.model, &revision, &error), 1)
        XCTAssertEqual(revision, 2)

        var invalidAmount = amount
        invalidAmount.number_value = 1.1
        XCTAssertEqual(
            we_scene_model_set_property_value(
                loaded.model,
                "amount",
                &invalidAmount,
                &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_VALUE
        )
        XCTAssertTrue(errorMessage(error).contains("minimum") ||
                      errorMessage(error).contains("maximum"))
        we_scene_runtime_error_destroy(error)
        error = nil
        XCTAssertEqual(we_scene_model_revision(loaded.model, &revision, &error), 1)
        XCTAssertEqual(revision, 2)

        var enabledAgain = WEScenePropertyValue(
            type: WE_SCENE_VALUE_BOOLEAN,
            boolean_value: 1,
            integer_value: 0,
            number_value: 0,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        let batchAmount = WEScenePropertyValue(
            type: WE_SCENE_VALUE_NUMBER,
            boolean_value: 0,
            integer_value: 0,
            number_value: 0.6,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        let batchResult = "enabled".withCString { enabledKey in
            "amount".withCString { amountKey in
                var updates = [
                    WEScenePropertyUpdate(key: enabledKey, value: enabledAgain),
                    WEScenePropertyUpdate(key: amountKey, value: batchAmount),
                ]
                return we_scene_model_set_property_values(
                    loaded.model, &updates, updates.count, &error
                )
            }
        }
        XCTAssertEqual(batchResult, 1, errorMessage(error))
        XCTAssertEqual(we_scene_model_revision(loaded.model, &revision, &error), 1)
        XCTAssertEqual(revision, 3, "A multi-value commit must advance one revision")

        var invalidBatchAmount = batchAmount
        invalidBatchAmount.number_value = 1.1
        enabledAgain.boolean_value = 0
        let invalidBatchResult = "enabled".withCString { enabledKey in
            "amount".withCString { amountKey in
                var updates = [
                    WEScenePropertyUpdate(key: enabledKey, value: enabledAgain),
                    WEScenePropertyUpdate(key: amountKey, value: invalidBatchAmount),
                ]
                return we_scene_model_set_property_values(
                    loaded.model, &updates, updates.count, &error
                )
            }
        }
        XCTAssertEqual(invalidBatchResult, 0)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_VALUE
        )
        we_scene_runtime_error_destroy(error)
        error = nil
        XCTAssertEqual(we_scene_model_revision(loaded.model, &revision, &error), 1)
        XCTAssertEqual(revision, 3, "A rejected batch must not partially mutate the model")

        var currentEnabled = WEScenePropertyValue()
        XCTAssertEqual(
            we_scene_model_property_value(
                loaded.model, enabledIndex, &currentEnabled, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(currentEnabled.type, WE_SCENE_VALUE_BOOLEAN)
        XCTAssertEqual(currentEnabled.boolean_value, 1)

        var nonIntegerCount = WEScenePropertyValue(
            type: WE_SCENE_VALUE_NUMBER,
            boolean_value: 0,
            integer_value: 0,
            number_value: 2.5,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        XCTAssertEqual(
            we_scene_model_set_property_value(
                loaded.model,
                "count",
                &nonIntegerCount,
                &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_VALUE
        )
        we_scene_runtime_error_destroy(error)
        error = nil

        let badComboResult = "missing".withCString { stringValue in
            var badCombo = WEScenePropertyValue(
                type: WE_SCENE_VALUE_STRING,
                boolean_value: 0,
                integer_value: 0,
                number_value: 0,
                string_value: stringValue,
                component_count: 0,
                vector_value: WESceneVector4()
            )
            return we_scene_model_set_property_value(
                loaded.model,
                "mode",
                &badCombo,
                &error
            )
        }
        XCTAssertEqual(badComboResult, 0)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_VALUE
        )
        we_scene_runtime_error_destroy(error)
        error = nil

        let badColorResult = "not a color".withCString { stringValue in
            var badColor = WEScenePropertyValue(
                type: WE_SCENE_VALUE_STRING,
                boolean_value: 0,
                integer_value: 0,
                number_value: 0,
                string_value: stringValue,
                component_count: 0,
                vector_value: WESceneVector4()
            )
            return we_scene_model_set_property_value(
                loaded.model,
                "tint",
                &badColor,
                &error
            )
        }
        XCTAssertEqual(badColorResult, 0)
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_VALUE
        )
        we_scene_runtime_error_destroy(error)
        error = nil

        XCTAssertEqual(
            we_scene_model_set_property_value(
                loaded.model,
                "display",
                &enabled,
                &error
            ),
            0
        )
        XCTAssertEqual(
            we_scene_runtime_error_code(error),
            WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_VALUE
        )
        we_scene_runtime_error_destroy(error)
        error = nil

        // Keep the compiler honest about the indices exercised above.
        XCTAssertNotEqual(enabledIndex, modeIndex)
        XCTAssertNotEqual(amountIndex, modeIndex)
    }

    func testSyntheticNestedReferenceErrorsAreStructuredAndNoPreviewFallback() throws {
        var missingFile = validDocuments()
        var project = missingFile["project.json"] as! [String: Any]
        project.removeValue(forKey: "file")
        missingFile["project.json"] = project
        let missingFileError = try loadSyntheticFailure(missingFile)
        XCTAssertEqual(
            missingFileError.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_MISSING_FIELD
        )
        XCTAssertEqual(missingFileError.assetPath, "project.json")
        XCTAssertEqual(missingFileError.jsonPointer, "/file")
        XCTAssertEqual(missingFileError.references, ["project.json"])
        XCTAssertTrue(missingFileError.message.contains("Required field"))

        var missingShader = validDocuments()
        var material = missingShader["materials/main.json"] as! [String: Any]
        var materialPasses = material["passes"] as! [[String: Any]]
        materialPasses[0].removeValue(forKey: "shader")
        material["passes"] = materialPasses
        missingShader["materials/main.json"] = material
        let nestedError = try loadSyntheticFailure(missingShader)
        XCTAssertEqual(
            nestedError.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_MISSING_FIELD
        )
        XCTAssertEqual(nestedError.assetPath, "materials/main.json")
        XCTAssertEqual(nestedError.jsonPointer, "/passes/0/shader")
        XCTAssertTrue(
            nestedError.references.contains("project.json#/file -> scene.json")
        )
        XCTAssertTrue(
            nestedError.references.contains(
                "scene.json#/objects/0/image -> models/main.json"
            )
        )
        XCTAssertTrue(
            nestedError.references.contains(
                "models/main.json#/material -> materials/main.json"
            )
        )

        var unsupportedProject = validDocuments()
        var unsupportedProjectJSON = unsupportedProject["project.json"] as! [String: Any]
        unsupportedProjectJSON["type"] = "video"
        unsupportedProject["project.json"] = unsupportedProjectJSON
        let unsupportedProjectError = try loadSyntheticFailure(unsupportedProject)
        XCTAssertEqual(
            unsupportedProjectError.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_UNSUPPORTED_PROJECT
        )
        XCTAssertEqual(unsupportedProjectError.jsonPointer, "/type")

        var nonIntegerCombo = validDocuments()
        var comboMaterial = nonIntegerCombo["materials/main.json"] as! [String: Any]
        var comboPasses = comboMaterial["passes"] as! [[String: Any]]
        comboPasses[0]["combos"] = ["VERSION": 1.5]
        comboMaterial["passes"] = comboPasses
        nonIntegerCombo["materials/main.json"] = comboMaterial
        let comboError = try loadSyntheticFailure(nonIntegerCombo)
        XCTAssertEqual(
            comboError.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_TYPE_MISMATCH
        )
        XCTAssertEqual(comboError.jsonPointer, "/passes/0/combos/VERSION")

        var dynamicWithoutValue = validDocuments()
        var scene = dynamicWithoutValue["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[0]["visible"] = ["user": "mode"]
        scene["objects"] = objects
        dynamicWithoutValue["scene.json"] = scene
        let dynamicError = try loadSyntheticFailure(dynamicWithoutValue)
        XCTAssertEqual(
            dynamicError.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_MISSING_FIELD
        )
        XCTAssertEqual(dynamicError.jsonPointer, "/objects/0/visible/value")

        var duplicateID = validDocuments()
        var duplicateScene = duplicateID["scene.json"] as! [String: Any]
        var duplicateObjects = duplicateScene["objects"] as! [[String: Any]]
        duplicateObjects[1]["id"] = 1
        duplicateScene["objects"] = duplicateObjects
        duplicateID["scene.json"] = duplicateScene
        let duplicateError = try loadSyntheticFailure(duplicateID)
        XCTAssertEqual(
            duplicateError.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_DUPLICATE_ID
        )
        XCTAssertEqual(duplicateError.jsonPointer, "/objects/1/id")

        var negativeDependencyIndex = validDocuments()
        var dependencyScene = negativeDependencyIndex["scene.json"] as! [String: Any]
        var dependencyObjects = dependencyScene["objects"] as! [[String: Any]]
        dependencyObjects[1]["dependencies"] = [[
            "id": 1, "index": -1, "type": "collisionmodel",
        ]]
        dependencyScene["objects"] = dependencyObjects
        negativeDependencyIndex["scene.json"] = dependencyScene
        let dependencyError = try loadSyntheticFailure(negativeDependencyIndex)
        XCTAssertEqual(
            dependencyError.code,
            WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_VALUE
        )
        XCTAssertEqual(
            dependencyError.jsonPointer,
            "/objects/1/dependencies/0/index"
        )
    }
}
