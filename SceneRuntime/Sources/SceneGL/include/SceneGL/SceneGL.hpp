#ifndef WE_SCENE_GL_HPP
#define WE_SCENE_GL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace we::scene::gl {

enum class ErrorCode : std::uint32_t {
    invalidArgument = 1,
    contextCreation = 2,
    unsupportedContext = 3,
    shaderCompilation = 4,
    programLink = 5,
    framebufferCreation = 6,
    draw = 7,
    readback = 8,
    internalFailure = 9,
    textureDecode = 10,
    textureUpload = 11,
    resourceValidation = 12,
};

class Error final : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message);

    [[nodiscard]] ErrorCode code() const noexcept;

private:
    ErrorCode code_;
};

// Owns a macOS OpenGL 4.1 Core context and an RGBA8 framebuffer. All GL
// operations temporarily make the owned context current and restore the
// caller's previous context before returning.
class OffscreenRenderer final {
public:
    OffscreenRenderer(std::uint32_t width, std::uint32_t height);
    ~OffscreenRenderer();

    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;
    OffscreenRenderer(OffscreenRenderer&&) = delete;
    OffscreenRenderer& operator=(OffscreenRenderer&&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::size_t rgba8ByteCount() const noexcept;

    // Invalidates the active program before attempting a new compilation.
    // A failed compilation therefore requires a successful retry before draw.
    void compileProgram(
        std::string_view vertexSource,
        std::string_view fragmentSource
    );
    void draw();

    // Returns tightly packed RGBA8 rows with a top-left origin.
    void readRGBA8(std::span<std::uint8_t> output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace we::scene::gl

#endif
