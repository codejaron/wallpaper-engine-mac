#ifndef WE_SCENE_MODEL_TEST_SUPPORT_H
#define WE_SCENE_MODEL_TEST_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WESceneModelTestHandle* WESceneModelTestHandleRef;

/* A stable, typed snapshot of the parsed model. Counts are uint64_t so the
 * ABI has the same width on Swift, arm64 and x86_64 clients. */
typedef struct WESceneModelTestStats {
    uint64_t object_count;
    uint64_t image_object_count;
    uint64_t text_object_count;
    uint64_t sound_object_count;
    uint64_t particle_object_count;
    uint64_t group_object_count;
    uint64_t effect_instance_count;
    uint64_t effect_override_pass_count;
    uint64_t unique_effect_definition_count;
    uint64_t unique_effect_material_count;
    uint64_t unique_object_model_count;
    uint64_t unique_object_material_count;
    uint64_t total_effect_dependency_count;
    uint64_t dynamic_script_count;
    uint64_t dynamic_user_count;
    uint64_t dynamic_condition_count;
    uint64_t dynamic_script_property_count;
} WESceneModelTestStats;

typedef enum WESceneModelTestTextureSet {
    WE_SCENE_MODEL_TEST_TEXTURES = 0,
    WE_SCENE_MODEL_TEST_USER_TEXTURES = 1,
} WESceneModelTestTextureSet;

typedef enum WESceneModelTestTextureKind {
    WE_SCENE_MODEL_TEST_TEXTURE_NULL = 0,
    WE_SCENE_MODEL_TEST_TEXTURE_NAME = 1,
} WESceneModelTestTextureKind;

/* The name pointer is borrowed from the handle and remains valid until the
 * handle is destroyed. For a null sparse slot, kind is NULL and name is NULL. */
typedef struct WESceneModelTestTextureSlot {
    WESceneModelTestTextureKind kind;
    const char* name;
} WESceneModelTestTextureSlot;

typedef struct WESceneModelTestTextInfo {
    int32_t object_id;
    const char* name;
    const char* font;
    const char* initial_text;
    const char* horizontal_alignment;
    const char* vertical_alignment;
    double point_size;
    const char* size;
    const char* padding;
    const char* spacing;
    int text_is_dynamic;
    int origin_is_dynamic;
    int color_is_dynamic;
    int visible_is_dynamic;
    int limit_rows;
    int limit_use_ellipsis;
    int limit_width;
    int32_t max_rows;
    double max_width;
} WESceneModelTestTextInfo;

typedef struct WESceneModelTestParticleInfo {
    int32_t object_id;
    const char* asset_path;
    const char* material_asset_path;
    uint32_t max_count;
    uint32_t flags;
    uint64_t emitter_count;
    uint64_t initializer_count;
    uint64_t operator_count;
    const char* renderer_name;
    const char* renderer_orientation;
} WESceneModelTestParticleInfo;

typedef enum WESceneModelTestParticleInitializerKind {
    WE_SCENE_MODEL_TEST_PARTICLE_LIFETIME_RANDOM = 0,
    WE_SCENE_MODEL_TEST_PARTICLE_SIZE_RANDOM = 1,
    WE_SCENE_MODEL_TEST_PARTICLE_COLOR_RANDOM = 2,
    WE_SCENE_MODEL_TEST_PARTICLE_ALPHA_RANDOM = 3,
    WE_SCENE_MODEL_TEST_PARTICLE_VELOCITY_RANDOM = 4,
    WE_SCENE_MODEL_TEST_PARTICLE_ROTATION_RANDOM = 5,
    WE_SCENE_MODEL_TEST_PARTICLE_ANGULAR_VELOCITY_RANDOM = 6,
    WE_SCENE_MODEL_TEST_PARTICLE_TURBULENT_VELOCITY_RANDOM = 7,
} WESceneModelTestParticleInitializerKind;

typedef struct WESceneModelTestParticleInitializerInfo {
    WESceneModelTestParticleInitializerKind kind;
    int has_id;
    int32_t id;
    int minimum_is_number;
    int maximum_is_number;
    double minimum_number;
    double maximum_number;
    const char* minimum_text;
    const char* maximum_text;
    int has_exponent;
    double exponent;
} WESceneModelTestParticleInitializerInfo;

/* All functions return non-zero on success. On failure they return zero and,
 * when error_buffer_size is non-zero, write a NUL-terminated diagnostic into
 * the caller-provided buffer (truncating only at a UTF-8 byte boundary is not
 * promised). C++ exceptions never cross this ABI. */
WESceneModelTestHandleRef we_scene_model_test_load(
    const char* assets_directory,
    const char* scene_package_path,
    const char* project_path,
    WESceneModelTestStats* out_stats,
    char* error_buffer,
    size_t error_buffer_size
);

void we_scene_model_test_destroy(WESceneModelTestHandleRef handle);

int we_scene_model_test_stats(
    WESceneModelTestHandleRef handle,
    WESceneModelTestStats* out_stats,
    char* error_buffer,
    size_t error_buffer_size
);

/* Reads an image object's instance texture array. */
int we_scene_model_test_object_texture_slot(
    WESceneModelTestHandleRef handle,
    size_t object_index,
    WESceneModelTestTextureSet texture_set,
    size_t slot_index,
    WESceneModelTestTextureSlot* out_slot,
    char* error_buffer,
    size_t error_buffer_size
);

/* Reads a material texture array from an effect definition pass attached to an
 * image object. effect_index is the image effect instance index and
 * pass_index indexes that definition's material passes. */
int we_scene_model_test_effect_texture_slot(
    WESceneModelTestHandleRef handle,
    size_t object_index,
    size_t effect_index,
    size_t pass_index,
    WESceneModelTestTextureSet texture_set,
    size_t slot_index,
    WESceneModelTestTextureSlot* out_slot,
    char* error_buffer,
    size_t error_buffer_size
);

/* Reads a texture array from an effect pass override attached to an image
 * object. */
int we_scene_model_test_effect_override_texture_slot(
    WESceneModelTestHandleRef handle,
    size_t object_index,
    size_t effect_index,
    size_t override_pass_index,
    WESceneModelTestTextureSet texture_set,
    size_t slot_index,
    WESceneModelTestTextureSlot* out_slot,
    char* error_buffer,
    size_t error_buffer_size
);

/* Reads a sound object's source path. The returned pointer is borrowed from the
 * handle and remains valid until it is destroyed. */
int we_scene_model_test_sound_source(
    WESceneModelTestHandleRef handle,
    size_t object_index,
    size_t source_index,
    const char** out_source,
    char* error_buffer,
    size_t error_buffer_size
);

/* Reads the retained authoring contract of a text object. String pointers are
 * borrowed from the handle and remain valid until it is destroyed. Literal
 * fields whose retained value is not the required type fail explicitly. */
int we_scene_model_test_text_info(
    WESceneModelTestHandleRef handle,
    size_t object_index,
    WESceneModelTestTextInfo* out_info,
    char* error_buffer,
    size_t error_buffer_size
);

int we_scene_model_test_particle_info(
    WESceneModelTestHandleRef handle,
    size_t object_index,
    WESceneModelTestParticleInfo* out_info,
    char* error_buffer,
    size_t error_buffer_size
);

int we_scene_model_test_particle_initializer_info(
    WESceneModelTestHandleRef handle,
    size_t object_index,
    size_t initializer_index,
    WESceneModelTestParticleInitializerInfo* out_info,
    char* error_buffer,
    size_t error_buffer_size
);

/* Reads an authoring dependency retained by an effect definition. The returned
 * pointer is borrowed from the handle and remains valid until it is destroyed. */
int we_scene_model_test_effect_dependency(
    WESceneModelTestHandleRef handle,
    size_t object_index,
    size_t effect_index,
    size_t dependency_index,
    const char** out_dependency,
    char* error_buffer,
    size_t error_buffer_size
);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* WE_SCENE_MODEL_TEST_SUPPORT_H */
