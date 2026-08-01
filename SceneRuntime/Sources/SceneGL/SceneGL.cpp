#include <SceneGL/SceneGL.hpp>

#include "SceneGLDevice.hpp"

#include <OpenGL/gl3.h>

#include <limits>
#include <utility>

namespace we::scene::gl {
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
    const std::size_t rgba8Bytes = static_cast<std::size_t>(width) * height * 4;
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
        vertexArray = current.createVertexArray();
    }

    Device device;
    std::uint32_t width;
    std::uint32_t height;
    std::size_t rgba8Bytes = 0;
    FramebufferResource framebuffer;
    GLuint vertexArray = 0;
    GLuint program = 0;
};

Error::Error(ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ErrorCode Error::code() const noexcept {
    return code_;
}

OffscreenRenderer::OffscreenRenderer(std::uint32_t width, std::uint32_t height)
    : impl_(std::make_unique<Impl>(validateDimensions(width, height))) {}

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

    // A failed reload must not expose either the previous program or frame.
    glBindFramebuffer(GL_FRAMEBUFFER, impl_->framebuffer.framebuffer);
    glViewport(
        0,
        0,
        static_cast<GLsizei>(impl_->width),
        static_cast<GLsizei>(impl_->height)
    );
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    current.checkError(ErrorCode::draw, "Clearing the offscreen frame");

    impl_->program = current.createProgram(vertexSource, fragmentSource);
}

void OffscreenRenderer::draw() {
    auto current = impl_->device.activate();
    if (impl_->program == 0) {
        throw Error(
            ErrorCode::draw,
            "A linked shader program is required before drawing"
        );
    }

    glBindFramebuffer(GL_FRAMEBUFFER, impl_->framebuffer.framebuffer);
    glViewport(
        0,
        0,
        static_cast<GLsizei>(impl_->width),
        static_cast<GLsizei>(impl_->height)
    );
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_MULTISAMPLE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(impl_->program);
    glBindVertexArray(impl_->vertexArray);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    current.checkError(ErrorCode::draw, "Drawing the offscreen frame");
}

void OffscreenRenderer::readRGBA8(std::span<std::uint8_t> output) {
    auto current = impl_->device.activate();
    current.readRGBA8(impl_->framebuffer, output);
}

}  // namespace we::scene::gl
