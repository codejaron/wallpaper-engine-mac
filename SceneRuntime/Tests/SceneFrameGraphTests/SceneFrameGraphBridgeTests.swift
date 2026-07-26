import Foundation
import SceneRuntimeBridge
import XCTest

final class SceneFrameGraphBridgeTests: XCTestCase {
    private enum TestFailure: Error {
        case runtime(String)
        case model(String)
        case graph(String)
        case frameGraph(String)
        case plan(String)
        case query(String)
    }

    private struct Fixture {
        let root: URL
        let assets: URL
        let package: URL
    }

    private struct LoadedFrameGraph {
        let fixtureRoot: URL?
        let runtime: WESceneRuntimeRef
        let model: WESceneModelRef
        let graph: WESceneGraphRef
        let frameGraph: WESceneFrameGraphRef
    }

    private func string(_ pointer: UnsafePointer<CChar>?) -> String {
        pointer.map(String.init(cString:)) ?? ""
    }

    private func errorMessage(_ error: WESceneRuntimeErrorRef?) -> String {
        string(we_scene_runtime_error_message(error))
    }

    private func appendUInt32(_ value: UInt32, to data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
        data.append(UInt8(truncatingIfNeeded: value >> 16))
        data.append(UInt8(truncatingIfNeeded: value >> 24))
    }

    private func appendUInt16(_ value: UInt16, to data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
    }

    private func appendFloat32(_ value: Float, to data: inout Data) {
        appendUInt32(value.bitPattern, to: &data)
    }

    private func makePuppetMesh(version: String = "MDLV0021") -> Data {
        precondition(version == "MDLV0021" || version == "MDLV0023")
        let vertices: [(Float, Float, Float, Float, Float)] = [
            (-200, 100, 0, 0, 0),
            (200, 100, 0, 1, 0),
            (-200, -100, 0, 0, 1),
        ]
        var result = Data(version.utf8)
        result.append(0)
        appendUInt32(0, to: &result)
        appendUInt32(UInt32(vertices.count * 80), to: &result)
        for (x, y, z, u, v) in vertices {
            let start = result.count
            appendFloat32(x, to: &result)
            appendFloat32(y, to: &result)
            appendFloat32(z, to: &result)
            result.append(Data(repeating: 0, count: 60))
            appendFloat32(u, to: &result)
            appendFloat32(v, to: &result)
            precondition(result.count - start == 80)
        }
        appendUInt32(6, to: &result)
        appendUInt16(0, to: &result)
        appendUInt16(1, to: &result)
        appendUInt16(2, to: &result)
        result.append(contentsOf: Array("MDLS".utf8))
        return result
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

    private func makeFixture(
        _ documents: [String: Any],
        binaryEntries: [(String, Data)] = []
    ) throws -> Fixture {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: shaders,
            withIntermediateDirectories: true
        )
        var entries = try documents.keys.sorted().map { path in
            (
                path,
                try JSONSerialization.data(
                    withJSONObject: documents[path]!,
                    options: [.sortedKeys]
                )
            )
        }
        entries.append(contentsOf: binaryEntries)
        try makePackage(entries).write(to: package)
        return Fixture(root: root, assets: assets, package: package)
    }

    private func syntheticDocuments(
        compose: Bool = false,
        primaryTexture: Bool = true,
        firstTarget: String = "quarterA",
        firstFramebufferUVs: String? = nil,
        solidLayer: Bool = false,
        imageSize: String = "400 200"
    ) -> [String: Any] {
        let properties: [String: Any] = [
            "amount": [
                "fraction": true, "max": 1.0, "min": 0.0,
                "step": 0.05, "text": "Amount", "type": "slider",
                "value": 0.25,
            ],
        ]
        let project: [String: Any] = [
            "file": "scene.json",
            "general": ["properties": properties],
            "title": "FrameGraph fixture",
            "type": "scene",
            "version": 2,
        ]
        let image: [String: Any] = [
            "effects": [[
                "file": "effects/multi/effect.json",
                "id": 20,
                "passes": [
                    [
                        "constantshadervalues": [
                            "amount": ["user": "amount", "value": 0.25],
                        ],
                        "textures": [NSNull(), "override-mask"],
                        "usertextures": [NSNull(), "override-user"],
                    ],
                    [:],
                    [:],
                ],
                "visible": true,
            ]],
            "id": 7,
            "image": "models/main.json",
            "instance": [
                "textures": [NSNull(), "instance-mask"],
                "usertextures": [NSNull(), "instance-user"],
            ],
            "name": "Image",
            "origin": "200 100 0",
            "size": imageSize,
            "visible": true,
        ]
        let scene: [String: Any] = [
            "camera": [
                "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0",
            ],
            "general": [
                "clearcolor": "0.1 0.2 0.3 0.4",
                "orthogonalprojection": ["height": 200, "width": 400],
            ],
            "objects": [image],
            "version": 1,
        ]
        var model: [String: Any] = [
            "material": "materials/base.json",
        ]
        if solidLayer {
            model["solidlayer"] = true
        }
        let baseTextures: [Any] = primaryTexture
            ? ["base", "base-mask"]
            : [NSNull(), "base-mask"]
        let baseMaterial: [String: Any] = [
            "passes": [[
                "blending": "translucent",
                "combos": ["BASE": 2],
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": "base",
                "textures": baseTextures,
                "usertextures": [NSNull(), "base-user"],
            ]],
        ]
        var firstFramebuffer: [String: Any] = [
            "format": "rgba8888", "name": "quarterA", "scale": 4,
        ]
        if let firstFramebufferUVs {
            firstFramebuffer["uvs"] = firstFramebufferUVs
        }
        let effect: [String: Any] = [
            "fbos": [
                firstFramebuffer,
                ["format": "rgba_backbuffer", "name": "quarterB", "scale": 4],
            ],
            "passes": [
                [
                    "bind": [["index": 0, "name": "previous"]],
                    "compose": compose,
                    "material": "materials/effect-a.json",
                    "target": firstTarget,
                ],
                [
                    "bind": [["index": 0, "name": "quarterA"]],
                    "material": "materials/effect-b.json",
                    "target": "quarterB",
                ],
                [
                    "command": "copy", "source": "quarterB",
                    "target": "quarterA",
                ],
                [
                    "bind": [
                        ["index": 0, "name": "quarterA"],
                        ["index": 2, "name": "previous"],
                    ],
                    "material": "materials/effect-c.json",
                ],
            ],
            "version": 1,
        ]
        let effectA: [String: Any] = [
            "passes": [[
                "blending": "normal",
                "constantshadervalues": ["amount": 0.5, "base": 1.0],
                "shader": "effect-a",
                "textures": [NSNull(), "effect-mask"],
                "usertextures": [NSNull(), "effect-user"],
            ]],
        ]
        let simpleEffect: (String) -> [String: Any] = { shader in
            ["passes": [["shader": shader]]]
        }
        return [
            "effects/multi/effect.json": effect,
            "materials/base.json": baseMaterial,
            "materials/effect-a.json": effectA,
            "materials/effect-b.json": simpleEffect("effect-b"),
            "materials/effect-c.json": simpleEffect("effect-c"),
            "models/main.json": model,
            "project.json": project,
            "scene.json": scene,
        ]
    }

    private func linuxCompatibilityDocuments(
        bloomEnabled: Bool = false,
        dynamicBloom: Bool = false,
        colorBlendMode: Double = 0,
        dynamicColorBlend: Bool = false,
        compositeMode: Int = 2,
        compositeColor: String? = nil,
        compositeEffectVisible: Bool = true,
        dynamicCompositeColor: Bool = false,
        dynamicCompositeEffectVisible: Bool = false,
        width: Int = 16,
        height: Int = 8
    ) -> [String: Any] {
        var properties: [String: Any] = [:]
        let bloom: Any
        if dynamicBloom {
            properties["bloom_enabled"] = [
                "text": "Bloom", "type": "bool", "value": bloomEnabled,
            ]
            bloom = ["user": "bloom_enabled", "value": bloomEnabled]
        } else {
            bloom = bloomEnabled
        }
        let blendMode: Any
        if dynamicColorBlend {
            properties["blend_mode"] = [
                "max": 32.0, "min": 0.0, "step": 1.0,
                "text": "Blend mode", "type": "slider",
                "value": colorBlendMode,
            ]
            blendMode = ["user": "blend_mode", "value": colorBlendMode]
        } else {
            blendMode = colorBlendMode
        }

        var image: [String: Any] = [
            "colorBlendMode": blendMode,
            "id": 1,
            "image": "models/base.json",
            "name": "Base image",
            "origin": "\(Double(width) * 0.5) \(Double(height) * 0.5) 0",
            "size": "\(width) \(height)",
            "visible": true,
        ]
        if let compositeColor {
            let effectColor: Any
            if dynamicCompositeColor {
                properties["composite_color"] = [
                    "text": "Composite color", "type": "color",
                    "value": compositeColor,
                ]
                effectColor = [
                    "user": "composite_color", "value": compositeColor,
                ]
            } else {
                effectColor = compositeColor
            }
            let effectVisible: Any
            if dynamicCompositeEffectVisible {
                properties["composite_visible"] = [
                    "text": "Composite visible", "type": "bool",
                    "value": compositeEffectVisible,
                ]
                effectVisible = [
                    "user": "composite_visible", "value": compositeEffectVisible,
                ]
            } else {
                effectVisible = compositeEffectVisible
            }
            image["effects"] = [[
                "file": "effects/composite/effect.json",
                "id": 20,
                "passes": [[
                    "combos": ["COMPOSITE": compositeMode],
                    "constantshadervalues": [
                        "compositecolor": effectColor,
                    ],
                ]],
                "visible": effectVisible,
            ]]
        }
        let project: [String: Any] = [
            "file": "scene.json",
            "general": ["properties": properties],
            "title": "Linux compatibility fixture",
            "type": "scene",
            "version": 2,
        ]
        let scene: [String: Any] = [
            "camera": [
                "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0",
            ],
            "general": [
                "bloom": bloom,
                "bloomstrength": 1.25,
                "bloomthreshold": 0.75,
                "clearcolor": "0 0 0 0",
                "orthogonalprojection": ["height": height, "width": width],
            ],
            "objects": [image],
            "version": 1,
        ]
        let material: (String, String, [String: Any]) -> [String: Any] = {
            shader, blending, constants in
            var pass: [String: Any] = [
                "blending": blending,
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": shader,
            ]
            if !constants.isEmpty {
                pass["constantshadervalues"] = constants
            }
            return ["passes": [pass]]
        }
        return [
            "effects/composite/effect.json": [
                "passes": [["material": "materials/composite-effect.json"]],
                "version": 1,
            ],
            "materials/base.json": [
                "passes": [[
                    "blending": "translucent",
                    "cullmode": "nocull",
                    "depthtest": "disabled",
                    "depthwrite": "disabled",
                    "shader": "base",
                    "textures": ["base"],
                ]],
            ],
            "materials/composite-effect.json": material(
                "composite-effect", "normal", [:]
            ),
            "materials/effects/tint.json": material(
                "magenta-tint", "normal", [:]
            ),
            "materials/util/blur_h_bloom.json": material(
                "bloom-horizontal", "normal",
                ["bloomstrength": -1.0, "bloomthreshold": -1.0]
            ),
            "materials/util/combine.json": material(
                "bloom-combine", "normal", ["combineonly": 9.0]
            ),
            "materials/util/downsample_eighth_blur_v.json": material(
                "bloom-eighth", "normal",
                ["bloomstrength": -1.0, "bloomthreshold": -1.0]
            ),
            "materials/util/downsample_quarter_bloom.json": material(
                "bloom-quarter", "normal",
                ["bloomstrength": -1.0, "bloomthreshold": -1.0]
            ),
            "materials/util/effectpassthrough.json": material(
                "blend-passthrough", "normal", [:]
            ),
            "models/base.json": ["material": "materials/base.json"],
            "project.json": project,
            "scene.json": scene,
        ]
    }

    private func particleRenderOrderDocuments() -> [String: Any] {
        var documents = syntheticDocuments()
        var project = documents["project.json"] as! [String: Any]
        var projectGeneral = project["general"] as! [String: Any]
        var properties = projectGeneral["properties"] as! [String: Any]
        properties["particle_lifetime"] = [
            "max": 10.0, "min": 0.0, "step": 0.1,
            "text": "Particle lifetime", "type": "slider", "value": 2.0,
        ]
        projectGeneral["properties"] = properties
        project["general"] = projectGeneral
        documents["project.json"] = project
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append([
            "angles": "0 0 0.5",
            "id": 8,
            "name": "Visible particle",
            "origin": "30 40 5",
            "parallaxDepth": "0.4 -0.2",
            "particle": "particles/test.json",
            "scale": "2 3 1",
            "visible": true,
        ])
        objects.append([
            "alpha": 0.8,
            "color": "0.1 0.2 0.3 1",
            "font": "Helvetica",
            "id": 9,
            "name": "Text",
            "pointsize": 18,
            "text": "Between particle and image",
            "visible": true,
        ])
        objects.append([
            "id": 11,
            "name": "Hidden particle",
            "particle": "particles/test.json",
            "visible": false,
        ])
        objects.append([
            "id": 10,
            "image": "models/last.json",
            "name": "Last image",
            "origin": "200 100 0",
            "size": "400 200",
            "visible": true,
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene
        documents["particles/test.json"] = [
            "emitter": [
                [
                    "directions": "1 1 0",
                    "distancemax": "32 64 0",
                    "distancemin": "4 8 0",
                    "name": "sphererandom",
                    "origin": "1 2 3",
                    "rate": 12,
                    "delay": 0.2,
                    "duration": 1.0,
                    "minperiodicdelay": 0.3,
                    "maxperiodicdelay": 0.7,
                    "minperiodicduration": 0.4,
                    "maxperiodicduration": 0.9,
                    "maxtoemitperperiod": 8,
                    "sign": "1 -1 0",
                    "speedmax": 2,
                    "speedmin": 6,
                    "controlpoint": 3,
                    "flags": 6,
                ],
            ],
            "initializer": [
                [
                    "max": 4.0,
                    "min": ["user": "particle_lifetime", "value": 2.0],
                    "name": "lifetimerandom",
                ],
                ["exponent": 2.0, "max": 24.0, "min": 8.0, "name": "sizerandom"],
                ["max": "0.2 0.3 0.4", "min": "0.9 0.8 0.7", "name": "colorrandom"],
                ["max": 0.9, "min": 0.4, "name": "alpharandom"],
                ["max": "-5 -6 -7", "min": "5 6 7", "name": "velocityrandom"],
                ["max": "0 0 1", "min": "0 0 0", "name": "rotationrandom"],
                ["exponent": 1.5, "max": "0 0 -2", "min": "0 0 2", "name": "angularvelocityrandom"],
                [
                    "forward": "0 1 0", "name": "turbulentvelocityrandom",
                    "phasemax": 0.2, "phasemin": -0.1,
                    "right": "0 0 1", "scale": 2.0,
                    "speedmax": 40.0, "speedmin": 5.0,
                    "timescale": 0.5,
                ],
            ],
            "material": "materials/particle/test.json",
            "maxcount": 64,
            "starttime": 1.25,
            "flags": 4,
            "animationmode": "randomframe",
            "sequencemultiplier": 2.5,
            "controlpoint": [
                ["id": 3, "offset": "4 5 6"],
            ],
            "operator": [
                ["drag": 0.25, "gravity": "0 -9.8 0", "name": "movement"],
                ["fadeintime": 0.2, "fadeouttime": 0.8, "name": "alphafade"],
                ["drag": 0.1, "force": "1 2 3", "name": "angularmovement"],
                [
                    "frequencymax": 2.0, "frequencymin": 1.0,
                    "mask": "1 0 -1", "name": "oscillateposition",
                    "phasemax": 1.0, "phasemin": -1.0,
                    "scalemax": 4.0, "scalemin": -2.0,
                ],
                [
                    "frequencymax": 3.0, "frequencymin": 0.5,
                    "name": "oscillatealpha", "phasemax": 2.0,
                    "phasemin": -2.0, "scalemax": 1.2, "scalemin": -0.2,
                ],
                [
                    "controlpoint": 3, "name": "controlpointattract",
                    "origin": "1 2 3", "scale": 50.0, "threshold": 100.0,
                ],
            ],
            "renderer": [["name": "sprite", "orientation": "screen"]],
        ]
        documents["materials/particle/test.json"] = [
            "passes": [[
                "blending": "additive",
                "combos": ["GS_ENABLED": 0],
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": "genericparticle",
                "textures": ["particle/test"],
            ]],
        ]
        documents["models/last.json"] = [
            "material": "materials/last.json",
        ]
        documents["materials/last.json"] = [
            "passes": [["shader": "base", "textures": ["last"]]],
        ]
        return documents
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

    private func load(assets: URL, package: URL, root: URL?) throws -> LoadedFrameGraph {
        let runtime = try createRuntime(assets: assets, package: package)
        do {
            var error: WESceneRuntimeErrorRef?
            guard let model = "project.json".withCString({
                we_scene_runtime_model_create(runtime, $0, &error)
            }) else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.model(message)
            }
            do {
                guard let graph = we_scene_model_graph_create(model, &error) else {
                    let message = errorMessage(error)
                    we_scene_runtime_error_destroy(error)
                    throw TestFailure.graph(message)
                }
                do {
                    guard let frameGraph = we_scene_graph_frame_graph_create(graph, &error) else {
                        let message = errorMessage(error)
                        we_scene_runtime_error_destroy(error)
                        throw TestFailure.frameGraph(message)
                    }
                    return LoadedFrameGraph(
                        fixtureRoot: root,
                        runtime: runtime,
                        model: model,
                        graph: graph,
                        frameGraph: frameGraph
                    )
                } catch {
                    we_scene_graph_destroy(graph)
                    throw error
                }
            } catch {
                we_scene_model_destroy(model)
                throw error
            }
        } catch {
            we_scene_runtime_destroy(runtime)
            throw error
        }
    }

    private func loadSynthetic(
        compose: Bool = false,
        primaryTexture: Bool = true,
        firstTarget: String = "quarterA",
        firstFramebufferUVs: String? = nil,
        solidLayer: Bool = false,
        imageSize: String = "400 200"
    ) throws -> LoadedFrameGraph {
        let fixture = try makeFixture(syntheticDocuments(
            compose: compose,
            primaryTexture: primaryTexture,
            firstTarget: firstTarget,
            firstFramebufferUVs: firstFramebufferUVs,
            solidLayer: solidLayer,
            imageSize: imageSize
        ))
        do {
            return try load(
                assets: fixture.assets,
                package: fixture.package,
                root: fixture.root
            )
        } catch {
            try? FileManager.default.removeItem(at: fixture.root)
            throw error
        }
    }

    private func loadLinuxCompatibility(
        bloomEnabled: Bool = false,
        dynamicBloom: Bool = false,
        colorBlendMode: Double = 0,
        dynamicColorBlend: Bool = false,
        compositeMode: Int = 2,
        compositeColor: String? = nil,
        compositeEffectVisible: Bool = true,
        dynamicCompositeColor: Bool = false,
        dynamicCompositeEffectVisible: Bool = false,
        width: Int = 16,
        height: Int = 8
    ) throws -> LoadedFrameGraph {
        let fixture = try makeFixture(linuxCompatibilityDocuments(
            bloomEnabled: bloomEnabled,
            dynamicBloom: dynamicBloom,
            colorBlendMode: colorBlendMode,
            dynamicColorBlend: dynamicColorBlend,
            compositeMode: compositeMode,
            compositeColor: compositeColor,
            compositeEffectVisible: compositeEffectVisible,
            dynamicCompositeColor: dynamicCompositeColor,
            dynamicCompositeEffectVisible: dynamicCompositeEffectVisible,
            width: width,
            height: height
        ))
        do {
            return try load(
                assets: fixture.assets,
                package: fixture.package,
                root: fixture.root
            )
        } catch {
            try? FileManager.default.removeItem(at: fixture.root)
            throw error
        }
    }

    private func passthroughDocuments(
        effectVisible: Bool,
        layerVisible: Bool = true,
        includeCompositeConsumer: Bool = false
    ) -> [String: Any] {
        let project: [String: Any] = [
            "file": "scene.json",
            "general": ["properties": [String: Any]()],
            "title": "Passthrough FrameGraph fixture",
            "type": "scene",
            "version": 2,
        ]
        var objects: [[String: Any]] = [
            [
                "id": 1,
                "image": "models/background.json",
                "name": "Background",
                "origin": "8 8 0",
                "size": "16 16",
                "visible": true,
            ],
            [
                "dependencies": [1],
                "effects": [[
                    "file": "effects/passthrough/effect.json",
                    "id": 20,
                    "passes": [[:]],
                    "visible": effectVisible,
                ]],
                "id": 2,
                "image": "models/passthrough.json",
                "name": "Passthrough",
                "origin": "8 8 0",
                "size": "16 16",
                "visible": layerVisible,
            ],
        ]
        if includeCompositeConsumer {
            objects.append([
                "dependencies": [2],
                "id": 3,
                "image": "models/consumer.json",
                "name": "Composite consumer",
                "origin": "8 8 0",
                "size": "16 16",
                "visible": true,
            ])
        }
        let scene: [String: Any] = [
            "camera": [
                "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0",
            ],
            "general": [
                "clearcolor": "0 0 0 0",
                "orthogonalprojection": ["height": 16, "width": 16],
            ],
            "objects": objects,
            "version": 1,
        ]
        return [
            "effects/passthrough/effect.json": [
                "passes": [["material": "materials/effect.json"]],
                "version": 1,
            ],
            "materials/background.json": [
                "passes": [["shader": "background", "textures": ["background"]]],
            ],
            "materials/consumer.json": [
                "passes": [[
                    "shader": "consumer",
                    "textures": ["_rt_imageLayerComposite_2_a"],
                ]],
            ],
            "materials/effect.json": [
                "passes": [["shader": "effect"]],
            ],
            "materials/passthrough.json": [
                "passes": [[
                    "blending": "translucent",
                    "shader": "composelayer",
                    "textures": ["_rt_FullFrameBuffer"],
                ]],
            ],
            "models/background.json": [
                "material": "materials/background.json",
            ],
            "models/consumer.json": [
                "material": "materials/consumer.json",
            ],
            "models/passthrough.json": [
                "material": "materials/passthrough.json",
                "passthrough": true,
            ],
            "project.json": project,
            "scene.json": scene,
        ]
    }

    private func loadPassthrough(
        effectVisible: Bool,
        layerVisible: Bool = true,
        includeCompositeConsumer: Bool = false
    ) throws -> LoadedFrameGraph {
        let fixture = try makeFixture(passthroughDocuments(
            effectVisible: effectVisible,
            layerVisible: layerVisible,
            includeCompositeConsumer: includeCompositeConsumer
        ))
        do {
            return try load(
                assets: fixture.assets,
                package: fixture.package,
                root: fixture.root
            )
        } catch {
            try? FileManager.default.removeItem(at: fixture.root)
            throw error
        }
    }

    private func forwardCompositeDocuments(
        producerWritesComposite: Bool,
        producerHasPrimaryTexture: Bool = true,
        consumerReadsCompositeB: Bool = false,
        producerMaterialOutsideShaderNamespace: Bool = false,
        producerVisible: Bool = true
    ) -> [String: Any] {
        let project: [String: Any] = [
            "file": "scene.json",
            "general": ["properties": [String: Any]()],
            "title": "Forward composite fixture",
            "type": "scene",
            "version": 2,
        ]
        let scene: [String: Any] = [
            "camera": [
                "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0",
            ],
            "general": [
                "clearcolor": "0 0 0 0",
                "orthogonalprojection": ["height": 16, "width": 16],
            ],
            "objects": [
                [
                    "dependencies": [2],
                    "id": 1,
                    "image": "models/consumer.json",
                    "name": "Consumer",
                    "origin": "8 8 0",
                    "size": "16 16",
                    "visible": true,
                ],
                [
                    "id": 2,
                    "image": "models/producer.json",
                    "name": "Producer",
                    "origin": "8 8 0",
                    "size": "16 16",
                    "visible": producerVisible,
                ],
            ],
            "version": 1,
        ]
        let material: (Any, Int) -> [String: Any] = { texture, passCount in
            let passes: [[String: Any]] = (0..<passCount).map { _ in
                ["shader": "base", "textures": [texture]]
            }
            return ["passes": passes]
        }
        let producerTexture: Any = producerHasPrimaryTexture
            ? "producer" as Any
            : NSNull() as Any
        let consumerTexture = consumerReadsCompositeB
            ? "_rt_imageLayerComposite_2_b"
            : "_rt_imageLayerComposite_2_a"
        let producerMaterialPath = producerMaterialOutsideShaderNamespace
            ? "producer.json"
            : "materials/producer.json"
        return [
            "materials/consumer.json": material(consumerTexture, 1),
            producerMaterialPath: material(
                producerTexture,
                producerWritesComposite ? 2 : 1
            ),
            "models/consumer.json": ["material": "materials/consumer.json"],
            "models/producer.json": ["material": producerMaterialPath],
            "project.json": project,
            "scene.json": scene,
        ]
    }

    private func loadForwardComposite(
        producerWritesComposite: Bool,
        producerHasPrimaryTexture: Bool = true,
        consumerReadsCompositeB: Bool = false,
        producerMaterialOutsideShaderNamespace: Bool = false,
        producerVisible: Bool = true
    ) throws -> LoadedFrameGraph {
        let fixture = try makeFixture(forwardCompositeDocuments(
            producerWritesComposite: producerWritesComposite,
            producerHasPrimaryTexture: producerHasPrimaryTexture,
            consumerReadsCompositeB: consumerReadsCompositeB,
            producerMaterialOutsideShaderNamespace:
                producerMaterialOutsideShaderNamespace,
            producerVisible: producerVisible
        ))
        do {
            return try load(
                assets: fixture.assets,
                package: fixture.package,
                root: fixture.root
            )
        } catch {
            try? FileManager.default.removeItem(at: fixture.root)
            throw error
        }
    }

    private func cameraDocuments() -> [String: Any] {
        var documents = syntheticDocuments()

        var project = documents["project.json"] as! [String: Any]
        var projectGeneral = project["general"] as! [String: Any]
        var properties = projectGeneral["properties"] as! [String: Any]
        properties["camera_center"] = [
            "text": "Camera center", "type": "textinput", "value": "1 2 3",
        ]
        properties["camera_near"] = [
            "max": 10.0, "min": 0.0, "step": 0.05,
            "text": "Camera near", "type": "slider", "value": 0.25,
        ]
        properties["parallax_amount"] = [
            "max": 2.0, "min": 0.0, "step": 0.05,
            "text": "Parallax amount", "type": "slider", "value": 0.75,
        ]
        projectGeneral["properties"] = properties
        project["general"] = projectGeneral
        documents["project.json"] = project

        var scene = documents["scene.json"] as! [String: Any]
        var camera = scene["camera"] as! [String: Any]
        camera["center"] = ["user": "camera_center", "value": "1 2 3"]
        camera["nearz"] = ["user": "camera_near", "value": 0.25]
        camera["farz"] = 8000.0
        camera["fov"] = 65.0
        scene["camera"] = camera
        var general = scene["general"] as! [String: Any]
        // Linux owns these fields under `camera`; conflicting general metadata
        // must not override the camera values above.
        general["nearz"] = 9.0
        general["farz"] = 9.0
        general["fov"] = 9.0
        general["cameraparallax"] = true
        general["cameraparallaxamount"] = [
            "user": "parallax_amount", "value": 0.75,
        ]
        general["cameraparallaxdelay"] = 0.2
        general["cameraparallaxmouseinfluence"] = 0.6
        scene["general"] = general
        documents["scene.json"] = scene
        return documents
    }

    private func loadCamera() throws -> LoadedFrameGraph {
        let fixture = try makeFixture(cameraDocuments())
        do {
            return try load(
                assets: fixture.assets,
                package: fixture.package,
                root: fixture.root
            )
        } catch {
            try? FileManager.default.removeItem(at: fixture.root)
            throw error
        }
    }

    private func destroy(_ loaded: LoadedFrameGraph) {
        we_scene_frame_graph_destroy(loaded.frameGraph)
        we_scene_graph_destroy(loaded.graph)
        we_scene_model_destroy(loaded.model)
        we_scene_runtime_destroy(loaded.runtime)
        if let root = loaded.fixtureRoot {
            try? FileManager.default.removeItem(at: root)
        }
    }

    private func createPlan(_ graph: WESceneFrameGraphRef) throws -> WESceneFramePlanRef {
        var error: WESceneRuntimeErrorRef?
        guard let plan = we_scene_frame_graph_plan_create(graph, &error) else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.plan(message)
        }
        return plan
    }

    private func createPlan(
        _ graph: WESceneFrameGraphRef,
        runtime: Double,
        frameTime: Double,
        pointerX: Double = 0.25,
        pointerY: Double = 0.75
    ) throws -> WESceneFramePlanRef {
        var error: WESceneRuntimeErrorRef?
        var inputs = WESceneFrameInputs(
            pointer_x: pointerX, pointer_y: pointerY,
            time_seconds: runtime, frame_time_seconds: frameTime
        )
        guard let plan = we_scene_frame_graph_plan_create_with_inputs(
            graph, &inputs, &error
        ) else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.plan(message)
        }
        return plan
    }

