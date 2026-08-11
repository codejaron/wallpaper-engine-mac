#ifndef WE_SCENE_METAL_BRIDGE_H
#define WE_SCENE_METAL_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WESceneMetalRenderer* WESceneMetalRendererRef;
typedef struct WESceneMetalError* WESceneMetalErrorRef;

typedef enum WESceneMetalErrorCode {
    WE_SCENE_METAL_ERROR_NONE = 0,
    WE_SCENE_METAL_ERROR_INVALID_ARGUMENT = 1,
    WE_SCENE_METAL_ERROR_CONTEXT_CREATION = 2,
    WE_SCENE_METAL_ERROR_UNSUPPORTED_CONTEXT = 3,
    WE_SCENE_METAL_ERROR_SHADER_COMPILATION = 4,
    WE_SCENE_METAL_ERROR_PROGRAM_LINK = 5,
    WE_SCENE_METAL_ERROR_FRAMEBUFFER_CREATION = 6,
    WE_SCENE_METAL_ERROR_DRAW = 7,
    WE_SCENE_METAL_ERROR_READBACK = 8,
    WE_SCENE_METAL_ERROR_INTERNAL_FAILURE = 9,
    WE_SCENE_METAL_ERROR_TEXTURE_DECODE = 10,
    WE_SCENE_METAL_ERROR_TEXTURE_UPLOAD = 11,
    WE_SCENE_METAL_ERROR_RESOURCE_VALIDATION = 12,
} WESceneMetalErrorCode;

WESceneMetalRendererRef we_scene_metal_renderer_create(
    uint32_t width,
    uint32_t height,
    WESceneMetalErrorRef* out_error
);
void we_scene_metal_renderer_destroy(WESceneMetalRendererRef renderer);

size_t we_scene_metal_renderer_rgba8_byte_count(WESceneMetalRendererRef renderer);

int we_scene_metal_renderer_compile_program(
    WESceneMetalRendererRef renderer,
    const char* vertex_source,
    const char* fragment_source,
    WESceneMetalErrorRef* out_error
);
int we_scene_metal_renderer_draw(
    WESceneMetalRendererRef renderer,
    WESceneMetalErrorRef* out_error
);

// Reads tightly packed RGBA8 rows with a top-left origin and width * 4 bytes
// per row.
int we_scene_metal_renderer_read_rgba8(
    WESceneMetalRendererRef renderer,
    uint8_t* output,
    size_t output_length,
    WESceneMetalErrorRef* out_error
);

WESceneMetalErrorCode we_scene_metal_error_code(WESceneMetalErrorRef error);
const char* we_scene_metal_error_message(WESceneMetalErrorRef error);
void we_scene_metal_error_destroy(WESceneMetalErrorRef error);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif
