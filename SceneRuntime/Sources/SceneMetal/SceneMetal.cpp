#include <SceneMetal/SceneMetal.hpp>

#include "SceneMetalDevice.hpp"

#include <SceneShader/ShaderCompiler.hpp>

#include <limits>
#include <utility>

namespace we::scene::metal {
namespace {

constexpr std::size_t maximumFramebufferBytes = 256 * 1024 * 1024;

struct Dimensions final {
    std::uint32_t width;
    std::uint32_t height;
    std::size_t rgba8Bytes;
};

Dimensions validateDimensions(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Offscreen framebuffer dimensions must be greater than zero"
        );
    }
    if (width > std::numeric_limits<std::size_t>::max() / height ||
        static_cast<std::size_t>(width) * height >
            std::numeric_limits<std::size_t>::max() / 4) {
        throw Error(
            ErrorCode::invalidArgument,
            "Offscreen framebuffer byte count overflows size_t"
        );
    }
    const std::size_t rgba8Bytes =
        static_cast<std::size_t>(width) * height * 4;
    if (rgba8Bytes > maximumFramebufferBytes) {
        throw Error(
            ErrorCode::invalidArgument,
            "Offscreen framebuffer exceeds the 256 MiB allocation limit"
        );
    }
    return {.width = width, .height = height, .rgba8Bytes = rgba8Bytes};
}

}  // namespace

struct OffscreenRenderer::Impl final {
    explicit Impl(Dimensions dimensions)
        : width(dimensions.width),
          height(dimensions.height),
          rgba8Bytes(dimensions.rgba8Bytes) {
        auto current = device.activate();
        framebuffer = current.createFramebuffer(
            PixelFormat::rgba8,
            width,
            height,
            TextureWrap::clampToEdge
        );
    }

    Device device;
    std::uint32_t width;
    std::uint32_t height;
    std::size_t rgba8Bytes = 0;
    FramebufferResource framebuffer;
    std::shared_ptr<Program> program;
};

Error::Error(ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ErrorCode Error::code() const noexcept { return code_; }

OffscreenRenderer::OffscreenRenderer(
    std::uint32_t width,
    std::uint32_t height
) : impl_(std::make_unique<Impl>(validateDimensions(width, height))) {}

OffscreenRenderer::~OffscreenRenderer() = default;

std::uint32_t OffscreenRenderer::width() const noexcept {
    return impl_->width;
}

std::uint32_t OffscreenRenderer::height() const noexcept {
    return impl_->height;
}

std::size_t OffscreenRenderer::rgba8ByteCount() const noexcept {
    return impl_->rgba8Bytes;
}

void OffscreenRenderer::compileProgram(
    std::string_view vertexSource,
    std::string_view fragmentSource
) {
    auto current = impl_->device.activate();
    current.destroyProgram(impl_->program);
    current.clear(impl_->framebuffer, {0.0F, 0.0F, 0.0F, 0.0F}, false);
    try {
        const TranslatedMetalShaderPair translated =
            ShaderCompiler::translateToMetal(
                vertexSource,
                fragmentSource,
                "offscreen.vert",
                "offscreen.frag"
            );
        impl_->program = current.createProgram(translated);
    } catch (const ShaderCompileError& error) {
        const char* description = "Shader compilation failed";
        if (error.phase() == ShaderCompilePhase::vertexParse) {
            description = "Vertex shader compilation failed";
        } else if (error.phase() == ShaderCompilePhase::fragmentParse) {
            description = "Fragment shader compilation failed";
        } else if (error.phase() == ShaderCompilePhase::link) {
            description = "Shader program linking failed";
        }
        throw Error(
            ErrorCode::shaderCompilation,
            std::string(description) + ": " + error.what()
        );
    }
}

void OffscreenRenderer::draw() {
    if (!impl_->program) {
        throw Error(
            ErrorCode::draw,
            "A compiled Metal program is required before drawing"
        );
    }
    auto current = impl_->device.activate();
    current.clear(impl_->framebuffer, {0.0F, 0.0F, 0.0F, 0.0F}, false);
    DrawRequest request{
        .program = impl_->program,
        .destination = &impl_->framebuffer,
    };
    current.draw(request, 0, 3);
}

void OffscreenRenderer::readRGBA8(std::span<std::uint8_t> output) {
    auto current = impl_->device.activate();
    current.readRGBA8(impl_->framebuffer, output);
}

}  // namespace we::scene::metal
