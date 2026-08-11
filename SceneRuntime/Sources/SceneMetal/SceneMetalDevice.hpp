#ifndef WE_SCENE_METAL_DEVICE_HPP
#define WE_SCENE_METAL_DEVICE_HPP

#include <SceneCore/Texture.hpp>
#include <SceneMetal/SceneMetal.hpp>
#include <SceneShader/ShaderCompiler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace we::scene::metal {

class MetalObject final {
public:
    MetalObject() = default;
    explicit MetalObject(void* borrowedObject) noexcept;
    ~MetalObject();
    MetalObject(const MetalObject&) = delete;
    MetalObject& operator=(const MetalObject&) = delete;
    MetalObject(MetalObject&& other) noexcept;
    MetalObject& operator=(MetalObject&& other) noexcept;

    void reset() noexcept;
    [[nodiscard]] void* get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

private:
    void* value_ = nullptr;
};

enum class PixelFormat {
    rgba8,
    bgra8,
    r8,
    rg16f,
    r16f,
};

enum class TextureWrap {
    repeat,
    clampToEdge,
    clampToBorder,
};

enum class TextureFilter {
    nearest,
    linear,
};

struct TextureResource final {
    MetalObject texture;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipmapCount = 1;
    PixelFormat format = PixelFormat::rgba8;
    TextureWrap wrap = TextureWrap::clampToEdge;
    TextureFilter filter = TextureFilter::linear;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(texture);
    }
};

struct FramebufferResource final {
    TextureResource colorTexture;
    MetalObject depthTexture;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::rgba8;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(colorTexture);
    }
    [[nodiscard]] bool hasDepth() const noexcept {
        return static_cast<bool>(depthTexture);
    }
};

struct AssetTextureResource final {
    AssetTextureResource() = default;
    ~AssetTextureResource();
    AssetTextureResource(const AssetTextureResource&) = delete;
    AssetTextureResource& operator=(const AssetTextureResource&) = delete;
    AssetTextureResource(AssetTextureResource&& other) noexcept;
    AssetTextureResource& operator=(AssetTextureResource&& other) noexcept;

    std::vector<TextureResource> images;
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
    void* videoDecoder = nullptr;
    std::uint64_t lastUploadedVideoFrameSerial = 0;
    std::uint64_t lastVideoUpdateFrame = 0;
    bool video = false;

    [[nodiscard]] bool isAnimated() const noexcept {
        return (flags & textureFlagIsGif) != 0;
    }
};

class Program final {
public:
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

private:
    struct Impl;
    explicit Program(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
};

struct BufferResource final {
    MetalObject buffer;
    std::size_t capacity = 0;
};

enum class VertexFormat {
    float2,
    float3,
    float4,
};

struct VertexAttributeLayout final {
    std::uint32_t location = 0;
    VertexFormat format = VertexFormat::float2;
    std::size_t offset = 0;
};

struct VertexLayout final {
    std::size_t stride = 0;
    std::vector<VertexAttributeLayout> attributes;
};

enum class BlendMode {
    replace,
    alpha,
    additive,
};

struct RenderState final {
    BlendMode blending = BlendMode::replace;
    bool cullBackFaces = false;
    bool depthTest = false;
    bool depthWrite = false;
    bool depthClamp = false;
    bool writeAlpha = true;
    bool alphaSourceOne = false;
};

struct UniformBytesBinding final {
    std::optional<std::uint32_t> vertexBufferIndex;
    std::optional<std::uint32_t> fragmentBufferIndex;
    const void* bytes = nullptr;
    std::size_t byteCount = 0;
};

struct TextureStageBinding final {
    const TextureResource* texture = nullptr;
    std::optional<TextureFilter> filterOverride;
    std::optional<std::uint32_t> vertexTextureIndex;
    std::optional<std::uint32_t> vertexSamplerIndex;
    std::optional<std::uint32_t> fragmentTextureIndex;
    std::optional<std::uint32_t> fragmentSamplerIndex;
};

struct DrawRequest final {
    std::shared_ptr<Program> program;
    FramebufferResource* destination = nullptr;
    RenderState state;
    VertexLayout vertexLayout;
    const BufferResource* vertexBuffer = nullptr;
    std::size_t vertexBufferOffset = 0;
    const BufferResource* indexBuffer = nullptr;
    std::size_t indexBufferOffset = 0;
    std::vector<UniformBytesBinding> uniforms;
    std::vector<TextureStageBinding> textures;
};

class Device final {
public:
    class Session final {
    public:
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;

        [[nodiscard]] std::shared_ptr<Program> createProgram(
            const TranslatedMetalShaderPair& shaders
        );
        void destroyProgram(std::shared_ptr<Program>& program) noexcept;

        void uploadBuffer(
            BufferResource& destination,
            std::span<const std::byte> bytes
        );
        void destroyBuffer(BufferResource& buffer) noexcept;

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
        void requestVideoTextureFrame(
            AssetTextureResource& texture,
            double timeSeconds
        );
        [[nodiscard]] bool updateVideoTexture(
            AssetTextureResource& texture,
            std::uint64_t frameSequence
        );
        void destroyTexture(AssetTextureResource& texture) noexcept;
        [[nodiscard]] TextureResource uploadCoverageTexture(
            std::uint32_t width,
            std::uint32_t height,
            std::uint32_t bytesPerRow,
            std::span<const std::uint8_t> coverage
        );
        [[nodiscard]] TextureResource uploadRGBA8Texture(
            std::uint32_t width,
            std::uint32_t height,
            std::span<const std::uint8_t> pixels
        );
        void destroyTexture(TextureResource& texture) noexcept;

        void clear(
            FramebufferResource& framebuffer,
            std::array<float, 4> color,
            bool clearDepth
        );
        void copy(
            const TextureResource& source,
            TextureResource& destination
        );
        void draw(
            const DrawRequest& request,
            std::uint32_t vertexStart,
            std::uint32_t vertexCount
        );
        void drawIndexed(
            const DrawRequest& request,
            std::uint32_t indexCount,
            bool index32
        );
        [[nodiscard]] FramebufferResource framebufferForDrawable(
            void* metalDrawable
        );
        void present(void* metalDrawable);

        void readRGBA8(
            const FramebufferResource& framebuffer,
            std::span<std::uint8_t> output
        );

        void finish(bool waitForCompletion = false);

    private:
        explicit Session(Device& device);
        void encodeDraw(
            const DrawRequest& request,
            std::optional<std::uint32_t> vertexStart,
            std::uint32_t count,
            bool index32
        );

        Device& device_;
        std::unique_lock<std::mutex> lock_;
        MetalObject commandBuffer_;
        bool finished_ = false;

        friend class Device;
    };

    Device();
    explicit Device(void* borrowedMetalDevice);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    [[nodiscard]] Session activate();
    [[nodiscard]] void* nativeDevice() const noexcept { return device_.get(); }

private:
    MetalObject device_;
    MetalObject commandQueue_;
    std::mutex mutex_;

    friend class Session;
};

}  // namespace we::scene::metal

#endif
