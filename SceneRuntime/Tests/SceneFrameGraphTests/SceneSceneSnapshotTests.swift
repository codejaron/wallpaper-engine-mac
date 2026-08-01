import Foundation
import SceneRuntimeBridge
import XCTest

/// Exercises the production SceneGraph -> SceneScript snapshot path without
/// relying on the SceneScript test-only snapshot injection adapter.
final class SceneSceneSnapshotTests: XCTestCase {
    private struct Fixture {
        let root: URL
        let assets: URL
        let package: URL
    }

    private enum Failure: Error {
        case runtime(String)
        case model(String)
        case graph(String)
        case frameGraph(String)
        case plan(String)
        case query(String)
    }

    private func message(_ error: WESceneRuntimeErrorRef?) -> String {
        error.map { String(cString: we_scene_runtime_error_message($0)) } ?? ""
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

    private func makeFixture() throws -> Fixture {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: shaders,
            withIntermediateDirectories: true
        )

        let project: [String: Any] = [
            "file": "scene.json",
            "general": [
                "properties": [
                    "sceneBloom": [
                        "text": "Scene bloom", "type": "bool", "value": true,
                    ],
                    "sceneAmbient": [
                        "text": "Scene ambient", "type": "color",
                        "value": "0.4 0.5 0.6",
                    ],
                ],
            ],
            "title": "Scene snapshot fixture",
            "type": "scene",
            "version": 2,
        ]
        let sceneScript = """
        export function update(value) {
            const close = (a, b) => Math.abs(a - b) < 0.000001;
            const clear = thisScene.clearcolor;
            const ambient = thisScene.ambientcolor;
            const skylight = thisScene.skylightcolor;
            return thisScene.bloom && thisScene.clearenabled &&
                thisScene.bloomstrength === 7 &&
                thisScene.bloomthreshold === 11 &&
                clear instanceof Vec3 && ambient instanceof Vec3 &&
                skylight instanceof Vec3 &&
                close(clear.x, 0.1) && close(clear.y, 0.2) &&
                close(clear.z, 0.3) && close(ambient.x, 0.4) &&
                close(ambient.y, 0.5) && close(ambient.z, 0.6) &&
                close(skylight.x, ambient.x) &&
                close(skylight.y, ambient.y) &&
                close(skylight.z, ambient.z) &&
                close(thisScene.fov, 55) && close(thisScene.nearz, 0.25) &&
                close(thisScene.farz, 900) && thisScene.camerafade &&
                thisScene.camerashake && close(thisScene.camerashakespeed, 2) &&
                close(thisScene.camerashakeamplitude, 3) &&
                close(thisScene.camerashakeroughness, 4) &&
                thisScene.cameraparallax &&
                close(thisScene.cameraparallaxamount, 5) &&
                close(thisScene.cameraparallaxdelay, 6) &&
                close(thisScene.cameraparallaxmouseinfluence, 7);
        }
        """
        let scene: [String: Any] = [
            "camera": [
                "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0",
                "fov": 55, "nearz": 0.25, "farz": 900,
            ],
            "general": [
                "ambientcolor": ["user": "sceneAmbient", "value": "0 0 0"],
                "bloom": ["user": "sceneBloom", "value": false],
                "bloomstrength": 7,
                "bloomthreshold": 11,
                "camerafade": true,
                "cameraparallax": true,
                "cameraparallaxamount": 5,
                "cameraparallaxdelay": 6,
                "cameraparallaxmouseinfluence": 7,
                "camerashake": true,
                "camerashakeamplitude": 3,
                "camerashakeroughness": 4,
                "camerashakespeed": 2,
                "clearcolor": "0.1 0.2 0.3",
                "orthogonalprojection": ["height": 100, "width": 100],
                "skylightcolor": "0.8 0.9 1.0",
            ],
            "objects": [[
                "id": 1,
                "image": "models/base.json",
                "name": "Snapshot probe",
                "origin": "50 50 0",
                "size": "100 100",
                "visible": ["value": false, "script": sceneScript],
            ]],
            "version": 1,
        ]

        // A dynamic bloom value makes the model build the same Linux bloom
        // graph used by real scenes. These utility materials are sufficient
        // for frame planning; GL shader source is not needed here.
        let material: (String, [String: Any]) -> [String: Any] = { shader, constants in
            [
                "passes": [[
                    "blending": "normal", "cullmode": "nocull",
                    "depthtest": "disabled", "depthwrite": "disabled",
                    "shader": shader,
                    "constantshadervalues": constants,
                ]],
            ]
        }
        let documents: [String: Any] = [
            "materials/base.json": [
                "passes": [[
                    "blending": "translucent", "cullmode": "nocull",
                    "depthtest": "disabled", "depthwrite": "disabled",
                    "shader": "base", "textures": ["base"],
                ]],
            ],
            "materials/util/blur_h_bloom.json": material(
                "bloom-horizontal", ["bloomstrength": -1.0, "bloomthreshold": -1.0]
            ),
            "materials/util/combine.json": material(
                "bloom-combine", ["combineonly": 9.0]
            ),
            "materials/util/downsample_eighth_blur_v.json": material(
                "bloom-eighth", ["bloomstrength": -1.0, "bloomthreshold": -1.0]
            ),
            "materials/util/downsample_quarter_bloom.json": material(
                "bloom-quarter", ["bloomstrength": -1.0, "bloomthreshold": -1.0]
            ),
            "materials/util/effectpassthrough.json": material("blend-passthrough", [:]),
            "models/base.json": ["material": "materials/base.json"],
            "project.json": project,
            "scene.json": scene,
        ]
        let entries = try documents.keys.sorted().map { path in
            (
                path,
                try JSONSerialization.data(
                    withJSONObject: documents[path]!,
                    options: [.sortedKeys]
                )
            )
        }
        try makePackage(entries).write(to: package)
        return Fixture(root: root, assets: assets, package: package)
    }

