#ifndef WE_SCENE_RUNTIME_BRIDGE_H
#define WE_SCENE_RUNTIME_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include <SceneRuntimeBridge/SceneMetalBridge.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WESceneRuntime* WESceneRuntimeRef;
typedef struct WESceneRuntimeError* WESceneRuntimeErrorRef;
typedef struct WESceneRuntimeAsset* WESceneRuntimeAssetRef;
typedef struct WESceneTexture* WESceneTextureRef;
typedef struct WESceneShaderTranslation* WESceneShaderTranslationRef;
typedef struct WESceneModel* WESceneModelRef;
typedef struct WESceneGraph* WESceneGraphRef;
typedef struct WESceneGraphSnapshot* WESceneGraphSnapshotRef;
typedef struct WESceneFrameGraph* WESceneFrameGraphRef;
typedef struct WESceneFramePlan* WESceneFramePlanRef;
typedef struct WESceneFrameExecutor* WESceneFrameExecutorRef;

typedef enum WESceneRuntimeErrorCode {
    WE_SCENE_RUNTIME_ERROR_NONE = 0,
    WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT = 1,
    WE_SCENE_RUNTIME_ERROR_ASSETS_DIRECTORY_NOT_FOUND = 2,
    WE_SCENE_RUNTIME_ERROR_ASSETS_PATH_NOT_DIRECTORY = 3,
    WE_SCENE_RUNTIME_ERROR_ASSETS_LAYOUT_INVALID = 4,
    WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_NOT_FOUND = 5,
    WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_NOT_REGULAR_FILE = 6,
    WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_UNREADABLE = 7,
    WE_SCENE_RUNTIME_ERROR_FILESYSTEM_FAILURE = 8,
    WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE = 9,
    WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_INVALID = 10,
    WE_SCENE_RUNTIME_ERROR_ASSET_NOT_FOUND = 11,
    WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID = 12,
    WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE = 13,
    WE_SCENE_RUNTIME_ERROR_GIF_SCENE_PACKAGE_INVALID = 14,
    WE_SCENE_RUNTIME_ERROR_ASSET_RESOLVER_FAILURE = 15,
    WE_SCENE_RUNTIME_ERROR_SHADER_INPUT_INVALID = 16,
    WE_SCENE_RUNTIME_ERROR_SHADER_PARSE_FAILURE = 17,
    WE_SCENE_RUNTIME_ERROR_SHADER_LINK_FAILURE = 18,
    WE_SCENE_RUNTIME_ERROR_SHADER_TRANSLATION_FAILURE = 19,
    WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_JSON = 20,
    WE_SCENE_RUNTIME_ERROR_SCENE_MISSING_FIELD = 21,
    WE_SCENE_RUNTIME_ERROR_SCENE_TYPE_MISMATCH = 22,
    WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_VALUE = 23,
    WE_SCENE_RUNTIME_ERROR_SCENE_UNSUPPORTED_PROJECT = 24,
    WE_SCENE_RUNTIME_ERROR_SCENE_UNSUPPORTED_OBJECT = 25,
    WE_SCENE_RUNTIME_ERROR_SCENE_DUPLICATE_ID = 26,
    WE_SCENE_RUNTIME_ERROR_SCENE_DANGLING_REFERENCE = 27,
    WE_SCENE_RUNTIME_ERROR_SCENE_REFERENCE_CYCLE = 28,
    WE_SCENE_RUNTIME_ERROR_SCENE_ASSET_FAILURE = 29,
    WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE = 30,
    WE_SCENE_RUNTIME_ERROR_METAL_CONTEXT_CREATION = 31,
    WE_SCENE_RUNTIME_ERROR_METAL_UNSUPPORTED_CONTEXT = 32,
    WE_SCENE_RUNTIME_ERROR_METAL_SHADER_COMPILATION = 33,
    WE_SCENE_RUNTIME_ERROR_METAL_PROGRAM_LINK = 34,
    WE_SCENE_RUNTIME_ERROR_METAL_FRAMEBUFFER_CREATION = 35,
    WE_SCENE_RUNTIME_ERROR_METAL_DRAW = 36,
    WE_SCENE_RUNTIME_ERROR_METAL_READBACK = 37,
    WE_SCENE_RUNTIME_ERROR_METAL_INTERNAL_FAILURE = 38,
    WE_SCENE_RUNTIME_ERROR_METAL_TEXTURE_DECODE = 39,
    WE_SCENE_RUNTIME_ERROR_METAL_TEXTURE_UPLOAD = 40,
    WE_SCENE_RUNTIME_ERROR_METAL_RESOURCE_VALIDATION = 41,
} WESceneRuntimeErrorCode;

typedef struct WESceneFrameInputs {
    // Drawable-normalized host pointer with a bottom-left origin. Drawable
    // rendering maps it through the selected presentation crop before scripts,
    // effects, and parallax consume it.
    double pointer_x;
    double pointer_y;
    double time_seconds;
    double frame_time_seconds;
} WESceneFrameInputs;

// Renderer quality is an explicit frame-graph policy. Values mirror the
// official quality threshold used for volumetric target allocation.
typedef enum WESceneFrameRenderQuality {
    WE_SCENE_FRAME_RENDER_POWER_SAVING = 1,
    WE_SCENE_FRAME_RENDER_BALANCED = 2,
    WE_SCENE_FRAME_RENDER_HIGH = 3,
    WE_SCENE_FRAME_RENDER_ULTRA = 4,
} WESceneFrameRenderQuality;

// Borrowed fixed-size arrays used only for the duration of one render call.
// Every pointer is required when this structure is supplied. Callers that do
// not have a real capture frame must use the legacy render entry points so an
// authored audio dependency remains an explicit unavailable error.
typedef struct WESceneAudioSpectrumInputs {
    const float* spectrum_16_left;
    const float* spectrum_16_right;
    const float* spectrum_32_left;
    const float* spectrum_32_right;
    const float* spectrum_64_left;
    const float* spectrum_64_right;
} WESceneAudioSpectrumInputs;

typedef enum WESceneMediaPlaybackState {
    WE_SCENE_MEDIA_STOPPED = 0,
    WE_SCENE_MEDIA_PLAYING = 1,
    WE_SCENE_MEDIA_PAUSED = 2,
} WESceneMediaPlaybackState;

