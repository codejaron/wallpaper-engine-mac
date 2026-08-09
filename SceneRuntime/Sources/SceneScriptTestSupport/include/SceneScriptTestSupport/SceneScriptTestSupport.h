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

typedef struct WESceneScriptTestAudioSpectrumInputs {
    const float* spectrum_16_left;
    const float* spectrum_16_right;
    const float* spectrum_32_left;
    const float* spectrum_32_right;
    const float* spectrum_64_left;
    const float* spectrum_64_right;
} WESceneScriptTestAudioSpectrumInputs;

typedef enum WESceneScriptTestLayerType {
    WE_SCENE_SCRIPT_TEST_LAYER_IMAGE = 1,
    WE_SCENE_SCRIPT_TEST_LAYER_TEXT = 2,
    WE_SCENE_SCRIPT_TEST_LAYER_PARTICLE = 3,
    WE_SCENE_SCRIPT_TEST_LAYER_SOUND = 4,
    WE_SCENE_SCRIPT_TEST_LAYER_GROUP = 5,
} WESceneScriptTestLayerType;

typedef enum WESceneScriptTestSoundRuntimeState {
    WE_SCENE_SCRIPT_TEST_SOUND_STOPPED = 0,
    WE_SCENE_SCRIPT_TEST_SOUND_PLAYING = 1,
    WE_SCENE_SCRIPT_TEST_SOUND_PAUSED = 2,
    WE_SCENE_SCRIPT_TEST_SOUND_ENDED = 3,
} WESceneScriptTestSoundRuntimeState;

typedef struct WESceneScriptTestLayer {
    int32_t id;
    const char* name;
    WESceneScriptTestLayerType type;
    const char* properties_json;
    // Optional test-only metadata: {"asset":"materials/example.tex",
    // "frames":[0.1,0.2,...]}. A null pointer means the layer has no
    // supplied animation descriptor.
    const char* texture_animation_json;
    int32_t parent_id;
    int has_parent;
} WESceneScriptTestLayer;

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
WESceneScriptTestInstanceRef we_scene_script_test_create_with_layers(
    const char* module_source,
    const char* initial_value_json,
    const char* script_properties_json,
    const WESceneScriptTestLayer* layers,
    size_t layer_count,
    int32_t owner_layer_id,
    const char* owner_property,
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

// Variant used by the SceneScript contract tests to provide the host's local
// day fraction without changing the production C bridge ABI.
int we_scene_script_test_evaluate_with_time_of_day(
    WESceneScriptTestInstanceRef instance,
    double runtime_seconds,
    double frame_time_seconds,
    double time_of_day,
    double pointer_x,
    double pointer_y,
    char* output_json,
    size_t output_json_size,
    WESceneScriptTestErrorRef* out_error
);

int we_scene_script_test_evaluate_with_audio_spectrum(
    WESceneScriptTestInstanceRef instance,
    double runtime_seconds,
    double frame_time_seconds,
    const WESceneScriptTestAudioSpectrumInputs* audio_spectrum,
    double pointer_x,
    double pointer_y,
    char* output_json,
    size_t output_json_size,
    WESceneScriptTestErrorRef* out_error
);

// Test-only JSON adapter for cursor/media host inputs.  `cursor_events_json`
// is an array (or null for no cursor events); `media_snapshot_json` is either
// null or one snapshot object.  The production C++ contract remains typed in
// SceneScript.hpp, while this bridge keeps Swift tests independent of that
// evolving host ABI.
int we_scene_script_test_evaluate_with_events_json(
    WESceneScriptTestInstanceRef instance,
    double runtime_seconds,
    double frame_time_seconds,
    const char* cursor_events_json,
    const char* media_snapshot_json,
    double pointer_x,
    double pointer_y,
    char* output_json,
    size_t output_json_size,
    WESceneScriptTestErrorRef* out_error
);

// Test-only adapter for the Linux `thisScene` contract. The JSON object must
// contain the typed snapshot fields documented by SceneScript.hpp; unlike the
// production host path this keeps the Swift contract tests independent from
// the evolving C++ scene model ABI.
int we_scene_script_test_evaluate_with_scene_snapshot_json(
    WESceneScriptTestInstanceRef instance,
    double runtime_seconds,
    double frame_time_seconds,
    const char* scene_snapshot_json,
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

// Replaces the complete project-level exposed-property snapshot supplied to
// subsequent evaluations. This is intentionally independent from the local
// DynamicValue script properties updated by
// we_scene_script_test_update_properties().
int we_scene_script_test_set_user_properties(
    WESceneScriptTestInstanceRef instance,
    const char* user_properties_json,
    WESceneScriptTestErrorRef* out_error
);

// Supplies the explicit host mode used by engine.isScreensaver() and
// engine.isWallpaper() in subsequent evaluations.
int we_scene_script_test_set_screensaver_state(
    WESceneScriptTestInstanceRef instance,
    int is_screensaver,
    WESceneScriptTestErrorRef* out_error
);

int we_scene_script_test_set_sound_runtime_state(
    WESceneScriptTestInstanceRef instance,
    int32_t layer_id,
    WESceneScriptTestSoundRuntimeState state,
    double position_seconds,
    WESceneScriptTestErrorRef* out_error
);

int we_scene_script_test_layer_property(
    WESceneScriptTestInstanceRef instance,
    int32_t layer_id,
    const char* property,
    char* output_json,
    size_t output_json_size,
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
