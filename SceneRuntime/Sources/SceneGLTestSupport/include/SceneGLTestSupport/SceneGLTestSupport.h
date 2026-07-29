#ifndef WE_SCENE_GL_TEST_SUPPORT_H
#define WE_SCENE_GL_TEST_SUPPORT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int we_scene_gl_test_render_text(uint8_t* rgba, size_t length, size_t* cache_count);
int we_scene_gl_test_text_cache_bound(size_t updates, size_t* cache_count);
int we_scene_gl_test_render_text_orientation(uint8_t* rgba, size_t length);
typedef struct WESceneGLTestParticleObjects {
    uint32_t vertex_array;
    uint32_t vertex_buffer;
    uint32_t element_buffer;
} WESceneGLTestParticleObjects;
int we_scene_gl_test_current_particle_objects(WESceneGLTestParticleObjects* objects);
int we_scene_gl_test_particle_objects_exist(
    const WESceneGLTestParticleObjects* objects,
    int* all_exist
);
int we_scene_gl_test_particle_first_lifetime(
    const WESceneGLTestParticleObjects* objects,
    float* lifetime
);
int we_scene_gl_test_decode_video(
    const uint8_t* bytes,
    size_t length,
    const char* source,
    uint32_t* width,
    uint32_t* height
);
typedef struct WESceneGLTestPresentationViewport {
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t drawable_width;
    uint32_t drawable_height;
} WESceneGLTestPresentationViewport;
typedef struct WESceneGLTestPresentationRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} WESceneGLTestPresentationRect;
typedef struct WESceneGLTestPresentationResult {
    double mapped_pointer_x;
    double mapped_pointer_y;
    int has_content;
    WESceneGLTestPresentationRect source;
    WESceneGLTestPresentationRect destination;
} WESceneGLTestPresentationResult;
int we_scene_gl_test_presentation_transform(
    uint32_t source_width,
    uint32_t source_height,
    const WESceneGLTestPresentationViewport* viewport,
    int scaling,
    double pointer_x,
    double pointer_y,
    WESceneGLTestPresentationResult* result
);
int we_scene_gl_test_blit_presentation_slice(
    const WESceneGLTestPresentationViewport* viewport,
    int scaling,
    uint8_t* rgba,
    size_t length
);
int we_scene_gl_test_present_pattern(
    uint32_t source_width,
    uint32_t source_height,
    const WESceneGLTestPresentationViewport* viewport,
    int scaling,
    uint8_t* rgba,
    size_t length
);
#ifdef __cplusplus
}
#endif
#endif