// Borrowed strings are copied by the setter before it returns. A caller that
// cannot supply a real media source must clear the snapshot rather than
// fabricating empty metadata or a stopped track.
typedef struct WESceneMediaSnapshot {
    uint64_t status_revision;
    uint64_t metadata_revision;
    uint64_t playback_revision;
    uint64_t timeline_revision;
    uint64_t thumbnail_revision;
    int available;
    WESceneMediaPlaybackState playback_state;
    const char* title;
    const char* artist;
    const char* content_type;
    const char* album_title;
    const char* sub_title;
    const char* album_artist;
    const char* genres;
    double position;
    double duration;
    int has_thumbnail;
    double primary_color[3];
    double secondary_color[3];
    double tertiary_color[3];
    double text_color[3];
    double high_contrast_color[3];
} WESceneMediaSnapshot;

// Borrowed RGBA8 rows are copied before the setter returns. bytes_per_row may
// include host padding, but pixel_length must describe exactly height rows.
// The first row is the image's top row, matching decoded album-cover data.
typedef struct WESceneMediaThumbnailRGBA8 {
    uint64_t revision;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    const uint8_t* pixels;
    size_t pixel_length;
} WESceneMediaThumbnailRGBA8;

typedef enum WEScenePresentationScaling {
    WE_SCENE_PRESENTATION_STRETCH = 0,
    WE_SCENE_PRESENTATION_ASPECT_FIT = 1,
    WE_SCENE_PRESENTATION_ASPECT_FILL = 2,
    WE_SCENE_PRESENTATION_AUTOMATIC = 3,
} WEScenePresentationScaling;

// High quality intentionally has no physical target and uses the existing
// author-resolution entry points. Ultra also preserves author resolution but
// carries the official level-4 shader and shadow allocation policy.
typedef enum WEScenePhysicalRenderQuality {
    WE_SCENE_PHYSICAL_RENDER_BALANCED = 0,
    WE_SCENE_PHYSICAL_RENDER_POWER_SAVING = 1,
    WE_SCENE_PHYSICAL_RENDER_ULTRA = 2,
} WEScenePhysicalRenderQuality;

// A backing-pixel target independent from the presentation drawable. In span
// mode callers pass the shared virtual canvas dimensions so every display
// reuses exactly the same physical scene output.
typedef struct WEScenePhysicalRenderTarget {
    uint32_t backing_width;
    uint32_t backing_height;
    WEScenePhysicalRenderQuality quality;
} WEScenePhysicalRenderTarget;

// One display's rectangle inside a bottom-left-origin virtual desktop canvas.
// Drawable dimensions describe the backing surface for that same viewport and
// may differ from its logical size on scaled displays.
typedef struct WEScenePresentationViewport {
    uint32_t virtual_canvas_width;
    uint32_t virtual_canvas_height;
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t drawable_width;
    uint32_t drawable_height;
} WEScenePresentationViewport;

typedef enum WESceneAssetSource {
    WE_SCENE_ASSET_SOURCE_VIRTUAL = 0,
    WE_SCENE_ASSET_SOURCE_PROJECT_DIRECTORY = 1,
    WE_SCENE_ASSET_SOURCE_SCENE_PACKAGE = 2,
    WE_SCENE_ASSET_SOURCE_GIF_SCENE_PACKAGE = 3,
    WE_SCENE_ASSET_SOURCE_OFFICIAL_ASSETS = 4,
    WE_SCENE_ASSET_SOURCE_UNKNOWN = 255,
} WESceneAssetSource;

typedef struct WESceneRuntimeConfiguration {
    const char* assets_directory;
    const char* scene_package_path;
} WESceneRuntimeConfiguration;

typedef struct WESceneLocalStorageConfiguration {
    // Stable identity of the wallpaper across all display instances.
    const char* wallpaper_identity;
    // Stable identity of the display used by LOCATION_SCREEN.
    const char* screen_identity;
} WESceneLocalStorageConfiguration;

typedef struct WEScenePackageEntryInfo {
    const char* path;
    uint32_t offset;
    uint32_t length;
} WEScenePackageEntryInfo;

typedef struct WESceneTextureInfo {
    uint32_t container_version;
    uint32_t animation_version;
    uint32_t format;
    uint32_t file_format;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t gif_width;
    uint32_t gif_height;
    uint32_t image_count;
    uint32_t frame_count;
    uint32_t spritesheet_columns;
    uint32_t spritesheet_rows;
    uint32_t spritesheet_frame_count;
    float spritesheet_duration;
    int is_video_mp4;
    int has_extra_texi_field;
} WESceneTextureInfo;

typedef struct WESceneTextureMipmapInfo {
    uint32_t width;
    uint32_t height;
    uint32_t compression;
    int32_t uncompressed_size;
    int32_t compressed_size;
} WESceneTextureMipmapInfo;

typedef struct WESceneTextureFrameInfo {
    uint32_t frame_number;
    float frame_time;
    float x;
    float y;
    float width;
    float height;
} WESceneTextureFrameInfo;

typedef struct WESceneShaderSources {
    const char* vertex_source;
    const char* fragment_source;
    const char* vertex_name;
    const char* fragment_name;
} WESceneShaderSources;

typedef enum WESceneShaderStage {
    WE_SCENE_SHADER_STAGE_VERTEX = 0,
    WE_SCENE_SHADER_STAGE_FRAGMENT = 1,
} WESceneShaderStage;

typedef enum WESceneShaderParameterDefaultType {
    WE_SCENE_SHADER_PARAMETER_DEFAULT_NONE = 0,
    WE_SCENE_SHADER_PARAMETER_DEFAULT_BOOLEAN = 1,
    WE_SCENE_SHADER_PARAMETER_DEFAULT_INTEGER = 2,
    WE_SCENE_SHADER_PARAMETER_DEFAULT_NUMBER = 3,
    WE_SCENE_SHADER_PARAMETER_DEFAULT_STRING = 4,
    WE_SCENE_SHADER_PARAMETER_DEFAULT_VECTOR = 5,
} WESceneShaderParameterDefaultType;

typedef struct WESceneShaderParameterInfo {
    const char* type;
    const char* name;
    const char* material;
    const char* metadata_json;
    WESceneShaderParameterDefaultType default_type;
    int default_boolean;
    int64_t default_integer;
    double default_number;
    const char* default_string;
    const double* default_vector;
    size_t default_vector_count;
} WESceneShaderParameterInfo;

typedef enum WESceneObjectType {
    WE_SCENE_OBJECT_GROUP = 0,
    WE_SCENE_OBJECT_IMAGE = 1,
    WE_SCENE_OBJECT_TEXT = 2,
    WE_SCENE_OBJECT_SOUND = 3,
} WESceneObjectType;

typedef enum WEScenePropertyType {
    WE_SCENE_PROPERTY_BOOLEAN = 0,
    WE_SCENE_PROPERTY_SLIDER = 1,
    WE_SCENE_PROPERTY_COMBO = 2,
    WE_SCENE_PROPERTY_COLOR = 3,
    WE_SCENE_PROPERTY_TEXT = 4,
    WE_SCENE_PROPERTY_SCENE_TEXTURE = 5,
    WE_SCENE_PROPERTY_FILE = 6,
    WE_SCENE_PROPERTY_DIRECTORY = 7,
    WE_SCENE_PROPERTY_TEXT_INPUT = 8,
    WE_SCENE_PROPERTY_USER_SHORTCUT = 9,
    WE_SCENE_PROPERTY_GROUP = 10,
} WEScenePropertyType;

typedef enum WESceneValueType {
    WE_SCENE_VALUE_NULL = 0,
    WE_SCENE_VALUE_BOOLEAN = 1,
    WE_SCENE_VALUE_INTEGER = 2,
    WE_SCENE_VALUE_NUMBER = 3,
    WE_SCENE_VALUE_STRING = 4,
    WE_SCENE_VALUE_ARRAY = 5,
    WE_SCENE_VALUE_OBJECT = 6,
} WESceneValueType;

typedef enum WESceneAudioInputStatus {
    WE_SCENE_AUDIO_INPUT_NOT_REQUESTED = 0,
    WE_SCENE_AUDIO_INPUT_UNAVAILABLE = 1,
} WESceneAudioInputStatus;

typedef struct WESceneProjectInfo {
    const char* asset_path;
    const char* title;
    const char* workshop_id;
    const char* scene_asset_path;
    int supports_audio_processing;
    WESceneAudioInputStatus audio_input_status;
    int projection_auto;
    int32_t projection_width;
    int32_t projection_height;
} WESceneProjectInfo;

typedef struct WESceneObjectInfo {
    int32_t id;
    const char* name;
    WESceneObjectType type;
    int has_parent;
    int32_t parent_id;
    size_t dependency_count;
    const char* referenced_asset_path;
} WESceneObjectInfo;

typedef struct WESceneObjectDependencyInfo {
    int32_t id;
    int has_index;
    int32_t index;
    const char* type;
} WESceneObjectDependencyInfo;

typedef enum WESceneDynamicValueSource {
    WE_SCENE_DYNAMIC_VALUE_LITERAL = 0,
    WE_SCENE_DYNAMIC_VALUE_USER = 1,
    WE_SCENE_DYNAMIC_VALUE_SCRIPT_INITIAL = 2,
    WE_SCENE_DYNAMIC_VALUE_SCRIPT = 3,
    WE_SCENE_DYNAMIC_VALUE_SCRIPT_UNAVAILABLE = 4,
} WESceneDynamicValueSource;

typedef struct WESceneVector3 {
    double x;
    double y;
    double z;
} WESceneVector3;

typedef struct WESceneVector4 {
    double x;
    double y;
    double z;
    double w;
} WESceneVector4;

typedef struct WESceneMatrix4x4 {
    double values[16];
} WESceneMatrix4x4;

typedef struct WESceneObjectTransform {
    WESceneVector3 origin;
    WESceneVector3 scale;
    WESceneVector3 angles;
} WESceneObjectTransform;

typedef struct WESceneGraphNodeInfo {
    size_t object_index;
    int32_t id;
    int has_parent;
    int32_t parent_id;
    int disable_propagation;
    int visible;
    WESceneDynamicValueSource origin_source;
    WESceneDynamicValueSource scale_source;
    WESceneDynamicValueSource angles_source;
    WESceneDynamicValueSource visible_source;
    WESceneObjectTransform local_transform;
    WESceneObjectTransform world_transform;
} WESceneGraphNodeInfo;

typedef enum WESceneFrameResourceKind {
    WE_SCENE_FRAME_RESOURCE_ASSET_TEXTURE = 0,
    WE_SCENE_FRAME_RESOURCE_FRAMEBUFFER = 1,
    WE_SCENE_FRAME_RESOURCE_USER_PROPERTY_TEXTURE = 2,
    WE_SCENE_FRAME_RESOURCE_HOST_TEXTURE = 3,
} WESceneFrameResourceKind;

typedef enum WESceneFramebufferFormat {
    WE_SCENE_FRAMEBUFFER_RGBA8 = 0,
    WE_SCENE_FRAMEBUFFER_R8 = 1,
    WE_SCENE_FRAMEBUFFER_RG16F = 2,
    WE_SCENE_FRAMEBUFFER_R16F = 3,
} WESceneFramebufferFormat;

typedef enum WESceneFramebufferWrapMode {
    WE_SCENE_FRAMEBUFFER_WRAP_CLAMP_TO_EDGE = 0,
    WE_SCENE_FRAMEBUFFER_WRAP_CLAMP_TO_BORDER = 1,
    WE_SCENE_FRAMEBUFFER_WRAP_REPEAT = 2,
} WESceneFramebufferWrapMode;

typedef enum WESceneFrameOperationKind {
    WE_SCENE_FRAME_OPERATION_RENDER = 0,
    WE_SCENE_FRAME_OPERATION_COPY = 1,
    WE_SCENE_FRAME_OPERATION_SWAP = 2,
    WE_SCENE_FRAME_OPERATION_CLEAR = 3,
    WE_SCENE_FRAME_OPERATION_TEXT = 4,
    WE_SCENE_FRAME_OPERATION_PARTICLE = 5,
} WESceneFrameOperationKind;

typedef enum WESceneFrameGeometryKind {
    WE_SCENE_FRAME_GEOMETRY_IMAGE_LOCAL = 0,
    WE_SCENE_FRAME_GEOMETRY_FULLSCREEN_LOCAL = 1,
    WE_SCENE_FRAME_GEOMETRY_IMAGE_SCENE = 2,
    WE_SCENE_FRAME_GEOMETRY_PASSTHROUGH_CAPTURE = 3,
    WE_SCENE_FRAME_GEOMETRY_PUPPET_MESH = 4,
    WE_SCENE_FRAME_GEOMETRY_LIGHT_VOLUME = 5,
} WESceneFrameGeometryKind;

typedef enum WESceneFrameTexCoordKind {
    WE_SCENE_FRAME_TEXCOORD_IMAGE = 0,
    WE_SCENE_FRAME_TEXCOORD_FULL = 1,
} WESceneFrameTexCoordKind;

typedef enum WESceneFrameBlendingMode {
    WE_SCENE_FRAME_BLENDING_NORMAL = 0,
    WE_SCENE_FRAME_BLENDING_TRANSLUCENT = 1,
    WE_SCENE_FRAME_BLENDING_ADDITIVE = 2,
    WE_SCENE_FRAME_BLENDING_ALPHA_TO_COVERAGE = 3,
} WESceneFrameBlendingMode;

typedef enum WESceneFrameCullingMode {
    WE_SCENE_FRAME_CULLING_NORMAL = 0,
    WE_SCENE_FRAME_CULLING_DISABLED = 1,
} WESceneFrameCullingMode;

typedef enum WESceneFrameDepthMode {
    WE_SCENE_FRAME_DEPTH_DISABLED = 0,
    WE_SCENE_FRAME_DEPTH_ENABLED = 1,
} WESceneFrameDepthMode;

typedef enum WESceneFramePlanIssueCode {
    WE_SCENE_FRAME_ISSUE_TEXT_RENDERING_UNAVAILABLE = 0,
    WE_SCENE_FRAME_ISSUE_SOUND_RUNTIME_UNAVAILABLE = 1,
    WE_SCENE_FRAME_ISSUE_SCRIPT_RUNTIME_UNAVAILABLE = 2,
    WE_SCENE_FRAME_ISSUE_PASSTHROUGH_UNAVAILABLE = 3,
    WE_SCENE_FRAME_ISSUE_PUPPET_UNAVAILABLE = 4,
    WE_SCENE_FRAME_ISSUE_COMPOSE_UNAVAILABLE = 5,
    WE_SCENE_FRAME_ISSUE_IMAGE_MATERIAL_UNAVAILABLE = 6,
    WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_DESCRIPTOR_MISSING = 7,
    WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_READ_BEFORE_WRITE = 8,
    WE_SCENE_FRAME_ISSUE_AUDIO_INPUT_UNAVAILABLE = 9,
    WE_SCENE_FRAME_ISSUE_PERSPECTIVE_PROJECTION_UNAVAILABLE = 10,
    WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_FEEDBACK_LOOP = 11,
    WE_SCENE_FRAME_ISSUE_OBJECT_PLANNING_FAILED = 12,
    WE_SCENE_FRAME_ISSUE_EFFECT_PASS_PLANNING_FAILED = 13,
} WESceneFramePlanIssueCode;

typedef enum WESceneFramePlanIssueSeverity {
    WE_SCENE_FRAME_ISSUE_WARNING = 0,
    WE_SCENE_FRAME_ISSUE_SKIP_PASS = 1,
    WE_SCENE_FRAME_ISSUE_SKIP_OBJECT = 2,
    WE_SCENE_FRAME_ISSUE_FRAME_FATAL = 3,
} WESceneFramePlanIssueSeverity;

typedef struct WESceneFrameResourceInfo {
    WESceneFrameResourceKind kind;
    const char* id;
    const char* logical_name;
} WESceneFrameResourceInfo;

typedef struct WESceneFramePlanInfo {
    uint64_t model_revision;
    uint32_t width;
    uint32_t height;
    int clear_enabled;
    double clear_red;
    double clear_green;
    double clear_blue;
    double clear_alpha;
    WESceneVector3 camera_center;
    WESceneVector3 camera_eye;
    WESceneVector3 camera_up;
    double camera_near_plane;
    double camera_far_plane;
    double camera_field_of_view;
    int camera_projection_auto;
    uint32_t camera_projection_width;
    uint32_t camera_projection_height;
    int parallax_enabled;
    double parallax_amount;
    double parallax_delay;
    double parallax_mouse_influence;
    int is_executable;
    size_t framebuffer_count;
    size_t image_count;
    size_t text_count;
    size_t operation_count;
    size_t issue_count;
    size_t script_evaluation_count;
    WESceneFrameResourceInfo output;
    size_t particle_count;
    int camera_orthographic;
    double camera_perspective_override_field_of_view;
} WESceneFramePlanInfo;

typedef enum WESceneScriptEvaluationStatus {
    WE_SCENE_SCRIPT_EVALUATION_SUCCESS = 0,
    WE_SCENE_SCRIPT_EVALUATION_UNAVAILABLE = 1,
} WESceneScriptEvaluationStatus;

typedef struct WESceneScriptEvaluationInfo {
    const char* json_pointer;
    WESceneScriptEvaluationStatus status;
    size_t execution_count;
    size_t cache_hit_count;
} WESceneScriptEvaluationInfo;

typedef struct WESceneFramebufferInfo {
    WESceneFrameResourceInfo resource;
    WESceneFramebufferFormat format;
    WESceneFramebufferWrapMode wrap_mode;
    uint32_t width;
    uint32_t height;
    double scale;
    int unique;
} WESceneFramebufferInfo;

typedef struct WESceneFrameImageInfo {
    size_t object_index;
    int32_t object_id;
    int visible;
    double width;
    double height;
    WESceneObjectTransform world_transform;
    WESceneFrameResourceInfo source;
    WESceneFrameResourceInfo composite_a;
    WESceneFrameResourceInfo composite_b;
    int perspective;
} WESceneFrameImageInfo;

typedef struct WESceneFrameTextInfo {
    size_t object_index;
    int32_t object_id;
    int visible;
    const char* text;
    const char* font;
    double point_size;
    double width;
    double height;
    double color_red;
    double color_green;
    double color_blue;
    double color_alpha;
    double alpha;
    double padding_x;
    double padding_y;
    double spacing_x;
    double spacing_y;
    WESceneObjectTransform world_transform;
    const char* horizontal_alignment;
    const char* vertical_alignment;
    int limit_rows;
    int limit_use_ellipsis;
    int limit_width;
    int32_t max_rows;
    double max_width;
    int perspective;
} WESceneFrameTextInfo;

