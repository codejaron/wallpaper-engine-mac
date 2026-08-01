#ifndef WE_SCENE_CORE_TEXTURE_HPP
#define WE_SCENE_CORE_TEXTURE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace we::scene {

enum class TextureContainerVersion : std::uint32_t {
    unknown = 0,
    texb0001 = 1,
    texb0002 = 2,
    texb0003 = 3,
    texb0004 = 4,
};

enum class TextureAnimationVersion : std::uint32_t {
    unknown = 0,
    texs0001 = 1,
    texs0002 = 2,
    texs0003 = 3,
};

enum class TextureFormat : std::uint32_t {
    argb8888 = 0,
    rgb888 = 1,
    rgb565 = 2,
    dxt5 = 4,
    dxt3 = 6,
    dxt1 = 7,
    rg88 = 8,
    r8 = 9,
    rg1616f = 10,
    r16f = 11,
    bc7 = 12,
    rgba1010102 = 13,
    rgba16161616f = 14,
    rgb161616f = 15,
    unknown = 0xffffffffU,
};

enum class TextureFileFormat : std::uint32_t {
    unknown = 0xffffffffU,
    bmp = 0,
    jpeg = 2,
    png = 13,
    gif = 25,
    hdr = 26,
    exr = 29,
    webp = 35,
    mp4 = 35,
};

enum TextureFlags : std::uint32_t {
    textureFlagNone = 0,
    textureFlagNoInterpolation = 1,
    textureFlagClampUVs = 2,
    textureFlagIsGif = 4,
    textureFlagClampUVsBorder = 8,
    textureFlagVideo = 32,
    textureFlagLUT = 64,
    textureFlagAlphaChannelPriority = 524288,
};

struct TextureFrame {
    std::uint32_t frameNumber = 0;
    float frameTime = 0.0F;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float widthAux = 0.0F;
    float heightAux = 0.0F;
};

struct TextureMipmap {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t compression = 0;
    std::int32_t uncompressedSize = 0;
    std::int32_t compressedSize = 0;
    std::string metadata;
    std::vector<std::uint8_t> bytes;
};

struct TextureImage {
    std::vector<TextureMipmap> mipmaps;
};

struct Texture final {
    TextureContainerVersion containerVersion =
        TextureContainerVersion::unknown;
    TextureAnimationVersion animationVersion =
        TextureAnimationVersion::unknown;
    TextureFormat format = TextureFormat::unknown;
    TextureFileFormat fileFormat = TextureFileFormat::unknown;
    std::uint32_t flags = textureFlagNone;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t textureWidth = 0;
    std::uint32_t textureHeight = 0;
    std::uint32_t gifWidth = 0;
    std::uint32_t gifHeight = 0;
    std::uint32_t imageCount = 0;
    bool isVideoMp4 = false;
    bool hasExtraTEXIField = false;
    std::vector<TextureImage> images;
    std::vector<TextureFrame> frames;
    std::uint32_t spritesheetColumns = 0;
    std::uint32_t spritesheetRows = 0;
    std::uint32_t spritesheetFrameCount = 0;
    float spritesheetDuration = 0.0F;

    [[nodiscard]] bool isAnimated() const noexcept {
        return (flags & textureFlagIsGif) != 0;
    }
};

class TextureParser final {
public:
    [[nodiscard]] static Texture parse(
        std::span<const std::uint8_t> bytes,
        std::string source = {},
        std::optional<std::string_view> metadata = std::nullopt
    );
    [[nodiscard]] static Texture parseFile(
        const std::filesystem::path& path,
        std::optional<std::string_view> metadata = std::nullopt
    );

    [[nodiscard]] static std::size_t expectedBlockCompressedSize(
        TextureFormat format,
        std::uint32_t width,
        std::uint32_t height
    );

    [[nodiscard]] static bool isBlockCompressed(
        TextureFormat format
    ) noexcept;
};

}  // namespace we::scene

#endif