    private func load(_ fixture: Fixture) throws -> (
        WESceneRuntimeRef, WESceneModelRef, WESceneGraphRef, WESceneFrameGraphRef
    ) {
        var error: WESceneRuntimeErrorRef?
        guard let runtime = fixture.assets.path.withCString({ assetsPath in
            fixture.package.path.withCString { packagePath in
                var configuration = WESceneRuntimeConfiguration(
                    assets_directory: assetsPath,
                    scene_package_path: packagePath
                )
                return we_scene_runtime_create(&configuration, &error)
            }
        }) else {
            throw Failure.runtime(message(error))
        }
        guard let model = "project.json".withCString({
            we_scene_runtime_model_create(runtime, $0, &error)
        }) else {
            we_scene_runtime_destroy(runtime)
            throw Failure.model(message(error))
        }
        guard let graph = we_scene_model_graph_create(model, &error) else {
            we_scene_model_destroy(model)
            we_scene_runtime_destroy(runtime)
            throw Failure.graph(message(error))
        }
        guard let frameGraph = we_scene_graph_frame_graph_create(graph, &error) else {
            we_scene_graph_destroy(graph)
            we_scene_model_destroy(model)
            we_scene_runtime_destroy(runtime)
            throw Failure.frameGraph(message(error))
        }
        return (runtime, model, graph, frameGraph)
    }

    private func setBoolean(
        _ model: WESceneModelRef,
        key: String,
        value: Bool
    ) throws {
        var property = WEScenePropertyValue(
            type: WE_SCENE_VALUE_BOOLEAN,
            boolean_value: value ? 1 : 0,
            integer_value: 0,
            number_value: 0,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        var error: WESceneRuntimeErrorRef?
        let result = key.withCString {
            we_scene_model_set_property_value(model, $0, &property, &error)
        }
        guard result == 1 else { throw Failure.query(message(error)) }
    }

    private func plan(
        _ frameGraph: WESceneFrameGraphRef,
        time: Double
    ) throws -> WESceneFramePlanRef {
        var inputs = WESceneFrameInputs(
            pointer_x: 0.5, pointer_y: 0.5,
            time_seconds: time, frame_time_seconds: 1.0 / 60.0
        )
        var error: WESceneRuntimeErrorRef?
        guard let result = we_scene_frame_graph_plan_create_with_inputs(
            frameGraph, &inputs, &error
        ) else {
            throw Failure.plan(message(error))
        }
        return result
    }

    private func imageInfo(
        _ plan: WESceneFramePlanRef,
        index: Int
    ) throws -> WESceneFrameImageInfo {
        var info = WESceneFrameImageInfo()
        var error: WESceneRuntimeErrorRef?
        guard we_scene_frame_plan_image_info(plan, index, &info, &error) == 1 else {
            throw Failure.query(message(error))
        }
        return info
    }

    func testProductionSceneSnapshotTracksUserBoundValues() throws {
        let fixture = try makeFixture()
        let (runtime, model, graph, frameGraph) = try load(fixture)
        defer {
            we_scene_frame_graph_destroy(frameGraph)
            we_scene_graph_destroy(graph)
            we_scene_model_destroy(model)
            we_scene_runtime_destroy(runtime)
            try? FileManager.default.removeItem(at: fixture.root)
        }

        let first = try plan(frameGraph, time: 1)
        defer { we_scene_frame_plan_destroy(first) }
        XCTAssertEqual(try imageInfo(first, index: 0).visible, 1)

        try setBoolean(model, key: "sceneBloom", value: false)
        let second = try plan(frameGraph, time: 2)
        defer { we_scene_frame_plan_destroy(second) }
        XCTAssertEqual(try imageInfo(second, index: 0).visible, 0)
    }
}