typedef struct WESceneFrameParticleInfo {
    size_t object_index;
    int32_t object_id;
    int visible;
    WESceneObjectTransform world_transform;
    const char* definition_identity;
    const char* shader;
    const char* vertex_shader_path;
    const char* fragment_shader_path;
    WESceneFrameBlendingMode blending;
    WESceneFrameCullingMode culling;
    WESceneFrameDepthMode depth_test;
    WESceneFrameDepthMode depth_write;
    WESceneFrameResourceInfo texture0;
    double parallax_depth_x;
    double parallax_depth_y;
    int perspective;
    const char* animation_mode;
    double sequence_multiplier;
    uint32_t max_count;
    double fixed_step_seconds;
    double start_time;
    uint32_t flags;
    int override_enabled;
    double override_alpha;
    double override_size;
    double override_lifetime;
    double override_rate;
    double override_speed;
    double override_count;
    WESceneVector3 override_color;
    WESceneVector3 override_color_multiplier;
    size_t emitter_count;
    size_t initializer_count;
    size_t operator_count;
    size_t control_point_count;
    size_t combo_count;
    int renderer_kind;
    double renderer_length;
    double renderer_max_length;
    double renderer_min_length;
    double renderer_subdivision;
    double renderer_segments;
    double renderer_uv_scale;
    int renderer_uv_scrolling;
    int renderer_uv_smoothing;
    int renderer_fade_alpha;
    int renderer_fade_size;
    size_t texture_count;
    size_t constant_count;
} WESceneFrameParticleInfo;

typedef struct WESceneFrameParticleControlPointInfo {
    int32_t id;
    WESceneVector3 position;
} WESceneFrameParticleControlPointInfo;

typedef struct WESceneFrameParticleEmitterInfo {
    int32_t control_point;
    uint32_t flags;
    double rate;
    double delay;
    double duration;
    double minimum_periodic_delay;
    double maximum_periodic_delay;
    double minimum_periodic_duration;
    double maximum_periodic_duration;
    uint32_t maximum_to_emit_per_period;
} WESceneFrameParticleEmitterInfo;

typedef enum WESceneFrameSoundPlaybackMode {
    WE_SCENE_FRAME_SOUND_PLAYBACK_LOOP = 1,
    WE_SCENE_FRAME_SOUND_PLAYBACK_RANDOM = 2,
    WE_SCENE_FRAME_SOUND_PLAYBACK_SINGLE = 3,
} WESceneFrameSoundPlaybackMode;

typedef enum WESceneFrameSoundPlaybackCommandAction {
    WE_SCENE_FRAME_SOUND_COMMAND_NONE = 0,
    WE_SCENE_FRAME_SOUND_COMMAND_PLAY = 1,
    WE_SCENE_FRAME_SOUND_COMMAND_PAUSE = 2,
    WE_SCENE_FRAME_SOUND_COMMAND_STOP = 3,
} WESceneFrameSoundPlaybackCommandAction;

typedef enum WESceneSoundRuntimeState {
    WE_SCENE_SOUND_RUNTIME_STOPPED = 0,
    WE_SCENE_SOUND_RUNTIME_PLAYING = 1,
    WE_SCENE_SOUND_RUNTIME_PAUSED = 2,
    WE_SCENE_SOUND_RUNTIME_ENDED = 3,
} WESceneSoundRuntimeState;

typedef struct WESceneSoundRuntimeStateInput {
    int32_t object_id;
    WESceneSoundRuntimeState state;
    double position;
} WESceneSoundRuntimeStateInput;

typedef struct WESceneFrameSoundInfo {
    size_t object_index;
    int32_t object_id;
    int visible;
    size_t source_count;
    WESceneFrameSoundPlaybackMode playback_mode;
    double volume;
    int start_silent;
    int mute_in_editor;
    double minimum_time;
    double maximum_time;
    WESceneFrameSoundPlaybackCommandAction playback_command;
    uint64_t playback_command_generation;
} WESceneFrameSoundInfo;

typedef struct WESceneFrameExecutorIssueInfo {
    WESceneFramePlanIssueSeverity severity;
    size_t object_index;
    int32_t object_id;
    size_t operation_index;
    const char* message;
} WESceneFrameExecutorIssueInfo;

typedef struct WESceneFrameOperationInfo {
    WESceneFrameOperationKind kind;
    size_t image_index;
    size_t text_index;
    int32_t object_id;
    int has_effect;
    size_t effect_index;
    int has_effect_pass;
    size_t effect_pass_index;
    size_t material_pass_index;
    const char* shader;
    const char* vertex_shader_path;
    const char* fragment_shader_path;
    WESceneFrameBlendingMode blending;
    WESceneFrameCullingMode culling;
    WESceneFrameDepthMode depth_test;
    WESceneFrameDepthMode depth_write;
    WESceneFrameGeometryKind geometry;
    WESceneFrameTexCoordKind texture_coordinates;
    WESceneFrameResourceInfo input;
    int has_previous_input;
    WESceneFrameResourceInfo previous_input;
    WESceneFrameResourceInfo source;
    WESceneFrameResourceInfo destination;
    size_t texture_count;
    size_t combo_count;
    size_t constant_count;
    int write_alpha;
    double clear_red;
    double clear_green;
    double clear_blue;
    double clear_alpha;
    size_t particle_index;
    int clear_depth;
    int has_light_index;
    size_t light_index;
    int has_alternate_view_projection;
    WESceneMatrix4x4 alternate_view_projection;
} WESceneFrameOperationInfo;

typedef struct WESceneFrameTextureBindingInfo {
    int32_t slot;
    WESceneFrameResourceInfo resource;
    int sample_depth;
} WESceneFrameTextureBindingInfo;

typedef struct WESceneFrameComboInfo {
    const char* name;
    int32_t value;
} WESceneFrameComboInfo;

typedef struct WESceneFramePlanIssueInfo {
    WESceneFramePlanIssueCode code;
    WESceneFramePlanIssueSeverity severity;
    int has_object;
    int32_t object_id;
    const char* asset_path;
    const char* json_pointer;
    const char* message;
} WESceneFramePlanIssueInfo;

typedef struct WEScenePropertyInfo {
    const char* key;
    const char* text;
    WEScenePropertyType type;
    int has_index;
    int32_t index;
    int has_order;
    int32_t order;
    int has_minimum;
    double minimum;
    int has_maximum;
    double maximum;
    int has_step;
    double step;
    int has_precision;
    int32_t precision;
    int has_fraction;
    int fraction;
    int is_read_only;
} WEScenePropertyInfo;

typedef struct WEScenePropertyOptionInfo {
    const char* value;
    const char* label;
} WEScenePropertyOptionInfo;

// This is intentionally not a union so Swift imports it predictably. Model
// property APIs populate only the field selected by type. Frame-plan constant
// APIs additionally expose synchronized scalar projections and represent a
// RuntimeValue vector as OBJECT plus component_count/vector_value.
typedef struct WEScenePropertyValue {
    WESceneValueType type;
    int boolean_value;
    int64_t integer_value;
    double number_value;
    const char* string_value;
    size_t component_count;
    WESceneVector4 vector_value;
} WEScenePropertyValue;

typedef struct WEScenePropertyUpdate {
    const char* key;
    WEScenePropertyValue value;
} WEScenePropertyUpdate;

typedef struct WESceneFrameConstantInfo {
    const char* name;
    WESceneDynamicValueSource source;
    WEScenePropertyValue value;
} WESceneFrameConstantInfo;

// Creates a configured runtime after validating both external paths and
// mounting its asset sources. On failure, returns NULL and stores an owned
// error in out_error when provided. Scene graph loading and rendering are
// separate lifecycle layers.
WESceneRuntimeRef we_scene_runtime_create(
    const WESceneRuntimeConfiguration* configuration,
    WESceneRuntimeErrorRef* out_error
);

void we_scene_runtime_destroy(WESceneRuntimeRef runtime);

WESceneModelRef we_scene_runtime_model_create(
    WESceneRuntimeRef runtime,
    const char* project_path,
    WESceneRuntimeErrorRef* out_error
);
void we_scene_model_destroy(WESceneModelRef model);
int we_scene_model_project_info(
    WESceneModelRef model,
    WESceneProjectInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_object_count(
    WESceneModelRef model,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_object_info(
    WESceneModelRef model,
    size_t index,
    WESceneObjectInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_object_dependency(
    WESceneModelRef model,
    size_t object_index,
    size_t dependency_index,
    int32_t* out_dependency_id,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_object_dependency_info(
    WESceneModelRef model,
    size_t object_index,
    size_t dependency_index,
    WESceneObjectDependencyInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_property_count(
    WESceneModelRef model,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_property_info(
    WESceneModelRef model,
    size_t index,
    WEScenePropertyInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_property_option_count(
    WESceneModelRef model,
    size_t property_index,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_property_option_info(
    WESceneModelRef model,
    size_t property_index,
    size_t option_index,
    WEScenePropertyOptionInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
// Returned string_value pointers remain valid until the next property-value
// query or successful setter on the same model.
int we_scene_model_property_value(
    WESceneModelRef model,
    size_t property_index,
    WEScenePropertyValue* out_value,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_set_property_value(
    WESceneModelRef model,
    const char* property_key,
    const WEScenePropertyValue* value,
    WESceneRuntimeErrorRef* out_error
);
// Validates every update before committing any value. A successful batch that
// changes one or more values advances the model revision exactly once.
int we_scene_model_set_property_values(
    WESceneModelRef model,
    const WEScenePropertyUpdate* updates,
    size_t update_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_model_revision(
    WESceneModelRef model,
    uint64_t* out_revision,
    WESceneRuntimeErrorRef* out_error
);

// Graphs retain their model/runtime. A snapshot is an immutable, coherent view
// of one model revision and remains valid after either owner handle is destroyed.
WESceneGraphRef we_scene_model_graph_create(
    WESceneModelRef model,
    WESceneRuntimeErrorRef* out_error
);
// Creates a graph with Wallpaper Engine-compatible persistent SceneScript
// localStorage. The ordinary graph constructor remains available to tooling
// that never evaluates scripts requiring host persistence.
WESceneGraphRef we_scene_model_graph_create_with_local_storage(
    WESceneModelRef model,
    const WESceneLocalStorageConfiguration* configuration,
    WESceneRuntimeErrorRef* out_error
);
void we_scene_graph_destroy(WESceneGraphRef graph);
WESceneGraphSnapshotRef we_scene_graph_snapshot_create(
    WESceneGraphRef graph,
    WESceneRuntimeErrorRef* out_error
);
void we_scene_graph_snapshot_destroy(WESceneGraphSnapshotRef snapshot);
int we_scene_graph_snapshot_revision(
    WESceneGraphSnapshotRef snapshot,
    uint64_t* out_revision,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_graph_snapshot_node_count(
    WESceneGraphSnapshotRef snapshot,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_graph_snapshot_node_info(
    WESceneGraphSnapshotRef snapshot,
    size_t index,
    WESceneGraphNodeInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_graph_snapshot_initialization_object_id(
    WESceneGraphSnapshotRef snapshot,
    size_t order_index,
    int32_t* out_object_id,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_graph_snapshot_render_object_id(
    WESceneGraphSnapshotRef snapshot,
    size_t order_index,
    int32_t* out_object_id,
    WESceneRuntimeErrorRef* out_error
);

// Frame graphs retain their SceneGraph. Plans are immutable and remain valid
// after the graph/model/runtime handles that created them are destroyed.
WESceneFrameGraphRef we_scene_graph_frame_graph_create(
    WESceneGraphRef graph,
    WESceneRuntimeErrorRef* out_error
);
void we_scene_frame_graph_destroy(WESceneFrameGraphRef graph);

// Executors copy the frame graph's shared ownership. The graph and all of its
// source handles may be destroyed immediately after this call succeeds.
WESceneFrameExecutorRef we_scene_frame_executor_create(
    WESceneFrameGraphRef graph,
    WESceneRuntimeErrorRef* out_error
);
// The borrowed MTLDevice must outlive the executor. The pointer is represented
// as void* to keep this C header framework-neutral.
WESceneFrameExecutorRef we_scene_frame_executor_create_with_metal_device(
    WESceneFrameGraphRef graph,
    void* metal_device,
    WESceneRuntimeErrorRef* out_error
);
void we_scene_frame_executor_destroy(WESceneFrameExecutorRef executor);
// Updates desktop pointer/button state for SceneScript cursor events. The
// state is sampled by the next evaluated frame; it does not synthesize an
// event by itself and remains independent from drawable pointer coordinates.
int we_scene_frame_executor_set_pointer_state(
    WESceneFrameExecutorRef executor,
    int active,
    int left_down,
    WESceneRuntimeErrorRef* out_error
);

// Supplies the actual host mode used by SceneScript's
// engine.isScreensaver()/engine.isWallpaper() methods.  The value is kept on
// the executor and applied to every subsequent evaluated frame.
int we_scene_frame_executor_set_screensaver_state(
    WESceneFrameExecutorRef executor,
    int is_screensaver,
    WESceneRuntimeErrorRef* out_error
);
// Replaces the host-observed playback truth used by SceneScript sound-layer
// isPlaying(). An empty array explicitly reports that no sound layer is
// currently playing; commands remain a separate output of frame evaluation.
int we_scene_frame_executor_set_sound_runtime_states(
    WESceneFrameExecutorRef executor,
    const WESceneSoundRuntimeStateInput* states,
    size_t state_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_set_media_snapshot(
    WESceneFrameExecutorRef executor,
    const WESceneMediaSnapshot* snapshot,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_clear_media_snapshot(
    WESceneFrameExecutorRef executor,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_set_media_thumbnail_rgba8(
    WESceneFrameExecutorRef executor,
    const WESceneMediaThumbnailRGBA8* thumbnail,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_clear_media_thumbnail(
    WESceneFrameExecutorRef executor,
    uint64_t revision,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_render(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_render_with_audio_spectrum(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_render_for_drawable(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_render_for_drawable_with_audio_spectrum(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_render_for_viewport(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_render_for_viewport_with_audio_spectrum(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
);
// The physical target is explicit and separate from the drawable/viewport
// used for presentation and pointer mapping. These entry points do not
// advance timing differently from their author-resolution counterparts.
int we_scene_frame_executor_render_for_drawable_with_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_render_for_drawable_with_audio_spectrum_and_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_render_for_viewport_with_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_render_for_viewport_with_audio_spectrum_and_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
);
// Rebuilds the last successful evaluated frame for a new drawable projection.
// This never evaluates scripts, advances particles, updates parallax, or
// publishes a new sound snapshot. It fails until a frame has rendered.
int we_scene_frame_executor_replay_for_drawable(
    WESceneFrameExecutorRef executor,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_replay_for_viewport(
    WESceneFrameExecutorRef executor,
    const WEScenePresentationViewport* viewport,
    WESceneRuntimeErrorRef* out_error
);
// Replays the last evaluated logical plan at an explicit physical target. It
// never re-evaluates scripts, advances particles, or changes frame time.
int we_scene_frame_executor_replay_for_drawable_with_physical_render_target(
    WESceneFrameExecutorRef executor,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_replay_for_viewport_with_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_present(
    WESceneFrameExecutorRef executor,
    void* metal_drawable,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_present_for_viewport(
    WESceneFrameExecutorRef executor,
    void* metal_drawable,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
);
uint32_t we_scene_frame_executor_width(WESceneFrameExecutorRef executor);
uint32_t we_scene_frame_executor_height(WESceneFrameExecutorRef executor);
size_t we_scene_frame_executor_rgba8_byte_count(WESceneFrameExecutorRef executor);
typedef struct WESceneFramebufferResourceStats {
    size_t framebuffer_count;
    size_t color_attachment_count;
    size_t depth_attachment_count;
    size_t inactive_framebuffer_count;
    size_t inactive_color_attachment_count;
    size_t inactive_depth_attachment_count;
} WESceneFramebufferResourceStats;
// Reports all framebuffer backing owned by the executor, split between the
// current reachable arena and persistent inactive backing. This
// query is valid before the first render and reports zeros until an arena is
// committed. Inactive resources preserve feedback across visibility changes;
// add current and inactive fields to obtain total allocation.
int we_scene_frame_executor_framebuffer_resource_stats(
    WESceneFrameExecutorRef executor,
    WESceneFramebufferResourceStats* out_stats,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_last_model_revision(
    WESceneFrameExecutorRef executor,
    uint64_t* out_revision,
    WESceneRuntimeErrorRef* out_error
);
// Sound strings remain valid until the next render attempt or executor destruction.
// All sound snapshot queries fail with FRAME_EXECUTOR_INVALID_STATE until a frame
// has rendered successfully and after any failed render attempt.
int we_scene_frame_executor_sound_count(
    WESceneFrameExecutorRef executor,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_sound_info(
    WESceneFrameExecutorRef executor,
    size_t index,
    WESceneFrameSoundInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_sound_source(
    WESceneFrameExecutorRef executor,
    size_t sound_index,
    size_t source_index,
    const char** out_source,
    WESceneRuntimeErrorRef* out_error
);
// Executor issue strings remain valid until the next render or replay attempt,
// explicit frame invalidation, or executor destruction. Queries fail with
// FRAME_EXECUTOR_INVALID_STATE until a frame has rendered successfully and after
// any failed render or replay attempt.
int we_scene_frame_executor_issue_count(
    WESceneFrameExecutorRef executor,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_issue_info(
    WESceneFrameExecutorRef executor,
    size_t index,
    WESceneFrameExecutorIssueInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_executor_read_rgba8(
    WESceneFrameExecutorRef executor,
    uint8_t* output,
    size_t output_length,
    WESceneRuntimeErrorRef* out_error
);
// Schedules a non-blocking readback of the current rendered frame. Pixel and
// error pointers are borrowed and remain valid only for the callback duration.
// Scheduling failures are returned synchronously through out_error and do not
// invoke the callback.
typedef void (*WESceneFrameExecutorRGBA8Callback)(
    void* context,
    const uint8_t* pixels,
    size_t pixel_count,
    const char* error_message
);
int we_scene_frame_executor_read_rgba8_async(
    WESceneFrameExecutorRef executor,
    void* context,
    WESceneFrameExecutorRGBA8Callback callback,
    WESceneRuntimeErrorRef* out_error
);

WESceneFramePlanRef we_scene_frame_graph_plan_create(
    WESceneFrameGraphRef graph,
    WESceneRuntimeErrorRef* out_error
);
WESceneFramePlanRef we_scene_frame_graph_plan_create_with_inputs(
    WESceneFrameGraphRef graph,
    const WESceneFrameInputs* inputs,
    WESceneRuntimeErrorRef* out_error
);
WESceneFramePlanRef we_scene_frame_graph_plan_create_with_inputs_and_render_quality(
    WESceneFrameGraphRef graph,
    const WESceneFrameInputs* inputs,
    WESceneFrameRenderQuality render_quality,
    WESceneRuntimeErrorRef* out_error
);
void we_scene_frame_plan_destroy(WESceneFramePlanRef plan);
int we_scene_frame_plan_info(
    WESceneFramePlanRef plan,
    WESceneFramePlanInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_framebuffer_info(
    WESceneFramePlanRef plan,
    size_t index,
    WESceneFramebufferInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_image_info(
    WESceneFramePlanRef plan,
    size_t index,
    WESceneFrameImageInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_text_info(
    WESceneFramePlanRef plan,
    size_t index,
    WESceneFrameTextInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_particle_info(
    WESceneFramePlanRef plan,
    size_t index,
    WESceneFrameParticleInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_particle_control_point_info(
    WESceneFramePlanRef plan,
    size_t particle_index,
    size_t control_point_index,
    WESceneFrameParticleControlPointInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_particle_emitter_info(
    WESceneFramePlanRef plan,
    size_t particle_index,
    size_t emitter_index,
    WESceneFrameParticleEmitterInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_operation_info(
    WESceneFramePlanRef plan,
    size_t index,
    WESceneFrameOperationInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_texture_binding_info(
    WESceneFramePlanRef plan,
    size_t operation_index,
    size_t binding_index,
    WESceneFrameTextureBindingInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_combo_info(
    WESceneFramePlanRef plan,
    size_t operation_index,
    size_t combo_index,
    WESceneFrameComboInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
// Returned string_value pointers are borrowed from plan and remain valid until
// the plan is destroyed. Vector values are output-only for this ABI.
int we_scene_frame_plan_constant_info(
    WESceneFramePlanRef plan,
    size_t operation_index,
    size_t constant_index,
    WESceneFrameConstantInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_issue_info(
    WESceneFramePlanRef plan,
    size_t issue_index,
    WESceneFramePlanIssueInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_frame_plan_script_evaluation_info(
    WESceneFramePlanRef plan,
    size_t evaluation_index,
    WESceneScriptEvaluationInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);

// Returned strings are owned by runtime and remain valid until it is destroyed.
const char* we_scene_runtime_assets_directory(WESceneRuntimeRef runtime);
const char* we_scene_runtime_scene_package_path(WESceneRuntimeRef runtime);

const char* we_scene_runtime_package_version(WESceneRuntimeRef runtime);
int we_scene_runtime_package_entry_count(
    WESceneRuntimeRef runtime,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_runtime_package_entry(
    WESceneRuntimeRef runtime,
    size_t index,
    WEScenePackageEntryInfo* out_entry,
    WESceneRuntimeErrorRef* out_error
);

WESceneRuntimeAssetRef we_scene_runtime_asset_create(
    WESceneRuntimeRef runtime,
    const char* path,
    WESceneRuntimeErrorRef* out_error
);
void we_scene_runtime_asset_destroy(WESceneRuntimeAssetRef asset);
const uint8_t* we_scene_runtime_asset_bytes(WESceneRuntimeAssetRef asset);
size_t we_scene_runtime_asset_length(WESceneRuntimeAssetRef asset);
WESceneAssetSource we_scene_runtime_asset_source(WESceneRuntimeAssetRef asset);

WESceneTextureRef we_scene_runtime_texture_create(
    WESceneRuntimeRef runtime,
    const char* path,
    WESceneRuntimeErrorRef* out_error
);
void we_scene_runtime_texture_destroy(WESceneTextureRef texture);
int we_scene_runtime_texture_info(
    WESceneTextureRef texture,
    WESceneTextureInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_runtime_texture_mipmap_count(
    WESceneTextureRef texture,
    size_t image_index,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_runtime_texture_mipmap_info(
    WESceneTextureRef texture,
    size_t image_index,
    size_t mipmap_index,
    WESceneTextureMipmapInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_runtime_texture_frame_info(
    WESceneTextureRef texture,
    size_t frame_index,
    WESceneTextureFrameInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);

WESceneShaderTranslationRef we_scene_shader_translate(
    const WESceneShaderSources* sources,
    WESceneRuntimeErrorRef* out_error
);
WESceneShaderTranslationRef we_scene_runtime_shader_translate_files(
    WESceneRuntimeRef runtime,
    const char* vertex_path,
    const char* fragment_path,
    WESceneRuntimeErrorRef* out_error
);
void we_scene_shader_translation_destroy(
    WESceneShaderTranslationRef translation
);
const char* we_scene_shader_translation_vertex_source(
    WESceneShaderTranslationRef translation
);
const char* we_scene_shader_translation_fragment_source(
    WESceneShaderTranslationRef translation
);
// Returns the source after Wallpaper Engine preprocessing and before SPIR-V
// translation. The pointer is owned by the translation object.
const char* we_scene_shader_translation_preprocessed_vertex_source(
    WESceneShaderTranslationRef translation
);
const char* we_scene_shader_translation_preprocessed_fragment_source(
    WESceneShaderTranslationRef translation
);
// Parameter strings and vector storage are owned by translation and remain
// valid until it is destroyed. Vertex and fragment parameters are queried
// separately so callers can define their own merge and precedence policy.
int we_scene_shader_translation_parameter_count(
    WESceneShaderTranslationRef translation,
    WESceneShaderStage stage,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
);
int we_scene_shader_translation_parameter_info(
    WESceneShaderTranslationRef translation,
    WESceneShaderStage stage,
    size_t index,
    WESceneShaderParameterInfo* out_info,
    WESceneRuntimeErrorRef* out_error
);
const char* we_scene_shader_glslang_revision(void);
const char* we_scene_shader_spirv_cross_revision(void);

WESceneRuntimeErrorCode we_scene_runtime_error_code(
    WESceneRuntimeErrorRef error
);

// The returned string is owned by error and remains valid until it is destroyed.
const char* we_scene_runtime_error_message(WESceneRuntimeErrorRef error);

// Scene model failures expose their structured location and reference chain.
// Other error categories return NULL/0 from these accessors.
const char* we_scene_runtime_error_asset_path(WESceneRuntimeErrorRef error);
const char* we_scene_runtime_error_json_pointer(WESceneRuntimeErrorRef error);
size_t we_scene_runtime_error_reference_count(WESceneRuntimeErrorRef error);
const char* we_scene_runtime_error_reference_at(
    WESceneRuntimeErrorRef error,
    size_t index
);

void we_scene_runtime_error_destroy(WESceneRuntimeErrorRef error);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif
