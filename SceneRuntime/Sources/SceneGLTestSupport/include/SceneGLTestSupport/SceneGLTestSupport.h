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
#ifdef __cplusplus
}
#endif
#endif
