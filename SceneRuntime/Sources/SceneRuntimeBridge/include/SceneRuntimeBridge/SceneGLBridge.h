#ifndef WE_SCENE_GL_BRIDGE_H
#define WE_SCENE_GL_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WESceneGLRenderer* WESceneGLRendererRef;
typedef struct WESceneGLError* WESceneGLErrorRef;

typedef enum WESceneGLErrorCode {
    WE_SCENE_GL_ERROR_NONE = 0,
    WE_SCENE_GL_ERROR_INVALID_ARGUMENT = 1,
    WE_SCENE_GL_ERROR_CONTEXT_CREATION = 2,
    WE_SCENE_GL_ERROR_UNSUPPORTED_CONTEXT = 3,
    WE_SCENE_GL_ERROR_SHADER_COMPILATION = 4,
    WE_SCENE_GL_ERROR_PROGRAM_LINK = 5,
    WE_SCENE_GL_ERROR_FRAMEBUFFER_CREATION = 6,
    WE_SCENE_GL_ERROR_DRAW = 7,
    WE_SCENE_GL_ERROR_READBACK = 8,
    WE_SCENE_GL_ERROR_INTERNAL_FAILURE = 9,
    WE_SCENE_GL_ERROR_TEXTURE_DECODE = 10,
    WE_SCENE_GL_ERROR_TEXTURE_UPLOAD = 11,
    WE_SCENE_GL_ERROR_RESOURCE_VALIDATION = 12,
} WESceneGLErrorCode;

WESceneGLRendererRef we_scene_gl_renderer_create(
    uint32_t width,
    uint32_t height,
    WESceneGLErrorRef* out_error
);
void we_scene_gl_renderer_destroy(WESceneGLRendererRef renderer);

size_t we_scene_gl_renderer_rgba8_byte_count(WESceneGLRendererRef renderer);

int we_scene_gl_renderer_compile_program(
    WESceneGLRendererRef renderer,
    const char* vertex_source,
    const char* fragment_source,
    WESceneGLErrorRef* out_error
);
int we_scene_gl_renderer_draw(
    WESceneGLRendererRef renderer,
    WESceneGLErrorRef* out_error
);

// Reads tightly packed RGBA8 rows with a top-left origin and width * 4 bytes
// per row.
int we_scene_gl_renderer_read_rgba8(
    WESceneGLRendererRef renderer,
    uint8_t* output,
    size_t output_length,
    WESceneGLErrorRef* out_error
);

WESceneGLErrorCode we_scene_gl_error_code(WESceneGLErrorRef error);
const char* we_scene_gl_error_message(WESceneGLErrorRef error);
void we_scene_gl_error_destroy(WESceneGLErrorRef error);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif
