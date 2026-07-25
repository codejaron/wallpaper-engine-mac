#ifndef WE_SCENE_SCRIPT_TEST_SUPPORT_H
#define WE_SCENE_SCRIPT_TEST_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WESceneScriptTestInstance* WESceneScriptTestInstanceRef;
typedef struct WESceneScriptTestRuntime* WESceneScriptTestRuntimeRef;
typedef struct WESceneScriptTestError* WESceneScriptTestErrorRef;

typedef enum WESceneScriptTestErrorCode {
    WE_SCENE_SCRIPT_TEST_ERROR_NONE = 0,
    WE_SCENE_SCRIPT_TEST_ERROR_MODULE = 1,
    WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION = 2,
    WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE = 3,
    WE_SCENE_SCRIPT_TEST_ERROR_NONFINITE_RESULT = 4,
    WE_SCENE_SCRIPT_TEST_ERROR_AUDIO_INPUT_UNAVAILABLE = 5,
    WE_SCENE_SCRIPT_TEST_ERROR_RESOURCE_LIMIT = 6,
} WESceneScriptTestErrorCode;

// This test-only bridge deliberately exchanges RuntimeValue as JSON. It
// keeps Swift contract tests independent from the production C++ API surface.
WESceneScriptTestInstanceRef we_scene_script_test_create(
    const char* module_source,
    const char* initial_value_json,
    const char* script_properties_json,
    WESceneScriptTestErrorRef* out_error
);
WESceneScriptTestInstanceRef we_scene_script_test_create_with_execution_budget_nanoseconds(
    const char* module_source,
    const char* initial_value_json,
    const char* script_properties_json,
    uint64_t execution_budget_nanoseconds,
    WESceneScriptTestErrorRef* out_error
);

// Test-only shared runtime support. Instances created from the same handle use
// one real production ScriptRuntime, so contract tests can detect pending-job
// leakage between independent script contexts.
WESceneScriptTestRuntimeRef we_scene_script_test_runtime_create(
    WESceneScriptTestErrorRef* out_error
);
void we_scene_script_test_runtime_destroy(WESceneScriptTestRuntimeRef runtime);
WESceneScriptTestInstanceRef we_scene_script_test_runtime_create_instance(
    WESceneScriptTestRuntimeRef runtime,
    const char* module_source,
    const char* initial_value_json,
    const char* script_properties_json,
    WESceneScriptTestErrorRef* out_error
);
void we_scene_script_test_destroy(WESceneScriptTestInstanceRef instance);

// The first evaluation injects this frame's inputs, invokes optional init once,
// then invokes update. Later evaluations invoke update only.
int we_scene_script_test_evaluate(
    WESceneScriptTestInstanceRef instance,
    double runtime_seconds,
    double frame_time_seconds,
    double pointer_x,
    double pointer_y,
    char* output_json,
    size_t output_json_size,
    WESceneScriptTestErrorRef* out_error
);

int we_scene_script_test_update_properties(
    WESceneScriptTestInstanceRef instance,
    const char* script_properties_json,
    WESceneScriptTestErrorRef* out_error
);

// System audio capture is intentionally outside the migration goal. A script
// requesting audio buffers must fail explicitly instead of receiving zeroes.
int we_scene_script_test_register_audio_buffers(
    WESceneScriptTestInstanceRef instance,
    WESceneScriptTestErrorRef* out_error
);

WESceneScriptTestErrorCode we_scene_script_test_error_code(
    WESceneScriptTestErrorRef error
);
const char* we_scene_script_test_error_message(WESceneScriptTestErrorRef error);
void we_scene_script_test_error_destroy(WESceneScriptTestErrorRef error);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif
