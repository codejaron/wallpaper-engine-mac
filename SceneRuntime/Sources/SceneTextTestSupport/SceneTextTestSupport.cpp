#include <SceneTextTestSupport/SceneTextTestSupport.h>

#include <SceneText/SceneText.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>

struct WESceneTextTestBitmap {
    we::scene::text::RasterizedText value;
};

namespace {
void copyError(const char* message, char* output, std::size_t size) {
    if (output == nullptr || size == 0) return;
    const std::size_t count = std::min(std::strlen(message), size - 1);
    std::memcpy(output, message, count);
    output[count] = '\0';
}

we::scene::text::HorizontalAlignment horizontalAlignment(int value) {
    switch (value) {
        case 0: return we::scene::text::HorizontalAlignment::left;
        case 1: return we::scene::text::HorizontalAlignment::center;
        case 2: return we::scene::text::HorizontalAlignment::right;
        default:
            throw we::scene::text::Error(
                we::scene::text::ErrorCode::invalidArgument,
                "Text test layout alignment is invalid"
            );
    }
}
}

extern "C" WESceneTextTestBitmapRef we_scene_text_test_rasterize_font_bytes(
    const char* utf8,
    double point_size,
    const uint8_t* font_bytes,
    size_t font_size,
    char* error_buffer,
    size_t error_buffer_size
) {
    if (error_buffer != nullptr && error_buffer_size > 0) error_buffer[0] = '\0';
    if (utf8 == nullptr || (font_bytes == nullptr && font_size != 0)) {
        copyError("Text test rasterizer received a null input", error_buffer, error_buffer_size);
        return nullptr;
    }
    try {
        auto result = std::make_unique<WESceneTextTestBitmap>();
        result->value = we::scene::text::rasterize({
            .utf8 = utf8,
            .pointSize = point_size,
            .font = we::scene::text::FontSource::bytes(
                std::span<const std::uint8_t>(font_bytes, font_size)
            ),
        });
        return result.release();
    } catch (const std::exception& error) {
        copyError(error.what(), error_buffer, error_buffer_size);
        return nullptr;
    }
}

extern "C" WESceneTextTestBitmapRef we_scene_text_test_rasterize_system_font(
    const char* utf8,
    double point_size,
    const char* font_name,
    char* error_buffer,
    size_t error_buffer_size
) {
    if (error_buffer != nullptr && error_buffer_size > 0) error_buffer[0] = '\0';
    if (utf8 == nullptr || font_name == nullptr) {
        copyError("Text test rasterizer received a null input", error_buffer, error_buffer_size);
        return nullptr;
    }
    try {
        auto result = std::make_unique<WESceneTextTestBitmap>();
        result->value = we::scene::text::rasterize({
            .utf8 = utf8,
            .pointSize = point_size,
            .font = we::scene::text::FontSource::system(font_name),
        });
        return result.release();
    } catch (const std::exception& error) {
        copyError(error.what(), error_buffer, error_buffer_size);
        return nullptr;
    }
}

extern "C" WESceneTextTestBitmapRef
we_scene_text_test_rasterize_font_bytes_with_layout(
    const char* utf8,
    double point_size,
    const uint8_t* font_bytes,
    size_t font_size,
    const WESceneTextTestLayoutOptions* layout,
    char* error_buffer,
    size_t error_buffer_size
) {
    if (error_buffer != nullptr && error_buffer_size > 0) error_buffer[0] = '\0';
    if (utf8 == nullptr || layout == nullptr ||
        (font_bytes == nullptr && font_size != 0)) {
        copyError("Text test rasterizer received a null input", error_buffer, error_buffer_size);
        return nullptr;
    }
    try {
        auto result = std::make_unique<WESceneTextTestBitmap>();
        result->value = we::scene::text::rasterize({
            .utf8 = utf8,
            .pointSize = point_size,
            .font = we::scene::text::FontSource::bytes(
                std::span<const std::uint8_t>(font_bytes, font_size)
            ),
            .maximumWidth = layout->maximum_width,
            .maximumRows = layout->maximum_rows,
            .useEllipsis = layout->use_ellipsis != 0,
            .characterSpacing = layout->character_spacing,
            .lineSpacing = layout->line_spacing,
            .horizontalAlignment = horizontalAlignment(
                layout->horizontal_alignment
            ),
        });
        return result.release();
    } catch (const std::exception& error) {
        copyError(error.what(), error_buffer, error_buffer_size);
        return nullptr;
    }
}

extern "C" int we_scene_text_test_bitmap_info(
    WESceneTextTestBitmapRef bitmap,
    WESceneTextTestBitmapInfo* out_info
) {
    if (bitmap == nullptr || out_info == nullptr) return 0;
    const auto& value = bitmap->value;
    *out_info = {
        .width = value.width,
        .height = value.height,
        .bytes_per_row = value.bytesPerRow,
        .coverage = value.coverage.data(),
        .coverage_size = value.coverage.size(),
        .baseline_from_top = value.baselineFromTop,
        .typographic_width = value.typographicWidth,
        .ascent = value.ascent,
        .descent = value.descent,
        .line_count = value.lineCount,
        .truncated = value.truncated ? 1 : 0,
    };
    return 1;
}

extern "C" void we_scene_text_test_bitmap_destroy(WESceneTextTestBitmapRef bitmap) {
    delete bitmap;
}
