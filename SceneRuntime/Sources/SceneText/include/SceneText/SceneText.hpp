#ifndef WE_SCENE_TEXT_HPP
#define WE_SCENE_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace we::scene::text {

enum class ErrorCode {
    invalidArgument,
    invalidUTF8,
    fontCreation,
    layout,
    resourceLimit,
    rasterization,
};

class Error final : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message);
    [[nodiscard]] ErrorCode code() const noexcept;

private:
    ErrorCode code_;
};

struct FontSource final {
    std::span<const std::uint8_t> data;
    std::string systemName;

    [[nodiscard]] static FontSource bytes(std::span<const std::uint8_t> value);
    [[nodiscard]] static FontSource system(std::string name);
};

enum class HorizontalAlignment {
    left,
    center,
    right,
};

struct RasterRequest final {
    std::string utf8;
    double pointSize = 0.0;
    FontSource font;
    double maximumWidth = 0.0;
    std::size_t maximumRows = 0;
    bool useEllipsis = false;
    double characterSpacing = 0.0;
    double lineSpacing = 0.0;
    HorizontalAlignment horizontalAlignment = HorizontalAlignment::center;
};

struct RasterizedText final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bytesPerRow = 0;
    // Top-to-bottom, tightly packed 8-bit glyph coverage. Color and opacity
    // are deliberately applied by the renderer so they do not invalidate this bitmap.
    std::vector<std::uint8_t> coverage;
    double baselineFromTop = 0.0;
    // Logical layout bounds stay independent from glyph image overhang so the
    // object anchor does not move when the rendered characters change.
    double logicalLeftFromBitmap = 0.0;
    double logicalTopFromBitmap = 0.0;
    double typographicWidth = 0.0;
    double typographicHeight = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
    std::size_t lineCount = 0;
    bool truncated = false;
};

[[nodiscard]] RasterizedText rasterize(const RasterRequest& request);

}  // namespace we::scene::text

#endif
