import SceneScriptTestSupport
import XCTest

final class SceneScriptContractTests: XCTestCase {
    private enum TestFailure: Error {
        case create(String)
        case evaluate(String)
    }

    private struct AudioSpectrum {
        var left16 = [Float](repeating: 0, count: 16)
        var right16 = [Float](repeating: 0, count: 16)
        var left32 = [Float](repeating: 0, count: 32)
        var right32 = [Float](repeating: 0, count: 32)
        var left64 = [Float](repeating: 0, count: 64)
        var right64 = [Float](repeating: 0, count: 64)
    }

    private func message(_ error: WESceneScriptTestErrorRef?) -> String {
        error.map { String(cString: we_scene_script_test_error_message($0)) } ?? ""
    }

    private func makeInstance(
        source: String,
        initialJSON: String,
        propertiesJSON: String = "{}"
    ) throws -> WESceneScriptTestInstanceRef {
        var error: WESceneScriptTestErrorRef?
        let instance = source.withCString { sourcePointer in
            initialJSON.withCString { initialPointer in
                propertiesJSON.withCString { propertiesPointer in
                    we_scene_script_test_create(
                        sourcePointer,
                        initialPointer,
                        propertiesPointer,
                        &error
                    )
                }
            }
        }
        guard let instance else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.create(message(error))
        }
        return instance
    }

    private func makeInstanceWithOwnerLayer(
        source: String,
        initialJSON: String,
        ownerLayerID: Int32 = 7,
        ownerLayerType: WESceneScriptTestLayerType = WE_SCENE_SCRIPT_TEST_LAYER_IMAGE,
        ownerProperty: String = "origin",
        ownerPropertiesJSON: String = #"{"origin":{"x":1,"y":2,"z":3},"visible":true}"#,
        textureAnimationJSON: String? = nil
    ) throws -> WESceneScriptTestInstanceRef {
        var error: WESceneScriptTestErrorRef?
        let instance = source.withCString { sourcePointer in
            initialJSON.withCString { initialPointer in
                "{}".withCString { propertiesPointer in
                    "Owner".withCString { ownerName in
                        ownerPropertiesJSON.withCString { ownerProperties in
                            ownerProperty.withCString { ownerPropertyPointer in
                                let create: (UnsafePointer<CChar>?) -> WESceneScriptTestInstanceRef? = {
                                    animationPointer in
                                    var layer = WESceneScriptTestLayer(
                                        id: ownerLayerID,
                                        name: ownerName,
                                        type: ownerLayerType,
                                        properties_json: ownerProperties,
                                        texture_animation_json: animationPointer
                                    )
                                    return withUnsafePointer(to: &layer) {
                                        we_scene_script_test_create_with_layers(
                                            sourcePointer,
                                            initialPointer,
                                            propertiesPointer,
                                            $0,
                                            1,
                                            ownerLayerID,
                                            ownerPropertyPointer,
                                            &error
                                        )
                                    }
                                }
                                return textureAnimationJSON.map {
                                    $0.withCString(create)
                                } ?? create(nil)
                            }
                        }
                    }
                }
            }
        }
        guard let instance else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.create(message(error))
        }
        return instance
    }

    private func makeRuntime() throws -> WESceneScriptTestRuntimeRef {
        var error: WESceneScriptTestErrorRef?
        guard let runtime = we_scene_script_test_runtime_create(&error) else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.create(message(error))
        }
        return runtime
    }

    private func makeInstance(
        runtime: WESceneScriptTestRuntimeRef,
        source: String,
        initialJSON: String,
        propertiesJSON: String = "{}"
    ) throws -> WESceneScriptTestInstanceRef {
        var error: WESceneScriptTestErrorRef?
        let instance = source.withCString { sourcePointer in
            initialJSON.withCString { initialPointer in
                propertiesJSON.withCString { propertiesPointer in
                    we_scene_script_test_runtime_create_instance(
                        runtime,
                        sourcePointer,
                        initialPointer,
                        propertiesPointer,
                        &error
                    )
                }
            }
        }
        guard let instance else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.create(message(error))
        }
        return instance
    }

    private func evaluate(
        _ instance: WESceneScriptTestInstanceRef,
        runtime: Double,
        frameTime: Double,
        pointerX: Double = 0,
        pointerY: Double = 0,
        timeOfDay: Double? = nil
    ) throws -> Any {
        var output = [CChar](repeating: 0, count: 4096)
        var error: WESceneScriptTestErrorRef?
        let succeeded = output.withUnsafeMutableBufferPointer { buffer in
            if let timeOfDay {
                return we_scene_script_test_evaluate_with_time_of_day(
                    instance,
                    runtime,
                    frameTime,
                    timeOfDay,
                    pointerX,
                    pointerY,
                    buffer.baseAddress,
                    buffer.count,
                    &error
                )
            }
            return we_scene_script_test_evaluate(
                instance, runtime, frameTime, pointerX, pointerY,
                buffer.baseAddress, buffer.count, &error
            )
        }
        guard succeeded == 1 else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.evaluate(message(error))
        }
        return try JSONSerialization.jsonObject(
            with: Data(String(cString: output).utf8),
            options: [.fragmentsAllowed]
        )
    }

    private func evaluateWithEvents(
        _ instance: WESceneScriptTestInstanceRef,
        runtime: Double,
        frameTime: Double,
        cursorEventsJSON: String = "[]",
        mediaSnapshotJSON: String? = nil,
        pointerX: Double = 0,
        pointerY: Double = 0
    ) throws -> Any {
        var output = [CChar](repeating: 0, count: 4096)
        var error: WESceneScriptTestErrorRef?
        let succeeded = output.withUnsafeMutableBufferPointer { outputBuffer in
            cursorEventsJSON.withCString { cursorEventsPointer in
                if let mediaSnapshotJSON {
                    return mediaSnapshotJSON.withCString { mediaPointer in
                        we_scene_script_test_evaluate_with_events_json(
                            instance,
                            runtime,
                            frameTime,
                            cursorEventsPointer,
                            mediaPointer,
                            pointerX,
                            pointerY,
                            outputBuffer.baseAddress,
                            outputBuffer.count,
                            &error
                        )
                    }
                }
                return we_scene_script_test_evaluate_with_events_json(
                    instance,
                    runtime,
                    frameTime,
                    cursorEventsPointer,
                    nil,
                    pointerX,
                    pointerY,
                    outputBuffer.baseAddress,
                    outputBuffer.count,
                    &error
                )
            }
        }
        guard succeeded == 1 else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.evaluate(message(error))
        }
        return try JSONSerialization.jsonObject(
            with: Data(String(cString: output).utf8),
            options: [.fragmentsAllowed]
        )
    }

    private func evaluateWithSceneSnapshot(
        _ instance: WESceneScriptTestInstanceRef,
        runtime: Double,
        frameTime: Double,
        snapshotJSON: String,
        pointerX: Double = 0,
        pointerY: Double = 0
    ) throws -> Any {
        var output = [CChar](repeating: 0, count: 4096)
        var error: WESceneScriptTestErrorRef?
        let succeeded = output.withUnsafeMutableBufferPointer { outputBuffer in
            snapshotJSON.withCString { snapshotPointer in
                we_scene_script_test_evaluate_with_scene_snapshot_json(
                    instance,
                    runtime,
                    frameTime,
                    snapshotPointer,
                    pointerX,
                    pointerY,
                    outputBuffer.baseAddress,
                    outputBuffer.count,
                    &error
                )
            }
        }
        guard succeeded == 1 else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.evaluate(message(error))
        }
        return try JSONSerialization.jsonObject(
            with: Data(String(cString: output).utf8),
            options: [.fragmentsAllowed]
        )
    }

    private func evaluate(
        _ instance: WESceneScriptTestInstanceRef,
        runtime: Double,
        frameTime: Double,
        audio: AudioSpectrum,
        pointerX: Double = 0,
        pointerY: Double = 0
    ) throws -> Any {
        var output = [CChar](repeating: 0, count: 4096)
        var error: WESceneScriptTestErrorRef?
        let succeeded = output.withUnsafeMutableBufferPointer { outputBuffer in
            audio.left16.withUnsafeBufferPointer { left16 in
                audio.right16.withUnsafeBufferPointer { right16 in
                    audio.left32.withUnsafeBufferPointer { left32 in
                        audio.right32.withUnsafeBufferPointer { right32 in
                            audio.left64.withUnsafeBufferPointer { left64 in
                                audio.right64.withUnsafeBufferPointer { right64 in
                                    var inputs = WESceneScriptTestAudioSpectrumInputs(
                                        spectrum_16_left: left16.baseAddress,
                                        spectrum_16_right: right16.baseAddress,
                                        spectrum_32_left: left32.baseAddress,
                                        spectrum_32_right: right32.baseAddress,
                                        spectrum_64_left: left64.baseAddress,
                                        spectrum_64_right: right64.baseAddress
                                    )
                                    return we_scene_script_test_evaluate_with_audio_spectrum(
                                        instance,
                                        runtime,
                                        frameTime,
                                        &inputs,
                                        pointerX,
                                        pointerY,
                                        outputBuffer.baseAddress,
                                        outputBuffer.count,
                                        &error
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
        guard succeeded == 1 else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.evaluate(message(error))
        }
        return try JSONSerialization.jsonObject(
            with: Data(String(cString: output).utf8),
            options: [.fragmentsAllowed]
        )
    }

    private func updateProperties(
        _ instance: WESceneScriptTestInstanceRef,
        json: String
    ) throws {
        var error: WESceneScriptTestErrorRef?
        let succeeded = json.withCString {
            we_scene_script_test_update_properties(instance, $0, &error)
        }
        guard succeeded == 1 else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.evaluate(message(error))
        }
    }

    private func setUserProperties(
        _ instance: WESceneScriptTestInstanceRef,
        json: String
    ) throws {
        var error: WESceneScriptTestErrorRef?
        let succeeded = json.withCString {
            we_scene_script_test_set_user_properties(instance, $0, &error)
        }
        guard succeeded == 1 else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.evaluate(message(error))
        }
    }

    private func setScreensaverState(
        _ instance: WESceneScriptTestInstanceRef,
        _ value: Bool
    ) throws {
        var error: WESceneScriptTestErrorRef?
        guard we_scene_script_test_set_screensaver_state(
            instance, value ? 1 : 0, &error
        ) == 1 else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.evaluate(message(error))
        }
    }

    private func layerProperty(
        _ instance: WESceneScriptTestInstanceRef,
        id: Int32,
        name: String
    ) throws -> Any {
        var output = [CChar](repeating: 0, count: 4096)
        var error: WESceneScriptTestErrorRef?
        let succeeded = name.withCString { property in
            output.withUnsafeMutableBufferPointer { buffer in
                we_scene_script_test_layer_property(
                    instance,
                    id,
                    property,
                    buffer.baseAddress,
                    buffer.count,
                    &error
                )
            }
        }
        guard succeeded == 1 else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.evaluate(message(error))
        }
        return try JSONSerialization.jsonObject(
            with: Data(String(cString: output).utf8),
            options: [.fragmentsAllowed]
        )
    }

    private func evaluationFailure(
        _ instance: WESceneScriptTestInstanceRef
    ) -> (code: WESceneScriptTestErrorCode, message: String) {
        var output = [CChar](repeating: 0, count: 256)
        var error: WESceneScriptTestErrorRef?
        _ = output.withUnsafeMutableBufferPointer {
            we_scene_script_test_evaluate(
                instance, 1, 1.0 / 60.0, 0, 0,
                $0.baseAddress, $0.count, &error
            )
        }
        defer { we_scene_script_test_error_destroy(error) }
        return (we_scene_script_test_error_code(error), message(error))
    }

    private func updatePropertiesFailure(
        _ instance: WESceneScriptTestInstanceRef,
        json: String
    ) -> (code: WESceneScriptTestErrorCode, message: String) {
        var error: WESceneScriptTestErrorRef?
        _ = json.withCString {
            we_scene_script_test_update_properties(instance, $0, &error)
        }
        defer { we_scene_script_test_error_destroy(error) }
        return (we_scene_script_test_error_code(error), message(error))
    }

    private func assertEvaluationError(
        source: String,
        initialJSON: String,
        code: WESceneScriptTestErrorCode,
        messageFragment: String,
        file: StaticString = #filePath,
        line: UInt = #line
    ) throws {
        let instance = try makeInstance(
            source: source,
            initialJSON: initialJSON
        )
        defer { we_scene_script_test_destroy(instance) }

        var output = [CChar](repeating: 0, count: 256)
        var error: WESceneScriptTestErrorRef?
        let succeeded = output.withUnsafeMutableBufferPointer { buffer in
            we_scene_script_test_evaluate(
                instance, 1, 1.0 / 60.0, 0, 0,
                buffer.baseAddress, buffer.count, &error
            )
        }
        defer { we_scene_script_test_error_destroy(error) }

        XCTAssertEqual(succeeded, 0, file: file, line: line)
        XCTAssertEqual(we_scene_script_test_error_code(error), code, file: file, line: line)
        XCTAssertTrue(
            message(error).contains(messageFragment),
            "Expected diagnostic containing '\(messageFragment)', got '\(message(error))'",
            file: file,
            line: line
        )
    }

    func testFirstFrameRunsESModuleInitThenUpdateAndLaterFramesOnlyUpdate() throws {
        let instance = try makeInstance(
            source: """
            let updates = 0;
            export function init(value) {
                value.x = engine.runtime;
                value.y += 1;
            }
            export function update(value) {
                updates += 1;
                value.z = updates;
                return value;
            }
            """,
            initialJSON: #"{"x":0,"y":0,"z":0}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let first = try XCTUnwrap(try evaluate(
            instance, runtime: 3.25, frameTime: 0.016
        ) as? [String: Any])
        XCTAssertEqual(first["x"] as? Double, 3.25)
        XCTAssertEqual(first["y"] as? Double, 1)
        XCTAssertEqual(first["z"] as? Double, 1)

        let second = try XCTUnwrap(try evaluate(
            instance, runtime: 4.0, frameTime: 0.02
        ) as? [String: Any])
        XCTAssertEqual(second["x"] as? Double, 3.25)
        XCTAssertEqual(second["y"] as? Double, 1)
        XCTAssertEqual(second["z"] as? Double, 2)
    }

    func testTopLevelAwaitRemainsSupported() throws {
        let instance = try makeInstance(
            source: """
            let ready = 0;
            await Promise.resolve();
            ready = 4;
            export function update(value) { return ready; }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(try evaluate(instance, runtime: 0, frameTime: 0) as? Int, 4)
    }

    func testEmptyScriptUsesUpstreamNullUpdateResult() throws {
        let instance = try makeInstance(source: "", initialJSON: "1")
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertTrue(
            try evaluate(instance, runtime: 0, frameTime: 0) is NSNull
        )
    }

    func testAsyncUpdateFailsExplicitlyInsteadOfConvertingPromiseToObject() throws {
        let instance = try makeInstance(
            source: """
            export async function update(value) {
                await Promise.resolve();
                return { x: 5 };
            }
            """,
            initialJSON: "null"
        )
        defer { we_scene_script_test_destroy(instance) }

        let failure = evaluationFailure(instance)
        XCTAssertEqual(failure.code, WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE)
        XCTAssertTrue(failure.message.contains("async update is not supported"))
    }

    func testAsyncInitFailsExplicitly() throws {
        let instance = try makeInstance(
            source: """
            export async function init(value) {
                await Promise.resolve();
                value.x = 5;
            }
            export function update(value) { return value; }
            """,
            initialJSON: #"{"x":0,"y":0}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let failure = evaluationFailure(instance)
        XCTAssertEqual(failure.code, WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE)
        XCTAssertTrue(failure.message.contains("async init is not supported"))
    }

    func testUpdateThatSchedulesMicrotaskFailsAndRemainsPoisoned() throws {
        let instance = try makeInstance(
            source: """
            export function update(value) {
                Promise.resolve().then(() => { value.x = 99; });
                return value;
            }
            """,
            initialJSON: #"{"x":0,"y":0}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let first = evaluationFailure(instance)
        let second = evaluationFailure(instance)
        XCTAssertEqual(first.code, WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE)
        XCTAssertTrue(first.message.contains("async update is not supported"))
        XCTAssertEqual(second.code, first.code)
        XCTAssertEqual(second.message, first.message)
    }

    func testResultGetterThatSchedulesMicrotaskFailsSynchronously() throws {
        let instance = try makeInstance(
            source: """
            export function update(value) {
                return {
                    get x() {
                        Promise.resolve().then(() => {});
                        return 1;
                    },
                    y: 2
                };
            }
            """,
            initialJSON: #"{"x":0,"y":0}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let failure = evaluationFailure(instance)
        XCTAssertEqual(failure.code, WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE)
        XCTAssertTrue(failure.message.contains("async update is not supported"))
    }

    func testInfinitePendingJobIsConsumedByOwningInstanceOnSharedRuntime() throws {
        let runtime = try makeRuntime()
        defer { we_scene_script_test_runtime_destroy(runtime) }
        let first = try makeInstance(
            runtime: runtime,
            source: """
            export function update(value) {
                Promise.resolve().then(() => { while (true) {} });
                return value;
            }
            """,
            initialJSON: "1"
        )
        defer { we_scene_script_test_destroy(first) }
        let second = try makeInstance(
            runtime: runtime,
            source: "export function update(value) { return value; }",
            initialJSON: "7"
        )
        defer { we_scene_script_test_destroy(second) }

        let failure = evaluationFailure(first)
        XCTAssertEqual(failure.code, WE_SCENE_SCRIPT_TEST_ERROR_RESOURCE_LIMIT)
        XCTAssertTrue(failure.message.contains("interrupted"))
        XCTAssertEqual(try evaluate(second, runtime: 0, frameTime: 0) as? Int, 7)
    }

    func testEngineRuntimeAndFrameTimeAreReplacedBeforeEveryEvaluation() throws {
        let instance = try makeInstance(
            source: """
            export function update(value) {
                return engine.runtime + engine.frametime;
            }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(try evaluate(instance, runtime: 2, frameTime: 0.25) as? Double, 2.25)
        XCTAssertEqual(try evaluate(instance, runtime: 9, frameTime: 0.5) as? Double, 9.5)
    }

    func testEngineConstantsReadOnlyInputsAndUtilitySurfaceMatchLinuxContract() throws {
        let instance = try makeInstance(
            source: """
            export function update() {
                var runtimeLocked = false;
                var constantLocked = false;
                try { engine.runtime = 99; } catch (_) { runtimeLocked = true; }
                try { engine.AUDIO_RESOLUTION_16 = 99; } catch (_) { constantLocked = true; }
                if (!runtimeLocked || !constantLocked) throw new Error('engine fields must be read-only');
                if (engine.openUserShortcut() !== undefined) throw new Error('shortcut return mismatch');
                if (typeof engine.setTimeout !== 'function' || typeof engine.setInterval !== 'function') {
                    throw new Error('timer API missing');
                }
                return engine.runtime + engine.frametime + engine.time +
                    engine.AUDIO_RESOLUTION_16 + engine.AUDIO_RESOLUTION_32 + engine.AUDIO_RESOLUTION_64;
            }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 2, frameTime: 0.25) as? Double,
            116.25
        )
    }

    func testEngineTimeOfDayIsHostSuppliedReadOnlyAndUpdatedPerFrame() throws {
        let instance = try makeInstance(
            source: """
            export function update() {
                var rejected = false;
                try { engine.timeOfDay = 0.1; } catch (_) { rejected = true; }
                if (!rejected) throw new Error('timeOfDay must be read-only');
                return engine.timeOfDay;
            }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0, timeOfDay: 0.25) as? Double,
            0.25
        )
        XCTAssertEqual(
            try evaluate(instance, runtime: 1, frameTime: 0.1, timeOfDay: 0.75) as? Double,
            0.75
        )
    }

    func testEngineWallpaperModeIsExplicitlySuppliedByHost() throws {
        let unavailable = try makeInstance(
            source: """
            export function update() {
                return `${engine.isScreensaver()},${engine.isWallpaper()}`;
            }
            """,
            initialJSON: #""""#
        )
        defer { we_scene_script_test_destroy(unavailable) }

        XCTAssertThrowsError(try evaluate(unavailable, runtime: 0, frameTime: 0)) { error in
            guard case TestFailure.evaluate(let message) = error else {
                return XCTFail("unexpected error: \(error)")
            }
            XCTAssertTrue(message.contains("isScreensaver"))
        }

        let instance = try makeInstance(
            source: """
            export function update() {
                return `${engine.isScreensaver()},${engine.isWallpaper()}`;
            }
            """,
            initialJSON: #""""#
        )
        defer { we_scene_script_test_destroy(instance) }
        try setScreensaverState(instance, true)
        XCTAssertEqual(
            try evaluate(instance, runtime: 1, frameTime: 0.1) as? String,
            "true,false"
        )
        try setScreensaverState(instance, false)
        XCTAssertEqual(
            try evaluate(instance, runtime: 2, frameTime: 0.1) as? String,
            "false,true"
        )
    }

    func testProjectUserPropertiesAreIndependentAndVisibleBeforeModuleInitAndUpdate() throws {
        let instance = try makeInstance(
            source: """
            export const scriptProperties = createScriptProperties()
                .addSlider({name: 'localAmount', value: 1})
                .finish();
            const moduleSpeed = engine.userProperties.speed;
            let applyCount = 0;
            let initialized = false;
            export function applyUserProperties(changed) {
                if (!initialized) {
                    throw new Error('project properties ran before init');
                }
                ++applyCount;
                if (engine.userProperties === scriptProperties) {
                    throw new Error('project properties aliased local script properties');
                }
                if (Object.keys(changed).sort().join(',') !== 'enabled,speed,tint') {
                    throw new Error('initial project property snapshot mismatch');
                }
                if (changed.speed !== engine.userProperties.speed) {
                    throw new Error('project property object was updated after applyUserProperties');
                }
                if (!(changed.tint instanceof Vec3) || changed.tint.w !== undefined) {
                    throw new Error('project color must be Vec3');
                }
            }
            export function init(value) {
                if (moduleSpeed !== 2 || applyCount !== 0) {
                    throw new Error('project properties were not visible before module/init');
                }
                initialized = true;
                return value;
            }
            export function update() {
                if (!initialized || applyCount !== 1) {
                    throw new Error('unexpected lifecycle or user-property dispatch order');
                }
                const tint = engine.userProperties.tint;
                if (!(tint instanceof Vec3) || tint.w !== undefined) {
                    throw new Error('engine.userProperties color must be Vec3');
                }
                return `${scriptProperties.localAmount}:${engine.userProperties.enabled}:${engine.userProperties.speed}:${tint.x.toFixed(1)}`;
            }
            """,
            initialJSON: #""""#,
            propertiesJSON: #"{"localAmount":9}"#
        )
        defer { we_scene_script_test_destroy(instance) }
        try setUserProperties(
            instance,
            json: #"{"enabled":true,"speed":2,"tint":{"x":0.1,"y":0.2,"z":0.3}}"#
        )

        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? String,
            "9:true:2:0.1"
        )
        try updateProperties(instance, json: #"{"localAmount":4}"#)
        XCTAssertEqual(
            try evaluate(instance, runtime: 1, frameTime: 0.1) as? String,
            "4:true:2:0.1"
        )
    }

    func testInitialUserPropertiesRunAfterInitCanUseInitializedVector() throws {
        let instance = try makeInstance(
            source: """
            let initialOrigin;
            let resolvedOrigin;
            export function init(value) {
                if (!(engine.userProperties.offset instanceof Vec3)) {
                    throw new Error('project properties were unavailable during init');
                }
                initialOrigin = value;
                return value;
            }
            export function applyUserProperties(changed) {
                if (changed.hasOwnProperty('offset')) {
                    resolvedOrigin = initialOrigin.add(changed.offset);
                }
            }
            export function update() {
                return resolvedOrigin;
            }
            """,
            initialJSON: #"{"x":1,"y":2,"z":3}"#
        )
        defer { we_scene_script_test_destroy(instance) }
        try setUserProperties(
            instance,
            json: #"{"offset":{"x":4,"y":5,"z":6}}"#
        )

        let result = try XCTUnwrap(
            try evaluate(instance, runtime: 0, frameTime: 0)
                as? [String: Any]
        )
        XCTAssertEqual(result["x"] as? Double, 5)
        XCTAssertEqual(result["y"] as? Double, 7)
        XCTAssertEqual(result["z"] as? Double, 9)
    }

    func testApplyUserPropertiesNeedsNoDeclarationsAndReceivesSparseChangesBeforeUpdate() throws {
        let instance = try makeInstance(
            source: """
            const events = [];
            let appliedSpeed = -1;
            export function applyUserProperties(changed) {
                events.push(Object.keys(changed).sort().join(','));
                if (changed.hasOwnProperty('speed')) {
                    if (changed.speed !== engine.userProperties.speed) {
                        throw new Error('engine.userProperties is not the current full snapshot');
                    }
                    appliedSpeed = changed.speed;
                }
            }
            export function update() {
                if (appliedSpeed !== engine.userProperties.speed) {
                    throw new Error('applyUserProperties ran after update');
                }
                return events.join('|');
            }
            """,
            initialJSON: #""""#
        )
        defer { we_scene_script_test_destroy(instance) }

        try setUserProperties(instance, json: #"{"enabled":true,"speed":2}"#)
        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? String,
            "enabled,speed"
        )

        try setUserProperties(instance, json: #"{"enabled":true,"speed":3}"#)
        XCTAssertEqual(
            try evaluate(instance, runtime: 1, frameTime: 0.1) as? String,
            "enabled,speed|speed"
        )

        try setUserProperties(instance, json: #"{"enabled":true,"speed":3}"#)
        XCTAssertEqual(
            try evaluate(instance, runtime: 2, frameTime: 0.1) as? String,
            "enabled,speed|speed"
        )
    }

    func testThisObjectIsTheSameLiveLayerViewAsThisLayer() throws {
        let instance = try makeInstanceWithOwnerLayer(
            source: """
            export function update() {
                if (thisObject !== thisLayer) throw new Error('thisObject identity mismatch');
                thisObject.origin = new Vec3(9, 8, 7);
                return undefined;
            }
            """,
            initialJSON: #"{"x":1,"y":2,"z":3}"#,
            ownerProperty: "origin",
            ownerPropertiesJSON: #"{"origin":{"x":1,"y":2,"z":3},"visible":true}"#
        )
        defer { we_scene_script_test_destroy(instance) }
        _ = try evaluate(instance, runtime: 0, frameTime: 0)
        let origin = try layerProperty(instance, id: 7, name: "origin") as? [String: Any]
        XCTAssertEqual(origin?["x"] as? Double, 9)
        XCTAssertEqual(origin?["y"] as? Double, 8)
        XCTAssertEqual(origin?["z"] as? Double, 7)
    }

    func testEngineTimersUseFrameRuntimeFireBeforeUpdateAndCanBeCancelled() throws {
        let instance = try makeInstance(
            source: """
            let events = '';
            const cancelInterval = engine.setInterval(() => { events += 'i'; }, 100);
            if (typeof cancelInterval !== 'function') throw new Error('timer did not return cancellation function');
            engine.setTimeout(() => { events += 't'; }, 50);
            engine.setTimeout(cancelInterval, 250);
            export function update() { return events; }
            """,
            initialJSON: #""""#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(try evaluate(instance, runtime: 0, frameTime: 0) as? String, "")
        XCTAssertEqual(try evaluate(instance, runtime: 0.05, frameTime: 0.05) as? String, "t")
        XCTAssertEqual(try evaluate(instance, runtime: 0.10, frameTime: 0.05) as? String, "ti")
        XCTAssertEqual(try evaluate(instance, runtime: 0.20, frameTime: 0.10) as? String, "tii")
        XCTAssertEqual(try evaluate(instance, runtime: 0.25, frameTime: 0.05) as? String, "tii")
        XCTAssertEqual(try evaluate(instance, runtime: 0.30, frameTime: 0.05) as? String, "tii")
    }

    func testTimerCallbackFailurePoisonsInstanceWithOriginalDiagnostic() throws {
        let instance = try makeInstance(
            source: """
            engine.setTimeout(() => { throw new Error('timer boom'); }, 0);
            export function update(value) { return value; }
            """,
            initialJSON: "1"
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(try evaluate(instance, runtime: 0, frameTime: 0) as? Int, 1)
        let first = evaluationFailure(instance)
        let second = evaluationFailure(instance)
        XCTAssertEqual(first.code, WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION)
        XCTAssertTrue(first.message.contains("timer boom"))
        XCTAssertEqual(second.code, first.code)
        XCTAssertEqual(second.message, first.message)
    }

    func testConsoleLogAndErrorExistAndReturnUndefined() throws {
        let instance = try makeInstance(
            source: """
            export function update(value) {
                if (console.log() !== undefined || console.error() !== undefined) {
                    throw new Error('console return mismatch');
                }
                return value;
            }
            """,
            initialJSON: "7"
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(try evaluate(instance, runtime: 0, frameTime: 0) as? Int, 7)
    }

    func testSharedObjectAndStoredRealmFunctionSurviveAcrossRuntimeContexts() throws {
        let runtime = try makeRuntime()
        defer { we_scene_script_test_runtime_destroy(runtime) }

        func seedSharedObject() throws {
            let owner = try makeInstance(
                runtime: runtime,
                source: """
                const cachedShared = shared;
                shared.counter = (shared.counter || 0) + 1;
                shared.realmFunction = () => `realm:${shared.counter}`;
                export function update() {
                    if (shared !== cachedShared) throw new Error('shared identity changed');
                    return shared.counter;
                }
                """,
                initialJSON: "0"
            )
            defer { we_scene_script_test_destroy(owner) }
            XCTAssertEqual(try evaluate(owner, runtime: 0, frameTime: 0) as? Int, 1)
        }
        try seedSharedObject()

        let consumer = try makeInstance(
            runtime: runtime,
            source: """
            export function update() {
                if (shared.counter !== 1) throw new Error('shared mutation missing');
                return shared.realmFunction();
            }
            """,
            initialJSON: #""""#
        )
        defer { we_scene_script_test_destroy(consumer) }
        XCTAssertEqual(
            try evaluate(consumer, runtime: 1, frameTime: 0.016) as? String,
            "realm:1"
        )
    }

    func testSharedAssignmentReplacesRuntimeWideValueAcrossContexts() throws {
        let runtime = try makeRuntime()
        defer { we_scene_script_test_runtime_destroy(runtime) }

        func replaceSharedValue() throws {
            let owner = try makeInstance(
                runtime: runtime,
                source: """
                shared = { counter: 40 };
                export function update() {
                    return ++shared.counter;
                }
                """,
                initialJSON: "0"
            )
            defer { we_scene_script_test_destroy(owner) }
            XCTAssertEqual(
                try evaluate(owner, runtime: 0, frameTime: 0) as? Int,
                41
            )
        }
        try replaceSharedValue()

        let consumer = try makeInstance(
            runtime: runtime,
            source: """
            export function update() {
                return shared.counter;
            }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(consumer) }
        XCTAssertEqual(
            try evaluate(consumer, runtime: 1, frameTime: 0.016) as? Int,
            41
        )
    }

    func testTimerCancellationRetainsCreatingInstanceAcrossSharedContexts() throws {
        let runtime = try makeRuntime()
        defer { we_scene_script_test_runtime_destroy(runtime) }

        let owner = try makeInstance(
            runtime: runtime,
            source: """
            shared.ownerCancel = engine.setTimeout(() => { shared.ownerFired = true; }, 100);
            export function update() {
                return shared.ownerFired === true ? 'owner-fired' : 'owner-pending';
            }
            """,
            initialJSON: #""""#
        )
        defer { we_scene_script_test_destroy(owner) }

        // The owner timer is created in the first instance's module realm.
        // Its cancellation function is intentionally retained in `shared`.
        XCTAssertEqual(
            try evaluate(owner, runtime: 0, frameTime: 0) as? String,
            "owner-pending"
        )

        let consumer = try makeInstance(
            runtime: runtime,
            source: """
            let cancelled = false;
            engine.setTimeout(() => { shared.consumerFired = true; }, 0.15 * 1000);
            export function update() {
                if (!cancelled) {
                    shared.ownerCancel();
                    // Module bindings are mutable; this local guard prevents
                    // repeated cancellation on later frames.
                    cancelled = true;
                }
                return `${shared.ownerFired === true ? 'owner-fired' : 'owner-pending'}:${shared.consumerFired === true ? 'consumer-fired' : 'consumer-pending'}`;
            }
            """,
            initialJSON: #""""#
        )
        defer { we_scene_script_test_destroy(consumer) }

        // A context-derived cancellation implementation would look for timer
        // id 1 in the consumer and leave the owner's timer alive.  The bound
        // closure must cancel the timer in the creating instance instead.
        XCTAssertEqual(
            try evaluate(consumer, runtime: 0.05, frameTime: 0.05) as? String,
            "owner-pending:consumer-pending"
        )
        XCTAssertEqual(
            try evaluate(owner, runtime: 0.20, frameTime: 0.15) as? String,
            "owner-pending"
        )
        XCTAssertEqual(
            try evaluate(consumer, runtime: 0.20, frameTime: 0) as? String,
            "owner-pending:consumer-fired"
        )
    }

    func testHostVectorConversionSurvivesScriptConstructorReplacement() throws {
        let instance = try makeInstance(
            source: """
            const OriginalVec3 = Vec3;
            let replaced = false;
            export const scriptProperties = createScriptProperties()
                .addColor({ name: 'color', value: new Vec3(0, 0, 0) })
                .finish();
            export function update() {
                if (!replaced) {
                    Vec3 = function ScriptReplacement() {};
                    replaced = true;
                }
                const color = scriptProperties.color;
                if (Object.getPrototypeOf(color) !== OriginalVec3.prototype) {
                    throw new Error('host vector conversion lost its realm prototype');
                }
                return color;
            }
            """,
            initialJSON: #""""#,
            propertiesJSON: #"{"color":{"x":1,"y":2,"z":3}}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        _ = try evaluate(instance, runtime: 0, frameTime: 0)
        try updateProperties(instance, json: #"{"color":{"x":4,"y":5,"z":6}}"#)
        let value = try XCTUnwrap(
            try evaluate(instance, runtime: 1, frameTime: 0.016) as? [String: Any]
        )
        XCTAssertEqual(value["x"] as? Double, 4)
        XCTAssertEqual(value["y"] as? Double, 5)
        XCTAssertEqual(value["z"] as? Double, 6)
    }

    func testThisLayerTextAndCachedThisSceneObjectsStayLiveAcrossFrames() throws {
        let instance = try makeInstance(
            source: """
            const cachedLayer = thisLayer;
            const cachedScene = thisScene;
            const cachedEngine = engine;
            let initCalls = 0;
            export function init() {
                initCalls += 1;
                cachedLayer.text = `init:${cachedScene.runtime}:${cachedScene.frametime}:${cachedScene.fps}:${cachedEngine.time}:${initCalls}`;
            }
            export function update() {
                cachedLayer.text += `|${cachedScene.time}:${cachedScene.currentTime}:${cachedScene.runtime}:${cachedScene.frametime}:${cachedScene.dt}:${cachedScene.fps}:${cachedEngine.time}`;
            }
            """,
            initialJSON: #""seed""#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 2, frameTime: 0.25) as? String,
            "init:2:0.25:4:2:1|2:2:2:0.25:0.25:4:2"
        )
        XCTAssertEqual(
            try evaluate(instance, runtime: 3, frameTime: 0.5) as? String,
            "init:2:0.25:4:2:1|2:2:2:0.25:0.25:4:2|3:3:3:0.5:0.5:2:3"
        )
        XCTAssertEqual(
            try evaluate(instance, runtime: 4, frameTime: 0) as? String,
            "init:2:0.25:4:2:1|2:2:2:0.25:0.25:4:2|3:3:3:0.5:0.5:2:3|4:4:4:0:0:60:4"
        )
    }

    func testThisSceneLinuxAccessorsReadTheHostSnapshotAndRemainReadOnly() throws {
        let instance = try makeInstance(
            source: """
            export function update() {
                const clear = thisScene.clearcolor;
                const ambient = thisScene.ambientcolor;
                const skylight = thisScene.skylightcolor;
                const names = [
                    'bloom', 'bloomstrength', 'bloomthreshold', 'clearenabled',
                    'clearcolor', 'ambientcolor', 'skylightcolor', 'fov', 'nearz',
                    'farz', 'camerafade', 'camerashake', 'camerashakespeed',
                    'camerashakeamplitude', 'camerashakeroughness', 'cameraparallax',
                    'cameraparallaxamount', 'cameraparallaxdelay',
                    'cameraparallaxmouseinfluence'
                ];
                const writesRejected = names.every((name) => {
                    try {
                        thisScene[name] = 99;
                        return false;
                    } catch (error) {
                        return String(error).includes('read-only');
                    }
                });
                return JSON.stringify({
                    bloom: thisScene.bloom,
                    bloomstrength: thisScene.bloomstrength,
                    bloomthreshold: thisScene.bloomthreshold,
                    clearenabled: thisScene.clearenabled,
                    clearcolor: [clear.x, clear.y, clear.z],
                    ambientcolor: [ambient.x, ambient.y, ambient.z],
                    skylightcolor: [skylight.x, skylight.y, skylight.z],
                    fov: thisScene.fov,
                    nearz: thisScene.nearz,
                    farz: thisScene.farz,
                    camerafade: thisScene.camerafade,
                    camerashake: thisScene.camerashake,
                    camerashakespeed: thisScene.camerashakespeed,
                    camerashakeamplitude: thisScene.camerashakeamplitude,
                    camerashakeroughness: thisScene.camerashakeroughness,
                    cameraparallax: thisScene.cameraparallax,
                    cameraparallaxamount: thisScene.cameraparallaxamount,
                    cameraparallaxdelay: thisScene.cameraparallaxdelay,
                    cameraparallaxmouseinfluence: thisScene.cameraparallaxmouseinfluence,
                    colorsAreVec3: clear instanceof Vec3 &&
                        ambient instanceof Vec3 && skylight instanceof Vec3,
                    writesRejected: writesRejected
                });
            }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        let encoded = try XCTUnwrap(
            try evaluateWithSceneSnapshot(
                instance,
                runtime: 1,
                frameTime: 1.0 / 60.0,
                snapshotJSON: #"""
                {
                    "bloom":true,
                    "bloomStrength":7,
                    "bloomThreshold":11,
                    "clearEnabled":false,
                    "clearColor":[0.1,0.2,0.3],
                    "ambientColor":[0.4,0.5,0.6],
                    "skylightColor":[0.8,0.9,1.0],
                    "fieldOfView":55,
                    "nearZ":0.25,
                    "farZ":900,
                    "cameraFade":true,
                    "cameraShake":true,
                    "cameraShakeSpeed":2,
                    "cameraShakeAmplitude":3,
                    "cameraShakeRoughness":4,
                    "cameraParallax":true,
                    "cameraParallaxAmount":5,
                    "cameraParallaxDelay":6,
                    "cameraParallaxMouseInfluence":7
                }
                """#
            ) as? String
        )
        let result = try XCTUnwrap(
            try JSONSerialization.jsonObject(
                with: Data(encoded.utf8),
                options: [.fragmentsAllowed]
            ) as? [String: Any]
        )
        XCTAssertEqual(result["bloom"] as? Bool, true)
        XCTAssertEqual(result["bloomstrength"] as? Int, 7)
        XCTAssertEqual(result["bloomthreshold"] as? Int, 11)
        // Linux's historical clearenabled accessor is wired to bloom, not to
        // the separately supplied clearEnabled snapshot field.
        XCTAssertEqual(result["clearenabled"] as? Bool, true)
        XCTAssertEqual(
            try XCTUnwrap(result["fov"] as? Double),
            55,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            try XCTUnwrap(result["nearz"] as? Double),
            0.25,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            try XCTUnwrap(result["farz"] as? Double),
            900,
            accuracy: 0.000_001
        )
        XCTAssertEqual(result["camerafade"] as? Bool, true)
        XCTAssertEqual(result["camerashake"] as? Bool, true)
        XCTAssertEqual(
            try XCTUnwrap(result["camerashakespeed"] as? Double),
            2,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            try XCTUnwrap(result["camerashakeamplitude"] as? Double),
            3,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            try XCTUnwrap(result["camerashakeroughness"] as? Double),
            4,
            accuracy: 0.000_001
        )
        XCTAssertEqual(result["cameraparallax"] as? Bool, true)
        XCTAssertEqual(
            try XCTUnwrap(result["cameraparallaxamount"] as? Double),
            5,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            try XCTUnwrap(result["cameraparallaxdelay"] as? Double),
            6,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            try XCTUnwrap(result["cameraparallaxmouseinfluence"] as? Double),
            7,
            accuracy: 0.000_001
        )
        XCTAssertEqual(result["colorsAreVec3"] as? Bool, true)
        XCTAssertEqual(result["writesRejected"] as? Bool, true)
        let clear = try XCTUnwrap(result["clearcolor"] as? [Double])
        XCTAssertEqual(clear.count, 3)
        XCTAssertEqual(clear[0], 0.1, accuracy: 0.000_001)
        XCTAssertEqual(clear[1], 0.2, accuracy: 0.000_001)
        XCTAssertEqual(clear[2], 0.3, accuracy: 0.000_001)
        // The Linux adapter intentionally aliases skylightcolor to ambient.
        let ambient = try XCTUnwrap(result["ambientcolor"] as? [Double])
        let skylight = try XCTUnwrap(result["skylightcolor"] as? [Double])
        XCTAssertEqual(ambient.count, 3)
        XCTAssertEqual(skylight.count, 3)
        for (index, expected) in [0.4, 0.5, 0.6].enumerated() {
            XCTAssertEqual(ambient[index], expected, accuracy: 0.000_001)
            XCTAssertEqual(skylight[index], expected, accuracy: 0.000_001)
        }
    }

    func testThisSceneAccessorFailsExplicitlyWithoutHostSnapshot() throws {
        let instance = try makeInstance(
            source: """
            export function update() {
                return thisScene.bloom;
            }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertThrowsError(try evaluate(instance, runtime: 0, frameTime: 0)) { error in
            guard case let TestFailure.evaluate(message) = error else {
                XCTFail("unexpected error: \\(error)")
                return
            }
            XCTAssertTrue(message.contains("thisScene values are unavailable"))
        }
    }

    func testLayerStringReturnOverridesTextMutationLikeLinuxRuntime() throws {
        let instance = try makeInstance(
            source: """
            let first = true;
            export function update() {
                if (first) {
                    first = false;
                    thisLayer.text = "assigned";
                    return "returned";
                }
                thisLayer.text += "!";
            }
            """,
            initialJSON: #""seed""#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? String,
            "returned"
        )
        XCTAssertEqual(
            try evaluate(instance, runtime: 1, frameTime: 0.1) as? String,
            "returned!"
        )
    }

    func testLayerMutationSurvivesNonStringUpdateReturn() throws {
        let instance = try makeInstance(
            source: """
            export function update() {
                thisLayer.text += "!";
                return 99;
            }
            """,
            initialJSON: #""seed""#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? String,
            "seed!"
        )
    }

    func testThisLayerPreservesJavaScriptValueUntilLinuxTextReadback() throws {
        let instance = try makeInstance(
            source: """
            export function update() {
                thisLayer.text = 42;
                thisLayer.text = `${typeof thisLayer.text}:${thisLayer.text}`;
            }
            """,
            initialJSON: #""seed""#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? String,
            "number:42"
        )
    }

    func testUnusedLayerAdapterDoesNotChangeGenericReturnSemantics() throws {
        let instance = try makeInstance(
            source: "export function update(value) { return 7; }",
            initialJSON: #""seed""#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? Int,
            7
        )
    }

    func testGlobalVectorsUseWEConstructorsAndCorrectArithmeticSemantics() throws {
        let instance = try makeInstance(
            source: """
            function close(a, b) { return Math.abs(a - b) < 0.00001; }
            function require(condition, message) { if (!condition) throw new Error(message); }
            export const scriptProperties = createScriptProperties()
                .addColor({ name: 'color', value: new Vec3(0, 0, 0) })
                .finish();
            export function update(value) {
                require(value instanceof Vec3, 'RuntimeValue vector must be a Vec3 instance');
                require(scriptProperties.color instanceof Vec3, 'script property vector must be a Vec3 instance');
                const v2 = new Vec2('3 4');
                require(close(v2.length(), 5) && close(v2.lengthSqr(), 25), 'Vec2 length contract');
                require(v2.perpendicular().equals(new Vec2(4, -3)), 'Vec2 perpendicular contract');
                require(close(v2.angle(), 53.130102354), 'Vec2 angle contract');
                require(new Vec2(1, 0).rotate(90).equals(new Vec2(0, 1)), 'Vec2 rotate contract');

                const v3 = new Vec3(10, 8, 6);
                require(v3.subtract(new Vec3(1, 2, 3)).equals(new Vec3(9, 6, 3)), 'subtract direction');
                require(v3.divide(new Vec3(2, 4, 3)).equals(new Vec3(5, 2, 2)), 'divide direction');
                require(v3.cross(new Vec3(0, 1, 0)).equals(new Vec3(-6, 0, 10)), 'cross direction');
                require(v3.dot(new Vec3(1, 2, 3)) === 44, 'dot scalar contract');
                require(v3.mix(new Vec3(0, 0, 0), 0.25).equals(new Vec3(7.5, 6, 4.5)), 'mix direction');
                require(new Vec3(1, 2, 3).toSpherical() instanceof Vec3, 'Vec3 spherical result type');
                require(new Vec3(1, 2, 3).clamp(1, 2).equals(new Vec3(1, 2, 2)), 'clamp contract');
                require(new Vec3(5.5, -1.5, 2.5).mod(2).equals(new Vec3(1.5, 0.5, 0.5)), 'mod contract');

                const v4 = new Vec4(1, 2, 3);
                require(v4.w === 3 && v4.lengthSqr() === 23, 'Vec4 constructor/lengthSqr contract');
                require(v4.reflect(new Vec4(0, 1, 0, 0)) instanceof Vec4, 'Vec4 result type');
                return new Vec4(v2.x, v3.cross(new Vec3(0, 1, 0)).z, v4.w, scriptProperties.color.z);
            }
            """,
            initialJSON: #"{"x":1,"y":2,"z":3}"#,
            propertiesJSON: #"{"color":{"x":0.1,"y":0.2,"z":0.3}}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let result = try XCTUnwrap(try evaluate(instance, runtime: 0, frameTime: 0) as? [String: Any])
        XCTAssertEqual(result["x"] as? Double, 3)
        XCTAssertEqual(result["y"] as? Double, 10)
        XCTAssertEqual(result["z"] as? Double, 3)
        XCTAssertEqual(try XCTUnwrap(result["w"] as? Double), 0.3, accuracy: 0.000_001)
    }

    func testMutableVectorResultPreservesScriptPropertyMutation() throws {
        let instance = try makeInstance(
            source: """
            export const scriptProperties = createScriptProperties()
                .addColor({ name: 'rate', value: { x: 0, y: 0, z: 0 } })
                .finish();
            export function update(value) {
                value.x += scriptProperties.rate.x * engine.frametime;
                value.y = scriptProperties.rate.y;
                return value;
            }
            """,
            initialJSON: #"{"x":1,"y":2,"z":3}"#,
            propertiesJSON: #"{"rate":{"x":8,"y":7,"z":0}}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let value = try XCTUnwrap(try evaluate(
            instance, runtime: 1, frameTime: 0.25
        ) as? [String: Any])
        XCTAssertEqual(value["x"] as? Double, 3)
        XCTAssertEqual(value["y"] as? Double, 7)
        XCTAssertEqual(value["z"] as? Double, 3)
    }

    func testWallpaperEnginePropertyBuilderUsesAllSuppliedValueKinds() throws {
        let instance = try makeInstance(
            source: """
            export const scriptProperties = createScriptProperties()
                .addSlider({ name: 'slider', value: 999 })
                .addCheckbox({ name: 'checkbox', value: false })
                .addText({ name: 'text', value: 'wrong' })
                .addCombo({ name: 'combo', value: 'wrong' })
                .addColor({ name: 'color', value: { x: 0, y: 0, z: 0 } })
                .finish();
            export function update(value) {
                if (scriptProperties.slider !== 0.25 ||
                    scriptProperties.checkbox !== true ||
                    scriptProperties.text !== 'host' ||
                    scriptProperties.combo !== 'b') {
                    throw new Error('script property value mismatch');
                }
                return scriptProperties.color;
            }
            """,
            initialJSON: "null",
            propertiesJSON: #"{"slider":0.25,"checkbox":true,"text":"host","combo":"b","color":{"x":0.1,"y":0.2,"z":0.3}}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let result = try XCTUnwrap(try evaluate(
            instance, runtime: 0, frameTime: 0
        ) as? [String: Any])
        XCTAssertEqual(
            try XCTUnwrap(result["x"] as? Double), 0.1, accuracy: 0.000_001
        )
        XCTAssertEqual(
            try XCTUnwrap(result["y"] as? Double), 0.2, accuracy: 0.000_001
        )
        XCTAssertEqual(
            try XCTUnwrap(result["z"] as? Double), 0.3, accuracy: 0.000_001
        )
    }

    func testWEColorModuleConvertsHSVToRGB() throws {
        let instance = try makeInstance(
            source: """
            import * as WEColor from 'WEColor';
            export function update(value) {
                return WEColor.hsv2rgb({ x: 120, y: 1, z: 0.75 });
            }
            """,
            initialJSON: #"{"x":0,"y":0,"z":0}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let result = try XCTUnwrap(try evaluate(
            instance, runtime: 0, frameTime: 0
        ) as? [String: Any])
        XCTAssertEqual(try XCTUnwrap(result["x"] as? Double), 0, accuracy: 0.000_001)
        XCTAssertEqual(try XCTUnwrap(result["y"] as? Double), 0.75, accuracy: 0.000_001)
        XCTAssertEqual(try XCTUnwrap(result["z"] as? Double), 0, accuracy: 0.000_001)
    }

    func testWEColorModuleExposesTheCompleteLinuxConversionSurface() throws {
        let instance = try makeInstance(
            source: """
            import * as WEColor from 'WEColor';
            export function update(value) {
                const hsv = WEColor.rgb2hsv(new Vec3(0, 0.75, 0));
                const normalized = WEColor.normalizeColor(new Vec3(255, 128, 0));
                const expanded = WEColor.expandColor(new Vec3(1, 0.5, 0));
                if (!(hsv instanceof Vec3) ||
                    !(normalized instanceof Vec3) ||
                    !(expanded instanceof Vec3)) {
                    throw new Error('WEColor results must preserve Vec3 identity');
                }
                return new Vec4(hsv.x, hsv.y, normalized.y, expanded.y);
            }
            """,
            initialJSON: #"{"x":0,"y":0,"z":0,"w":0}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let result = try XCTUnwrap(try evaluate(
            instance, runtime: 0, frameTime: 0
        ) as? [String: Any])
        XCTAssertEqual(try XCTUnwrap(result["x"] as? Double), 120, accuracy: 0.000_001)
        XCTAssertEqual(try XCTUnwrap(result["y"] as? Double), 1, accuracy: 0.000_001)
        XCTAssertEqual(
            try XCTUnwrap(result["z"] as? Double),
            128.0 / 255.0,
            accuracy: 0.000_001
        )
        XCTAssertEqual(try XCTUnwrap(result["w"] as? Double), 127.5, accuracy: 0.000_001)
    }

    func testWEMathModuleMatchesTheLinuxScalarContract() throws {
        let instance = try makeInstance(
            source: """
            import * as WEMath from 'WEMath';
            export function update(value) {
                return new Vec4(
                    WEMath.smoothStep(0, 1, 0.5),
                    WEMath.mix(10, 20, 0.25),
                    WEMath.deg2rad,
                    WEMath.rad2deg
                );
            }
            """,
            initialJSON: #"{"x":0,"y":0,"z":0,"w":0}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let result = try XCTUnwrap(try evaluate(
            instance, runtime: 0, frameTime: 0
        ) as? [String: Any])
        XCTAssertEqual(try XCTUnwrap(result["x"] as? Double), 0.5, accuracy: 0.000_001)
        XCTAssertEqual(try XCTUnwrap(result["y"] as? Double), 12.5, accuracy: 0.000_001)
        XCTAssertEqual(
            try XCTUnwrap(result["z"] as? Double),
            .pi / 180.0,
            accuracy: 0.000_001
        )
        XCTAssertEqual(
            try XCTUnwrap(result["w"] as? Double),
            180.0 / .pi,
            accuracy: 0.000_001
        )
    }

    func testUnknownScriptModuleFailsExplicitly() throws {
        let source = """
            import * as unsupported from 'filesystem';
            export function update(value) { return value; }
            """
        var error: WESceneScriptTestErrorRef?
        let instance = source.withCString { sourcePointer in
            we_scene_script_test_create(
                sourcePointer, "1", "{}", &error
            )
        }
        defer { we_scene_script_test_error_destroy(error) }
        XCTAssertNil(instance)
        XCTAssertEqual(we_scene_script_test_error_code(error), WE_SCENE_SCRIPT_TEST_ERROR_MODULE)
        XCTAssertTrue(message(error).contains("Unsupported wallpaper script module"))
    }

    func testPropertyBuilderRejectsMissingHostValue() throws {
        let instance = try makeInstance(
            source: """
            export const scriptProperties = createScriptProperties()
                .addSlider({ name: 'required', value: 12 })
                .finish();
            export function update(value) { return value; }
            """,
            initialJSON: "1"
        )
        defer { we_scene_script_test_destroy(instance) }

        var output = [CChar](repeating: 0, count: 256)
        var error: WESceneScriptTestErrorRef?
        let succeeded = output.withUnsafeMutableBufferPointer {
            we_scene_script_test_evaluate(
                instance, 0, 0, 0, 0, $0.baseAddress, $0.count, &error
            )
        }
        defer { we_scene_script_test_error_destroy(error) }
        XCTAssertEqual(succeeded, 0)
        XCTAssertEqual(
            we_scene_script_test_error_code(error),
            WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION
        )
        XCTAssertTrue(message(error).contains("required"))
        XCTAssertTrue(message(error).contains("no supplied value"))
    }

    func testUpdatePropertiesMutatesCachedScriptPropertiesObjectInPlace() throws {
        let instance = try makeInstance(
            source: """
            export const scriptProperties = createScriptProperties()
                .addSlider({ name: 'speed', value: 1 })
                .finish();
            const cached = scriptProperties;
            export function update(value) { return cached.speed; }
            """,
            initialJSON: "0",
            propertiesJSON: #"{"speed":2}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(try evaluate(instance, runtime: 0, frameTime: 0) as? Int, 2)
        try updateProperties(instance, json: #"{"speed":7}"#)
        XCTAssertEqual(try evaluate(instance, runtime: 1, frameTime: 1) as? Int, 7)
    }

    func testUpdatePropertiesRejectsMissingDeclaredValueWithoutMutation() throws {
        let instance = try makeInstance(
            source: """
            export const scriptProperties = createScriptProperties()
                .addSlider({ name: 'speed', value: 1 })
                .finish();
            export function update(value) { return scriptProperties.speed; }
            """,
            initialJSON: "0",
            propertiesJSON: #"{"speed":2}"#
        )
        defer { we_scene_script_test_destroy(instance) }
        _ = try evaluate(instance, runtime: 0, frameTime: 0)

        var error: WESceneScriptTestErrorRef?
        XCTAssertEqual(
            we_scene_script_test_update_properties(instance, "{}", &error),
            0
        )
        defer { we_scene_script_test_error_destroy(error) }
        XCTAssertEqual(
            we_scene_script_test_error_code(error),
            WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE
        )
        XCTAssertTrue(message(error).contains("speed"))
    }

    func testUpdatePropertiesSetterUsesBudgetAndPoisonsInstance() throws {
        let instance = try makeInstance(
            source: """
            export const scriptProperties = createScriptProperties()
                .addSlider({ name: 'speed', value: 1 })
                .finish();
            let armed = false;
            export function update(value) {
                if (!armed) {
                    Object.defineProperty(scriptProperties, 'speed', {
                        configurable: true,
                        set(_) { while (true) {} }
                    });
                    armed = true;
                }
                return value;
            }
            """,
            initialJSON: "1",
            propertiesJSON: #"{"speed":1}"#
        )
        defer { we_scene_script_test_destroy(instance) }
        _ = try evaluate(instance, runtime: 0, frameTime: 0)

        let setterFailure = updatePropertiesFailure(instance, json: #"{"speed":2}"#)
        XCTAssertEqual(setterFailure.code, WE_SCENE_SCRIPT_TEST_ERROR_RESOURCE_LIMIT)
        XCTAssertTrue(setterFailure.message.contains("interrupted"))
        let poisonedFailure = evaluationFailure(instance)
        XCTAssertEqual(poisonedFailure.code, setterFailure.code)
        XCTAssertEqual(poisonedFailure.message, setterFailure.message)
    }

    func testUpdatePropertiesRejectsScheduledMicrotaskAndPoisonsInstance() throws {
        let instance = try makeInstance(
            source: """
            export const scriptProperties = createScriptProperties()
                .addSlider({ name: 'speed', value: 1 })
                .finish();
            let armed = false;
            export function update(value) {
                if (!armed) {
                    Object.defineProperty(scriptProperties, 'speed', {
                        configurable: true,
                        set(_) { Promise.resolve().then(() => {}); }
                    });
                    armed = true;
                }
                return value;
            }
            """,
            initialJSON: "1",
            propertiesJSON: #"{"speed":1}"#
        )
        defer { we_scene_script_test_destroy(instance) }
        _ = try evaluate(instance, runtime: 0, frameTime: 0)

        let propertyFailure = updatePropertiesFailure(instance, json: #"{"speed":2}"#)
        XCTAssertEqual(propertyFailure.code, WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE)
        XCTAssertTrue(propertyFailure.message.contains("async updateProperties is not supported"))
        let poisonedFailure = evaluationFailure(instance)
        XCTAssertEqual(poisonedFailure.code, propertyFailure.code)
        XCTAssertEqual(poisonedFailure.message, propertyFailure.message)
    }

    func testJavaScriptExceptionIsReturnedWithItsOriginalDiagnostic() throws {
        try assertEvaluationError(
            source: """
            export function update(value) {
                throw new Error("contract boom");
            }
            """,
            initialJSON: "1",
            code: WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION,
            messageFragment: "contract boom"
        )
    }

    func testAudioBufferRegistrationFailsExplicitlyWhenCaptureIsUnavailable() throws {
        let instance = try makeInstance(
            source: "export function update(value) { return value; }",
            initialJSON: "1"
        )
        defer { we_scene_script_test_destroy(instance) }

        var error: WESceneScriptTestErrorRef?
        XCTAssertEqual(we_scene_script_test_register_audio_buffers(instance, &error), 0)
        defer { we_scene_script_test_error_destroy(error) }
        XCTAssertEqual(
            we_scene_script_test_error_code(error),
            WE_SCENE_SCRIPT_TEST_ERROR_AUDIO_INPUT_UNAVAILABLE
        )
        XCTAssertTrue(message(error).contains("audioInputUnavailable"))
    }

    func testScriptAudioRequestFailsWithTypedUnavailableError() throws {
        try assertEvaluationError(
            source: """
            export function update(value) {
                engine.registerAudioBuffers(16);
                return value;
            }
            """,
            initialJSON: "1",
            code: WE_SCENE_SCRIPT_TEST_ERROR_AUDIO_INPUT_UNAVAILABLE,
            messageFragment: "audioInputUnavailable"
        )
    }

    func testAudioUnavailableFrameCanRecoverWhenSpectrumArrives() throws {
        let instance = try makeInstance(
            source: """
            export function update() {
                return engine.registerAudioBuffers(16).average[0];
            }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        let firstFailure = evaluationFailure(instance)
        XCTAssertEqual(
            firstFailure.code,
            WE_SCENE_SCRIPT_TEST_ERROR_AUDIO_INPUT_UNAVAILABLE
        )
        XCTAssertTrue(firstFailure.message.contains("audioInputUnavailable"))

        var spectrum = AudioSpectrum()
        spectrum.left16[0] = 0.2
        spectrum.right16[0] = 0.8
        XCTAssertEqual(
            try XCTUnwrap(
                try evaluate(instance, runtime: 2, frameTime: 1.0 / 60.0, audio: spectrum) as? Double
            ),
            0.5,
            accuracy: 0.000_001
        )
    }

    func testTopLevelAudioRegistrationRebuildsRealmBeforeRecoveryTimers() throws {
        let instance = try makeInstance(
            source: """
            engine.setTimeout(() => {
                shared.timerCount = (shared.timerCount || 0) + 1;
            }, 0);
            const audio = engine.registerAudioBuffers(16);
            export function update() {
                return new Vec2(shared.timerCount || 0, audio.average[0]);
            }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        let firstFailure = evaluationFailure(instance)
        XCTAssertEqual(
            firstFailure.code,
            WE_SCENE_SCRIPT_TEST_ERROR_AUDIO_INPUT_UNAVAILABLE
        )

        var spectrum = AudioSpectrum()
        spectrum.left16[0] = 0.2
        spectrum.right16[0] = 0.8
        let recovery = try XCTUnwrap(
            try evaluate(instance, runtime: 2, frameTime: 1.0 / 60.0, audio: spectrum) as? [String: Any]
        )
        XCTAssertEqual(recovery["x"] as? Double, 0)
        XCTAssertEqual(recovery["y"] as? Double ?? .nan, 0.5, accuracy: 0.000_001)
        let next = try XCTUnwrap(
            try evaluate(instance, runtime: 3, frameTime: 1.0 / 60.0, audio: spectrum) as? [String: Any]
        )
        XCTAssertEqual(next["x"] as? Double, 1)
        XCTAssertEqual(next["y"] as? Double ?? .nan, 0.5, accuracy: 0.000_001)
    }

    func testInitAudioRegistrationRebuildsRealmBeforeRecoveryTimers() throws {
        let instance = try makeInstance(
            source: """
            let audio;
            engine.setTimeout(() => {
                shared.timerCount = (shared.timerCount || 0) + 1;
            }, 0);
            export function init() {
                audio = engine.registerAudioBuffers(16);
            }
            export function update() {
                return new Vec2(shared.timerCount || 0, audio.average[0]);
            }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        let firstFailure = evaluationFailure(instance)
        XCTAssertEqual(
            firstFailure.code,
            WE_SCENE_SCRIPT_TEST_ERROR_AUDIO_INPUT_UNAVAILABLE
        )

        var spectrum = AudioSpectrum()
        spectrum.left16[0] = 0.2
        spectrum.right16[0] = 0.8
        let recovery = try XCTUnwrap(
            try evaluate(instance, runtime: 2, frameTime: 1.0 / 60.0, audio: spectrum) as? [String: Any]
        )
        XCTAssertEqual(recovery["x"] as? Double, 0)
        XCTAssertEqual(recovery["y"] as? Double ?? .nan, 0.5, accuracy: 0.000_001)
        let next = try XCTUnwrap(
            try evaluate(instance, runtime: 3, frameTime: 1.0 / 60.0, audio: spectrum) as? [String: Any]
        )
        XCTAssertEqual(next["x"] as? Double, 1)
        XCTAssertEqual(next["y"] as? Double ?? .nan, 0.5, accuracy: 0.000_001)
    }

    func testRegisteredAudioBuffersArePersistentAndUpdatedBeforeTimers() throws {
        let instance = try makeInstance(
            source: """
            const audio16 = engine.registerAudioBuffers(engine.AUDIO_RESOLUTION_16);
            const audio32 = engine.registerAudioBuffers(engine.AUDIO_RESOLUTION_32);
            const audio64 = engine.registerAudioBuffers(engine.AUDIO_RESOLUTION_64);
            const left16 = audio16.left;
            const right16 = audio16.right;
            const average16 = audio16.average;
            engine.setTimeout(() => { shared.timerAverage = average16[1]; }, 0);

            function require(condition, message) {
                if (!condition) throw new Error(message);
            }

            export function update() {
                require(engine.registerAudioBuffers(16) === audio16, 'audio object identity changed');
                require(audio16.left === left16 && audio16.right === right16 &&
                    audio16.average === average16, 'audio array identity changed');
                require(audio16.left.length === 16 && audio32.left.length === 32 &&
                    audio64.left.length === 64, 'audio resolution length mismatch');
                return new Vec4(
                    average16[0],
                    shared.timerAverage === undefined ? -1 : shared.timerAverage,
                    audio32.average[31],
                    audio64.average[63]
                );
            }
            """,
            initialJSON: #"{"x":0,"y":0,"z":0,"w":0}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        var first = AudioSpectrum()
        first.left16[0] = 0.2
        first.right16[0] = 0.6
        first.left16[1] = 0.1
        first.right16[1] = 0.3
        first.left32[31] = 0.4
        first.right32[31] = 0.8
        first.left64[63] = -0.2
        first.right64[63] = 0.6
        let firstValue = try XCTUnwrap(
            try evaluate(
                instance, runtime: 0, frameTime: 0, audio: first
            ) as? [String: Any]
        )
        XCTAssertEqual(try XCTUnwrap(firstValue["x"] as? Double), 0.4, accuracy: 0.000_001)
        XCTAssertEqual(firstValue["y"] as? Double, -1)
        XCTAssertEqual(try XCTUnwrap(firstValue["z"] as? Double), 0.6, accuracy: 0.000_001)
        XCTAssertEqual(try XCTUnwrap(firstValue["w"] as? Double), 0.2, accuracy: 0.000_001)

        var second = AudioSpectrum()
        second.left16[0] = 0.8
        second.right16[0] = 0.4
        second.left16[1] = 0.3
        second.right16[1] = 0.7
        second.left32[31] = 0.25
        second.right32[31] = 0.75
        second.left64[63] = 0.1
        second.right64[63] = 0.9
        let secondValue = try XCTUnwrap(
            try evaluate(
                instance, runtime: 1, frameTime: 0.016, audio: second
            ) as? [String: Any]
        )
        XCTAssertEqual(try XCTUnwrap(secondValue["x"] as? Double), 0.6, accuracy: 0.000_001)
        XCTAssertEqual(try XCTUnwrap(secondValue["y"] as? Double), 0.5, accuracy: 0.000_001)
        XCTAssertEqual(try XCTUnwrap(secondValue["z"] as? Double), 0.5, accuracy: 0.000_001)
        XCTAssertEqual(try XCTUnwrap(secondValue["w"] as? Double), 0.5, accuracy: 0.000_001)
    }

    func testAudioRequestFromMicrotaskOutranksAsyncLifecycleError() throws {
        try assertEvaluationError(
            source: """
            export function update(value) {
                Promise.resolve().then(() => engine.registerAudioBuffers(16));
                return value;
            }
            """,
            initialJSON: "1",
            code: WE_SCENE_SCRIPT_TEST_ERROR_AUDIO_INPUT_UNAVAILABLE,
            messageFragment: "audioInputUnavailable"
        )
    }

    func testUserCannotSpoofAudioCapabilityErrorWithMessageText() throws {
        try assertEvaluationError(
            source: #"export function update(value) { throw new Error("audioInputUnavailable"); }"#,
            initialJSON: "1",
            code: WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION,
            messageFragment: "audioInputUnavailable"
        )
    }

    func testFailedInstanceRemainsPoisonedWithOriginalDiagnostic() throws {
        let instance = try makeInstance(
            source: """
            let calls = 0;
            export function update(value) {
                calls += 1;
                throw new Error("poison call " + calls);
            }
            """,
            initialJSON: "1"
        )
        defer { we_scene_script_test_destroy(instance) }

        func failureMessage() -> String {
            var output = [CChar](repeating: 0, count: 256)
            var error: WESceneScriptTestErrorRef?
            _ = output.withUnsafeMutableBufferPointer {
                we_scene_script_test_evaluate(instance, 1, 0.016, 0, 0, $0.baseAddress, $0.count, &error)
            }
            defer { we_scene_script_test_error_destroy(error) }
            return message(error)
        }

        let first = failureMessage()
        let second = failureMessage()
        XCTAssertTrue(first.contains("poison call 1"))
        XCTAssertEqual(second, first)
        XCTAssertFalse(second.contains("poison call 2"))
    }

    func testExecutionBudgetInterruptsInfiniteLoop() throws {
        try assertEvaluationError(
            source: "export function update(value) { while (true) {} }",
            initialJSON: "1",
            code: WE_SCENE_SCRIPT_TEST_ERROR_RESOURCE_LIMIT,
            messageFragment: "interrupted"
        )
    }

    func testExecutionBudgetInterruptsInfiniteLoopDuringModuleEvaluation() throws {
        try assertEvaluationError(
            source: "while (true) {} export function update(value) { return value; }",
            initialJSON: "1",
            code: WE_SCENE_SCRIPT_TEST_ERROR_RESOURCE_LIMIT,
            messageFragment: "interrupted"
        )
    }

    func testExecutionBudgetInterruptsInfiniteLoopDuringModuleJob() throws {
        try assertEvaluationError(
            source: "await Promise.resolve(); while (true) {} export function update(value) { return value; }",
            initialJSON: "1",
            code: WE_SCENE_SCRIPT_TEST_ERROR_RESOURCE_LIMIT,
            messageFragment: "interrupted"
        )
    }

    func testExecutionBudgetInterruptsInfiniteLoopDuringInit() throws {
        try assertEvaluationError(
            source: "export function init(value) { while (true) {} } export function update(value) { return value; }",
            initialJSON: "1",
            code: WE_SCENE_SCRIPT_TEST_ERROR_RESOURCE_LIMIT,
            messageFragment: "interrupted"
        )
    }

    func testCreationAndCompilationDoNotConsumeExecutionBudget() throws {
        let declarations = (0..<4_096)
            .map { "const compileOnly_\($0) = \($0);" }
            .joined(separator: "\n")
        let source = declarations + "\nexport function update(value) { return value; }"
        var error: WESceneScriptTestErrorRef?
        let instance = source.withCString { sourcePointer in
            "1".withCString { initialPointer in
                "{}".withCString { propertiesPointer in
                    we_scene_script_test_create_with_execution_budget_nanoseconds(
                        sourcePointer,
                        initialPointer,
                        propertiesPointer,
                        1,
                        &error
                    )
                }
            }
        }
        defer { we_scene_script_test_error_destroy(error) }
        guard let instance else {
            XCTFail("Creation must not consume the execution budget: \(message(error))")
            return
        }
        we_scene_script_test_destroy(instance)
    }

    func testNonFiniteNumberDoesNotBecomeNullOrReuseThePreviousValue() throws {
        try assertEvaluationError(
            source: "export function update(value) { return 0 / 0; }",
            initialJSON: "4",
            code: WE_SCENE_SCRIPT_TEST_ERROR_NONFINITE_RESULT,
            messageFragment: "non-finite"
        )
    }

    func testScriptResultMayChangeItsUnderlyingDynamicValueType() throws {
        let instance = try makeInstance(
            source: #"export function update(value) { return "not a number"; }"#,
            initialJSON: "4"
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? String,
            "not a number"
        )
    }

    func testNumericLookingTextRemainsAString() throws {
        let instance = try makeInstance(
            source: #"export function update(value) { return "1 2"; }"#,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }
        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? String,
            "1 2"
        )
    }

    func testLayerRegistryUsesStableRealViewsAndPreservesUndefinedWriteback() throws {
        let source = """
        const cachedOwner = thisScene.getLayer(7);
        export function update(value) {
            const layers = thisScene.enumerateLayers();
            const target = thisScene.getLayer("Target");
            if (cachedOwner !== thisLayer || layers[0] !== cachedOwner ||
                layers[1] !== target || thisScene.getLayer(999) !== undefined) {
                throw new Error("layer registry identity mismatch");
            }
            cachedOwner.origin = new Vec3(cachedOwner.origin.x + 1, 20, 3);
            target.visible = false;
            return undefined;
        }
        """
        var error: WESceneScriptTestErrorRef?
        let instance = source.withCString { sourcePointer in
            #"{"x":1,"y":2,"z":3}"#.withCString { initialPointer in
                "{}".withCString { propertiesPointer in
                    "Owner".withCString { ownerName in
                        #"{"origin":{"x":1,"y":2,"z":3},"visible":true}"#
                            .withCString { ownerProperties in
                                "Target".withCString { targetName in
                                    #"{"origin":{"x":9,"y":8,"z":7},"visible":true}"#
                                        .withCString { targetProperties in
                                            "origin".withCString { ownerProperty in
                                                let layers = [
                                                    WESceneScriptTestLayer(
                                                        id: 7,
                                                        name: ownerName,
                                                        type: WE_SCENE_SCRIPT_TEST_LAYER_IMAGE,
                                                        properties_json: ownerProperties,
                                                        texture_animation_json: nil
                                                    ),
                                                    WESceneScriptTestLayer(
                                                        id: 8,
                                                        name: targetName,
                                                        type: WE_SCENE_SCRIPT_TEST_LAYER_IMAGE,
                                                        properties_json: targetProperties,
                                                        texture_animation_json: nil
                                                    ),
                                                ]
                                                return layers.withUnsafeBufferPointer {
                                                    we_scene_script_test_create_with_layers(
                                                        sourcePointer,
                                                        initialPointer,
                                                        propertiesPointer,
                                                        $0.baseAddress,
                                                        $0.count,
                                                        7,
                                                        ownerProperty,
                                                        &error
                                                    )
                                                }
                                            }
                                        }
                                }
                            }
                    }
                }
            }
        }
        guard let instance else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.create(message(error))
        }
        defer { we_scene_script_test_destroy(instance) }

        let first = try XCTUnwrap(
            try evaluate(instance, runtime: 1, frameTime: 1.0 / 60.0)
                as? [String: Any]
        )
        XCTAssertEqual(first["x"] as? Double, 2)
        XCTAssertEqual(first["y"] as? Double, 20)
        XCTAssertEqual(
            try layerProperty(instance, id: 8, name: "visible") as? Bool,
            false
        )

        let second = try XCTUnwrap(
            try evaluate(instance, runtime: 2, frameTime: 1.0 / 60.0)
                as? [String: Any]
        )
        XCTAssertEqual(second["x"] as? Double, 3)
        XCTAssertEqual(second["y"] as? Double, 20)
    }

    func testImageLayerTextureAnimationExposesMetadataAndStableIdentity() throws {
        let source = """
        const first = thisLayer.getTextureAnimation();
        export function update() {
            const second = thisScene.getLayer(7).getTextureAnimation();
            return `${first === second},${first.frameCount},${first.duration},${first.rate},${first.isPlaying()}`;
        }
        """
        let instance = try makeInstanceWithOwnerLayer(
            source: source,
            initialJSON: #"{"x":1,"y":2,"z":3}"#,
            textureAnimationJSON: #"{"asset":"materials/animated.tex","frames":[0.1,0.2,0.3]}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? String,
            "true,3,0.6000000000000001,1,true"
        )
    }

    func testImageLayerTextureAnimationControlsRealTimelineState() throws {
        let source = """
        let animation;
        let positioned = false;
        export function init() {
            animation = thisLayer.getTextureAnimation();
            animation.rate = 2;
            animation.stop();
            animation.play();
        }
        export function update() {
            if (thisScene.runtime >= 1.2 && !positioned) {
                animation.pause();
                animation.setFrame(2);
                positioned = true;
            }
            if (thisScene.runtime >= 1.4 && thisScene.runtime < 1.6) {
                animation.stop();
            }
            if (thisScene.runtime >= 1.6) {
                animation.join();
            }
            return `${animation.getFrame()},${animation.rate},${animation.isPlaying()}`;
        }
        """
        let instance = try makeInstanceWithOwnerLayer(
            source: source,
            initialJSON: #"{"x":1,"y":2,"z":3}"#,
            textureAnimationJSON: #"{"asset":"materials/animated.tex","frames":[0.1,0.2,0.3]}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertEqual(
            try evaluate(instance, runtime: 1.0, frameTime: 0) as? String,
            "0,2,true"
        )
        XCTAssertEqual(
            try evaluate(instance, runtime: 1.11, frameTime: 0.11) as? String,
            "1,2,true"
        )
        XCTAssertEqual(
            try evaluate(instance, runtime: 1.2, frameTime: 0.09) as? String,
            "2,2,false"
        )
        XCTAssertEqual(
            try evaluate(instance, runtime: 1.5, frameTime: 0.3) as? String,
            "0,2,false"
        )
        XCTAssertEqual(
            try evaluate(instance, runtime: 1.65, frameTime: 0.15) as? String,
            "2,1,true"
        )
    }

    func testStaticImageLayerReturnsUndefinedTextureAnimation() throws {
        let source = """
        export function update() {
            return thisLayer.getTextureAnimation() === undefined;
        }
        """
        let instance = try makeInstanceWithOwnerLayer(
            source: source,
            initialJSON: #"{"x":1,"y":2,"z":3}"#
        )
        defer { we_scene_script_test_destroy(instance) }
        XCTAssertEqual(
            try evaluate(instance, runtime: 0, frameTime: 0) as? Bool,
            true
        )
    }

    func testSoundLayerPlaybackMethodsAndVolumeAreStateful() throws {
        let source = """
        const sound = thisScene.getLayer(7);
        const target = thisScene.getLayer(8);
        export function init() {
            sound.volume = 0.25;
            sound.play();
            target.visible = sound.isPlaying();
        }
        export function update() {
            if (thisScene.runtime >= 1 && thisScene.runtime < 2) sound.pause();
            if (thisScene.runtime >= 2 && thisScene.runtime < 3) sound.play();
            if (thisScene.runtime >= 3) sound.stop();
            const listed = thisScene.enumerateLayers();
            if (listed[0] !== sound || listed[1] !== target) throw new Error("sound layer identity mismatch");
            target.visible = sound.isPlaying();
            return undefined;
        }
        """
        var error: WESceneScriptTestErrorRef?
        let instance = source.withCString { sourcePointer in
            "1".withCString { initialPointer in
                "{}".withCString { propertiesPointer in
                    "Sound".withCString { soundName in
                                        #"{"visible":true,"volume":1,"state":0}"#.withCString { soundProperties in
                            "Target".withCString { targetName in
                                #"{"visible":false}"#.withCString { targetProperties in
                                    "state".withCString { ownerProperty in
                                        var layers = [
                                            WESceneScriptTestLayer(
                                                id: 7,
                                                name: soundName,
                                                type: WE_SCENE_SCRIPT_TEST_LAYER_SOUND,
                                                properties_json: soundProperties,
                                                texture_animation_json: nil
                                            ),
                                            WESceneScriptTestLayer(
                                                id: 8,
                                                name: targetName,
                                                type: WE_SCENE_SCRIPT_TEST_LAYER_IMAGE,
                                                properties_json: targetProperties,
                                                texture_animation_json: nil
                                            ),
                                        ]
                                        return layers.withUnsafeMutableBufferPointer {
                                            we_scene_script_test_create_with_layers(
                                                sourcePointer,
                                                initialPointer,
                                                propertiesPointer,
                                                $0.baseAddress,
                                                $0.count,
                                                7,
                                                ownerProperty,
                                                &error
                                            )
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        guard let instance else {
            defer { we_scene_script_test_error_destroy(error) }
            throw TestFailure.create(message(error))
        }
        defer { we_scene_script_test_destroy(instance) }

        func setSoundState(
            _ state: WESceneScriptTestSoundRuntimeState,
            position: Double
        ) throws {
            var stateError: WESceneScriptTestErrorRef?
            let result = we_scene_script_test_set_sound_runtime_state(
                instance, 7, state, position, &stateError
            )
            defer { we_scene_script_test_error_destroy(stateError) }
            guard result == 1 else {
                throw TestFailure.evaluate(message(stateError))
            }
        }

        try setSoundState(WE_SCENE_SCRIPT_TEST_SOUND_STOPPED, position: 0)
        _ = try evaluate(instance, runtime: 0, frameTime: 0)
        let volume = try layerProperty(instance, id: 7, name: "volume") as? NSNumber
        XCTAssertEqual(volume?.doubleValue, 0.25)
        XCTAssertEqual(try layerProperty(instance, id: 8, name: "visible") as? Bool, false)
        try setSoundState(WE_SCENE_SCRIPT_TEST_SOUND_PLAYING, position: 0.5)
        _ = try evaluate(instance, runtime: 1, frameTime: 1)
        XCTAssertEqual(try layerProperty(instance, id: 8, name: "visible") as? Bool, true)
        try setSoundState(WE_SCENE_SCRIPT_TEST_SOUND_PAUSED, position: 0.5)
        _ = try evaluate(instance, runtime: 2, frameTime: 1)
        XCTAssertEqual(try layerProperty(instance, id: 8, name: "visible") as? Bool, false)
        try setSoundState(WE_SCENE_SCRIPT_TEST_SOUND_PLAYING, position: 0.5)
        _ = try evaluate(instance, runtime: 3, frameTime: 1)
        XCTAssertEqual(try layerProperty(instance, id: 8, name: "visible") as? Bool, true)
    }

    func testImageLayerSolidAndPropertyAliasesShareCanonicalState() throws {
        let source = """
        export function update() {
            thisLayer.opacity = 0.25;
            thisLayer.solid = true;
            return undefined;
        }
        """
        let instance = try makeInstanceWithOwnerLayer(
            source: source,
            initialJSON: "1",
            ownerProperty: "alpha",
            ownerPropertiesJSON: #"{"alpha":1,"solid":false}"#
        )
        defer { we_scene_script_test_destroy(instance) }
        _ = try evaluate(instance, runtime: 0, frameTime: 0)
        let alpha = try layerProperty(instance, id: 7, name: "alpha") as? NSNumber
        XCTAssertEqual(alpha?.doubleValue, 0.25)
        XCTAssertEqual(try layerProperty(instance, id: 7, name: "solid") as? Bool, true)
    }

    func testCursorEventsAreFilteredOrderedAndDeliveredBeforeUpdate() throws {
        let instance = try makeInstanceWithOwnerLayer(
            source: """
            const calls = [];
            function require(condition, message) {
                if (!condition) throw new Error(message);
            }
            export function init(value) {
                calls.push('init');
                return value;
            }
            export function cursorEnter(event) {
                require(event.worldPosition instanceof Vec3, 'world vector type');
                require(event.localPosition instanceof Vec3, 'local vector type');
                require(event.worldPosition.x === 1 && event.worldPosition.y === 2 &&
                    event.worldPosition.z === 3, 'world coordinates');
                require(event.localPosition.x === 4 && event.localPosition.y === 5 &&
                    event.localPosition.z === 6, 'local coordinates');
                require(event.hitBox === 'head', 'hit box');
                calls.push('enter');
            }
            export function cursorLeave(event) {
                require(event.hitBox === undefined, 'missing hit box must be undefined');
                calls.push('leave');
            }
            export function cursorMove() { calls.push('move'); }
            export function cursorDown() { calls.push('down'); }
            export function cursorUp() { calls.push('up'); }
            export function cursorClick() { calls.push('click'); }
            export function update() {
                calls.push('update');
                return calls.join('|');
            }
            """,
            initialJSON: "\"\"",
            ownerProperty: "colorBlendMode",
            ownerPropertiesJSON: #"{"colorBlendMode":""}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let events = #"""
        [
          {"type":"enter","layerId":7,"worldX":1,"worldY":2,"worldZ":3,"localX":4,"localY":5,"localZ":6,"hitBox":"head"},
          {"type":"move","layerId":8,"worldX":90,"worldY":90,"worldZ":90,"localX":90,"localY":90,"localZ":90},
          {"type":"leave","layerId":7},
          {"type":"move","layerId":7},
          {"type":"down","layerId":7},
          {"type":"up","layerId":7},
          {"type":"click","layerId":7}
        ]
        """#
        XCTAssertEqual(
            try evaluateWithEvents(
                instance,
                runtime: 1,
                frameTime: 1.0 / 60.0,
                cursorEventsJSON: events
            ) as? String,
            "init|enter|leave|move|down|up|click|update"
        )
    }

    func testCursorEventLayerMutationFlowsThroughUndefinedUpdate() throws {
        let instance = try makeInstanceWithOwnerLayer(
            source: """
            export function cursorClick() {
                thisLayer.origin = new Vec3(9, 8, 7);
            }
            export function update() { return undefined; }
            """,
            initialJSON: #"{"x":1,"y":2,"z":3}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let value = try XCTUnwrap(
            try evaluateWithEvents(
                instance,
                runtime: 1,
                frameTime: 1.0 / 60.0,
                cursorEventsJSON: #"[{"type":"click","layerId":7}]"#
            ) as? [String: Any]
        )
        XCTAssertEqual(value["x"] as? Double, 9)
        XCTAssertEqual(value["y"] as? Double, 8)
        XCTAssertEqual(value["z"] as? Double, 7)
    }

    func testUndefinedUpdateWithoutMutationPreservesOwnerValue() throws {
        let instance = try makeInstanceWithOwnerLayer(
            source: "export function update(value) {}",
            initialJSON: #"{"x":1,"y":2,"z":3}"#,
            ownerProperty: "scale",
            ownerPropertiesJSON: #"{"scale":{"x":1,"y":2,"z":3}}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        let value = try XCTUnwrap(
            try evaluate(instance, runtime: 1, frameTime: 1.0 / 60.0)
                as? [String: Any]
        )
        XCTAssertEqual(value["x"] as? Double, 1)
        XCTAssertEqual(value["y"] as? Double, 2)
        XCTAssertEqual(value["z"] as? Double, 3)
        let stored = try XCTUnwrap(
            try layerProperty(instance, id: 7, name: "scale")
                as? [String: Any]
        )
        XCTAssertEqual(stored["x"] as? Double, 1)
        XCTAssertEqual(stored["y"] as? Double, 2)
        XCTAssertEqual(stored["z"] as? Double, 3)
    }

    func testExplicitNullUpdateStillClearsOwnerValue() throws {
        let instance = try makeInstanceWithOwnerLayer(
            source: "export function update(value) { return null; }",
            initialJSON: "1",
            ownerProperty: "alpha",
            ownerPropertiesJSON: #"{"alpha":1}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        XCTAssertTrue(
            try evaluate(instance, runtime: 1, frameTime: 1.0 / 60.0)
                is NSNull
        )
        XCTAssertTrue(
            try layerProperty(instance, id: 7, name: "alpha") is NSNull
        )
    }

    func testCursorEventCallbacksMustRemainSynchronous() throws {
        let instance = try makeInstanceWithOwnerLayer(
            source: """
            export function cursorClick() { return Promise.resolve(); }
            export function update(value) { return value; }
            """,
            initialJSON: #"{"x":1,"y":2,"z":3}"#
        )
        defer { we_scene_script_test_destroy(instance) }

        var output = [CChar](repeating: 0, count: 256)
        var error: WESceneScriptTestErrorRef?
        let succeeded = #"[{"type":"click","layerId":7}]"#.withCString { events in
            output.withUnsafeMutableBufferPointer {
                we_scene_script_test_evaluate_with_events_json(
                    instance,
                    1,
                    1.0 / 60.0,
                    events,
                    nil,
                    0,
                    0,
                    $0.baseAddress,
                    $0.count,
                    &error
                )
            }
        }
        defer { we_scene_script_test_error_destroy(error) }
        XCTAssertEqual(succeeded, 0)
        XCTAssertEqual(
            we_scene_script_test_error_code(error),
            WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE
        )
        XCTAssertTrue(message(error).contains("async cursorClick"))
    }

    func testMediaSnapshotEventsExposeOfficialFieldsAndDeduplicateIndependentRevisions() throws {
        let instance = try makeInstance(
            source: """
            const calls = [];
            function require(condition, message) {
                if (!condition) throw new Error(message);
            }
            export function mediaStatusChanged(event) {
                calls.push('status:' + event.enabled);
            }
            export function mediaPropertiesChanged(event) {
                require(event.title === 'Song' && event.artist === 'Artist', 'media text');
                require(event.contentType === 'music' && event.albumTitle === '', 'optional media text');
                require(event.subTitle === '' && event.albumArtist === '' && event.genres === '', 'empty optional fields');
                calls.push('properties');
            }
            export function mediaPlaybackChanged(event) {
                require(event.state === MediaPlaybackEvent.PLAYBACK_PLAYING, 'playback constant');
                calls.push('playback');
            }
            export function mediaTimelineChanged(event) {
                require((event.position === 1.25 || event.position === 2.5)
                    && event.duration === 4.5, 'timeline fields');
                calls.push('timeline:' + event.position);
            }
            export function mediaThumbnailChanged(event) {
                require(event.hasThumbnail === true, 'thumbnail flag');
                require(event.primaryColor instanceof Vec3 && event.secondaryColor instanceof Vec3 &&
                    event.tertiaryColor instanceof Vec3 && event.textColor instanceof Vec3 &&
                    event.highContrastColor instanceof Vec3, 'thumbnail color types');
                require(event.textColor.x === 0.4 && event.highContrastColor.x === 1, 'thumbnail colors');
                calls.push('thumbnail');
            }
            export function update() { return calls.join('|'); }
            """,
            initialJSON: "0"
        )
        defer { we_scene_script_test_destroy(instance) }

        let first = #"""
        {"statusRevision":1,"metadataRevision":1,"playbackRevision":1,"timelineRevision":1,"thumbnailRevision":1,"available":true,"playbackState":1,"title":"Song","artist":"Artist","contentType":"music","albumTitle":"","subTitle":"","albumArtist":"","genres":"","position":1.25,"duration":4.5,"hasThumbnail":true,"primaryColor":{"x":0.1,"y":0.2,"z":0.3},"secondaryColor":{"x":0.2,"y":0.3,"z":0.4},"tertiaryColor":{"x":0.3,"y":0.4,"z":0.5},"textColor":{"x":0.4,"y":0.5,"z":0.6},"highContrastColor":{"x":1,"y":1,"z":1}}
        """#
        XCTAssertEqual(
            try evaluateWithEvents(instance, runtime: 1, frameTime: 1.0 / 60.0, mediaSnapshotJSON: first) as? String,
            "status:true|properties|playback|timeline:1.25|thumbnail"
        )

        let sameRevision = first.replacingOccurrences(of: "Song", with: "Different")
        XCTAssertEqual(
            try evaluateWithEvents(instance, runtime: 2, frameTime: 1.0 / 60.0, mediaSnapshotJSON: sameRevision) as? String,
            "status:true|properties|playback|timeline:1.25|thumbnail"
        )

        let timelineOnly = first
            .replacingOccurrences(of: #""timelineRevision":1"#, with: #""timelineRevision":2"#)
            .replacingOccurrences(of: #""position":1.25"#, with: #""position":2.5"#)
        XCTAssertEqual(
            try evaluateWithEvents(instance, runtime: 2.5, frameTime: 1.0 / 60.0, mediaSnapshotJSON: timelineOnly) as? String,
            "status:true|properties|playback|timeline:1.25|thumbnail|timeline:2.5",
            "A timeline tick must not retrigger metadata, playback, or thumbnail callbacks"
        )

        let unavailable = #"{"statusRevision":2,"metadataRevision":1,"playbackRevision":1,"timelineRevision":2,"thumbnailRevision":1,"available":false}"#
        XCTAssertEqual(
            try evaluateWithEvents(instance, runtime: 3, frameTime: 1.0 / 60.0, mediaSnapshotJSON: unavailable) as? String,
            "status:true|properties|playback|timeline:1.25|thumbnail|timeline:2.5|status:false"
        )
        XCTAssertEqual(
            try evaluateWithEvents(instance, runtime: 4, frameTime: 1.0 / 60.0, mediaSnapshotJSON: unavailable) as? String,
            "status:true|properties|playback|timeline:1.25|thumbnail|timeline:2.5|status:false"
        )
    }
}
