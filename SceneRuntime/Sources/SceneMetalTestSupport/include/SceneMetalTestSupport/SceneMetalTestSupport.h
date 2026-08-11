#ifndef WE_SCENE_METAL_TEST_SUPPORT_H
#define WE_SCENE_METAL_TEST_SUPPORT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int we_scene_metal_test_render_text(uint8_t* rgba, size_t length, size_t* cache_count);
int we_scene_metal_test_text_cache_bound(size_t updates, size_t* cache_count);
int we_scene_metal_test_render_text_orientation(uint8_t* rgba, size_t length);
// Exercises the pure framebuffer liveness/depth analysis with a frame plan
// that combines render, copy, swap, clear, text, particle and texture-input
// references. It does not create synthetic GL output.
int we_scene_metal_test_framebuffer_plan_requirements(void);
// Exercises the fixed backing-pixel quality policy and verifies that applying
// a physical size changes only framebuffer dimensions, not logical camera
// projection or the source plan.
int we_scene_metal_test_physical_render_policy(void);
int we_scene_metal_test_decode_video(
    const uint8_t* bytes,
    size_t length,
    const char* source,
    uint32_t* width,
    uint32_t* height
);
typedef struct WESceneMetalTestVideoPipelineResult {
    uint64_t initial_serial;
    uint64_t decoded_serial;
    uint32_t bytes_per_row;
    int same_frame_update_skipped;
} WESceneMetalTestVideoPipelineResult;
int we_scene_metal_test_video_pipeline(
    const uint8_t* bytes,
    size_t length,
    const char* source,
    double target_time,
    WESceneMetalTestVideoPipelineResult* result
);
typedef struct WESceneMetalTestPresentationViewport {
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t drawable_width;
    uint32_t drawable_height;
} WESceneMetalTestPresentationViewport;
typedef struct WESceneMetalTestPresentationRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} WESceneMetalTestPresentationRect;
typedef struct WESceneMetalTestPresentationResult {
    double mapped_pointer_x;
    double mapped_pointer_y;
    int has_content;
    WESceneMetalTestPresentationRect source;
    WESceneMetalTestPresentationRect destination;
} WESceneMetalTestPresentationResult;
int we_scene_metal_test_presentation_transform(
    uint32_t source_width,
    uint32_t source_height,
    const WESceneMetalTestPresentationViewport* viewport,
    int scaling,
    double pointer_x,
    double pointer_y,
    WESceneMetalTestPresentationResult* result
);
int we_scene_metal_test_blit_presentation_slice(
    const WESceneMetalTestPresentationViewport* viewport,
    int scaling,
    uint8_t* rgba,
    size_t length
);
int we_scene_metal_test_present_pattern(
    uint32_t source_width,
    uint32_t source_height,
    const WESceneMetalTestPresentationViewport* viewport,
    int scaling,
    uint8_t* rgba,
    size_t length
);
#ifdef __cplusplus
}
#endif
#endif
