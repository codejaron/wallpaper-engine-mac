import SceneScriptTestSupport
import XCTest

final class SceneScriptContractTests: XCTestCase {
    private enum TestFailure: Error {
        case create(String)
        case evaluate(String)
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
        pointerY: Double = 0
    ) throws -> Any {
        var output = [CChar](repeating: 0, count: 4096)
        var error: WESceneScriptTestErrorRef?
        let succeeded = output.withUnsafeMutableBufferPointer { buffer in
            we_scene_script_test_evaluate(
                instance,
                runtime,
                frameTime,
                pointerX,
                pointerY,
                buffer.baseAddress,
                buffer.count,
                &error
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
}
