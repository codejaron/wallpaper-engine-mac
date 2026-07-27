#ifndef WE_SCENE_TEXT_TEST_SUPPORT_H
#define WE_SCENE_TEXT_TEST_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WESceneTextTestBitmap* WESceneTextTestBitmapRef;

typedef struct WESceneTextTestBitmapInfo {
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    const uint8_t* coverage;
    size_t coverage_size;
    double baseline_from_top;
    double typographic_width;
    double ascent;
    double descent;
    size_t line_count;
    int truncated;
} WESceneTextTestBitmapInfo;

typedef struct WESceneTextTestLayoutOptions {
    double maximum_width;
    size_t maximum_rows;
    int use_ellipsis;
    double character_spacing;
    double line_spacing;
    int horizontal_alignment;
} WESceneTextTestLayoutOptions;

WESceneTextTestBitmapRef we_scene_text_test_rasterize_font_bytes(
    const char* utf8,
    double point_size,
    const uint8_t* font_bytes,
    size_t font_size,
    char* error_buffer,
    size_t error_buffer_size
);
WESceneTextTestBitmapRef we_scene_text_test_rasterize_system_font(
    const char* utf8,
    double point_size,
    const char* font_name,
    char* error_buffer,
    size_t error_buffer_size
);
WESceneTextTestBitmapRef we_scene_text_test_rasterize_font_bytes_with_layout(
    const char* utf8,
    double point_size,
    const uint8_t* font_bytes,
    size_t font_size,
    const WESceneTextTestLayoutOptions* layout,
    char* error_buffer,
    size_t error_buffer_size
);
int we_scene_text_test_bitmap_info(
    WESceneTextTestBitmapRef bitmap,
    WESceneTextTestBitmapInfo* out_info
);
void we_scene_text_test_bitmap_destroy(WESceneTextTestBitmapRef bitmap);

#ifdef __cplusplus
}
#endif

#endif
