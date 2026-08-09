import Foundation
import SceneRuntimeBridge
import XCTest

final class SceneGraphBridgeTests: XCTestCase {
    private enum TestFailure: Error {
        case runtime(String)
        case model(String)
        case graph(String)
        case snapshot(String)
        case query(String)
    }

    private struct Fixture {
        let root: URL
        let assets: URL
        let package: URL
    }

    private struct LoadedGraph {
        let fixture: Fixture
        let runtime: WESceneRuntimeRef
        let model: WESceneModelRef
        let graph: WESceneGraphRef
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

    private func documents(
        objects: [[String: Any]],
        properties: [String: Any] = [:]
    ) -> [String: Any] {
        [
            "project.json": [
                "file": "scene.json",
                "general": ["properties": properties],
                "title": "Graph fixture",
                "type": "scene",
                "version": 2,
            ],
            "scene.json": [
                "camera": [
                    "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0",
                ],
                "general": [
                    "clearcolor": "0 0 0",
                    "orthogonalprojection": ["height": 100, "width": 100],
                ],
                "objects": objects,
                "version": 1,
            ],
        ]
    }

    private func makeFixture(_ documents: [String: Any]) throws -> Fixture {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let assets = root.appendingPathComponent("assets", isDirectory: true)
        let shaders = assets.appendingPathComponent("shaders", isDirectory: true)
        let package = root.appendingPathComponent("scene.pkg")
        try FileManager.default.createDirectory(
            at: shaders,
            withIntermediateDirectories: true
        )
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

    private func createRuntime(_ fixture: Fixture) throws -> WESceneRuntimeRef {
        var error: WESceneRuntimeErrorRef?
        let runtime = fixture.assets.path.withCString { assetsPath in
            fixture.package.path.withCString { packagePath in
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

    private func createModel(_ runtime: WESceneRuntimeRef) throws -> WESceneModelRef {
        var error: WESceneRuntimeErrorRef?
        let model = "project.json".withCString {
            we_scene_runtime_model_create(runtime, $0, &error)
        }
        guard let model else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.model(message)
        }
        return model
    }

    private func loadGraph(
        objects: [[String: Any]],
        properties: [String: Any] = [:]
    ) throws -> LoadedGraph {
        let fixture = try makeFixture(documents(
            objects: objects,
            properties: properties
        ))
        do {
            let runtime = try createRuntime(fixture)
            do {
                let model = try createModel(runtime)
                var error: WESceneRuntimeErrorRef?
                guard let graph = we_scene_model_graph_create(model, &error) else {
                    let message = errorMessage(error)
                    we_scene_runtime_error_destroy(error)
                    we_scene_model_destroy(model)
                    throw TestFailure.graph(message)
                }
                return LoadedGraph(
                    fixture: fixture,
                    runtime: runtime,
                    model: model,
                    graph: graph
                )
            } catch {
                we_scene_runtime_destroy(runtime)
                throw error
            }
        } catch {
            try? FileManager.default.removeItem(at: fixture.root)
            throw error
        }
    }

    private func createSnapshot(
        _ graph: WESceneGraphRef
    ) throws -> WESceneGraphSnapshotRef {
        var error: WESceneRuntimeErrorRef?
        guard let snapshot = we_scene_graph_snapshot_create(graph, &error) else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.snapshot(message)
        }
        return snapshot
    }

    private func nodeInfos(
        _ snapshot: WESceneGraphSnapshotRef
    ) throws -> [WESceneGraphNodeInfo] {
        var count = 0
        var error: WESceneRuntimeErrorRef?
        guard we_scene_graph_snapshot_node_count(snapshot, &count, &error) == 1 else {
            let message = errorMessage(error)
            we_scene_runtime_error_destroy(error)
            throw TestFailure.query(message)
        }
        return try (0..<count).map { index in
            var info = WESceneGraphNodeInfo()
            guard we_scene_graph_snapshot_node_info(
                snapshot,
                index,
                &info,
                &error
            ) == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return info
        }
    }

    private func order(
        _ snapshot: WESceneGraphSnapshotRef,
        initialization: Bool
    ) throws -> [Int32] {
        let count = try nodeInfos(snapshot).count
        return try (0..<count).map { index in
            var id: Int32 = 0
            var error: WESceneRuntimeErrorRef?
            let result = initialization
                ? we_scene_graph_snapshot_initialization_object_id(
                    snapshot, index, &id, &error
                )
                : we_scene_graph_snapshot_render_object_id(
                    snapshot, index, &id, &error
                )
            guard result == 1 else {
                let message = errorMessage(error)
                we_scene_runtime_error_destroy(error)
                throw TestFailure.query(message)
            }
            return id
        }
    }

    private func assertVector(
        _ vector: WESceneVector3,
        _ expected: (Double, Double, Double),
        accuracy: Double = 1e-6,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        XCTAssertEqual(vector.x, expected.0, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(vector.y, expected.1, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(vector.z, expected.2, accuracy: accuracy, file: file, line: line)
    }

    func testDynamicWrapperWithAnimationMetadataUsesInitialScale() throws {
        let loaded = try loadGraph(objects: [[
            "id": 1,
            "name": "Animated scale",
            "scale": [
                "animation": [
                    "c0": [["frame": 0, "value": 0]],
                    "options": [
                        "fps": 30, "length": 60, "mode": "single",
                        "startpaused": true,
                    ],
                    "relative": true,
                ],
                "value": "0.1 0.2 0.3",
            ],
        ]])
        defer {
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.fixture.root)
        }

        let snapshot = try createSnapshot(loaded.graph)
        defer { we_scene_graph_snapshot_destroy(snapshot) }
        let node = try XCTUnwrap(nodeInfos(snapshot).first)

        XCTAssertEqual(node.scale_source, WE_SCENE_DYNAMIC_VALUE_LITERAL)
        assertVector(node.local_transform.scale, (0.1, 0.2, 0.3))
    }

    func testUserBindingsFollowUpstreamConnectionRules() throws {
        let properties: [String: Any] = [
            "enabled": ["type": "bool", "value": false],
            "mode": [
                "options": [["label": "A", "value": "a"]],
                "type": "combo",
                "value": "a",
            ],
        ]
        let loaded = try loadGraph(
            objects: [
                [
                    "id": 1, "name": "Missing property",
                    "visible": ["user": "missing", "value": true],
                ],
                [
                    "id": 2, "name": "Condition on boolean",
                    "visible": [
                        "user": ["condition": "unused", "name": "enabled"],
                        "value": true,
                    ],
                ],
                [
                    "id": 3, "name": "Unknown combo option",
                    "visible": [
                        "user": ["condition": "missing", "name": "mode"],
                        "value": true,
                    ],
                ],
            ],
            properties: properties
        )
        defer {
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.fixture.root)
        }

        let snapshot = try createSnapshot(loaded.graph)
        defer { we_scene_graph_snapshot_destroy(snapshot) }
        let nodes = try nodeInfos(snapshot)

        XCTAssertEqual(nodes[0].visible_source, WE_SCENE_DYNAMIC_VALUE_LITERAL)
        XCTAssertEqual(nodes[0].visible, 1)
        XCTAssertEqual(nodes[1].visible_source, WE_SCENE_DYNAMIC_VALUE_USER)
        XCTAssertEqual(nodes[1].visible, 0)
        XCTAssertEqual(nodes[2].visible_source, WE_SCENE_DYNAMIC_VALUE_USER)
        XCTAssertEqual(nodes[2].visible, 0)
    }

    func testDeterministicOrdersWorldTransformsAndDynamicSources() throws {
        let properties: [String: Any] = [
            "mode": [
                "options": [
                    ["label": "A", "value": "a"],
                    ["label": "B", "value": "b"],
                ],
                "text": "Mode", "type": "combo", "value": "a",
            ],
        ]
        let objects: [[String: Any]] = [
            [
                "id": 40,
                "name": "Script initial",
                "origin": [
                    "script": "export function update() { return '9 9 9'; }",
                    "value": "1 1 1",
                ],
            ],
            [
                "id": 50, "name": "Parent only", "origin": "2 0 0",
                "parent": 10,
            ],
            [
                "dependencies": [
                    ["id": 20, "index": 0, "type": "collisionmodel"],
                    30,
                ],
                "id": 30,
                "name": "Leaf",
                "origin": "1 2 3",
                "parent": 20,
                "scale": "3 0.5 2",
                "visible": [
                    "user": ["condition": "a", "name": "mode"],
                    "value": true,
                ],
            ],
            [
                "dependencies": [10],
                "id": 20,
                "name": "Parent",
                "origin": "1 0 2",
                "parent": 10,
                "scale": "0.5 2 1",
            ],
            [
                "angles": "0 0 1.5707963267948966",
                "id": 10,
                "name": "Root",
                "origin": "10 20 3",
                "scale": "2 3 4",
            ],
        ]

        let loaded = try loadGraph(objects: objects, properties: properties)
        var ownerHandlesDestroyed = false
        defer {
            if !ownerHandlesDestroyed {
                we_scene_model_destroy(loaded.model)
                we_scene_runtime_destroy(loaded.runtime)
            }
            we_scene_graph_destroy(loaded.graph)
            try? FileManager.default.removeItem(at: loaded.fixture.root)
        }

        let first = try createSnapshot(loaded.graph)
        defer { we_scene_graph_snapshot_destroy(first) }
        XCTAssertEqual(
            try order(first, initialization: true),
            [40, 10, 50, 20, 30]
        )
        XCTAssertEqual(
            try order(first, initialization: false),
            [40, 50, 10, 20, 30]
        )

        let firstNodes = try nodeInfos(first)
        XCTAssertEqual(firstNodes.map(\.id), [40, 50, 30, 20, 10])
        XCTAssertEqual(
            firstNodes[0].origin_source,
            WE_SCENE_DYNAMIC_VALUE_SCRIPT_INITIAL
        )
        assertVector(firstNodes[0].local_transform.origin, (1, 1, 1))
        XCTAssertEqual(firstNodes[2].visible_source, WE_SCENE_DYNAMIC_VALUE_USER)
        XCTAssertEqual(firstNodes[2].visible, 1)
        assertVector(firstNodes[3].world_transform.origin, (10, 22, 11))
        assertVector(firstNodes[3].world_transform.scale, (1, 6, 4))
        assertVector(firstNodes[2].world_transform.origin, (-2, 23, 23))
        assertVector(firstNodes[2].world_transform.scale, (3, 3, 8))

        var dependency = WESceneObjectDependencyInfo()
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_model_object_dependency_info(
                loaded.model, 2, 0, &dependency, &error
            ),
            1,
            errorMessage(error)
        )
        XCTAssertEqual(dependency.id, 20)
        XCTAssertEqual(dependency.has_index, 1)
        XCTAssertEqual(dependency.index, 0)
        XCTAssertEqual(string(dependency.type), "collisionmodel")

        let updateResult = "b".withCString { value in
            var property = WEScenePropertyValue(
                type: WE_SCENE_VALUE_STRING,
                boolean_value: 0,
                integer_value: 0,
                number_value: 0,
                string_value: value,
                component_count: 0,
                vector_value: WESceneVector4()
            )
            return we_scene_model_set_property_value(
                loaded.model, "mode", &property, &error
            )
        }
        XCTAssertEqual(updateResult, 1, errorMessage(error))
        XCTAssertNil(error)

        let second = try createSnapshot(loaded.graph)
        defer { we_scene_graph_snapshot_destroy(second) }
        var firstRevision: UInt64 = .max
        var secondRevision: UInt64 = .max
        XCTAssertEqual(
            we_scene_graph_snapshot_revision(first, &firstRevision, &error), 1
        )
        XCTAssertEqual(
            we_scene_graph_snapshot_revision(second, &secondRevision, &error), 1
        )
        XCTAssertEqual(firstRevision, 0)
        XCTAssertEqual(secondRevision, 1)
        XCTAssertEqual(try nodeInfos(first)[2].visible, 1)
        XCTAssertEqual(try nodeInfos(second)[2].visible, 0)

        // Graph ownership is independent of the bridge handles that created it.
        we_scene_model_destroy(loaded.model)
        we_scene_runtime_destroy(loaded.runtime)
        ownerHandlesDestroyed = true
        let retained = try createSnapshot(loaded.graph)
        XCTAssertEqual(try nodeInfos(retained).count, 5)
        we_scene_graph_snapshot_destroy(retained)
    }

    func testParentVisibilityPropagatesThroughTheCompleteHierarchy() throws {
        let loaded = try loadGraph(
            objects: [
                ["id": 30, "name": "Grandchild", "parent": 20, "visible": true],
                ["id": 40, "name": "Independent", "visible": true],
                ["id": 20, "name": "Child", "parent": 10, "visible": true],
                ["id": 60, "name": "Hidden branch child", "parent": 50, "visible": true],
                [
                    "id": 10,
                    "name": "User controlled root",
                    "visible": ["user": "showGroup", "value": true],
                ],
                ["id": 50, "name": "Locally hidden child", "parent": 10, "visible": false],
            ],
            properties: [
                "showGroup": ["type": "bool", "value": false],
            ]
        )
        defer {
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.fixture.root)
        }

        func visibility(
            _ snapshot: WESceneGraphSnapshotRef
        ) throws -> [Int32: Int32] {
            Dictionary(uniqueKeysWithValues: try nodeInfos(snapshot).map {
                ($0.id, $0.visible)
            })
        }

        let hidden = try createSnapshot(loaded.graph)
        defer { we_scene_graph_snapshot_destroy(hidden) }
        XCTAssertEqual(
            try visibility(hidden),
            [10: 0, 20: 0, 30: 0, 40: 1, 50: 0, 60: 0]
        )

        var property = WEScenePropertyValue(
            type: WE_SCENE_VALUE_BOOLEAN,
            boolean_value: 1,
            integer_value: 0,
            number_value: 0,
            string_value: nil,
            component_count: 0,
            vector_value: WESceneVector4()
        )
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_model_set_property_value(
                loaded.model, "showGroup", &property, &error
            ),
            1,
            errorMessage(error)
        )

        let shown = try createSnapshot(loaded.graph)
        defer { we_scene_graph_snapshot_destroy(shown) }
        XCTAssertEqual(
            try visibility(shown),
            [10: 1, 20: 1, 30: 1, 40: 1, 50: 0, 60: 0]
        )
    }

    func testDependencyAndParentCyclesAreStructuredFailures() throws {
        let cases: [([[String: Any]], String)] = [
            (
                [
                    ["dependencies": [2], "id": 1, "name": "One"],
                    ["dependencies": [1], "id": 2, "name": "Two"],
                ],
                "/objects/1/dependencies/0"
            ),
            (
                [
                    ["id": 1, "name": "One", "parent": 2],
                    ["id": 2, "name": "Two", "parent": 1],
                ],
                "/objects/1/parent"
            ),
            (
                [
                    ["dependencies": [2], "id": 1, "name": "One"],
                    ["id": 2, "name": "Two", "parent": 1],
                ],
                "/objects/1/parent"
            ),
        ]

        for (objects, expectedPointer) in cases {
            let fixture = try makeFixture(documents(objects: objects))
            defer { try? FileManager.default.removeItem(at: fixture.root) }
            let runtime = try createRuntime(fixture)
            defer { we_scene_runtime_destroy(runtime) }
            let model = try createModel(runtime)
            defer { we_scene_model_destroy(model) }

            var error: WESceneRuntimeErrorRef?
            let graph = we_scene_model_graph_create(model, &error)
            XCTAssertNil(graph)
            if let graph { we_scene_graph_destroy(graph) }
            XCTAssertEqual(
                we_scene_runtime_error_code(error),
                WE_SCENE_RUNTIME_ERROR_SCENE_REFERENCE_CYCLE
            )
            XCTAssertEqual(
                string(we_scene_runtime_error_json_pointer(error)),
                expectedPointer
            )
            XCTAssertEqual(we_scene_runtime_error_reference_count(error), 3)
            XCTAssertTrue(errorMessage(error).contains("cycle"))
            we_scene_runtime_error_destroy(error)
        }
    }

    func testTransformUsesLinuxVectorProjectionRules() throws {
        let loaded = try loadGraph(objects: [[
            "id": 1, "name": "Projected transform", "origin": "1 2",
        ]])
        defer {
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.fixture.root)
        }

        let snapshot = try createSnapshot(loaded.graph)
        defer { we_scene_graph_snapshot_destroy(snapshot) }
        let node = try XCTUnwrap(try nodeInfos(snapshot).first)
        assertVector(node.local_transform.origin, (1, 2, 0))
    }

    func testDeepDependencyChainDoesNotUseTheCallStack() throws {
        let objectCount = 20_000
        let objects: [[String: Any]] = (0..<objectCount).map { index in
            var object: [String: Any] = ["id": index, "name": "Node \(index)"]
            if index + 1 < objectCount {
                object["dependencies"] = [index + 1]
            }
            return object
        }
        let loaded = try loadGraph(objects: objects)
        defer {
            we_scene_graph_destroy(loaded.graph)
            we_scene_model_destroy(loaded.model)
            we_scene_runtime_destroy(loaded.runtime)
            try? FileManager.default.removeItem(at: loaded.fixture.root)
        }
        let snapshot = try createSnapshot(loaded.graph)
        defer { we_scene_graph_snapshot_destroy(snapshot) }

        var count = 0
        var first: Int32 = -1
        var last: Int32 = -1
        var error: WESceneRuntimeErrorRef?
        XCTAssertEqual(
            we_scene_graph_snapshot_node_count(snapshot, &count, &error), 1
        )
        XCTAssertEqual(count, objectCount)
        XCTAssertEqual(
            we_scene_graph_snapshot_render_object_id(
                snapshot, 0, &first, &error
            ),
            1
        )
        XCTAssertEqual(
            we_scene_graph_snapshot_render_object_id(
                snapshot, objectCount - 1, &last, &error
            ),
            1
        )
        XCTAssertEqual(first, Int32(objectCount - 1))
        XCTAssertEqual(last, 0)
    }

}