    private func planInfo(_ plan: WESceneFramePlanRef) throws -> WESceneFramePlanInfo {
        var info = WESceneFramePlanInfo()
        var error: WESceneRuntimeErrorRef?
        guard we_scene_frame_plan_info(plan, &info, &error) == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.query(message)
        }
        return info
    }

    private func operations(
        _ plan: WESceneFramePlanRef
    ) throws -> [WESceneFrameOperationInfo] {
        let count = try planInfo(plan).operation_count
        return try (0..<count).map { index in
            var info = WESceneFrameOperationInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_operation_info(plan, index, &info, &error) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func framebuffers(
        _ plan: WESceneFramePlanRef
    ) throws -> [WESceneFramebufferInfo] {
        let count = try planInfo(plan).framebuffer_count
        return try (0..<count).map { index in
            var info = WESceneFramebufferInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_framebuffer_info(
                plan, index, &info, &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func images(
        _ plan: WESceneFramePlanRef
    ) throws -> [WESceneFrameImageInfo] {
        let count = try planInfo(plan).image_count
        return try (0..<count).map { index in
            var info = WESceneFrameImageInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_image_info(plan, index, &info, &error) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func texts(
        _ plan: WESceneFramePlanRef
    ) throws -> [WESceneFrameTextInfo] {
        let count = try planInfo(plan).text_count
        return try (0..<count).map { index in
            var info = WESceneFrameTextInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_text_info(plan, index, &info, &error) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func particles(
        _ plan: WESceneFramePlanRef
    ) throws -> [WESceneFrameParticleInfo] {
        let count = try planInfo(plan).particle_count
        return try (0..<count).map { index in
            var info = WESceneFrameParticleInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_particle_info(
                plan, index, &info, &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func particleControlPoints(
        _ plan: WESceneFramePlanRef,
        particleIndex: Int
    ) throws -> [WESceneFrameParticleControlPointInfo] {
        let descriptor = try particles(plan)[particleIndex]
        return try (0..<descriptor.control_point_count).map { index in
            var info = WESceneFrameParticleControlPointInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_particle_control_point_info(
                plan, particleIndex, index, &info, &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func particleEmitter(
        _ plan: WESceneFramePlanRef,
        particleIndex: Int,
        emitterIndex: Int
    ) throws -> WESceneFrameParticleEmitterInfo {
        var info = WESceneFrameParticleEmitterInfo()
        var error: WESceneRuntimeErrorRef?
        guard we_scene_frame_plan_particle_emitter_info(
            plan, particleIndex, emitterIndex, &info, &error
        ) == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.query(message)
        }
        return info
    }

    private func issues(
        _ plan: WESceneFramePlanRef
    ) throws -> [WESceneFramePlanIssueInfo] {
        let count = try planInfo(plan).issue_count
        return try (0..<count).map { index in
            var info = WESceneFramePlanIssueInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_issue_info(
                plan, index, &info, &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func scriptEvaluations(
        _ plan: WESceneFramePlanRef
    ) throws -> [WESceneScriptEvaluationInfo] {
        let count = try planInfo(plan).script_evaluation_count
        return try (0..<count).map { index in
            var info = WESceneScriptEvaluationInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_script_evaluation_info(
                plan, index, &info, &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func textures(
        _ plan: WESceneFramePlanRef,
        operation: Int
    ) throws -> [Int32: String] {
        let info = try operations(plan)[operation]
        return try Dictionary(uniqueKeysWithValues: (0..<info.texture_count).map { index in
            var binding = WESceneFrameTextureBindingInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_texture_binding_info(
                plan, operation, index, &binding, &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return (binding.slot, string(binding.resource.id))
        })
    }

    private func combos(
        _ plan: WESceneFramePlanRef,
        operation: Int
    ) throws -> [String: Int32] {
        let info = try operations(plan)[operation]
        return try Dictionary(uniqueKeysWithValues: (0..<info.combo_count).map { index in
            var combo = WESceneFrameComboInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_combo_info(
                plan, operation, index, &combo, &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return (string(combo.name), combo.value)
        })
    }

    private func constants(
        _ plan: WESceneFramePlanRef,
        operation: Int
    ) throws -> [String: WESceneFrameConstantInfo] {
        let info = try operations(plan)[operation]
        return try Dictionary(uniqueKeysWithValues: (0..<info.constant_count).map { index in
            var constant = WESceneFrameConstantInfo()
            var error: WESceneRuntimeErrorRef?
            guard we_scene_frame_plan_constant_info(
                plan, operation, index, &constant, &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return (string(constant.name), constant)
        })
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
        guard result == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.query(message)
        }
    }

    private func setString(
        _ model: WESceneModelRef,
        key: String,
        value: String
    ) throws {
        var error: WESceneRuntimeErrorRef?
        let result = value.withCString { valuePointer in
            var property = WEScenePropertyValue(
                type: WE_SCENE_VALUE_STRING,
                boolean_value: 0,
                integer_value: 0,
                number_value: 0,
                string_value: valuePointer,
                component_count: 0,
                vector_value: WESceneVector4()
            )
            return key.withCString {
                we_scene_model_set_property_value(model, $0, &property, &error)
            }
        }
        guard result == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.query(message)
        }
    }

    private func setNumber(
        _ model: WESceneModelRef,
        key: String,
        value: Double
    ) throws {
        var property = WEScenePropertyValue(
            type: WE_SCENE_VALUE_NUMBER,
            boolean_value: 0,
            integer_value: 0,
            number_value: value,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        var error: WESceneRuntimeErrorRef?
        let result = key.withCString {
            we_scene_model_set_property_value(model, $0, &property, &error)
        }
        guard result == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.query(message)
        }
    }

    func testEvaluatedPlanUsesOneFrameInputAndPropertyRevisionForScripts() throws {
        var documents = syntheticDocuments()
        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        var objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])
        var image = objects[0]
        image["origin"] = [
            "value": "200 100 0",
            "script": """
            export const scriptProperties = createScriptProperties()
                .addSlider({ name: 'amount', value: 0 }).finish();
            export function update(value) {
                return { x: engine.runtime, y: scriptProperties.amount * 100, z: 0 };
            }
            """,
            "scriptproperties": ["amount": ["user": "amount", "value": 0.25]],
        ]
        objects[0] = image
        scene["objects"] = objects
        documents["scene.json"] = scene

        var effectA = try XCTUnwrap(documents["materials/effect-a.json"] as? [String: Any])
        var passes = try XCTUnwrap(effectA["passes"] as? [[String: Any]])
        var pass = passes[0]
        var shaderValues = try XCTUnwrap(pass["constantshadervalues"] as? [String: Any])
        shaderValues["base"] = [
            "value": 0.5,
            "script": """
            export const scriptProperties = createScriptProperties()
                .addSlider({ name: 'amount', value: 0 }).finish();
            export function update(value) {
                return engine.runtime + engine.frametime + scriptProperties.amount;
            }
            """,
            "scriptproperties": ["amount": ["user": "amount", "value": 0.25]],
        ]
        pass["constantshadervalues"] = shaderValues
        passes[0] = pass
        effectA["passes"] = passes
        documents["materials/effect-a.json"] = effectA

        let fixture = try makeFixture(documents)
        let loaded = try load(assets: fixture.assets, package: fixture.package, root: fixture.root)
        defer { destroy(loaded) }

        let first = try createPlan(loaded.frameGraph, runtime: 3, frameTime: 0.5)
        defer { we_scene_frame_plan_destroy(first) }
        XCTAssertEqual(try planInfo(first).model_revision, 0)
        XCTAssertEqual(try scriptEvaluations(first).count, 2)
        XCTAssertTrue(try scriptEvaluations(first).allSatisfy { $0.execution_count == 1 })
        var imageInfo = WESceneFrameImageInfo()
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(we_scene_frame_plan_image_info(first, 0, &imageInfo, &error), 1)
        XCTAssertEqual(imageInfo.world_transform.origin.x, 3, accuracy: 0.000_001)
        XCTAssertEqual(imageInfo.world_transform.origin.y, 25, accuracy: 0.000_001)
        let firstScriptAmount = try XCTUnwrap(
            try operations(first).indices.compactMap { operation -> WESceneFrameConstantInfo? in
                let value = try constants(first, operation: operation)["base"]
                return value?.source == WE_SCENE_DYNAMIC_VALUE_SCRIPT ? value : nil
            }.first
        )
        XCTAssertEqual(firstScriptAmount.value.number_value, 3.75, accuracy: 0.000_001)

        try setNumber(loaded.model, key: "amount", value: 0.8)
        let second = try createPlan(loaded.frameGraph, runtime: 4, frameTime: 0.25)
        defer { we_scene_frame_plan_destroy(second) }
        XCTAssertEqual(try planInfo(second).model_revision, 1)
        XCTAssertEqual(we_scene_frame_plan_image_info(second, 0, &imageInfo, &error), 1)
        XCTAssertEqual(imageInfo.world_transform.origin.x, 4, accuracy: 0.000_001)
        XCTAssertEqual(imageInfo.world_transform.origin.y, 80, accuracy: 0.000_001)
        let secondScriptAmount = try XCTUnwrap(
            try operations(second).indices.compactMap { operation -> WESceneFrameConstantInfo? in
                let value = try constants(second, operation: operation)["base"]
                return value?.source == WE_SCENE_DYNAMIC_VALUE_SCRIPT ? value : nil
            }.first
        )
        XCTAssertEqual(secondScriptAmount.value.number_value, 5.05, accuracy: 0.000_001)
        XCTAssertEqual(firstScriptAmount.value.number_value, 3.75, accuracy: 0.000_001)
    }

    func testSceneLayerRegistryWritesFlowIntoSameFrameGraphSnapshot() throws {
        var documents = syntheticDocuments()
        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        let objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])
        var owner = objects[0]
        owner["effects"] = []
        owner["name"] = "Owner"
        owner["origin"] = [
            "value": "200 100 0",
            "script": """
            export function update(value) {
                const target = thisScene.getLayer("Target");
                if (target !== thisScene.enumerateLayers()[1] ||
                    thisScene.getLayer(7) !== thisLayer) {
                    throw new Error('layer registry view mismatch');
                }
                thisLayer.origin = new Vec3(value.x + 10, value.y, value.z);
                target.visible = false;
                return undefined;
            }
            """,
        ]
        var target = owner
        target["id"] = 8
        target["name"] = "Target"
        target["origin"] = "300 100 0"
        target["visible"] = true
        scene["objects"] = [owner, target]
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }
        let descriptors = try images(plan)
        XCTAssertEqual(descriptors.count, 2)
        XCTAssertEqual(
            descriptors[0].world_transform.origin.x,
            210,
            accuracy: 0.000_001
        )
        XCTAssertEqual(descriptors[0].world_transform.origin.y, 100, accuracy: 0.000_001)
        XCTAssertEqual(descriptors[1].visible, 0)
    }

    func testTemplateLayerCloneAndTimelineAnimationFlowIntoFrameGraph() throws {
        var documents = syntheticDocuments()
        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        let objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])

        var controller = objects[0]
        controller["effects"] = []
        controller["name"] = "Controller"
        controller["origin"] = [
            "value": "200 100 0",
            "script": """
            let clone;

            export function init(value) {
                const template = thisScene.getLayer('Template');
                const config = thisScene.getInitialLayerConfig(template);
                config.origin.value = new Vec3(40, 50, 0);
                clone = thisScene.createLayer(config);
                const animation = clone.getAnimation('origin');
                if (animation.fps !== 10 || animation.frameCount !== 10 ||
                    animation.duration !== 1 || animation.rate !== 1 ||
                    animation.isPlaying()) {
                    throw new Error('timeline metadata or initial state mismatch');
                }
                animation.rate = 2;
                animation.setFrame(2);
                animation.pause();
                if (animation.rate !== 2 || animation.getFrame() !== 2 ||
                    animation.isPlaying()) {
                    throw new Error('timeline pause or positioning mismatch');
                }
                animation.stop();
                if (animation.getFrame() !== 0 || animation.isPlaying()) {
                    throw new Error('timeline stop mismatch');
                }
                animation.rate = 1;
                animation.play();
                return value;
            }

            export function update(value) {
                if (thisScene.getLayerCount() !== 3 ||
                    thisScene.getLayerIndex(clone) !== 2) {
                    throw new Error('dynamic layer topology is stale');
                }
                const animation = clone.getAnimation('origin');
                thisLayer.scale = new Vec3(
                    animation.getFrame(),
                    clone.alpha,
                    1
                );
                return value;
            }
            """,
        ]

        var template = controller
        template["id"] = 8
        template["name"] = "Template"
        template["origin"] = [
            "animation": [
                "c0": [
                    ["frame": 0, "value": 0],
                    ["frame": 10, "value": 100],
                ],
                "c1": [
                    ["frame": 0, "value": 0],
                    ["frame": 10, "value": 0],
                ],
                "c2": [
                    ["frame": 0, "value": 0],
                    ["frame": 10, "value": 0],
                ],
                "options": [
                    "children": [["key": "alpha"]],
                    "fps": 10,
                    "length": 10,
                    "mode": "single",
                    "startpaused": true,
                ],
                "relative": true,
            ],
            "value": "10 20 0",
        ]
        template["alpha"] = [
            "animation": [
                "c0": [
                    ["frame": 0, "value": 0],
                    ["frame": 10, "value": 1],
                ],
                "options": [
                    "fps": 10,
                    "length": 10,
                    "mode": "single",
                    "parent": ["key": "origin"],
                ],
            ],
            "value": 1.0,
        ]
        template["visible"] = false
        scene["objects"] = [controller, template]
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let initial = try createPlan(
            loaded.frameGraph,
            runtime: 0,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(initial) }
        let initialImages = try images(initial)
        XCTAssertEqual(initialImages.count, 3)
        let initialClone = try XCTUnwrap(initialImages.first { $0.object_id < 0 })
        XCTAssertEqual(initialClone.visible, 1)
        XCTAssertEqual(initialClone.world_transform.origin.x, 40, accuracy: 0.000_001)
        XCTAssertEqual(initialClone.world_transform.origin.y, 50, accuracy: 0.000_001)

        let animated = try createPlan(
            loaded.frameGraph,
            runtime: 0.5,
            frameTime: 0.5
        )
        defer { we_scene_frame_plan_destroy(animated) }
        let animatedImages = try images(animated)
        let animatedClone = try XCTUnwrap(
            animatedImages.first { $0.object_id == initialClone.object_id }
        )
        XCTAssertEqual(animatedClone.visible, 1)
        XCTAssertEqual(animatedClone.world_transform.origin.x, 90, accuracy: 0.000_001)
        XCTAssertEqual(animatedClone.world_transform.origin.y, 50, accuracy: 0.000_001)

        let animatedController = try XCTUnwrap(
            animatedImages.first { $0.object_id == 7 }
        )
        XCTAssertEqual(
            animatedController.world_transform.scale.x,
            5,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            animatedController.world_transform.scale.y,
            0.5,
            accuracy: 0.000_001
        )
    }

    func testDynamicLayerSortAndDeferredDestroyUpdateTheNextFrameTopology() throws {
        var documents = syntheticDocuments()
        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        let objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])

        var controller = objects[0]
        controller["effects"] = []
        controller["name"] = "Controller"
        controller["origin"] = [
            "value": "200 100 0",
            "script": """
            let clone;
            let firstUpdate = true;

            export function init(value) {
                const config = thisScene.getInitialLayerConfig('Template');
                clone = thisScene.createLayer(config);
                if (!thisScene.sortLayer(clone, 0) ||
                    thisScene.getLayerIndex(clone) !== 0 ||
                    !thisScene.destroyLayer(clone) ||
                    thisScene.destroyLayer(clone)) {
                    throw new Error('dynamic layer lifecycle mutation failed');
                }
                return value;
            }

            export function update(value) {
                const expectedCount = firstUpdate ? 3 : 2;
                if (thisScene.getLayerCount() !== expectedCount) {
                    throw new Error('deferred dynamic layer topology mismatch');
                }
                firstUpdate = false;
                return value;
            }
            """,
        ]

        var template = controller
        template["id"] = 8
        template["name"] = "Template"
        template["origin"] = "40 50 0"
        scene["objects"] = [controller, template]
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let first = try createPlan(
            loaded.frameGraph,
            runtime: 0,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(first) }
        let firstImages = try images(first)
        XCTAssertEqual(firstImages.count, 3)
        XCTAssertLessThan(firstImages[0].object_id, 0)

        let second = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1
        )
        defer { we_scene_frame_plan_destroy(second) }
        XCTAssertEqual(try images(second).count, 2)
    }

    func testDynamicLayerPlanningFailureDoesNotSkipSiblingInstancesOfTemplate() throws {
        var documents = syntheticDocuments()
        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        let objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])

        var controller = objects[0]
        controller["effects"] = []
        controller["origin"] = [
            "value": "200 100 0",
            "script": """
            export function init(value) {
                const config = thisScene.getInitialLayerConfig('Text template');
                config.pointsize = 0;
                config.text = 'invalid clone';
                thisScene.createLayer(config);
                config.pointsize = 20;
                config.text = 'valid clone';
                thisScene.createLayer(config);
                return value;
            }
            """,
        ]

        let template: [String: Any] = [
            "id": 8,
            "name": "Text template",
            "origin": "100 50 0",
            "pointsize": 20,
            "size": "200 40",
            "text": "template",
            "visible": true,
        ]
        scene["objects"] = [controller, template]
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 0,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }

        let descriptors = try texts(plan)
        XCTAssertEqual(descriptors.map { string($0.text) }, ["template", "valid clone"])
        XCTAssertEqual(descriptors.map(\.object_id), [8, -2])
        XCTAssertFalse(try operations(plan).contains { $0.object_id == -1 })
        let issue = try XCTUnwrap(try issues(plan).first {
            $0.code == WE_SCENE_FRAME_ISSUE_OBJECT_PLANNING_FAILED
        })
        XCTAssertEqual(issue.object_id, -1)
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_SKIP_OBJECT)
    }

    func testUnsupportedObjectDiagnosticsUseEveryRuntimeLayerIdentity() throws {
        var documents = syntheticDocuments()
        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        let objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])

        var controller = objects[0]
        controller["effects"] = []
        controller["origin"] = [
            "value": "200 100 0",
            "script": """
            export function init(value) {
                const config = thisScene.getInitialLayerConfig('Perspective template');
                thisScene.createLayer(config);
                return value;
            }
            """,
        ]

        var template = controller
        template["id"] = 8
        template["name"] = "Perspective template"
        template["origin"] = "100 50 0"
        template["perspective"] = true
        scene["objects"] = [controller, template]
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 0,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }

        let perspectiveIssues = try issues(plan).filter {
            $0.code == WE_SCENE_FRAME_ISSUE_PERSPECTIVE_PROJECTION_UNAVAILABLE
        }
        XCTAssertEqual(Set(perspectiveIssues.map(\.object_id)), Set([8, -1]))
    }

    func testArbitraryDynamicLayerConfigurationFailsExplicitly() throws {
        var documents = syntheticDocuments()
        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        var objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])
        objects[0]["origin"] = [
            "value": "200 100 0",
            "script": """
            export function init(value) {
                thisScene.createLayer({
                    origin: new Vec3(10, 20, 0),
                    visible: true
                });
                return value;
            }
            """,
        ]
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        XCTAssertThrowsError(
            try createPlan(
                loaded.frameGraph,
                runtime: 0,
                frameTime: 1.0 / 60.0
            )
        ) { error in
            guard case let TestFailure.plan(message) = error else {
                return XCTFail("unexpected error: \(error)")
            }
            XCTAssertTrue(message.contains("arbitrary layer construction is unsupported"))
        }
    }

    func testEffectVisibilityScriptReceivesTypedThisObjectAndOwningLayer() throws {
        var documents = syntheticDocuments()
        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        var objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])
        var image = objects[0]
        var effects = try XCTUnwrap(image["effects"] as? [[String: Any]])
        var effect = effects[0]
        effect["name"] = "Typed effect"
        effect["visible"] = [
            "value": true,
            "script": """
            export function init(value) {
                if (thisLayer.name !== 'Image') {
                    throw new Error('effect script lost its owning layer');
                }
                if (thisObject === thisLayer) {
                    throw new Error('effect owner was aliased to the layer');
                }
                if (thisObject.name !== 'Typed effect' || thisObject.visible !== true) {
                    throw new Error('effect owner properties are unavailable');
                }
                thisObject.visible = false;
            }
            """,
        ]
        effects[0] = effect
        image["effects"] = effects
        objects[0] = image
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }

        XCTAssertEqual(try operations(plan).map { string($0.shader) }, ["base"])
        XCTAssertEqual(try planInfo(plan).issue_count, 0)
    }

    func testMaterialThisObjectWritesSiblingShaderPropertiesInTheSameFrame() throws {
        var documents = syntheticDocuments()
        var material = try XCTUnwrap(
            documents["materials/effect-a.json"] as? [String: Any]
        )
        var passes = try XCTUnwrap(material["passes"] as? [[String: Any]])
        var pass = passes[0]
        var values = try XCTUnwrap(
            pass["constantshadervalues"] as? [String: Any]
        )
        values["alpha"] = [
            "value": 0.1,
            "script": """
            export function applyUserProperties() {
                if (thisLayer.name !== 'Image') {
                    throw new Error('material script lost its owning layer');
                }
                if (thisObject === thisLayer) {
                    throw new Error('material owner was aliased to the layer');
                }
                if (thisObject.color.x !== 0 || thisObject.color.y !== 0 ||
                    thisObject.color.z !== 0) {
                    throw new Error('material sibling property was not readable');
                }
                thisObject.alpha = 0.75;
                thisObject.color = new Vec3(0.2, 0.4, 0.6);
            }
            """,
        ]
        values["color"] = "0 0 0"
        pass["constantshadervalues"] = values
        passes[0] = pass
        material["passes"] = passes
        documents["materials/effect-a.json"] = material

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }
        let effectOperation = try XCTUnwrap(
            try operations(plan).firstIndex { string($0.shader) == "effect-a" }
        )
        let shaderValues = try constants(plan, operation: effectOperation)
        XCTAssertEqual(
            try XCTUnwrap(shaderValues["alpha"]).value.number_value,
            0.75,
            accuracy: 0.000_001
        )
        let color = try XCTUnwrap(shaderValues["color"]).value
        XCTAssertEqual(color.vector_value.x, 0.2, accuracy: 0.000_001)
        XCTAssertEqual(color.vector_value.y, 0.4, accuracy: 0.000_001)
        XCTAssertEqual(color.vector_value.z, 0.6, accuracy: 0.000_001)
        XCTAssertEqual(try planInfo(plan).issue_count, 0)
    }

    func testMaterialScriptInstancesDoNotCollideAcrossRenderPasses() throws {
        var documents = syntheticDocuments()
        for (path, expected) in [
            ("materials/effect-a.json", 11.0),
            ("materials/effect-b.json", 22.0),
        ] {
            var material = try XCTUnwrap(documents[path] as? [String: Any])
            var passes = try XCTUnwrap(material["passes"] as? [[String: Any]])
            var pass = passes[0]
            var values = (pass["constantshadervalues"] as? [String: Any]) ?? [:]
            values["isolated"] = [
                "value": 0,
                "script": "export function update() { return \(expected); }",
            ]
            pass["constantshadervalues"] = values
            passes[0] = pass
            material["passes"] = passes
            documents[path] = material
        }

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }
        let scheduled = try operations(plan)
        for (shader, expected) in [("effect-a", 11.0), ("effect-b", 22.0)] {
            let operation = try XCTUnwrap(
                scheduled.firstIndex { string($0.shader) == shader }
            )
            XCTAssertEqual(
                try XCTUnwrap(try constants(plan, operation: operation)["isolated"])
                    .value.number_value,
                expected,
                accuracy: 0.000_001
            )
        }
        XCTAssertEqual(try scriptEvaluations(plan).count, 2)
    }

    func testRuntimeValueTagsAndProjectionsFlowThroughUserScriptAndFramePlan() throws {
        var documents = syntheticDocuments()

        var project = try XCTUnwrap(documents["project.json"] as? [String: Any])
        var projectGeneral = try XCTUnwrap(project["general"] as? [String: Any])
        var properties = try XCTUnwrap(projectGeneral["properties"] as? [String: Any])
        properties["tint"] = [
            "text": "Tint", "type": "color", "value": "#000000",
        ]
        properties["cssTint"] = [
            "text": "CSS tint", "type": "color", "value": "#123456",
        ]
        properties["mode"] = [
            "options": [
                ["label": "Enabled", "value": "enabled"],
                ["label": "Disabled", "value": "disabled"],
            ],
            "text": "Mode", "type": "combo", "value": "enabled",
        ]
        projectGeneral["properties"] = properties
        project["general"] = projectGeneral
        documents["project.json"] = project

        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        var objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])
        var image = objects[0]
        image["origin"] = [
            "value": "1 2",
            "script": """
            export function update(value) {
                if (typeof value === 'object') {
                    if (value.x !== 1 || value.y !== 2 ||
                        value.z !== undefined) {
                        throw new Error('initial Vec2 was not passed as a vector object');
                    }
                } else if (value !== 2) {
                    throw new Error('subsequent frames did not retain the scalar result');
                }
                return 2;
            }
            """,
        ]
        objects[0] = image
        scene["objects"] = objects
        documents["scene.json"] = scene

        var material = try XCTUnwrap(
            documents["materials/effect-a.json"] as? [String: Any]
        )
        var passes = try XCTUnwrap(material["passes"] as? [[String: Any]])
        var pass = passes[0]
        var values = try XCTUnwrap(
            pass["constantshadervalues"] as? [String: Any]
        )
        values["base"] = [
            "value": 0,
            "user": "amount",
            "script": """
            export function update(value) {
                if (typeof value !== 'number') {
                    throw new Error('changed user value did not replace script state');
                }
                return { x: value + 0.25, y: 2, z: 3 };
            }
            """,
        ]
        values["half"] = 0.5
        values["plainVec4"] = "0 0 0 1"
        values["colorProjection"] = [
            "value": "255 255 255 255", "user": "tint",
        ]
        values["cssColorProjection"] = [
            "value": "255 255 255 255", "user": "cssTint",
        ]
        values["scriptText"] = [
            "value": "seed",
            "script": #"export function update(value) { return "1 2"; }"#,
        ]
        values["conditionedScript"] = [
            "value": false,
            "user": ["condition": "enabled", "name": "mode"],
            "script": #"export function update(value) { return "disabled"; }"#,
        ]
        pass["constantshadervalues"] = values
        passes[0] = pass
        material["passes"] = passes
        documents["materials/effect-a.json"] = material

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph, runtime: 1, frameTime: 1.0 / 60.0)
        defer { we_scene_frame_plan_destroy(plan) }

        let imageInfo = try XCTUnwrap(try images(plan).first)
        XCTAssertEqual(imageInfo.world_transform.origin.x, 2, accuracy: 0.000_001)
        XCTAssertEqual(imageInfo.world_transform.origin.y, 2, accuracy: 0.000_001)
        XCTAssertEqual(imageInfo.world_transform.origin.z, 2, accuracy: 0.000_001)

        let initialOperation = try XCTUnwrap(
            try operations(plan).firstIndex { string($0.shader) == "effect-a" }
        )
        let initialConstants = try constants(plan, operation: initialOperation)
        let scriptedConstant = try XCTUnwrap(initialConstants["base"])
        XCTAssertEqual(scriptedConstant.source, WE_SCENE_DYNAMIC_VALUE_SCRIPT)
        let scriptedVector = scriptedConstant.value
        XCTAssertEqual(scriptedVector.type, WE_SCENE_VALUE_OBJECT)
        XCTAssertEqual(scriptedVector.component_count, 3)
        XCTAssertEqual(scriptedVector.vector_value.x, 0.5, accuracy: 0.000_001)
        XCTAssertEqual(scriptedVector.vector_value.y, 2, accuracy: 0.000_001)
        XCTAssertEqual(scriptedVector.vector_value.z, 3, accuracy: 0.000_001)
        XCTAssertEqual(scriptedVector.vector_value.w, 0, accuracy: 0.000_001)
        XCTAssertEqual(scriptedVector.number_value, 0.5, accuracy: 0.000_001)
        XCTAssertEqual(scriptedVector.integer_value, 0)
        XCTAssertEqual(scriptedVector.boolean_value, 1)

        let half = try XCTUnwrap(initialConstants["half"]).value
        XCTAssertEqual(half.type, WE_SCENE_VALUE_NUMBER)
        XCTAssertEqual(half.boolean_value, 0)
        XCTAssertEqual(half.integer_value, 0)

        let plainVector = try XCTUnwrap(initialConstants["plainVec4"]).value
        XCTAssertEqual(plainVector.component_count, 4)
        XCTAssertEqual(plainVector.boolean_value, 0)

        let color = try XCTUnwrap(initialConstants["colorProjection"]).value
        XCTAssertEqual(color.component_count, 4)
        XCTAssertEqual(color.vector_value.x, 0, accuracy: 0.000_001)
        XCTAssertEqual(color.vector_value.w, 1, accuracy: 0.000_001)
        XCTAssertEqual(color.integer_value, 0)
        XCTAssertEqual(color.boolean_value, 1)

        let cssColor = try XCTUnwrap(initialConstants["cssColorProjection"]).value
        XCTAssertEqual(cssColor.component_count, 4)
        XCTAssertEqual(cssColor.vector_value.x, 0x12 / 255.0, accuracy: 0.000_001)
        XCTAssertEqual(cssColor.vector_value.y, 0x34 / 255.0, accuracy: 0.000_001)
        XCTAssertEqual(cssColor.vector_value.z, 0x56 / 255.0, accuracy: 0.000_001)
        XCTAssertEqual(cssColor.vector_value.w, 1, accuracy: 0.000_001)
        XCTAssertEqual(cssColor.integer_value, 18)

        let text = try XCTUnwrap(initialConstants["scriptText"]).value
        XCTAssertEqual(text.type, WE_SCENE_VALUE_STRING)
        XCTAssertEqual(string(text.string_value), "1 2")
        XCTAssertEqual(text.component_count, 0)

        let conditioned = try XCTUnwrap(initialConstants["conditionedScript"]).value
        XCTAssertEqual(conditioned.type, WE_SCENE_VALUE_BOOLEAN)
        XCTAssertEqual(conditioned.boolean_value, 0)
        XCTAssertEqual(conditioned.number_value, 0, accuracy: 0.000_001)

        try setNumber(loaded.model, key: "amount", value: 0.5)
        let updatedPlan = try createPlan(
            loaded.frameGraph,
            runtime: 2,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(updatedPlan) }
        let updatedOperations = try operations(updatedPlan)
        let updatedIssues = try issues(updatedPlan).map {
            "\(string($0.json_pointer)): \(string($0.message))"
        }
        let updatedOperation = try XCTUnwrap(
            updatedOperations.firstIndex { string($0.shader) == "effect-a" },
            "operations=\(updatedOperations.map { string($0.shader) }), issues=\(updatedIssues)"
        )
        let updatedConstants = try constants(
            updatedPlan,
            operation: updatedOperation
        )
        let updatedConstant = try XCTUnwrap(updatedConstants["base"])
        XCTAssertEqual(updatedConstant.source, WE_SCENE_DYNAMIC_VALUE_SCRIPT)
        let updatedVector = updatedConstant.value
        XCTAssertEqual(updatedVector.type, WE_SCENE_VALUE_OBJECT)
        XCTAssertEqual(updatedVector.vector_value.x, 0.75, accuracy: 0.000_001)
    }

    func testUserBoundScriptRemainsUnavailableUntilQuickJSRuns() throws {
        var documents = syntheticDocuments()
        var material = try XCTUnwrap(
            documents["materials/effect-a.json"] as? [String: Any]
        )
        var passes = try XCTUnwrap(material["passes"] as? [[String: Any]])
        var pass = passes[0]
        var values = try XCTUnwrap(
            pass["constantshadervalues"] as? [String: Any]
        )
        values["base"] = [
            "value": 0,
            "user": "amount",
            "script": "export function update(value) { return value + 1; }",
        ]
        pass["constantshadervalues"] = values
        passes[0] = pass
        material["passes"] = passes
        documents["materials/effect-a.json"] = material

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let snapshot = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(snapshot) }
        let snapshotValue = try XCTUnwrap(
            try constants(snapshot, operation: 1)["base"]
        )
        XCTAssertEqual(snapshotValue.source, WE_SCENE_DYNAMIC_VALUE_SCRIPT_INITIAL)
        XCTAssertEqual(snapshotValue.value.number_value, 0.25, accuracy: 0.000_001)
        XCTAssertEqual(try planInfo(snapshot).is_executable, 1)
        let snapshotIssues = try issues(snapshot)
        XCTAssertEqual(snapshotIssues.count, 1)
        let snapshotIssue = try XCTUnwrap(snapshotIssues.first)
        XCTAssertEqual(
            snapshotIssue.code,
            WE_SCENE_FRAME_ISSUE_SCRIPT_RUNTIME_UNAVAILABLE
        )
        XCTAssertEqual(snapshotIssue.severity, WE_SCENE_FRAME_ISSUE_WARNING)

        let evaluated = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(evaluated) }
        let evaluatedValue = try XCTUnwrap(
            try constants(evaluated, operation: 1)["base"]
        )
        XCTAssertEqual(evaluatedValue.source, WE_SCENE_DYNAMIC_VALUE_SCRIPT)
        XCTAssertEqual(evaluatedValue.value.number_value, 1.25, accuracy: 0.000_001)
        let evaluatedInfo = try planInfo(evaluated)
        XCTAssertEqual(evaluatedInfo.is_executable, 1)
        XCTAssertEqual(evaluatedInfo.issue_count, 0)
    }

    func testAudioScriptSkipsOnlyItsObjectAndKeepsThePlanExecutable() throws {
        var documents = syntheticDocuments()
        var material = try XCTUnwrap(documents["materials/effect-a.json"] as? [String: Any])
        var passes = try XCTUnwrap(material["passes"] as? [[String: Any]])
        var pass = passes[0]
        var shaderValues = try XCTUnwrap(pass["constantshadervalues"] as? [String: Any])
        shaderValues["base"] = [
            "value": 0.5,
            "script": """
            const audio = engine.registerAudioBuffers(16);
            export function update(value) { return value; }
            """,
        ]
        pass["constantshadervalues"] = shaderValues
        passes[0] = pass
        material["passes"] = passes
        documents["materials/effect-a.json"] = material
        let fixture = try makeFixture(documents)
        let loaded = try load(assets: fixture.assets, package: fixture.package, root: fixture.root)
        defer { destroy(loaded) }

        let plan = try createPlan(loaded.frameGraph, runtime: 1, frameTime: 1.0 / 60.0)
        defer { we_scene_frame_plan_destroy(plan) }
        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.operation_count, 0)
        let audioIssues = try issues(plan).filter {
            $0.code == WE_SCENE_FRAME_ISSUE_AUDIO_INPUT_UNAVAILABLE
        }
        XCTAssertEqual(audioIssues.count, 1)
        let audioIssue = try XCTUnwrap(audioIssues.first)
        XCTAssertEqual(audioIssue.severity, WE_SCENE_FRAME_ISSUE_SKIP_OBJECT)
        XCTAssertTrue(string(audioIssue.message).contains("audio"))
        let evaluation = try XCTUnwrap(try scriptEvaluations(plan).first)
        XCTAssertEqual(evaluation.status, WE_SCENE_SCRIPT_EVALUATION_UNAVAILABLE)
        XCTAssertEqual(evaluation.execution_count, 1)
    }

    func testGlobalAudioDependentCameraIssueIsFrameFatal() throws {
        var documents = syntheticDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var camera = scene["camera"] as! [String: Any]
        camera["center"] = [
            "value": "0 0 -1",
            "script": """
            const audio = engine.registerAudioBuffers(16);
            export function update(value) { return value; }
            """,
        ]
        scene["camera"] = camera
        documents["scene.json"] = scene
        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }
        XCTAssertEqual(try planInfo(plan).is_executable, 0)
        let issue = try XCTUnwrap(try issues(plan).first {
            $0.code == WE_SCENE_FRAME_ISSUE_AUDIO_INPUT_UNAVAILABLE &&
                $0.has_object == 0
        })
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_FRAME_FATAL)
    }

    func testAudioUnavailableSkipsObjectAfterPreviousSuccessfulEvaluation() throws {
        var documents = syntheticDocuments()
        var material = try XCTUnwrap(
            documents["materials/effect-a.json"] as? [String: Any]
        )
        var passes = try XCTUnwrap(material["passes"] as? [[String: Any]])
        var pass = passes[0]
        var shaderValues = try XCTUnwrap(
            pass["constantshadervalues"] as? [String: Any]
        )
        shaderValues["base"] = [
            "value": 0.5,
            "script": """
            export function update(value) {
                if (engine.runtime >= 2) {
                    engine.registerAudioBuffers(16);
                }
                return value + 0.25;
            }
            """,
        ]
        pass["constantshadervalues"] = shaderValues
        passes[0] = pass
        material["passes"] = passes
        documents["materials/effect-a.json"] = material

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let successful = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(successful) }
        let successfulValue = try XCTUnwrap(
            try constants(successful, operation: 1)["base"]
        )
        XCTAssertEqual(successfulValue.source, WE_SCENE_DYNAMIC_VALUE_SCRIPT)
        XCTAssertEqual(successfulValue.value.number_value, 0.75, accuracy: 0.000_001)

        let unavailable = try createPlan(
            loaded.frameGraph,
            runtime: 2,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(unavailable) }
        let unavailableInfo = try planInfo(unavailable)
        XCTAssertEqual(unavailableInfo.is_executable, 1)
        XCTAssertEqual(unavailableInfo.operation_count, 0)
        let issue = try XCTUnwrap(try issues(unavailable).first {
            $0.code == WE_SCENE_FRAME_ISSUE_AUDIO_INPUT_UNAVAILABLE
        })
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_SKIP_OBJECT)
    }

    func testDeterministicMultiPassTargetsOverridesAndPreviousBinding() throws {
        let loaded = try loadSynthetic()
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.model_revision, 0)
        XCTAssertEqual(info.width, 400)
        XCTAssertEqual(info.height, 200)
        XCTAssertEqual(info.clear_red, 0.1, accuracy: 1e-6)
        XCTAssertEqual(info.clear_alpha, 1, accuracy: 1e-6)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.framebuffer_count, 9)
        XCTAssertEqual(info.operation_count, 5)

        let operations = try operations(plan)
        XCTAssertEqual(operations.map(\.kind), [
            WE_SCENE_FRAME_OPERATION_RENDER,
            WE_SCENE_FRAME_OPERATION_RENDER,
            WE_SCENE_FRAME_OPERATION_RENDER,
            WE_SCENE_FRAME_OPERATION_COPY,
            WE_SCENE_FRAME_OPERATION_RENDER,
        ])
        XCTAssertEqual(string(operations[0].shader), "base")
        XCTAssertEqual(string(operations[0].vertex_shader_path), "shaders/base.vert")
        XCTAssertEqual(string(operations[0].fragment_shader_path), "shaders/base.frag")
        XCTAssertEqual(operations[0].blending, WE_SCENE_FRAME_BLENDING_NORMAL)
        XCTAssertEqual(string(operations[0].input.id), "materials/base.tex")
        XCTAssertTrue(string(operations[0].destination.id).hasSuffix("_a"))
        XCTAssertEqual(try textures(plan, operation: 0)[1], "materials/base-user.tex")
        XCTAssertEqual(try combos(plan, operation: 0), ["BASE": 2])

        XCTAssertEqual(string(operations[1].shader), "effect-a")
        XCTAssertEqual(
            string(operations[1].vertex_shader_path),
            "shaders/effect-a.vert"
        )
        XCTAssertEqual(
            string(operations[1].fragment_shader_path),
            "shaders/effect-a.frag"
        )
        XCTAssertTrue(string(operations[1].destination.id).hasSuffix(":quarterA"))
        XCTAssertEqual(try textures(plan, operation: 1)[0], string(operations[0].destination.id))
        XCTAssertEqual(try textures(plan, operation: 1)[1], "materials/override-user.tex")
        let firstConstants = try constants(plan, operation: 1)
        XCTAssertEqual(firstConstants["amount"]?.source, WE_SCENE_DYNAMIC_VALUE_USER)
        XCTAssertEqual(
            try XCTUnwrap(firstConstants["amount"]).value.number_value,
            0.25,
            accuracy: 1e-6
        )
        XCTAssertEqual(firstConstants["base"]?.value.type, WE_SCENE_VALUE_INTEGER)
        XCTAssertEqual(firstConstants["base"]?.value.integer_value, 1)

        XCTAssertTrue(string(operations[2].destination.id).hasSuffix(":quarterB"))
        XCTAssertTrue(string(operations[3].source.id).hasSuffix(":quarterB"))
        XCTAssertTrue(string(operations[3].destination.id).hasSuffix(":quarterA"))
        XCTAssertEqual(string(operations[4].destination.logical_name), "_rt_FullFrameBuffer")
        XCTAssertEqual(operations[4].geometry, WE_SCENE_FRAME_GEOMETRY_IMAGE_SCENE)
        XCTAssertEqual(operations[4].texture_coordinates, WE_SCENE_FRAME_TEXCOORD_FULL)
        XCTAssertEqual(operations[4].blending, WE_SCENE_FRAME_BLENDING_TRANSLUCENT)
        XCTAssertEqual(operations[4].write_alpha, 0)
        XCTAssertTrue((try textures(plan, operation: 4)[0] ?? "").hasSuffix(":quarterA"))
        XCTAssertEqual(try textures(plan, operation: 4)[2], string(operations[0].destination.id))
    }

    func testMissingOrNullClearColorUsesTheUpstreamOpaqueWhiteDefault() throws {
        for authoredNull in [false, true] {
            var documents = syntheticDocuments()
            var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
            var general = try XCTUnwrap(scene["general"] as? [String: Any])
            if authoredNull {
                general["clearcolor"] = NSNull()
            } else {
                general.removeValue(forKey: "clearcolor")
            }
            scene["general"] = general
            documents["scene.json"] = scene

            let fixture = try makeFixture(documents)
            let loaded = try load(
                assets: fixture.assets,
                package: fixture.package,
                root: fixture.root
            )
            defer { destroy(loaded) }
            let plan = try createPlan(loaded.frameGraph)
            defer { we_scene_frame_plan_destroy(plan) }
            let info = try planInfo(plan)

            XCTAssertEqual(info.clear_red, 1, accuracy: 0.000_001)
            XCTAssertEqual(info.clear_green, 1, accuracy: 0.000_001)
            XCTAssertEqual(info.clear_blue, 1, accuracy: 0.000_001)
            XCTAssertEqual(info.clear_alpha, 1, accuracy: 0.000_001)
        }
    }

    func testUnsupportedComposeWarnsAndExecutesTheAuthoredPassLikeLinux() throws {
        let loaded = try loadSynthetic(compose: true)
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.issue_count, 1)
        XCTAssertEqual(info.operation_count, 5)
        let issue = try XCTUnwrap(try issues(plan).first)
        XCTAssertEqual(issue.code, WE_SCENE_FRAME_ISSUE_COMPOSE_UNAVAILABLE)
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_WARNING)
        XCTAssertEqual(issue.object_id, 7)
        XCTAssertTrue(string(issue.message).contains("compose"))
        let shaders = try operations(plan).map { string($0.shader) }
        XCTAssertTrue(shaders.contains("effect-a"))
        XCTAssertTrue(shaders.contains("effect-b"))
        XCTAssertTrue(shaders.contains("effect-c"))
    }

    func testInvalidEffectTargetSkipsOnlyThatPass() throws {
        let loaded = try loadSynthetic(firstTarget: "base")
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.image_count, 1)
        XCTAssertEqual(info.operation_count, 4)
        let issue = try XCTUnwrap(try issues(plan).first {
            $0.code == WE_SCENE_FRAME_ISSUE_EFFECT_PASS_PLANNING_FAILED
        })
        XCTAssertEqual(issue.object_id, 7)
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_SKIP_PASS)
        let shaders = try operations(plan).map { string($0.shader) }
        XCTAssertFalse(shaders.contains("effect-a"))
        XCTAssertTrue(shaders.contains("effect-b"))
        XCTAssertTrue(shaders.contains("effect-c"))
    }

    func testEffectMaterialTakesPrecedenceOverStaleCommandLikeLinux() throws {
        var documents = syntheticDocuments()
        var effect = try XCTUnwrap(
            documents["effects/multi/effect.json"] as? [String: Any]
        )
        var passes = try XCTUnwrap(effect["passes"] as? [[String: Any]])
        passes[0]["command"] = "copy"
        passes[0]["source"] = "previous"
        effect["passes"] = passes
        documents["effects/multi/effect.json"] = effect

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let scheduled = try operations(plan)
        XCTAssertEqual(
            scheduled.filter {
                $0.kind == WE_SCENE_FRAME_OPERATION_RENDER &&
                    string($0.shader) == "effect-a"
            }.count,
            1
        )
        XCTAssertEqual(
            scheduled.filter {
                $0.kind == WE_SCENE_FRAME_OPERATION_COPY
            }.count,
            1
        )
    }

    func testDuplicateEffectFramebufferUsesTheLastDefinitionLikeLinux() throws {
        var documents = syntheticDocuments()
        var effect = try XCTUnwrap(
            documents["effects/multi/effect.json"] as? [String: Any]
        )
        var framebufferDefinitions = try XCTUnwrap(
            effect["fbos"] as? [[String: Any]]
        )
        framebufferDefinitions.append([
            "format": "rgba8888",
            "name": "quarterA",
            "scale": 2,
        ])
        effect["fbos"] = framebufferDefinitions
        documents["effects/multi/effect.json"] = effect

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let quarterA = try framebuffers(plan).filter {
            string($0.resource.logical_name) == "quarterA"
        }
        XCTAssertEqual(quarterA.count, 1)
        XCTAssertEqual(quarterA.first?.width, 200)
        XCTAssertEqual(quarterA.first?.height, 100)
    }

    func testEffectBindsOutsideLinuxTextureSlotsAreIgnoredWithIssues() throws {
        var documents = syntheticDocuments()
        var effect = try XCTUnwrap(
            documents["effects/multi/effect.json"] as? [String: Any]
        )
        var passes = try XCTUnwrap(effect["passes"] as? [[String: Any]])
        passes[0]["bind"] = [
            ["index": -1, "name": "previous"],
            ["index": 0, "name": "previous"],
            ["index": 7, "name": "previous"],
            ["index": 7, "name": "quarterB"],
            ["index": 8, "name": "previous"],
        ]
        effect["passes"] = passes
        documents["effects/multi/effect.json"] = effect

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let scheduled = try operations(plan)
        let effectAIndex = try XCTUnwrap(scheduled.firstIndex {
            $0.kind == WE_SCENE_FRAME_OPERATION_RENDER &&
                string($0.shader) == "effect-a"
        })
        let bindings = try textures(plan, operation: effectAIndex)
        XCTAssertTrue(bindings.keys.allSatisfy { (0...7).contains($0) })
        XCTAssertEqual(bindings[7], bindings[0])

        let bindIssues = try issues(plan).filter {
            $0.code == WE_SCENE_FRAME_ISSUE_EFFECT_PASS_PLANNING_FAILED &&
                string($0.json_pointer).contains("/bind/")
        }
        XCTAssertEqual(bindIssues.count, 2)
        XCTAssertTrue(bindIssues.allSatisfy {
            $0.severity == WE_SCENE_FRAME_ISSUE_WARNING
        })
    }

    func testPerspectiveImageWarnsAndUsesTheLinuxOrthographicFallback() throws {
        var documents = syntheticDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var camera = scene["camera"] as! [String: Any]
        camera["fov"] = 65.0
        scene["camera"] = camera
        var general = scene["general"] as! [String: Any]
        general["perspectiveoverridefov"] = 95.0
        scene["general"] = general
        var objects = scene["objects"] as! [[String: Any]]
        objects[0]["perspective"] = true
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.camera_field_of_view, 65, accuracy: 1e-6)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.image_count, 1)
        XCTAssertEqual(info.operation_count, 5)
        XCTAssertEqual(info.issue_count, 1)
        let issue = try XCTUnwrap(try issues(plan).first)
        XCTAssertEqual(
            issue.code,
            WE_SCENE_FRAME_ISSUE_PERSPECTIVE_PROJECTION_UNAVAILABLE
        )
        XCTAssertEqual(issue.object_id, 7)
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_WARNING)
        XCTAssertEqual(string(issue.json_pointer), "/objects/0/perspective")
        XCTAssertTrue(string(issue.message).contains("Perspective"))
    }

    func testPerspectiveTextWarnsAndUsesTheLinuxOrthographicFallback() throws {
        var documents = syntheticDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append([
            "id": 8,
            "name": "Perspective text",
            "perspective": true,
            "pointsize": 20,
            "size": "100 40",
            "text": "depth",
            "visible": true,
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.text_count, 1)
        XCTAssertTrue(try operations(plan).contains { $0.object_id == 8 })
        let issue = try XCTUnwrap(try issues(plan).first {
            $0.object_id == 8 &&
                string($0.json_pointer) == "/objects/1/perspective"
        })
        XCTAssertEqual(
            issue.code,
            WE_SCENE_FRAME_ISSUE_PERSPECTIVE_PROJECTION_UNAVAILABLE
        )
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_WARNING)
        XCTAssertTrue(string(issue.message).contains("Perspective"))
    }

    func testPerspectiveWarningDoesNotBlockRenderableSibling() throws {
        var documents = syntheticDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append([
            "id": 8,
            "image": "models/main.json",
            "name": "Unsupported perspective sibling",
            "origin": "200 100 0",
            "perspective": true,
            "size": "400 200",
            "visible": true,
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.image_count, 2)
        let scheduledObjectIds = Set(try operations(plan).map(\.object_id))
        XCTAssertTrue(scheduledObjectIds.contains(7))
        XCTAssertTrue(scheduledObjectIds.contains(8))
        let issue = try XCTUnwrap(try issues(plan).first {
            $0.object_id == 8
        })
        XCTAssertEqual(
            issue.code,
            WE_SCENE_FRAME_ISSUE_PERSPECTIVE_PROJECTION_UNAVAILABLE
        )
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_WARNING)
    }

    func testAuthoredFOVMetadataDoesNotEnablePerspectiveProjection() throws {
        var documents = syntheticDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var camera = scene["camera"] as! [String: Any]
        camera["fov"] = 65.0
        scene["camera"] = camera
        var general = scene["general"] as! [String: Any]
        general["perspectiveoverridefov"] = 95.0
        scene["general"] = general
        var objects = scene["objects"] as! [[String: Any]]
        objects[0]["perspective"] = false
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.camera_field_of_view, 65, accuracy: 1e-6)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.issue_count, 0)
    }

    func testImageWithoutPrimaryTextureUsesTheLinuxTransparentSource() throws {
        let loaded = try loadSynthetic(primaryTexture: false)
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.framebuffer_count, 10)
        XCTAssertEqual(info.image_count, 1)
        XCTAssertEqual(info.operation_count, 6)
        XCTAssertEqual(info.issue_count, 1)

        let issue = try XCTUnwrap(try issues(plan).first)
        XCTAssertEqual(
            issue.code,
            WE_SCENE_FRAME_ISSUE_IMAGE_MATERIAL_UNAVAILABLE
        )
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_WARNING)
        XCTAssertEqual(issue.object_id, 7)
        XCTAssertEqual(string(issue.json_pointer), "/objects/0/image")
        XCTAssertTrue(string(issue.message).contains("primary texture"))
        let image = try XCTUnwrap(try images(plan).first)
        XCTAssertEqual(image.source.kind, WE_SCENE_FRAME_RESOURCE_FRAMEBUFFER)
        XCTAssertTrue(
            string(image.source.logical_name).hasPrefix("_rt_missingTextureSource_")
        )
        XCTAssertEqual(try operations(plan).first?.kind, WE_SCENE_FRAME_OPERATION_CLEAR)
    }

    func testUserTextureDoesNotReplaceTheRenderablePrimarySource() throws {
        var documents = syntheticDocuments()
        var project = documents["project.json"] as! [String: Any]
        var general = project["general"] as! [String: Any]
        var properties = general["properties"] as! [String: Any]
        properties["selected_texture"] = [
            "text": "Texture",
            "type": "scenetexture",
            "value": "alternate",
        ]
        general["properties"] = properties
        project["general"] = general
        documents["project.json"] = project

        var material = documents["materials/base.json"] as! [String: Any]
        var passes = material["passes"] as! [[String: Any]]
        passes[0]["usertextures"] = [[
            "name": "selected_texture",
            "type": "scenetexture",
        ]]
        material["passes"] = passes
        documents["materials/base.json"] = material

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let image = try XCTUnwrap(try images(plan).first)
        XCTAssertEqual(string(image.source.id), "materials/base.tex")
        XCTAssertEqual(
            image.source.kind,
            WE_SCENE_FRAME_RESOURCE_ASSET_TEXTURE
        )

        let binding = try XCTUnwrap(try textures(plan, operation: 0)[0])
        XCTAssertEqual(binding, "user-property:selected_texture")
    }

    func testPassthroughCapturesSceneBeforeApplyingVisibleEffect() throws {
        let loaded = try loadPassthrough(effectVisible: true)
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.issue_count, 0)

        let scheduled = try operations(plan)
        XCTAssertEqual(scheduled.map(\.object_id), [1, 2, 2])
        XCTAssertEqual(scheduled.map(\.kind), [
            WE_SCENE_FRAME_OPERATION_RENDER,
            WE_SCENE_FRAME_OPERATION_RENDER,
            WE_SCENE_FRAME_OPERATION_RENDER,
        ])
        guard scheduled.count == 3 else {
            return XCTFail(
                "Passthrough must schedule background, capture, and effect render operations"
            )
        }

        let outputID = string(scheduled[0].destination.id)
        XCTAssertEqual(string(scheduled[1].input.id), outputID)
        XCTAssertEqual(try textures(plan, operation: 1)[0], outputID)
        XCTAssertNotEqual(string(scheduled[1].destination.id), outputID)
        XCTAssertTrue(string(scheduled[1].destination.id).hasSuffix("_a"))
        XCTAssertEqual(
            scheduled[1].geometry,
            WE_SCENE_FRAME_GEOMETRY_PASSTHROUGH_CAPTURE
        )

        XCTAssertEqual(
            string(scheduled[2].input.id),
            string(scheduled[1].destination.id)
        )
        XCTAssertEqual(
            try textures(plan, operation: 2)[0],
            string(scheduled[1].destination.id)
        )
        XCTAssertEqual(string(scheduled[2].destination.id), outputID)
        XCTAssertEqual(
            scheduled[2].geometry,
            WE_SCENE_FRAME_GEOMETRY_IMAGE_SCENE
        )
    }

    func testPassthroughWithNoVisibleEffectIsAnExecutableNoOp() throws {
        let loaded = try loadPassthrough(effectVisible: false)
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.issue_count, 0)
        let scheduled = try operations(plan)
        XCTAssertEqual(scheduled.map(\.object_id), [1])
        XCTAssertEqual(
            string(scheduled[0].destination.logical_name),
            "_rt_FullFrameBuffer"
        )
    }

    func testPassthroughRenderOperationsNeverSampleTheirDestination() throws {
        let loaded = try loadPassthrough(effectVisible: true)
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        let scheduled = try operations(plan)
        guard scheduled.count == 3 else {
            return XCTFail(
                "Passthrough must schedule background, capture, and effect render operations"
            )
        }
        for (index, operation) in scheduled.enumerated() {
            let destination = string(operation.destination.id)
            XCTAssertNotEqual(
                string(operation.input.id), destination,
                "Render operation \(index) must not sample its destination"
            )
            if operation.has_previous_input == 1 {
                XCTAssertNotEqual(
                    string(operation.previous_input.id), destination,
                    "Render operation \(index) must not sample its destination as previous input"
                )
            }
            XCTAssertFalse(
                try textures(plan, operation: index).values.contains(destination),
                "Render operation \(index) must not bind its destination as a texture"
            )
        }
    }

    func testHiddenPassthroughDependencyCapturesSceneIntoComposite() throws {
        let loaded = try loadPassthrough(
            effectVisible: false,
            layerVisible: false,
            includeCompositeConsumer: true
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        let scheduled = try operations(plan)
        XCTAssertEqual(scheduled.map(\.object_id), [1, 2, 3])
        guard scheduled.count == 3 else { return }
        XCTAssertEqual(
            scheduled[1].geometry,
            WE_SCENE_FRAME_GEOMETRY_PASSTHROUGH_CAPTURE
        )
        XCTAssertEqual(
            string(scheduled[1].input.logical_name),
            "_rt_FullFrameBuffer"
        )
        XCTAssertEqual(
            string(scheduled[1].destination.logical_name),
            "_rt_imageLayerComposite_2_a"
        )
        XCTAssertEqual(
            string(scheduled[2].input.id),
            string(scheduled[1].destination.id)
        )
    }

    func testForwardCompositeDependencySchedulesProducerWriteBeforeConsumer() throws {
        let loaded = try loadForwardComposite(producerWritesComposite: true)
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.image_count, 2)
        XCTAssertEqual(info.operation_count, 3)

        let consumer = try XCTUnwrap(
            try images(plan).first { $0.object_id == 1 }
        )
        XCTAssertEqual(consumer.source.kind, WE_SCENE_FRAME_RESOURCE_FRAMEBUFFER)
        XCTAssertEqual(
            string(consumer.source.id),
            "object:2:_rt_imageLayerComposite_2_a"
        )

        let scheduled = try operations(plan)
        XCTAssertEqual(scheduled.map(\.object_id), [2, 2, 1])
        guard let firstProducer = scheduled.first else {
            return XCTFail("Expected producer operations before the consumer")
        }
        XCTAssertEqual(
            string(firstProducer.destination.id),
            string(consumer.source.id),
            "The producer must define its composite before the consumer reads it"
        )
    }

    func testEvaluatedHiddenDependencyRendersOffscreenBeforeConsumer() throws {
        let loaded = try loadForwardComposite(
            producerWritesComposite: false,
            producerVisible: false
        )
        defer { destroy(loaded) }
        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }

        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        let producer = try XCTUnwrap(try images(plan).first {
            $0.object_id == 2
        })
        XCTAssertEqual(producer.visible, 0)

        let scheduled = try operations(plan)
        XCTAssertEqual(scheduled.map(\.object_id), [2, 1])
        guard scheduled.count == 2 else { return }
        XCTAssertEqual(
            string(scheduled[0].destination.id),
            "object:2:_rt_imageLayerComposite_2_a"
        )
        XCTAssertEqual(
            string(scheduled[1].input.id),
            string(scheduled[0].destination.id)
        )
        XCTAssertFalse(try issues(plan).contains {
            $0.code == WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_READ_BEFORE_WRITE
        })
    }

    func testSinglePassForwardCompositeProducerIsNotRedrawnForConsumer() throws {
        let loaded = try loadForwardComposite(producerWritesComposite: false)
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.operation_count, 2)

        let scheduled = try operations(plan)
        let producerOperations = scheduled.filter {
            $0.object_id == 2
        }
        XCTAssertEqual(producerOperations.count, 1)
        let producer = try XCTUnwrap(producerOperations.first)
        XCTAssertEqual(
            string(producer.destination.id),
            "scene:_rt_FullFrameBuffer"
        )
        XCTAssertFalse(scheduled.contains {
            $0.object_id == 2 &&
                string($0.destination.id).contains("_rt_imageLayerComposite_2_")
        })
        let consumer = try XCTUnwrap(scheduled.first { $0.object_id == 1 })
        XCTAssertEqual(
            string(consumer.input.id),
            "object:2:_rt_imageLayerComposite_2_a"
        )
        XCTAssertFalse(try issues(plan).contains {
            $0.code == WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_READ_BEFORE_WRITE
        })
    }

    func testMissingProducerTextureUsesTransparentSourceAndKeepsConsumer() throws {
        let loaded = try loadForwardComposite(
            producerWritesComposite: false,
            producerHasPrimaryTexture: false
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.image_count, 2)
        XCTAssertEqual(info.operation_count, 3)
        let allIssues = try issues(plan)
        let producerIssue = try XCTUnwrap(allIssues.first {
            $0.code == WE_SCENE_FRAME_ISSUE_IMAGE_MATERIAL_UNAVAILABLE &&
                $0.object_id == 2
        })
        XCTAssertEqual(producerIssue.severity, WE_SCENE_FRAME_ISSUE_WARNING)
        XCTAssertFalse(allIssues.contains {
            $0.code == WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_DESCRIPTOR_MISSING
        })
        let scheduled = try operations(plan)
        XCTAssertEqual(scheduled.map(\.object_id), [2, 2, 1])
        guard scheduled.count == 3 else {
            return XCTFail("Expected producer clear/render followed by consumer")
        }
        XCTAssertEqual(scheduled[0].kind, WE_SCENE_FRAME_OPERATION_CLEAR)
        let consumer = scheduled[2]
        XCTAssertEqual(consumer.object_id, 1)
        XCTAssertEqual(
            string(consumer.input.id),
            "object:2:_rt_imageLayerComposite_2_a"
        )
        let producer = try XCTUnwrap(try images(plan).first {
            $0.object_id == 2
        })
        XCTAssertTrue(
            string(producer.source.logical_name).hasPrefix(
                "_rt_missingTextureSource_"
            )
        )
    }

    func testProducerSchedulingFailureKeepsBothCompositesForConsumers() throws {
        let loaded = try loadForwardComposite(
            producerWritesComposite: false,
            consumerReadsCompositeB: true,
            producerMaterialOutsideShaderNamespace: true
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.image_count, 1)
        XCTAssertEqual(info.operation_count, 1)
        let allIssues = try issues(plan)
        let producerIssue = try XCTUnwrap(allIssues.first {
            $0.code == WE_SCENE_FRAME_ISSUE_OBJECT_PLANNING_FAILED &&
                $0.object_id == 2
        })
        XCTAssertEqual(producerIssue.severity, WE_SCENE_FRAME_ISSUE_SKIP_OBJECT)
        XCTAssertFalse(allIssues.contains {
            $0.code == WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_DESCRIPTOR_MISSING
        })
        let consumer = try XCTUnwrap(try operations(plan).first)
        XCTAssertEqual(consumer.object_id, 1)
        XCTAssertEqual(
            string(consumer.input.id),
            "object:2:_rt_imageLayerComposite_2_b"
        )
    }

    func testRuntimeResourceNameIsNotRewrittenAsAnAssetTexture() throws {
        let loaded = try loadSynthetic(firstTarget: "previous")
        defer { destroy(loaded) }

        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }
        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        XCTAssertEqual(try planInfo(plan).image_count, 0)
        let issue = try XCTUnwrap(try issues(plan).first {
            $0.code == WE_SCENE_FRAME_ISSUE_OBJECT_PLANNING_FAILED
        })
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_SKIP_OBJECT)
        XCTAssertTrue(string(issue.message).contains("runtime resource 'previous'"))
        XCTAssertFalse(string(issue.message).contains("materials/previous.tex"))
    }

    func testLinuxBloomTogglePlansTheFourPassPostProcessWithExactBindings() throws {
        let bloomObjectId = Int32.min
        let loaded = try loadLinuxCompatibility(
            bloomEnabled: false,
            dynamicBloom: true
        )
        defer { destroy(loaded) }

        let disabled = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(disabled) }
        let disabledInfo = try planInfo(disabled)
        XCTAssertEqual(disabledInfo.is_executable, 1)
        XCTAssertEqual(disabledInfo.image_count, 1)
        XCTAssertFalse(try operations(disabled).contains {
            $0.object_id == bloomObjectId
        })

        let disabledFramebuffers = Dictionary(
            uniqueKeysWithValues: try framebuffers(disabled).map {
                (string($0.resource.logical_name), $0)
            }
        )
        XCTAssertEqual(disabledFramebuffers["_rt_4FrameBuffer"]?.width, 4)
        XCTAssertEqual(disabledFramebuffers["_rt_4FrameBuffer"]?.height, 2)
        XCTAssertEqual(disabledFramebuffers["_rt_8FrameBuffer"]?.width, 2)
        XCTAssertEqual(disabledFramebuffers["_rt_8FrameBuffer"]?.height, 1)
        XCTAssertEqual(disabledFramebuffers["_rt_Bloom"]?.width, 2)
        XCTAssertEqual(disabledFramebuffers["_rt_Bloom"]?.height, 1)
        XCTAssertEqual(disabledFramebuffers["_rt_shadowAtlas"]?.width, 16)
        XCTAssertEqual(disabledFramebuffers["_rt_shadowAtlas"]?.height, 8)

        try setBoolean(loaded.model, key: "bloom_enabled", value: true)
        let enabled = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(enabled) }
        let enabledInfo = try planInfo(enabled)
        let enabledIssueDescriptions = try issues(enabled).map {
            "\($0.code.rawValue): \(string($0.message))"
        }
        let enabledDiagnostics = enabledIssueDescriptions.joined(separator: " | ")
        XCTAssertEqual(enabledInfo.is_executable, 1, enabledDiagnostics)
        XCTAssertEqual(enabledInfo.image_count, 2, enabledDiagnostics)
        XCTAssertEqual(enabledInfo.issue_count, 0, enabledDiagnostics)

        let allOperations = try operations(enabled)
        let bloomOperationIndices = allOperations.indices.filter {
            allOperations[$0].object_id == bloomObjectId
        }
        XCTAssertEqual(bloomOperationIndices.count, 5, enabledDiagnostics)
        guard bloomOperationIndices.count == 5 else { return }
        let bloomOperations = bloomOperationIndices.map { allOperations[$0] }
        XCTAssertTrue(bloomOperations.allSatisfy {
            $0.kind == WE_SCENE_FRAME_OPERATION_RENDER
        })
        XCTAssertEqual(
            bloomOperations.map { string($0.destination.logical_name) },
            [
                "_rt_imageLayerComposite_-2147483648_a",
                "_rt_4FrameBuffer",
                "_rt_8FrameBuffer",
                "_rt_Bloom",
                "_rt_FullFrameBuffer",
            ]
        )
        XCTAssertEqual(
            bloomOperations.map { string($0.fragment_shader_path) },
            [
                "shaders/genericimage2.frag",
                "shaders/bloom-quarter.frag",
                "shaders/bloom-eighth.frag",
                "shaders/bloom-horizontal.frag",
                "shaders/bloom-combine.frag",
            ]
        )

        let quarterTextures = try textures(
            enabled, operation: bloomOperationIndices[1]
        )
        let eighthTextures = try textures(
            enabled, operation: bloomOperationIndices[2]
        )
        let horizontalTextures = try textures(
            enabled, operation: bloomOperationIndices[3]
        )
        let combineTextures = try textures(
            enabled, operation: bloomOperationIndices[4]
        )
        XCTAssertEqual(quarterTextures[0], "scene:_rt_FullFrameBuffer")
        XCTAssertEqual(eighthTextures[0], "scene:_rt_4FrameBuffer")
        XCTAssertEqual(horizontalTextures[0], "scene:_rt_8FrameBuffer")
        XCTAssertEqual(
            combineTextures[0],
            "object:-2147483648:_rt_imageLayerComposite_-2147483648_a"
        )
        XCTAssertEqual(combineTextures[1], "scene:_rt_Bloom")

        for operationIndex in bloomOperationIndices[1...3] {
            let values = try constants(enabled, operation: operationIndex)
            XCTAssertEqual(
                try XCTUnwrap(values["bloomstrength"]).value.number_value,
                1.25,
                accuracy: 0.000_001
            )
            XCTAssertEqual(
                try XCTUnwrap(values["bloomthreshold"]).value.number_value,
                0.75,
                accuracy: 0.000_001
            )
        }
        let combineConstants = try constants(
            enabled, operation: bloomOperationIndices[4]
        )
        XCTAssertNil(combineConstants["bloomstrength"])
        XCTAssertNil(combineConstants["bloomthreshold"])
        XCTAssertEqual(
            try XCTUnwrap(combineConstants["combineonly"]).value.number_value,
            9.0,
            accuracy: 0.000_001
        )

        try setBoolean(loaded.model, key: "bloom_enabled", value: false)
        let disabledAgain = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(disabledAgain) }
        XCTAssertEqual(try planInfo(disabledAgain).image_count, 1)
        XCTAssertFalse(try operations(disabledAgain).contains {
            $0.object_id == bloomObjectId
        })
    }

    func testDynamicLayerAndBloomUseDisjointRuntimeIdentities() throws {
        var documents = linuxCompatibilityDocuments(bloomEnabled: true)
        var scene = try XCTUnwrap(documents["scene.json"] as? [String: Any])
        var objects = try XCTUnwrap(scene["objects"] as? [[String: Any]])
        objects[0]["origin"] = [
            "value": "8 4 0",
            "script": """
            export function init(value) {
                const config = thisScene.getInitialLayerConfig(thisLayer);
                thisScene.createLayer(config);
                return value;
            }
            """,
        ]
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 0,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        let diagnostics = try issues(plan).map {
            "\($0.code.rawValue): \(string($0.message))"
        }.joined(separator: " | ")
        XCTAssertEqual(info.is_executable, 1, diagnostics)
        XCTAssertTrue(try images(plan).contains { $0.object_id == -1 })
        let allOperations = try operations(plan)
        let bloomOperationIndices = allOperations.indices.filter {
            allOperations[$0].object_id == Int32.min
        }
        XCTAssertEqual(bloomOperationIndices.count, 5, diagnostics)
        guard bloomOperationIndices.count == 5 else { return }
        let combineTextures = try textures(
            plan, operation: bloomOperationIndices[4]
        )
        XCTAssertEqual(
            combineTextures[0],
            "object:-2147483648:_rt_imageLayerComposite_-2147483648_a"
        )
    }

    func testLinuxColorBlendModeAddsAndRemovesTheFinalCompatibilityPass() throws {
        let loaded = try loadLinuxCompatibility(
            colorBlendMode: 0,
            dynamicColorBlend: true
        )
        defer { destroy(loaded) }

        let initial = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(initial) }
        let initialOperations = try operations(initial).filter {
            $0.object_id == 1
        }
        XCTAssertEqual(initialOperations.count, 1)
        XCTAssertEqual(
            initialOperations[0].blending,
            WE_SCENE_FRAME_BLENDING_TRANSLUCENT
        )
        XCTAssertTrue(try combos(initial, operation: 0)["BLENDMODE"] == nil)

        try setNumber(loaded.model, key: "blend_mode", value: 7)
        let blended = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(blended) }
        let allOperations = try operations(blended)
        let blendOperationIndices = allOperations.indices.filter {
            allOperations[$0].object_id == 1
        }
        XCTAssertEqual(blendOperationIndices.count, 2)
        let blendOperations = blendOperationIndices.map { allOperations[$0] }
        XCTAssertEqual(blendOperations[0].blending, WE_SCENE_FRAME_BLENDING_NORMAL)
        XCTAssertEqual(
            blendOperations[1].blending,
            WE_SCENE_FRAME_BLENDING_TRANSLUCENT
        )
        XCTAssertEqual(
            string(blendOperations[1].fragment_shader_path),
            "shaders/blend-passthrough.frag"
        )
        XCTAssertEqual(
            try combos(blended, operation: blendOperationIndices[1])["BLENDMODE"],
            7
        )
        XCTAssertEqual(
            string(blendOperations[0].destination.logical_name),
            "_rt_imageLayerComposite_1_a"
        )
        XCTAssertEqual(
            string(blendOperations[1].destination.logical_name),
            "_rt_FullFrameBuffer"
        )

        try setNumber(loaded.model, key: "blend_mode", value: 0)
        let restored = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(restored) }
        XCTAssertEqual(
            try operations(restored).filter { $0.object_id == 1 }.count,
            1
        )
    }

    func testLinuxMagentaCompositeAddsTintBeforeColorBlendCompatibilityPass() throws {
        let loaded = try loadLinuxCompatibility(
            colorBlendMode: 7,
            compositeColor: "0.8 0.1 0.7"
        )
        defer { destroy(loaded) }

        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }
        let allOperations = try operations(plan)
        let operationIndices = allOperations.indices.filter {
            allOperations[$0].object_id == 1
        }
        XCTAssertEqual(operationIndices.count, 4)
        XCTAssertEqual(
            operationIndices.map { string(allOperations[$0].fragment_shader_path) },
            [
                "shaders/base.frag",
                "shaders/composite-effect.frag",
                "shaders/magenta-tint.frag",
                "shaders/blend-passthrough.frag",
            ]
        )

        let tintIndex = operationIndices[2]
        XCTAssertEqual(try combos(plan, operation: tintIndex)["BLENDMODE"], 30)
        let tintConstants = try constants(plan, operation: tintIndex)
        let color = try XCTUnwrap(tintConstants["color"])
        XCTAssertEqual(color.value.type, WE_SCENE_VALUE_OBJECT)
        XCTAssertEqual(color.value.component_count, 3)
        XCTAssertEqual(color.value.vector_value.x, 0.8, accuracy: 0.000_001)
        XCTAssertEqual(color.value.vector_value.y, 0.1, accuracy: 0.000_001)
        XCTAssertEqual(color.value.vector_value.z, 0.7, accuracy: 0.000_001)
        XCTAssertEqual(
            try XCTUnwrap(tintConstants["alpha"]).value.number_value,
            1,
            accuracy: 0.000_001
        )
    }

    func testLinuxMagentaCompositeTintTracksDynamicColorAndVisibility() throws {
        let loaded = try loadLinuxCompatibility(
            compositeColor: "0.2 0.1 0.2",
            compositeEffectVisible: false,
            dynamicCompositeColor: true,
            dynamicCompositeEffectVisible: true
        )
        defer { destroy(loaded) }

        let hasTint = { () throws -> Bool in
            let plan = try self.createPlan(loaded.frameGraph)
            defer { we_scene_frame_plan_destroy(plan) }
            return try self.operations(plan).contains {
                self.string($0.fragment_shader_path) ==
                    "shaders/magenta-tint.frag"
            }
        }

        XCTAssertFalse(try hasTint())
        try setBoolean(loaded.model, key: "composite_visible", value: true)
        XCTAssertFalse(try hasTint())
        try setString(
            loaded.model,
            key: "composite_color",
            value: "0.8 0.1 0.7"
        )
        XCTAssertTrue(try hasTint())
        try setBoolean(loaded.model, key: "composite_visible", value: false)
        XCTAssertFalse(try hasTint())
    }

    func testLinuxCompositeTintRequiresVisibleCompositeTwoAndStrictMagentaThresholds() throws {
        let cases: [(mode: Int, color: String, visible: Bool)] = [
            (1, "0.8 0.1 0.7", true),
            (2, "0.55 0.1 0.7", true),
            (2, "0.8 0.25 0.7", true),
            (2, "0.8 0.1 0.45", true),
            (2, "0.8 0.1 0.7", false),
        ]

        for candidate in cases {
            let loaded = try loadLinuxCompatibility(
                compositeMode: candidate.mode,
                compositeColor: candidate.color,
                compositeEffectVisible: candidate.visible
            )
            defer { destroy(loaded) }
            let plan = try createPlan(loaded.frameGraph)
            defer { we_scene_frame_plan_destroy(plan) }

            XCTAssertFalse(try operations(plan).contains {
                string($0.fragment_shader_path) == "shaders/magenta-tint.frag"
            }, "Unexpected tint for \(candidate)")
        }
    }

    func testScaledEffectFramebufferDimensionsClampToOnePixel() throws {
        let loaded = try loadSynthetic(imageSize: "1 1")
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        XCTAssertFalse(try issues(plan).contains {
            $0.code == WE_SCENE_FRAME_ISSUE_OBJECT_PLANNING_FAILED
        })
        let byName = Dictionary(uniqueKeysWithValues: try framebuffers(plan).map {
            (string($0.resource.logical_name), $0)
        })
        XCTAssertEqual(byName["quarterA"]?.width, 1)
        XCTAssertEqual(byName["quarterA"]?.height, 1)
        XCTAssertEqual(byName["quarterB"]?.width, 1)
        XCTAssertEqual(byName["quarterB"]?.height, 1)
    }

    func testLinuxRuntimeFramebuffersRemainValidForOnePixelScenes() throws {
        let loaded = try loadLinuxCompatibility(
            bloomEnabled: true,
            width: 1,
            height: 1
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        for framebuffer in try framebuffers(plan) {
            XCTAssertGreaterThanOrEqual(framebuffer.width, 1)
            XCTAssertGreaterThanOrEqual(framebuffer.height, 1)
        }
    }

    func testFramebufferWrapModesAreStrictAndExposedByTheBridge() throws {
        let loaded = try loadSynthetic(firstFramebufferUVs: "repeat")
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let byName = Dictionary(uniqueKeysWithValues: try framebuffers(plan).map {
            (string($0.resource.logical_name), $0.wrap_mode)
        })
        XCTAssertEqual(
            byName["_rt_FullFrameBuffer"],
            WE_SCENE_FRAMEBUFFER_WRAP_CLAMP_TO_EDGE
        )
        XCTAssertEqual(
            byName["quarterA"],
            WE_SCENE_FRAMEBUFFER_WRAP_REPEAT
        )
        XCTAssertEqual(
            byName["quarterB"],
            WE_SCENE_FRAMEBUFFER_WRAP_CLAMP_TO_EDGE
        )
    }

    func testUnknownFramebufferWrapModeSkipsOnlyItsObject() throws {
        let loaded = try loadSynthetic(firstFramebufferUVs: "mirror")
        defer { destroy(loaded) }

        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }
        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        XCTAssertEqual(try planInfo(plan).image_count, 0)
        let issue = try XCTUnwrap(try issues(plan).first {
            $0.code == WE_SCENE_FRAME_ISSUE_OBJECT_PLANNING_FAILED
        })
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_SKIP_OBJECT)
        let message = string(issue.message)
        XCTAssertTrue(message.contains("UV wrap mode 'mirror'"))
        XCTAssertTrue(message.contains("clamp"))
        XCTAssertTrue(message.contains("border"))
        XCTAssertTrue(message.contains("repeat"))
    }

    func testCameraDefaultsAndOrthogonalProjectionAreInThePlanSnapshot() throws {
        let loaded = try loadSynthetic()
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.camera_center.x, 0, accuracy: 1e-6)
        XCTAssertEqual(info.camera_center.y, 0, accuracy: 1e-6)
        XCTAssertEqual(info.camera_center.z, -1, accuracy: 1e-6)
        XCTAssertEqual(info.camera_eye.x, 0, accuracy: 1e-6)
        XCTAssertEqual(info.camera_eye.y, 0, accuracy: 1e-6)
        XCTAssertEqual(info.camera_eye.z, 0, accuracy: 1e-6)
        XCTAssertEqual(info.camera_up.x, 0, accuracy: 1e-6)
        XCTAssertEqual(info.camera_up.y, 1, accuracy: 1e-6)
        XCTAssertEqual(info.camera_up.z, 0, accuracy: 1e-6)
        XCTAssertEqual(info.camera_near_plane, 0, accuracy: 1e-6)
        XCTAssertEqual(info.camera_far_plane, 1000, accuracy: 1e-6)
        XCTAssertEqual(info.camera_field_of_view, 50, accuracy: 1e-6)
        XCTAssertEqual(info.camera_projection_auto, 0)
        XCTAssertEqual(info.camera_projection_width, 400)
        XCTAssertEqual(info.camera_projection_height, 200)
    }

    func testAutomaticProjectionUsesStableRawImageExtents() throws {
        var documents = syntheticDocuments(imageSize: "100 20")
        var project = documents["project.json"] as! [String: Any]
        var projectGeneral = project["general"] as! [String: Any]
        var properties = projectGeneral["properties"] as! [String: Any]
        properties["origin"] = [
            "text": "Origin", "type": "textinput", "value": "-100 40 0",
        ]
        projectGeneral["properties"] = properties
        project["general"] = projectGeneral
        documents["project.json"] = project

        var scene = documents["scene.json"] as! [String: Any]
        var general = scene["general"] as! [String: Any]
        general["orthogonalprojection"] = ["auto": true]
        scene["general"] = general
        var objects = scene["objects"] as! [[String: Any]]
        objects[0]["origin"] = ["user": "origin", "value": "-100 40 0"]
        objects[0]["scale"] = "50 60 1"
        objects[0]["angles"] = "0 0 45"
        objects[0]["visible"] = false
        objects.append([
            "id": 8,
            "image": "models/main.json",
            "name": "Second image",
            "origin": "300 -80 0",
            "size": "40 60",
            "visible": true,
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(assets: fixture.assets, package: fixture.package, root: fixture.root)
        defer { destroy(loaded) }
        let initial = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(initial) }
        XCTAssertEqual(try planInfo(initial).width, 640)
        XCTAssertEqual(try planInfo(initial).height, 220)
        XCTAssertEqual(try planInfo(initial).camera_projection_auto, 1)

        try setString(loaded.model, key: "origin", value: "1000 2000 0")
        let updated = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(updated) }
        XCTAssertEqual(try planInfo(updated).model_revision, 1)
        XCTAssertEqual(try planInfo(updated).width, 640)
        XCTAssertEqual(try planInfo(updated).height, 220)
    }

    func testCameraUserValuesShareThePlanPropertyRevision() throws {
        let loaded = try loadCamera()
        defer { destroy(loaded) }
        let initial = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(initial) }

        let initialInfo = try planInfo(initial)
        XCTAssertEqual(initialInfo.model_revision, 0)
        XCTAssertEqual(initialInfo.camera_center.x, 1, accuracy: 1e-6)
        XCTAssertEqual(initialInfo.camera_center.y, 2, accuracy: 1e-6)
        XCTAssertEqual(initialInfo.camera_center.z, 3, accuracy: 1e-6)
        XCTAssertEqual(initialInfo.camera_near_plane, 0.25, accuracy: 1e-6)
        XCTAssertEqual(initialInfo.camera_far_plane, 8000, accuracy: 1e-6)
        XCTAssertEqual(initialInfo.camera_field_of_view, 65, accuracy: 1e-6)
        XCTAssertEqual(initialInfo.parallax_enabled, 1)
        XCTAssertEqual(initialInfo.parallax_amount, 0.75, accuracy: 1e-6)
        XCTAssertEqual(initialInfo.parallax_delay, 0.2, accuracy: 1e-6)
        XCTAssertEqual(initialInfo.parallax_mouse_influence, 0.6, accuracy: 1e-6)

        try setString(loaded.model, key: "camera_center", value: "4 5 6")
        try setNumber(loaded.model, key: "camera_near", value: 0.5)
        try setNumber(loaded.model, key: "parallax_amount", value: 1.25)
        let updated = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(updated) }

        let updatedInfo = try planInfo(updated)
        XCTAssertEqual(updatedInfo.model_revision, 3)
        XCTAssertEqual(updatedInfo.camera_center.x, 4, accuracy: 1e-6)
        XCTAssertEqual(updatedInfo.camera_center.y, 5, accuracy: 1e-6)
        XCTAssertEqual(updatedInfo.camera_center.z, 6, accuracy: 1e-6)
        XCTAssertEqual(updatedInfo.camera_near_plane, 0.5, accuracy: 1e-6)
        XCTAssertEqual(updatedInfo.parallax_amount, 1.25, accuracy: 1e-6)

        XCTAssertEqual(try planInfo(initial).model_revision, 0)
        XCTAssertEqual(try planInfo(initial).camera_center.x, 1, accuracy: 1e-6)
        XCTAssertEqual(try planInfo(initial).camera_near_plane, 0.25, accuracy: 1e-6)
        XCTAssertEqual(try planInfo(initial).parallax_amount, 0.75, accuracy: 1e-6)
    }

    func testPlanHandleOwnsItsSnapshotAfterSourceHandlesAreDestroyed() throws {
        let fixture = try makeFixture(syntheticDocuments())
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        we_scene_frame_graph_destroy(loaded.frameGraph)
        we_scene_graph_destroy(loaded.graph)
        we_scene_model_destroy(loaded.model)
        we_scene_runtime_destroy(loaded.runtime)
        try FileManager.default.removeItem(at: fixture.root)

        let info = try planInfo(plan)
        XCTAssertEqual(info.width, 400)
        XCTAssertEqual(info.operation_count, 5)
        let retainedOperations = try operations(plan)
        XCTAssertEqual(string(retainedOperations[0].shader), "base")
        XCTAssertEqual(
            string(retainedOperations[4].destination.id),
            "scene:_rt_FullFrameBuffer"
        )
    }

    func testTextDescriptorCarriesEvaluatedLayoutAndPreservesRenderOrder() throws {
        var documents = syntheticDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append([
            "alpha": 0.75,
            "color": "0.2 0.4 0.6 0.8",
            "font": "Helvetica",
            "horizontalalign": "right",
            "id": 8,
            "name": "Text",
            "origin": "30 40 5",
            "padding": "3 4",
            "pointsize": 27.5,
            "size": "120 48",
            "spacing": "0 0",
            "text": "Frame text",
            "verticalalign": "bottom",
            "visible": true,
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene
        let fixture = try makeFixture(documents)
        let loaded = try load(assets: fixture.assets, package: fixture.package, root: fixture.root)
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph, runtime: 1, frameTime: 1.0 / 60.0)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.text_count, 1)
        XCTAssertEqual(info.issue_count, 0)
        let descriptor = try XCTUnwrap(texts(plan).first)
        XCTAssertEqual(descriptor.object_index, 1)
        XCTAssertEqual(descriptor.object_id, 8)
        XCTAssertEqual(descriptor.visible, 1)
        XCTAssertEqual(string(descriptor.text), "Frame text")
        XCTAssertEqual(string(descriptor.font), "Helvetica")
        XCTAssertEqual(descriptor.point_size, 27.5, accuracy: 1e-6)
        XCTAssertEqual(descriptor.width, 120, accuracy: 1e-6)
        XCTAssertEqual(descriptor.height, 48, accuracy: 1e-6)
        XCTAssertEqual(descriptor.color_red, 0.2, accuracy: 1e-6)
        XCTAssertEqual(descriptor.color_green, 0.4, accuracy: 1e-6)
        XCTAssertEqual(descriptor.color_blue, 0.6, accuracy: 1e-6)
        XCTAssertEqual(descriptor.color_alpha, 0.8, accuracy: 1e-6)
        XCTAssertEqual(descriptor.alpha, 0.75, accuracy: 1e-6)
        XCTAssertEqual(descriptor.padding_x, 3, accuracy: 1e-6)
        XCTAssertEqual(descriptor.padding_y, 4, accuracy: 1e-6)
        XCTAssertEqual(descriptor.world_transform.origin.x, 30, accuracy: 1e-6)
        XCTAssertEqual(descriptor.world_transform.origin.y, 40, accuracy: 1e-6)
        XCTAssertEqual(string(descriptor.horizontal_alignment), "right")
        XCTAssertEqual(string(descriptor.vertical_alignment), "bottom")
        let allOperations = try operations(plan)
        XCTAssertEqual(allOperations.last?.kind, WE_SCENE_FRAME_OPERATION_TEXT)
        XCTAssertEqual(allOperations.last?.text_index, 0)
        XCTAssertEqual(allOperations.last?.object_id, 8)
        XCTAssertEqual(string(allOperations.last?.destination.id), "scene:_rt_FullFrameBuffer")
    }

    func testTextWithoutAuthoredColorUsesNormalizedWhiteDefault() throws {
        var documents = syntheticDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append([
            "id": 8,
            "name": "Default color text",
            "text": "white",
            "visible": true,
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let descriptor = try XCTUnwrap(texts(plan).first)
        XCTAssertEqual(descriptor.color_red, 1, accuracy: 1e-6)
        XCTAssertEqual(descriptor.color_green, 1, accuracy: 1e-6)
        XCTAssertEqual(descriptor.color_blue, 1, accuracy: 1e-6)
        XCTAssertEqual(descriptor.color_alpha, 1, accuracy: 1e-6)
    }

    func testNullScriptUpdateRetainsTheUpstreamStringProjection() throws {
        var documents = syntheticDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append([
            "id": 8,
            "name": "Retained text",
            "text": [
                "value": "seed",
                "script": """
                export function update(value) {
                    return engine.runtime < 2 ? "kept" : null;
                }
                """,
            ],
            "visible": true,
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let first = try createPlan(
            loaded.frameGraph,
            runtime: 1,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(first) }
        XCTAssertEqual(string(try XCTUnwrap(texts(first).first).text), "kept")

        let second = try createPlan(
            loaded.frameGraph,
            runtime: 2,
            frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(second) }
        XCTAssertEqual(string(try XCTUnwrap(texts(second).first).text), "kept")
    }

    func testParticleDescriptorUsesConcreteConfigurationAndGlobalRenderOrder() throws {
        let fixture = try makeFixture(particleRenderOrderDocuments())
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(
            loaded.frameGraph, runtime: 2, frameTime: 1.0 / 60.0
        )
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.particle_count, 2)
        XCTAssertEqual(info.issue_count, 0)

        let descriptors = try particles(plan)
        XCTAssertEqual(descriptors.count, 2)
        let visible = descriptors[0]
        XCTAssertEqual(visible.object_index, 1)
        XCTAssertEqual(visible.object_id, 8)
        XCTAssertEqual(visible.visible, 1)
        XCTAssertEqual(visible.world_transform.origin.x, 30, accuracy: 1e-6)
        XCTAssertEqual(visible.world_transform.origin.y, 40, accuracy: 1e-6)
        XCTAssertEqual(visible.world_transform.origin.z, 5, accuracy: 1e-6)
        XCTAssertEqual(visible.world_transform.scale.x, 2, accuracy: 1e-6)
        XCTAssertEqual(visible.world_transform.scale.y, 3, accuracy: 1e-6)
        XCTAssertEqual(string(visible.definition_identity), "particles/test.json")
        XCTAssertEqual(string(visible.shader), "genericparticle")
        XCTAssertEqual(string(visible.vertex_shader_path), "shaders/genericparticle.vert")
        XCTAssertEqual(string(visible.fragment_shader_path), "shaders/genericparticle.frag")
        XCTAssertEqual(visible.blending, WE_SCENE_FRAME_BLENDING_ADDITIVE)
        XCTAssertEqual(visible.culling, WE_SCENE_FRAME_CULLING_DISABLED)
        XCTAssertEqual(visible.depth_test, WE_SCENE_FRAME_DEPTH_DISABLED)
        XCTAssertEqual(visible.depth_write, WE_SCENE_FRAME_DEPTH_DISABLED)
        XCTAssertEqual(visible.texture0.kind, WE_SCENE_FRAME_RESOURCE_ASSET_TEXTURE)
        XCTAssertEqual(string(visible.texture0.id), "materials/particle/test.tex")
        XCTAssertEqual(visible.parallax_depth_x, 0.4, accuracy: 1e-6)
        XCTAssertEqual(visible.parallax_depth_y, -0.2, accuracy: 1e-6)
        XCTAssertEqual(visible.perspective, 1)
        XCTAssertEqual(string(visible.animation_mode), "randomframe")
        XCTAssertEqual(visible.sequence_multiplier, 2.5, accuracy: 1e-6)
        XCTAssertEqual(visible.max_count, 64)
        XCTAssertEqual(visible.fixed_step_seconds, 1.0 / 120.0, accuracy: 1e-15)
        XCTAssertEqual(visible.start_time, 1.25, accuracy: 1e-6)
        XCTAssertEqual(visible.flags, 4)
        XCTAssertEqual(visible.override_enabled, 0)
        XCTAssertEqual(visible.override_alpha, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_size, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_lifetime, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_rate, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_speed, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_count, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_color.x, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_color.y, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_color.z, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_color_multiplier.x, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_color_multiplier.y, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.override_color_multiplier.z, 1, accuracy: 1e-6)
        XCTAssertEqual(visible.emitter_count, 1)
        XCTAssertEqual(visible.initializer_count, 8)
        XCTAssertEqual(visible.operator_count, 6)
        XCTAssertEqual(visible.control_point_count, 1)
        XCTAssertEqual(visible.combo_count, 4)
        let emitter = try particleEmitter(plan, particleIndex: 0, emitterIndex: 0)
        XCTAssertEqual(emitter.control_point, 3)
        XCTAssertEqual(emitter.flags, 6)
        XCTAssertEqual(emitter.rate, 12, accuracy: 1e-6)
        XCTAssertEqual(emitter.delay, 0.2, accuracy: 1e-6)
        XCTAssertEqual(emitter.duration, 1.0, accuracy: 1e-6)
        XCTAssertEqual(emitter.minimum_periodic_delay, 0.3, accuracy: 1e-6)
        XCTAssertEqual(emitter.maximum_periodic_delay, 0.7, accuracy: 1e-6)
        XCTAssertEqual(emitter.minimum_periodic_duration, 0.4, accuracy: 1e-6)
        XCTAssertEqual(emitter.maximum_periodic_duration, 0.9, accuracy: 1e-6)
        XCTAssertEqual(emitter.maximum_to_emit_per_period, 8)

        XCTAssertEqual(descriptors[1].object_index, 3)
        XCTAssertEqual(descriptors[1].object_id, 11)
        XCTAssertEqual(descriptors[1].visible, 0)

        let scheduled = try operations(plan)
        XCTAssertEqual(scheduled.map(\.object_id), [7, 7, 7, 7, 7, 8, 9, 10])
        XCTAssertEqual(scheduled.map(\.kind), [
            WE_SCENE_FRAME_OPERATION_RENDER,
            WE_SCENE_FRAME_OPERATION_RENDER,
            WE_SCENE_FRAME_OPERATION_RENDER,
            WE_SCENE_FRAME_OPERATION_COPY,
            WE_SCENE_FRAME_OPERATION_RENDER,
            WE_SCENE_FRAME_OPERATION_PARTICLE,
            WE_SCENE_FRAME_OPERATION_TEXT,
            WE_SCENE_FRAME_OPERATION_RENDER,
        ])
        XCTAssertEqual(scheduled[5].particle_index, 0)
        XCTAssertEqual(string(scheduled[5].destination.id), "scene:_rt_FullFrameBuffer")
        XCTAssertFalse(scheduled.contains { $0.object_id == 11 })
    }

    func testParticleRendererDescriptorsCoverLinuxSpriteTrailAndRopePaths() throws {
        let renderers: [(name: String, kind: Int, shader: String)] = [
            ("sprite", 0, "genericparticle"),
            ("spritetrail", 1, "genericparticle"),
            ("rope", 2, "genericropeparticle"),
            ("ropetrail", 3, "genericropeparticle"),
        ]
        for renderer in renderers {
            var documents = particleRenderOrderDocuments()
            var definition = documents["particles/test.json"] as! [String: Any]
            definition["renderer"] = [[
                "length": 0.25,
                "maxlength": 8,
                "minlength": 0.1,
                "name": renderer.name,
                "segments": 6,
                "subdivision": 3,
                "uvscale": 2,
                "uvscrolling": true,
                "uvsmoothing": false,
            ]]
            documents["particles/test.json"] = definition
            let fixture = try makeFixture(documents)
            let loaded = try load(
                assets: fixture.assets,
                package: fixture.package,
                root: fixture.root
            )
            defer {
                destroy(loaded)
            }
            let plan = try createPlan(
                loaded.frameGraph, runtime: 1, frameTime: 0
            )
            defer { we_scene_frame_plan_destroy(plan) }
            let descriptors = try particles(plan)
            XCTAssertEqual(Int(descriptors[0].renderer_kind), renderer.kind)
            XCTAssertEqual(string(descriptors[0].shader), renderer.shader)
            XCTAssertEqual(descriptors[0].renderer_length, 0.25, accuracy: 1e-9)
            XCTAssertEqual(descriptors[0].renderer_subdivision, 3, accuracy: 1e-9)
            XCTAssertEqual(descriptors[0].renderer_uv_scale, 2, accuracy: 1e-9)
            XCTAssertEqual(descriptors[0].renderer_uv_scrolling, 1)
            XCTAssertEqual(descriptors[0].renderer_uv_smoothing, 0)
        }
    }

    func testUnknownParticleRendererSkipsOnlyThatObjectDuringPlanning() throws {
        var documents = particleRenderOrderDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        definition["renderer"] = [["name": "future-renderer"]]
        documents["particles/test.json"] = definition
        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer {
            destroy(loaded)
        }
        let plan = try createPlan(
            loaded.frameGraph, runtime: 1, frameTime: 0
        )
        defer { we_scene_frame_plan_destroy(plan) }
        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.particle_count, 0)
        XCTAssertTrue(try issues(plan).contains {
            $0.object_id == 8 && string($0.message).contains("future-renderer")
        })
    }

    func testParticleVectorAndEmitterRangesAllowReverseEndpoints() throws {
        let fixture = try makeFixture(particleRenderOrderDocuments())
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        XCTAssertEqual(try particles(plan).first?.initializer_count, 8)
    }

    func testParticleControlPointsUseCanonicalPointerAndFullInverseTransform() throws {
        var documents = particleRenderOrderDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[1]["origin"] = "220 80 0"
        objects[1]["scale"] = "2 4 1"
        objects[1]["angles"] = "0 0 1.5707963267948966"
        scene["objects"] = objects
        documents["scene.json"] = scene

        var definition = documents["particles/test.json"] as! [String: Any]
        definition["controlpoint"] = [
            ["id": 3, "flags": 1, "offset": "12 8 0"],
            ["id": 4, "flags": 2, "offset": "112 -42 0"],
            ["id": 5, "offset": "5 6 7"],
        ]
        documents["particles/test.json"] = definition

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(
            loaded.frameGraph,
            runtime: 2,
            frameTime: 1.0 / 60.0,
            pointerX: 0.75,
            pointerY: 0.25
        )
        defer { we_scene_frame_plan_destroy(plan) }

        let points = try particleControlPoints(plan, particleIndex: 0)
        XCTAssertEqual(points.count, 3)
        XCTAssertEqual(points[0].id, 3)
        XCTAssertEqual(points[0].position.x, 31, accuracy: 1e-5)
        XCTAssertEqual(points[0].position.y, 23, accuracy: 1e-6)
        XCTAssertEqual(points[0].position.z, 0, accuracy: 1e-6)
        XCTAssertEqual(points[1].id, 4)
        XCTAssertEqual(points[1].position.x, 31, accuracy: 1e-5)
        XCTAssertEqual(points[1].position.y, 23, accuracy: 1e-6)
        XCTAssertEqual(points[1].position.z, 0, accuracy: 1e-6)
        XCTAssertEqual(points[2].id, 5)
        XCTAssertEqual(points[2].position.x, 5, accuracy: 1e-6)
        XCTAssertEqual(points[2].position.y, 6, accuracy: 1e-6)
        XCTAssertEqual(points[2].position.z, 7, accuracy: 1e-6)
    }

    func testDuplicateParticleControlPointUsesTheLastDefinitionLikeLinux() throws {
        var documents = particleRenderOrderDocuments()
        var definition = documents["particles/test.json"] as! [String: Any]
        definition["controlpoint"] = [
            ["id": 3, "offset": "1 2 3"],
            ["id": 3, "offset": "9 8 7"],
            ["id": 4, "offset": "4 5 6"],
        ]
        documents["particles/test.json"] = definition

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let points = try particleControlPoints(plan, particleIndex: 0)
        XCTAssertEqual(points.map(\.id), [3, 4])
        XCTAssertEqual(points[0].position.x, 9, accuracy: 1e-6)
        XCTAssertEqual(points[0].position.y, 8, accuracy: 1e-6)
        XCTAssertEqual(points[0].position.z, 7, accuracy: 1e-6)
    }

    func testParticleInstanceOverrideValuesRemainActiveWhenEditorFlagIsDisabled() throws {
        var documents = particleRenderOrderDocuments()
        var project = documents["project.json"] as! [String: Any]
        var general = project["general"] as! [String: Any]
        var properties = general["properties"] as! [String: Any]
        properties["particle_override_enabled"] = [
            "text": "Particle override", "type": "bool", "value": true,
        ]
        properties["particle_override_alpha"] = [
            "max": 1.0, "min": 0.0, "step": 0.05,
            "text": "Particle alpha", "type": "slider", "value": 0.4,
        ]
        properties["particle_override_color"] = [
            "text": "Particle color", "type": "color", "value": "0.1 0.2 0.3",
        ]
        general["properties"] = properties
        project["general"] = general
        documents["project.json"] = project

        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects[1]["instanceoverride"] = [
            "enabled": ["user": "particle_override_enabled", "value": true],
            "alpha": ["user": "particle_override_alpha", "value": 0.4],
            "size": 2.0,
            "lifetime": 3.0,
            "rate": 4.0,
            "speed": 5.0,
            "count": 0.5,
            "color": ["user": "particle_override_color", "value": "0.1 0.2 0.3"],
            "colorn": "0.8 0.9 1.0",
        ]
        scene["objects"] = objects
        documents["scene.json"] = scene

        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let initialPlan = try createPlan(loaded.frameGraph)
        let initial = try XCTUnwrap(try particles(initialPlan).first)
        XCTAssertEqual(initial.override_alpha, 0.4, accuracy: 1e-6)
        XCTAssertEqual(initial.override_size, 2, accuracy: 1e-6)
        XCTAssertEqual(initial.override_lifetime, 3, accuracy: 1e-6)
        XCTAssertEqual(initial.override_rate, 4, accuracy: 1e-6)
        XCTAssertEqual(initial.override_speed, 5, accuracy: 1e-6)
        XCTAssertEqual(initial.override_count, 0.5, accuracy: 1e-6)
        XCTAssertEqual(initial.override_color.x, 0.1, accuracy: 1e-6)
        XCTAssertEqual(initial.override_color_multiplier.z, 1.0, accuracy: 1e-6)
        we_scene_frame_plan_destroy(initialPlan)

        try setNumber(loaded.model, key: "particle_override_alpha", value: 0.75)
        try setString(
            loaded.model,
            key: "particle_override_color",
            value: "0.6 0.5 0.4"
        )
        let updatedPlan = try createPlan(loaded.frameGraph)
        let updated = try XCTUnwrap(try particles(updatedPlan).first)
        XCTAssertEqual(updated.override_alpha, 0.75, accuracy: 1e-6)
        XCTAssertEqual(updated.override_color.x, 0.6, accuracy: 1e-6)
        XCTAssertEqual(updated.override_color.z, 0.4, accuracy: 1e-6)
        we_scene_frame_plan_destroy(updatedPlan)

        try setBoolean(loaded.model, key: "particle_override_enabled", value: false)
        let disabledPlan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(disabledPlan) }
        let disabled = try XCTUnwrap(try particles(disabledPlan).first)
        XCTAssertEqual(disabled.override_enabled, 0)
        XCTAssertEqual(disabled.override_alpha, 0.75, accuracy: 1e-6)
        XCTAssertEqual(disabled.override_size, 2, accuracy: 1e-6)
        XCTAssertEqual(disabled.override_lifetime, 3, accuracy: 1e-6)
        XCTAssertEqual(disabled.override_rate, 4, accuracy: 1e-6)
        XCTAssertEqual(disabled.override_speed, 5, accuracy: 1e-6)
        XCTAssertEqual(disabled.override_count, 0.5, accuracy: 1e-6)
        XCTAssertEqual(disabled.override_color.x, 0.6, accuracy: 1e-6)
        XCTAssertEqual(disabled.override_color_multiplier.z, 1, accuracy: 1e-6)
    }

    func testParticleDynamicReverseRangeRemainsExecutableLikeLinux() throws {
        let fixture = try makeFixture(particleRenderOrderDocuments())
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let initial = try createPlan(loaded.frameGraph)
        we_scene_frame_plan_destroy(initial)
        try setNumber(loaded.model, key: "particle_lifetime", value: 5)

        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }
        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        XCTAssertTrue(try particles(plan).contains { $0.object_id == 8 })
        XCTAssertTrue(try operations(plan).contains { $0.object_id == 8 })
        XCTAssertFalse(try issues(plan).contains {
            $0.code == WE_SCENE_FRAME_ISSUE_OBJECT_PLANNING_FAILED &&
                $0.object_id == 8
        })
    }

    func testParticleMaterialAcceptsSpritesheetComboAndTextureMetadata() throws {
        var documents = particleRenderOrderDocuments()
        var material = documents["materials/particle/test.json"] as! [String: Any]
        var passes = material["passes"] as! [[String: Any]]
        passes[0]["combos"] = ["SPRITESHEET": 1]
        passes[0]["textures"] = [[
            "name": "particle/test",
            "sidecar": "particle/test.tex-json",
        ]]
        material["passes"] = passes
        documents["materials/particle/test.json"] = material
        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }
        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        XCTAssertEqual(try particles(plan).first?.combo_count, 4)
    }

    func testParticleMaterialUsesOnlyFirstPassLikeLinux() throws {
        var documents = particleRenderOrderDocuments()
        var material = documents["materials/particle/test.json"] as! [String: Any]
        var passes = material["passes"] as! [[String: Any]]
        passes.append([
            "shader": "unsupported-second-particle-pass",
            "textures": ["particle/unused"],
        ])
        material["passes"] = passes
        documents["materials/particle/test.json"] = material
        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }
        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        let descriptor = try XCTUnwrap(try particles(plan).first)
        XCTAssertEqual(string(descriptor.shader), "genericparticle")
        XCTAssertTrue(try operations(plan).contains { $0.object_id == 8 })
    }

    func testSpriteParticleKeepsAuthoredFirstPassShaderLikeLinux() throws {
        var documents = particleRenderOrderDocuments()
        var material = documents["materials/particle/test.json"] as! [String: Any]
        var passes = material["passes"] as! [[String: Any]]
        passes[0]["shader"] = "customparticle"
        material["passes"] = passes
        documents["materials/particle/test.json"] = material
        let fixture = try makeFixture(documents)
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }

        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }
        XCTAssertEqual(try planInfo(plan).is_executable, 1)
        let descriptor = try XCTUnwrap(try particles(plan).first)
        XCTAssertEqual(string(descriptor.shader), "customparticle")
        XCTAssertEqual(string(descriptor.vertex_shader_path), "shaders/customparticle.vert")
        XCTAssertEqual(string(descriptor.fragment_shader_path), "shaders/customparticle.frag")
        XCTAssertTrue(try operations(plan).contains { $0.object_id == 8 })
    }

    func testUnsupportedTextSpacingWarnsAndUsesTheLinuxDefaultLayout() throws {
        var documents = syntheticDocuments()
        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append([
            "id": 8, "name": "Text", "text": "spaced", "pointsize": 20,
            "spacing": "1 0", "visible": true,
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene
        let fixture = try makeFixture(documents)
        let loaded = try load(assets: fixture.assets, package: fixture.package, root: fixture.root)
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }
        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.text_count, 1)
        XCTAssertTrue(try operations(plan).contains { $0.object_id == 7 })
        XCTAssertTrue(try operations(plan).contains { $0.object_id == 8 })
        let issue = try XCTUnwrap(try issues(plan).first {
            $0.code == WE_SCENE_FRAME_ISSUE_TEXT_RENDERING_UNAVAILABLE &&
                string($0.json_pointer).hasSuffix("/spacing")
        })
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_WARNING)
    }

    func testSolidLayerUsesTransparentProceduralFramebufferSource() throws {
        let loaded = try loadSynthetic(
            primaryTexture: false,
            solidLayer: true,
            imageSize: "0 0"
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        let allOperations = try operations(plan)
        guard let clear = allOperations.first(where: {
            $0.kind == WE_SCENE_FRAME_OPERATION_CLEAR && $0.object_id == 7
        }) else {
            return XCTFail("solid layer must initialize its procedural source")
        }
        XCTAssertEqual(clear.clear_red, 0, accuracy: 1e-6)
        XCTAssertEqual(clear.clear_green, 0, accuracy: 1e-6)
        XCTAssertEqual(clear.clear_blue, 0, accuracy: 1e-6)
        XCTAssertEqual(clear.clear_alpha, 0, accuracy: 1e-6)
        XCTAssertEqual(string(clear.destination.logical_name), "_rt_solidLayerSource_7")

        var source: WESceneFramebufferInfo?
        for index in 0..<info.framebuffer_count {
            var framebuffer = WESceneFramebufferInfo()
            var error: WESceneRuntimeErrorRef?
            XCTAssertEqual(
                we_scene_frame_plan_framebuffer_info(plan, index, &framebuffer, &error),
                1,
                errorMessage(error)
            )
            if string(framebuffer.resource.id) == string(clear.destination.id) {
                source = framebuffer
            }
            we_scene_runtime_error_destroy(error)
        }
        XCTAssertEqual(source?.format, WE_SCENE_FRAMEBUFFER_RGBA8)
        XCTAssertEqual(source?.width, 400)
        XCTAssertEqual(source?.height, 200)

        let allIssues = try issues(plan)
        XCTAssertFalse(allIssues.contains {
            $0.code == WE_SCENE_FRAME_ISSUE_IMAGE_MATERIAL_UNAVAILABLE
        })
        XCTAssertFalse(allIssues.contains {
            $0.code == WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_READ_BEFORE_WRITE
        })
    }

    func testPuppetImageUsesIndexedGeometryOnItsFirstPass() throws {
        var documents = syntheticDocuments()
        var puppetModel = documents["models/main.json"] as! [String: Any]
        puppetModel["puppet"] = "models/main.mdl"
        documents["models/main.json"] = puppetModel

        var scene = documents["scene.json"] as! [String: Any]
        var objects = scene["objects"] as! [[String: Any]]
        objects.append([
            "id": 8,
            "image": "models/healthy.json",
            "name": "Healthy sibling",
            "origin": "200 100 0",
            "size": "400 200",
            "visible": true,
        ])
        scene["objects"] = objects
        documents["scene.json"] = scene
        documents["models/healthy.json"] = [
            "material": "materials/healthy.json",
        ]
        documents["materials/healthy.json"] = [
            "passes": [["shader": "base", "textures": ["healthy"]]],
        ]

        let fixture = try makeFixture(
            documents,
            binaryEntries: [("models/main.mdl", makePuppetMesh())]
        )
        let loaded = try load(
            assets: fixture.assets,
            package: fixture.package,
            root: fixture.root
        )
        defer { destroy(loaded) }
        let plan = try createPlan(loaded.frameGraph)
        defer { we_scene_frame_plan_destroy(plan) }

        let info = try planInfo(plan)
        XCTAssertEqual(info.is_executable, 1)
        XCTAssertEqual(info.image_count, 2)
        let allOperations = try operations(plan)
        let puppetOperations = allOperations.filter { $0.object_id == 7 }
        XCTAssertFalse(puppetOperations.isEmpty)
        XCTAssertEqual(
            puppetOperations.first(where: {
                $0.kind == WE_SCENE_FRAME_OPERATION_RENDER
            })?.geometry,
            WE_SCENE_FRAME_GEOMETRY_PUPPET_MESH
        )
        let puppetRenders = puppetOperations.filter {
            $0.kind == WE_SCENE_FRAME_OPERATION_RENDER
        }
        let puppetClearIndex = puppetOperations.firstIndex {
            $0.kind == WE_SCENE_FRAME_OPERATION_CLEAR
        }
        let puppetRenderIndex = puppetOperations.firstIndex {
            $0.kind == WE_SCENE_FRAME_OPERATION_RENDER
        }
        XCTAssertNotNil(puppetClearIndex)
        XCTAssertNotNil(puppetRenderIndex)
        if let puppetClearIndex, let puppetRenderIndex {
            XCTAssertLessThan(puppetClearIndex, puppetRenderIndex)
            XCTAssertEqual(puppetOperations[puppetClearIndex].clear_red, 0)
            XCTAssertEqual(puppetOperations[puppetClearIndex].clear_green, 0)
            XCTAssertEqual(puppetOperations[puppetClearIndex].clear_blue, 0)
            XCTAssertEqual(puppetOperations[puppetClearIndex].clear_alpha, 0)
        }
        XCTAssertEqual(
            puppetRenders.first?.blending,
            WE_SCENE_FRAME_BLENDING_TRANSLUCENT,
            "The first Puppet draw must use Linux's forced translucent blend"
        )
        XCTAssertTrue(
            puppetRenders.dropFirst().allSatisfy {
                $0.geometry != WE_SCENE_FRAME_GEOMETRY_PUPPET_MESH
            },
            "Only the first Puppet image pass may use indexed geometry"
        )
        XCTAssertTrue(allOperations.contains { $0.object_id == 8 })
        XCTAssertFalse(try issues(plan).contains {
            $0.code == WE_SCENE_FRAME_ISSUE_PUPPET_UNAVAILABLE
        })
    }

}
