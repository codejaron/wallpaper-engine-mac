import Foundation
import SceneRuntimeBridge
import XCTest

final class SceneCursorExecutorTests: XCTestCase {
    private struct Layer {
        let id: Int
        let origin: String
        let size: String
        let solid: Bool
        let script: String
        var angles: String? = nil
        var alignment: String? = nil
        var perspective = false
    }

    private struct Pipeline {
        let root: URL
        let runtime: WESceneRuntimeRef
        let model: WESceneModelRef
        let graph: WESceneGraphRef
        let frameGraph: WESceneFrameGraphRef
        let executor: WESceneFrameExecutorRef
        let width: Int
        let height: Int
    }

    func testFirstCommittedPlanDefersHeldButtonEdgeAndOrdersReleaseBeforeClick() throws {
        let loaded = try makePipeline(
            width: 8,
            height: 4,
            layers: [fullLayer(id: 1)]
        )
        defer { destroy(loaded) }

        let first = try render(
            loaded, x: 0.25, y: 0.5, active: true, leftDown: true, time: 1
        )
        XCTAssertEqual(red(first, x: 1, y: 2, width: 8), 0)

        let held = try render(
            loaded, x: 0.25, y: 0.5, active: true, leftDown: true, time: 2
        )
        XCTAssertEqual(
            red(held, x: 1, y: 2, width: 8),
            12,
            "The first hittable frame must dispatch enter then the deferred down edge"
        )

        let released = try render(
            loaded, x: 0.25, y: 0.5, active: true, leftDown: false, time: 3
        )
        XCTAssertEqual(
            red(released, x: 1, y: 2, width: 8),
            46,
            "A same-layer release must dispatch up before click"
        )
    }

    func testPressAndReleaseBetweenFramesPreserveBothButtonEdges() throws {
        let script = """
        let events = 0;
        export function cursorDown() { events += 1; }
        export function cursorUp() { events += 2; }
        export function cursorClick() { events += 4; }
        export function update() {
            const result = events;
            events = 0;
            return result / 255.0;
        }
        """
        let loaded = try makePipeline(
            width: 8,
            height: 4,
            layers: [Layer(
                id: 1,
                origin: "4 2 0",
                size: "8 4",
                solid: true,
                script: script
            )]
        )
        defer { destroy(loaded) }

        _ = try render(
            loaded, x: 0.25, y: 0.5, active: true, leftDown: false, time: 1
        )
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_frame_executor_set_pointer_state(
                loaded.executor, 1, 1, &error
            ),
            1,
            error.map { String(cString: we_scene_runtime_error_message($0)) } ?? ""
        )
        XCTAssertNil(error)
        XCTAssertEqual(
            we_scene_frame_executor_set_pointer_state(
                loaded.executor, 1, 0, &error
            ),
            1,
            error.map { String(cString: we_scene_runtime_error_message($0)) } ?? ""
        )
        XCTAssertNil(error)

        let clicked = try render(
            loaded, x: 0.25, y: 0.5, active: true, leftDown: false, time: 2
        )
        XCTAssertEqual(
            red(clicked, x: 1, y: 2, width: 8),
            7,
            "A complete desktop click must not disappear between evaluated frames"
        )
    }

    func testCursorCallbackExportMakesNonSolidLayerInteractive() throws {
        let script = """
        let events = 0;
        export function cursorDown() { events += 1; }
        export function cursorUp() { events += 2; }
        export function cursorClick() { events += 4; }
        export function update() {
            const result = events;
            events = 0;
            return result / 255.0;
        }
        """
        let loaded = try makePipeline(
            width: 8,
            height: 4,
            layers: [Layer(
                id: 1,
                origin: "4 2 0",
                size: "8 4",
                solid: false,
                script: script
            )]
        )
        defer { destroy(loaded) }

        _ = try render(
            loaded, x: 0.5, y: 0.5, active: true, leftDown: false, time: 1
        )
        let pressed = try render(
            loaded, x: 0.5, y: 0.5, active: true, leftDown: true, time: 2
        )
        XCTAssertEqual(red(pressed, x: 4, y: 2, width: 8), 1)

        let released = try render(
            loaded, x: 0.5, y: 0.5, active: true, leftDown: false, time: 3
        )
        XCTAssertEqual(
            red(released, x: 4, y: 2, width: 8),
            6,
            "The callback owner must receive up followed by click even when the serialized Solid field is absent"
        )
    }

    func testCursorWorldPositionAndLeftButtonUseTheCurrentProjection() throws {
        let inputScript = """
        export function update() {
            const world = input.cursorWorldPosition;
            const screen = input.cursorScreenPosition;
            const matches = Math.abs(world.x - 2.0) < 0.000001
                && Math.abs(world.y - 1.0) < 0.000001
                && Math.abs(world.z) < 0.000001
                && Math.abs(screen.x - 0.25) < 0.000001
                && Math.abs(screen.y - 0.75) < 0.000001;
            return input.cursorLeftDown && matches ? 1.0 : 0.0;
        }
        """
        let loaded = try makePipeline(
            width: 8,
            height: 4,
            layers: [Layer(
                id: 1,
                origin: "4 2 0",
                size: "8 4",
                solid: true,
                script: inputScript
            )]
        )
        defer { destroy(loaded) }

        let pixels = try render(
            loaded, x: 0.25, y: 0.75, active: true, leftDown: true, time: 1
        )
        XCTAssertEqual(
            red(pixels, x: 1, y: 1, width: 8),
            255,
            "Script input must expose real scene pixels and the sampled left-button state on the first frame"
        )
    }

    func testEveryOverlappingInteractiveLayerReceivesEnter() throws {
        let solidTop = try makePipeline(
            width: 8,
            height: 4,
            layers: [
                fullLayer(id: 1),
                Layer(
                    id: 2,
                    origin: "6 2 0",
                    size: "4 4",
                    solid: true,
                    script: Self.eventScript
                ),
            ]
        )
        defer { destroy(solidTop) }

        _ = try render(
            solidTop, x: 0.75, y: 0.5, active: false, leftDown: false, time: 1
        )
        let topHit = try render(
            solidTop, x: 0.75, y: 0.5, active: true, leftDown: false, time: 2
        )
        XCTAssertEqual(
            red(topHit, x: 1, y: 2, width: 8),
            1,
            "An overlapping interactive layer must not hide the lower layer's cursor callbacks"
        )
        XCTAssertEqual(
            red(topHit, x: 6, y: 2, width: 8),
            1,
            "The upper solid layer must receive enter"
        )

        let nonSolidTop = try makePipeline(
            width: 8,
            height: 4,
            layers: [
                fullLayer(id: 1),
                Layer(
                    id: 2,
                    origin: "6 2 0",
                    size: "4 4",
                    solid: false,
                    script: Self.eventScript
                ),
            ]
        )
        defer { destroy(nonSolidTop) }

        _ = try render(
            nonSolidTop, x: 0.75, y: 0.5, active: false, leftDown: false, time: 1
        )
        let fallthroughHit = try render(
            nonSolidTop, x: 0.75, y: 0.5, active: true, leftDown: false, time: 2
        )
        XCTAssertEqual(
            red(fallthroughHit, x: 1, y: 2, width: 8),
            1,
            "A callback-bound non-solid layer must not block the interactive layer below it"
        )
        XCTAssertEqual(
            red(fallthroughHit, x: 6, y: 2, width: 8),
            1,
            "A cursor callback export makes its owning layer interactive"
        )
    }

    func testRotatedAlignedLayerUsesTheInverseRenderedTransform() throws {
        let loaded = try makePipeline(
            width: 10,
            height: 10,
            layers: [Layer(
                id: 1,
                origin: "3 5 0",
                size: "2 4",
                solid: true,
                script: Self.eventScript,
                angles: "0 0 1.5707963267948966",
                alignment: "left"
            )]
        )
        defer { destroy(loaded) }

        _ = try render(
            loaded, x: 0.25, y: 0.5, active: false, leftDown: false, time: 1
        )
        let rotatedHit = try render(
            loaded, x: 0.25, y: 0.5, active: true, leftDown: false, time: 2
        )
        XCTAssertEqual(
            red(rotatedHit, x: 3, y: 5, width: 10),
            1,
            "This point is inside only after applying the authored left alignment and Z rotation"
        )

        let unrotatedOnly = try render(
            loaded, x: 0.4, y: 0.65, active: true, leftDown: false, time: 3
        )
        XCTAssertEqual(
            red(unrotatedOnly, x: 3, y: 5, width: 10),
            2,
            "A point inside the unrotated box but outside the rendered box must dispatch leave"
        )
    }

    func testPerspectiveLayerDepthUsesTheRenderedRayPlaneIntersection() throws {
        let loaded = try makePipeline(
            width: 100,
            height: 100,
            layers: [Layer(
                id: 1,
                origin: "50 50 500",
                size: "20 20",
                solid: true,
                script: Self.eventScript,
                perspective: true
            )]
        )
        defer { destroy(loaded) }

        _ = try render(
            loaded, x: 0.65, y: 0.5, active: false, leftDown: false, time: 1
        )
        let hit = try render(
            loaded, x: 0.65, y: 0.5, active: true, leftDown: false, time: 2
        )
        XCTAssertEqual(
            red(hit, x: 50, y: 50, width: 100),
            1,
            "A Z=500 perspective layer is visibly twice its authored size and must receive the matching cursor enter"
        )
    }

    func testLayerCrossingOrdersLeaveEnterMove() throws {
        let loaded = try makePipeline(
            width: 8,
            height: 4,
            layers: [
                Layer(
                    id: 1,
                    origin: "2 2 0",
                    size: "4 4",
                    solid: true,
                    script: Self.eventScript
                ),
                Layer(
                    id: 2,
                    origin: "6 2 0",
                    size: "4 4",
                    solid: true,
                    script: Self.eventScript
                ),
            ]
        )
        defer { destroy(loaded) }

        _ = try render(
            loaded, x: 0.25, y: 0.5, active: false, leftDown: false, time: 1
        )
        _ = try render(
            loaded, x: 0.25, y: 0.5, active: true, leftDown: false, time: 2
        )
        let crossed = try render(
            loaded, x: 0.75, y: 0.5, active: true, leftDown: false, time: 3
        )
        XCTAssertEqual(red(crossed, x: 1, y: 2, width: 8), 2)
        XCTAssertEqual(
            red(crossed, x: 6, y: 2, width: 8),
            11,
            "The new layer must receive enter before move in the same frame"
        )
    }

    func testDragKeepsPressedLayerAndUpdatesCapturedCoordinates() throws {
        let capturedScript = """
        let events = 0;
        function push(code) { events = events * 8 + code; }
        export function cursorEnter() { push(1); }
        export function cursorLeave() { push(2); }
        export function cursorMove(event) {
            push(event.worldPosition.x > 6.0 ? 7 : 3);
        }
        export function cursorDown() { push(4); }
        export function cursorUp() { push(5); }
        export function cursorClick() { push(6); }
        export function update() {
            const result = events;
            events = 0;
            return result / 255.0;
        }
        """
        let loaded = try makePipeline(
            width: 8,
            height: 4,
            layers: [
                Layer(
                    id: 1,
                    origin: "2 2 0",
                    size: "4 4",
                    solid: true,
                    script: capturedScript
                ),
                Layer(
                    id: 2,
                    origin: "6 2 0",
                    size: "4 4",
                    solid: true,
                    script: Self.eventScript
                ),
            ]
        )
        defer { destroy(loaded) }

        _ = try render(
            loaded, x: 0.25, y: 0.5, active: false, leftDown: false, time: 1
        )
        _ = try render(
            loaded, x: 0.25, y: 0.5, active: true, leftDown: false, time: 2
        )
        let pressed = try render(
            loaded, x: 0.375, y: 0.5, active: true, leftDown: true, time: 3
        )
        XCTAssertEqual(red(pressed, x: 1, y: 2, width: 8), 28)

        let crossed = try render(
            loaded, x: 0.8125, y: 0.5, active: true, leftDown: true, time: 4
        )
        XCTAssertEqual(
            red(crossed, x: 1, y: 2, width: 8),
            23,
            "The pressed layer must receive leave then a captured move carrying the current world position"
        )
        XCTAssertEqual(red(crossed, x: 6, y: 2, width: 8), 1)

        let continued = try render(
            loaded, x: 0.875, y: 0.5, active: true, leftDown: true, time: 5
        )
        XCTAssertEqual(red(continued, x: 1, y: 2, width: 8), 7)
        XCTAssertEqual(
            red(continued, x: 6, y: 2, width: 8),
            0,
            "Hover layers must not steal cursorMove while another layer owns the drag"
        )

        let releasedOutside = try render(
            loaded, x: 0.875, y: 0.5, active: false, leftDown: false, time: 6
        )
        XCTAssertEqual(
            red(releasedOutside, x: 1, y: 2, width: 8),
            5,
            "The captured layer must receive up even when the desktop pointer becomes inactive"
        )
        XCTAssertEqual(red(releasedOutside, x: 6, y: 2, width: 8), 2)
    }

    private static let eventScript = """
    let events = 0;
    function push(code) { events = events * 8 + code; }
    export function cursorEnter() { push(1); }
    export function cursorLeave() { push(2); }
    export function cursorMove() { push(3); }
    export function cursorDown() { push(4); }
    export function cursorUp() { push(5); }
    export function cursorClick() { push(6); }
    export function update() {
        const result = events;
        events = 0;
        return result / 255.0;
    }
    """

    private func fullLayer(id: Int) -> Layer {
        Layer(
            id: id,
            origin: "4 2 0",
            size: "8 4",
            solid: true,
            script: Self.eventScript
        )
    }

    private func makePipeline(
        width: Int,
        height: Int,
        layers: [Layer]
    ) throws -> Pipeline {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: shaders,
            withIntermediateDirectories: true
        )
        try Data(Self.vertexShader.utf8).write(
            to: shaders.appendingPathComponent("cursor.vert")
        )
        try Data(Self.fragmentShader.utf8).write(
            to: shaders.appendingPathComponent("cursor.frag")
        )

        let objects: [[String: Any]] = layers.map { layer in
            var object: [String: Any] = [
                "alpha": ["value": 0.0, "script": layer.script],
                "id": layer.id,
                "image": "models/cursor.json",
                "name": "Cursor layer \(layer.id)",
                "origin": layer.origin,
                "perspective": layer.perspective,
                "size": layer.size,
                "solid": layer.solid,
                "visible": true,
            ]
            if let angles = layer.angles { object["angles"] = angles }
            if let alignment = layer.alignment {
                object["alignment"] = alignment
            }
            return object
        }
        let project: [String: Any] = [
            "file": "scene.json",
            "title": "Cursor executor fixture",
            "type": "scene",
            "version": 2,
        ]
        let scene: [String: Any] = [
            "camera": [
                "center": "0 0 -1",
                "eye": "0 0 0",
                "nearz": 0,
                "up": "0 1 0",
            ],
            "general": [
                "clearcolor": "0 0 0 1",
                "orthogonalprojection": ["height": height, "width": width],
            ],
            "objects": objects,
            "version": 1,
        ]
        let material: [String: Any] = [
            "passes": [[
                "blending": "normal",
                "cullmode": "nocull",
                "depthtest": "disabled",
                "depthwrite": "disabled",
                "shader": "cursor",
            ]],
        ]
        let model: [String: Any] = [
            "material": "materials/cursor.json",
            "solidlayer": true,
        ]
        try makePackage([
            ("materials/cursor.json", try json(material)),
            ("models/cursor.json", try json(model)),
            ("project.json", try json(project)),
            ("scene.json", try json(scene)),
        ]).write(to: package)

        do {
            var error: WESceneRuntimeErrorRef?
            guard let runtime = assets.path.withCString({ assetsPath in
                package.path.withCString { packagePath in
                    var configuration = WESceneRuntimeConfiguration(
                        assets_directory: assetsPath,
                        scene_package_path: packagePath
                    )
                    return we_scene_runtime_create(&configuration, &error)
                }
            }) else { throw failure("runtime", error) }
            guard let loadedModel = "project.json".withCString({
                we_scene_runtime_model_create(runtime, $0, &error)
            }) else {
                we_scene_runtime_destroy(runtime)
                throw failure("model", error)
            }
            guard let graph = we_scene_model_graph_create(loadedModel, &error) else {
                we_scene_model_destroy(loadedModel)
                we_scene_runtime_destroy(runtime)
                throw failure("graph", error)
            }
            guard let frameGraph = we_scene_graph_frame_graph_create(graph, &error) else {
                we_scene_graph_destroy(graph)
                we_scene_model_destroy(loadedModel)
                we_scene_runtime_destroy(runtime)
                throw failure("frame graph", error)
            }
            guard let executor = we_scene_frame_executor_create(frameGraph, &error) else {
                we_scene_frame_graph_destroy(frameGraph)
                we_scene_graph_destroy(graph)
                we_scene_model_destroy(loadedModel)
                we_scene_runtime_destroy(runtime)
                throw failure("executor", error)
            }
            return Pipeline(
                root: root,
                runtime: runtime,
                model: loadedModel,
                graph: graph,
                frameGraph: frameGraph,
                executor: executor,
                width: width,
                height: height
            )
        } catch {
            try? FileManager.default.removeItem(at: root)
            throw error
        }
    }

    private func destroy(_ loaded: Pipeline) {
        we_scene_frame_executor_destroy(loaded.executor)
        we_scene_frame_graph_destroy(loaded.frameGraph)
        we_scene_graph_destroy(loaded.graph)
        we_scene_model_destroy(loaded.model)
        we_scene_runtime_destroy(loaded.runtime)
        try? FileManager.default.removeItem(at: loaded.root)
    }

    private func render(
        _ loaded: Pipeline,
        x: Double,
        y: Double,
        active: Bool,
        leftDown: Bool,
        time: Double
    ) throws -> [UInt8] {
        var error: WESceneRuntimeErrorRef?
        guard we_scene_frame_executor_set_pointer_state(
            loaded.executor,
            active ? 1 : 0,
            leftDown ? 1 : 0,
            &error
        ) == 1 else {
            throw failure("pointer state", error)
        }
        var inputs = WESceneFrameInputs(
            pointer_x: x,
            pointer_y: y,
            time_seconds: time,
            frame_time_seconds: 1.0 / 60.0
        )
        guard we_scene_frame_executor_render(
            loaded.executor, &inputs, &error
        ) == 1 else {
            throw failure("render", error)
        }
        var pixels = [UInt8](
            repeating: 0,
            count: we_scene_frame_executor_rgba8_byte_count(loaded.executor)
        )
        let read = pixels.withUnsafeMutableBufferPointer { buffer in
            we_scene_frame_executor_read_rgba8(
                loaded.executor,
                buffer.baseAddress,
                buffer.count,
                &error
            )
        }
        guard read == 1 else { throw failure("readback", error) }
        return pixels
    }

    private func red(_ pixels: [UInt8], x: Int, y: Int, width: Int) -> UInt8 {
        pixels[(y * width + x) * 4]
    }

    private func json(_ value: Any) throws -> Data {
        try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
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

    private func failure(
        _ phase: String,
        _ error: WESceneRuntimeErrorRef?
    ) -> NSError {
        let message = we_scene_runtime_error_message(error)
            .map(String.init(cString:)) ?? "No runtime error"
        we_scene_runtime_error_destroy(error)
        return NSError(
            domain: "SceneCursorExecutorTests",
            code: 1,
            userInfo: [NSLocalizedDescriptionKey: "\(phase) failed: \(message)"]
        )
    }

    private static let vertexShader = """
    attribute vec3 a_Position;
    attribute vec2 a_TexCoord;
    uniform mat4 g_ModelViewProjectionMatrix;
    void main() {
        gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);
    }
    """

    private static let fragmentShader = """
    uniform float g_Alpha;
    void main() {
        gl_FragColor = vec4(g_Alpha, 0.0, 0.0, 1.0);
    }
    """
}
