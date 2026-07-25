#ifndef WE_SCENE_GL_DEVICE_HPP
#define WE_SCENE_GL_DEVICE_HPP

#include <SceneCore/Texture.hpp>
#include <SceneGL/SceneGL.hpp>

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace we::scene::gl {

enum class PixelFormat {
    rgba8,
    r8,
    rg16f,
    r16f,
};

enum class TextureWrap {
    repeat,
    clampToEdge,
    clampToBorder,
};

struct FramebufferResource final {
    FramebufferResource() = default;
    FramebufferResource(
        GLuint framebufferValue,
        GLuint colorTextureValue,
        GLuint depthRenderbufferValue,
        std::uint32_t widthValue,
        std::uint32_t heightValue,
        PixelFormat formatValue
    ) noexcept;
    FramebufferResource(const FramebufferResource&) = delete;
    FramebufferResource& operator=(const FramebufferResource&) = delete;
    FramebufferResource(FramebufferResource&& other) noexcept;
    FramebufferResource& operator=(FramebufferResource&& other) noexcept;

    GLuint framebuffer = 0;
    GLuint colorTexture = 0;
    GLuint depthRenderbuffer = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::rgba8;
};

// Device readback always returns tightly packed top-down rows. Ordinary GL
// framebuffers store logical bottom-left content at their first physical row,
// while Wallpaper Engine deliberately keeps its top-left row there until the
// final presentation boundary.
enum class ReadbackSourceOrientation {
    openGLBottomLeft,
    wallpaperEngineTopLeft,
};

struct AssetTextureResource final {
    AssetTextureResource() = default;
    ~AssetTextureResource();
    AssetTextureResource(const AssetTextureResource&) = delete;
    AssetTextureResource& operator=(const AssetTextureResource&) = delete;
    AssetTextureResource(AssetTextureResource&& other) noexcept;
    AssetTextureResource& operator=(AssetTextureResource&& other) noexcept;

    std::vector<GLuint> images;
    std::vector<std::uint32_t> imageWidths;
    std::vector<std::uint32_t> imageHeights;
    std::array<float, 4> resolution{};
    TextureFormat format = TextureFormat::unknown;
    std::uint32_t flags = textureFlagNone;
    std::vector<TextureFrame> frames;
    std::uint32_t spritesheetColumns = 0;
    std::uint32_t spritesheetRows = 0;
    std::uint32_t spritesheetFrameCount = 0;
    float spritesheetDuration = 0.0F;
    // Opaque AVFoundation-backed decoder for TEX video assets. The decoder
    // is owned by this resource; OpenGL images remain owned by Device.
    void* videoDecoder = nullptr;
    bool video = false;

    [[nodiscard]] bool isAnimated() const noexcept {
        return (flags & textureFlagIsGif) != 0;
    }
};

// Owns one OpenGL context and every resource allocated through it. A Session
// serializes access, makes the context current, and restores the caller's
// previous context on destruction.
class Device final {
public:
    class Session final {
    public:
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;

        [[nodiscard]] GLuint createProgram(
            std::string_view vertexSource,
            std::string_view fragmentSource
        );
        void destroyProgram(GLuint& program) noexcept;

        [[nodiscard]] GLuint createVertexArray();
        void destroyVertexArray(GLuint& vertexArray) noexcept;

        [[nodiscard]] GLuint createBuffer();
        void destroyBuffer(GLuint& buffer) noexcept;

        [[nodiscard]] FramebufferResource createFramebuffer(
            PixelFormat format,
            std::uint32_t width,
            std::uint32_t height,
            TextureWrap wrap,
            bool depthAttachment = false
        );
        void ensureDepthAttachment(FramebufferResource& framebuffer);
        void destroyFramebuffer(FramebufferResource& framebuffer) noexcept;

        [[nodiscard]] AssetTextureResource uploadTexture(
            const Texture& texture,
            std::string_view source
        );
        void updateVideoTexture(
            AssetTextureResource& texture,
            double timeSeconds
        );
        void destroyTexture(AssetTextureResource& texture) noexcept;
        [[nodiscard]] GLuint uploadCoverageTexture(
            std::uint32_t width,
            std::uint32_t height,
            std::uint32_t bytesPerRow,
            std::span<const std::uint8_t> coverage
        );
        void destroyTexture(GLuint& texture) noexcept;

        void readRGBA8(
            const FramebufferResource& framebuffer,
            std::span<std::uint8_t> output,
            ReadbackSourceOrientation sourceOrientation =
                ReadbackSourceOrientation::openGLBottomLeft
        );

        void checkError(ErrorCode code, const char* operation) const;

    private:
        explicit Session(Device& device);

        Device& device_;
        std::unique_lock<std::mutex> lock_;
        CGLContextObj previous_ = nullptr;

        friend class Device;
    };

    Device();
    explicit Device(CGLContextObj borrowedContext);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    [[nodiscard]] Session activate();

private:
    void validateContext();
    void destroyTrackedResources() noexcept;

    CGLContextObj context_ = nullptr;
    bool ownsContext_ = true;
    std::mutex mutex_;
    std::unordered_set<GLuint> textures_;
    std::unordered_set<GLuint> framebuffers_;
    std::unordered_set<GLuint> renderbuffers_;
    std::unordered_set<GLuint> programs_;
    std::unordered_set<GLuint> vertexArrays_;
    std::unordered_set<GLuint> buffers_;

    friend class Session;
};

}  // namespace we::scene::gl

#endif
