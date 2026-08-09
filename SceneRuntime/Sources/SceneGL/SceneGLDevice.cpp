#include "SceneGLDevice.hpp"
#include "SceneVideoDecoder.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace we::scene::gl {
namespace {

constexpr std::size_t maximumShaderSourceBytes = 16 * 1024 * 1024;
constexpr std::size_t maximumResourceBytes = 256 * 1024 * 1024;

template <typename T>
class CFReference final {
public:
    explicit CFReference(T value = nullptr) noexcept : value_(value) {}
    ~CFReference() {
        if (value_ != nullptr) {
            CFRelease(value_);
        }
    }

    CFReference(const CFReference&) = delete;
    CFReference& operator=(const CFReference&) = delete;

    [[nodiscard]] T get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

private:
    T value_;
};

struct DecodedImage final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

struct GLPixelFormat final {
    GLint internalFormat = GL_RGBA8;
    GLenum externalFormat = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;
    std::size_t bytesPerPixel = 4;
};

class PixelStoreGuard final {
public:
    PixelStoreGuard(GLenum alignment, GLenum rowLength)
        : alignment_(alignment), rowLength_(rowLength) {
        glGetIntegerv(alignment_, &savedAlignment_);
        glGetIntegerv(rowLength_, &savedRowLength_);
    }

    ~PixelStoreGuard() {
        glPixelStorei(alignment_, savedAlignment_);
        glPixelStorei(rowLength_, savedRowLength_);
    }

    PixelStoreGuard(const PixelStoreGuard&) = delete;
    PixelStoreGuard& operator=(const PixelStoreGuard&) = delete;

private:
    GLenum alignment_;
    GLenum rowLength_;
    GLint savedAlignment_ = 4;
    GLint savedRowLength_ = 0;
};

class TextureBindingGuard final {
public:
    TextureBindingGuard() {
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2D_);
    }

    ~TextureBindingGuard() {
        glActiveTexture(static_cast<GLenum>(activeTexture_));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2D_));
    }

    TextureBindingGuard(const TextureBindingGuard&) = delete;
    TextureBindingGuard& operator=(const TextureBindingGuard&) = delete;

private:
    GLint activeTexture_ = GL_TEXTURE0;
    GLint texture2D_ = 0;
};

std::string cglErrorMessage(CGLError error) {
    const char* description = CGLErrorString(error);
    return description != nullptr ? description : "unknown CGL error";
}

std::string glErrorMessage(GLenum error) {
    std::ostringstream result;
    result << "OpenGL error 0x" << std::hex << error;
    return result.str();
}

void validateShaderSource(std::string_view source, const char* stage) {
    if (source.empty()) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(stage) + " shader source is empty"
        );
    }
    if (source.size() > maximumShaderSourceBytes) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(stage) + " shader source exceeds the 16 MiB limit"
        );
    }
    if (source.find('\0') != std::string_view::npos) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(stage) + " shader source contains a null byte"
        );
    }
}

void validateResourceDimensions(
    std::uint32_t width,
    std::uint32_t height,
    std::size_t bytesPerPixel,
    std::string_view description
) {
    if (width == 0 || height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(description) + " dimensions must be greater than zero"
        );
    }
    if (width > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(description) + " dimensions exceed OpenGL's signed range"
        );
    }
    const std::size_t widthValue = width;
    if (widthValue > std::numeric_limits<std::size_t>::max() / height ||
        widthValue * height >
            std::numeric_limits<std::size_t>::max() / bytesPerPixel) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(description) + " byte count overflows size_t"
        );
    }
    const std::size_t byteCount = widthValue * height * bytesPerPixel;
    if (byteCount > maximumResourceBytes) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(description) + " exceeds the 256 MiB allocation limit"
        );
    }
}

void validateMaximumTextureSize(
    std::uint32_t width,
    std::uint32_t height,
    std::string_view description
) {
    GLint maximumTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
    if (maximumTextureSize <= 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "OpenGL reported an invalid maximum texture size"
        );
    }
    if (width > static_cast<std::uint32_t>(maximumTextureSize) ||
        height > static_cast<std::uint32_t>(maximumTextureSize)) {
        throw Error(
            ErrorCode::invalidArgument,
            std::string(description) + " dimensions exceed GL_MAX_TEXTURE_SIZE (" +
                std::to_string(maximumTextureSize) + ")"
        );
    }
}

std::string shaderInfoLog(GLuint shader) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) {
        return "OpenGL returned no diagnostic";
    }
    std::vector<GLchar> bytes(static_cast<std::size_t>(length));
    GLsizei written = 0;
    glGetShaderInfoLog(shader, length, &written, bytes.data());
    return std::string(bytes.data(), static_cast<std::size_t>(std::max(0, written)));
}

std::string programInfoLog(GLuint program) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) {
        return "OpenGL returned no diagnostic";
    }
    std::vector<GLchar> bytes(static_cast<std::size_t>(length));
    GLsizei written = 0;
    glGetProgramInfoLog(program, length, &written, bytes.data());
    return std::string(bytes.data(), static_cast<std::size_t>(std::max(0, written)));
}

GLuint compileShader(GLenum type, std::string_view source, const char* stage) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        throw Error(
            ErrorCode::shaderCompilation,
            std::string("Creating the ") + stage + " shader failed"
        );
    }
    try {
        const GLchar* bytes = source.data();
        const GLint length = static_cast<GLint>(source.size());
        glShaderSource(shader, 1, &bytes, &length);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) {
            throw Error(
                ErrorCode::shaderCompilation,
                std::string(stage) + " shader compilation failed: " +
                    shaderInfoLog(shader)
            );
        }
        return shader;
    } catch (...) {
        glDeleteShader(shader);
        throw;
    }
}

GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader) {
    const GLuint program = glCreateProgram();
    if (program == 0) {
        throw Error(ErrorCode::programLink, "Creating a shader program failed");
    }
    try {
        glAttachShader(program, vertexShader);
        glAttachShader(program, fragmentShader);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            throw Error(
                ErrorCode::programLink,
                "Shader program linking failed: " + programInfoLog(program)
            );
        }
        return program;
    } catch (...) {
        glDeleteProgram(program);
        throw;
    }
}

GLPixelFormat framebufferPixelFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::rgba8:
            return {};
        case PixelFormat::r8:
            return {
                .internalFormat = GL_R8,
                .externalFormat = GL_RED,
                .type = GL_UNSIGNED_BYTE,
                .bytesPerPixel = 1,
            };
        case PixelFormat::rg16f:
            return {
                .internalFormat = GL_RG16F,
                .externalFormat = GL_RG,
                .type = GL_HALF_FLOAT,
                .bytesPerPixel = 4,
            };
        case PixelFormat::r16f:
            return {
                .internalFormat = GL_R16F,
                .externalFormat = GL_RED,
                .type = GL_HALF_FLOAT,
                .bytesPerPixel = 2,
            };
    }
    throw Error(ErrorCode::resourceValidation, "Unknown framebuffer pixel format");
}

GLenum glWrap(TextureWrap wrap) {
    switch (wrap) {
        case TextureWrap::repeat:
            return GL_REPEAT;
        case TextureWrap::clampToEdge:
            return GL_CLAMP_TO_EDGE;
        case TextureWrap::clampToBorder:
            return GL_CLAMP_TO_BORDER;
    }
    throw Error(ErrorCode::resourceValidation, "Unknown texture wrap mode");
}

DecodedImage decodeEmbeddedImage(
    std::span<const std::uint8_t> bytes,
    std::string_view source
) {
    if (bytes.empty() ||
        bytes.size() > static_cast<std::size_t>(std::numeric_limits<CFIndex>::max())) {
        throw Error(
            ErrorCode::textureDecode,
            "Embedded texture image is empty or too large in '" +
                std::string(source) + "'"
        );
    }
    CFReference<CFDataRef> data(CFDataCreate(
        kCFAllocatorDefault,
        bytes.data(),
        static_cast<CFIndex>(bytes.size())
    ));
    if (!data) {
        throw Error(
            ErrorCode::textureDecode,
            "Creating image data failed for texture '" + std::string(source) + "'"
        );
    }
    CFReference<CGImageSourceRef> imageSource(
        CGImageSourceCreateWithData(data.get(), nullptr)
    );
    if (!imageSource || CGImageSourceGetCount(imageSource.get()) == 0) {
        throw Error(
            ErrorCode::textureDecode,
            "ImageIO could not recognize embedded image data in texture '" +
                std::string(source) + "'"
        );
    }
    CFReference<CGImageRef> image(
        CGImageSourceCreateImageAtIndex(imageSource.get(), 0, nullptr)
    );
    if (!image) {
        throw Error(
            ErrorCode::textureDecode,
            "ImageIO could not decode embedded image data in texture '" +
                std::string(source) + "'"
        );
    }

    const std::size_t width = CGImageGetWidth(image.get());
    const std::size_t height = CGImageGetHeight(image.get());
    if (width == 0 || height == 0 ||
        width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max()) {
        throw Error(
            ErrorCode::textureDecode,
            "Decoded image dimensions are invalid in texture '" +
                std::string(source) + "'"
        );
    }
    validateResourceDimensions(
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
        4,
        "Decoded texture image"
    );

    DecodedImage result{
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
        .rgba8 = std::vector<std::uint8_t>(width * height * 4),
    };
    CFReference<CGColorSpaceRef> colorSpace(CGColorSpaceCreateDeviceRGB());
    if (!colorSpace) {
        throw Error(
            ErrorCode::textureDecode,
            "Creating an RGB color space failed for texture '" +
                std::string(source) + "'"
        );
    }
    CFReference<CGContextRef> context(CGBitmapContextCreate(
        result.rgba8.data(),
        width,
        height,
        8,
        width * 4,
        colorSpace.get(),
        static_cast<CGBitmapInfo>(
            static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
            static_cast<std::uint32_t>(kCGBitmapByteOrder32Big)
        )
    ));
    if (!context) {
        throw Error(
            ErrorCode::textureDecode,
            "Creating an RGBA8 decode surface failed for texture '" +
                std::string(source) + "'"
        );
    }
    CGContextSetBlendMode(context.get(), kCGBlendModeCopy);
    CGContextDrawImage(
        context.get(),
        CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)),
        image.get()
    );

    // Bitmap contexts expose premultiplied RGBA. Wallpaper Engine's upstream
    // decoder uploads straight RGBA, so convert before handing bytes to GL.
    for (std::size_t offset = 0; offset < result.rgba8.size(); offset += 4) {
        const std::uint32_t alpha = result.rgba8[offset + 3];
        if (alpha == 0) {
            result.rgba8[offset] = 0;
            result.rgba8[offset + 1] = 0;
            result.rgba8[offset + 2] = 0;
            continue;
        }
        if (alpha == 255) {
            continue;
        }
        for (std::size_t component = 0; component < 3; ++component) {
            const std::uint32_t value = result.rgba8[offset + component];
            result.rgba8[offset + component] = static_cast<std::uint8_t>(
                std::min<std::uint32_t>(255, (value * 255 + alpha / 2) / alpha)
            );
        }
    }

    // Keep ImageIO's top-down rows unchanged. Raw/compressed TEX payloads and
    // Linux's stb path are uploaded without per-resource orientation fixes;
    // the scene is flipped exactly once when it leaves the final framebuffer.
    return result;
}

struct TextureUploadFormat final {
    GLint internalFormat = GL_RGBA8;
    GLenum externalFormat = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;
    bool compressed = false;
    std::size_t bytesPerPixel = 4;
};

TextureUploadFormat uploadFormat(const Texture& texture, std::string_view source) {
    if (texture.fileFormat != TextureFileFormat::unknown) {
        return {};
    }
    switch (texture.format) {
        case TextureFormat::argb8888:
            return {};
        case TextureFormat::r8:
            return {
                .internalFormat = GL_R8,
                .externalFormat = GL_RED,
                .type = GL_UNSIGNED_BYTE,
                .compressed = false,
                .bytesPerPixel = 1,
            };
        case TextureFormat::rg88:
            return {
                .internalFormat = GL_RG8,
                .externalFormat = GL_RG,
                .type = GL_UNSIGNED_BYTE,
                .compressed = false,
                .bytesPerPixel = 2,
            };
        case TextureFormat::dxt1:
            return {
                .internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                .compressed = true,
            };
        case TextureFormat::dxt3:
            return {
                .internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,
                .compressed = true,
            };
        case TextureFormat::dxt5:
            return {
                .internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                .compressed = true,
            };
        case TextureFormat::rgb888:
        case TextureFormat::rgb565:
        case TextureFormat::rg1616f:
        case TextureFormat::r16f:
        case TextureFormat::bc7:
        case TextureFormat::rgba1010102:
        case TextureFormat::rgba16161616f:
        case TextureFormat::rgb161616f:
        case TextureFormat::unknown:
            break;
    }
    throw Error(
        ErrorCode::textureUpload,
        "Texture '" + std::string(source) + "' uses unsupported format " +
            std::to_string(static_cast<std::uint32_t>(texture.format))
    );
}

void configureTextureParameters(const Texture& texture, std::size_t mipmapCount) {
    if (mipmapCount == 0 ||
        mipmapCount - 1 >
            static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
        throw Error(
            ErrorCode::textureUpload,
            "Texture mipmap level exceeds OpenGL's signed range"
        );
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_MAX_LEVEL,
        static_cast<GLint>(mipmapCount - 1)
    );
    GLenum wrap = GL_REPEAT;
    if ((texture.flags & textureFlagClampUVs) != 0) {
        wrap = GL_CLAMP_TO_EDGE;
    } else if ((texture.flags & textureFlagClampUVsBorder) != 0) {
        wrap = GL_CLAMP_TO_BORDER;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrap));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrap));
    if ((texture.flags & textureFlagNoInterpolation) != 0) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    }
}

}  // namespace

FramebufferResource::FramebufferResource(
    GLuint framebufferValue,
    GLuint colorTextureValue,
    GLuint depthRenderbufferValue,
    std::uint32_t widthValue,
    std::uint32_t heightValue,
    PixelFormat formatValue
) noexcept
    : framebuffer(framebufferValue),
      colorTexture(colorTextureValue),
      depthRenderbuffer(depthRenderbufferValue),
      width(widthValue),
      height(heightValue),
      format(formatValue) {}

FramebufferResource::FramebufferResource(FramebufferResource&& other) noexcept
    : FramebufferResource(
          std::exchange(other.framebuffer, 0),
          std::exchange(other.colorTexture, 0),
          std::exchange(other.depthRenderbuffer, 0),
          std::exchange(other.width, 0),
          std::exchange(other.height, 0),
          std::exchange(other.format, PixelFormat::rgba8)
      ) {}

FramebufferResource& FramebufferResource::operator=(
    FramebufferResource&& other
) noexcept {
    if (this == &other) {
        return *this;
    }
    if (framebuffer != 0 || colorTexture != 0 || depthRenderbuffer != 0) {
        std::terminate();
    }
    framebuffer = std::exchange(other.framebuffer, 0);
    colorTexture = std::exchange(other.colorTexture, 0);
    depthRenderbuffer = std::exchange(other.depthRenderbuffer, 0);
    width = std::exchange(other.width, 0);
    height = std::exchange(other.height, 0);
    format = std::exchange(other.format, PixelFormat::rgba8);
    return *this;
}

AssetTextureResource::AssetTextureResource(AssetTextureResource&& other) noexcept
    : images(std::move(other.images)),
      imageWidths(std::move(other.imageWidths)),
      imageHeights(std::move(other.imageHeights)),
      resolution(std::exchange(other.resolution, {})),
      format(std::exchange(other.format, TextureFormat::unknown)),
      flags(std::exchange(other.flags, textureFlagNone)),
      frames(std::move(other.frames)),
      spritesheetColumns(std::exchange(other.spritesheetColumns, 0)),
      spritesheetRows(std::exchange(other.spritesheetRows, 0)),
      spritesheetFrameCount(std::exchange(other.spritesheetFrameCount, 0)),
      spritesheetDuration(std::exchange(other.spritesheetDuration, 0.0F)),
      videoDecoder(std::exchange(other.videoDecoder, nullptr)),
      lastUploadedVideoFrameSerial(std::exchange(
          other.lastUploadedVideoFrameSerial, 0
      )),
      lastVideoUpdateFrame(std::exchange(other.lastVideoUpdateFrame, 0)),
      video(std::exchange(other.video, false)) {
    other.images.clear();
    other.imageWidths.clear();
    other.imageHeights.clear();
    other.frames.clear();
}

AssetTextureResource::~AssetTextureResource() {
    if (videoDecoder != nullptr) {
        destroyVideoDecoder(videoDecoder);
        videoDecoder = nullptr;
    }
}

AssetTextureResource& AssetTextureResource::operator=(
    AssetTextureResource&& other
) noexcept {
    if (this == &other) {
        return *this;
    }
    if (!images.empty()) {
        std::terminate();
    }
    if (videoDecoder != nullptr) {
        destroyVideoDecoder(videoDecoder);
    }
    images = std::move(other.images);
    imageWidths = std::move(other.imageWidths);
    imageHeights = std::move(other.imageHeights);
    resolution = std::exchange(other.resolution, {});
    format = std::exchange(other.format, TextureFormat::unknown);
    flags = std::exchange(other.flags, textureFlagNone);
    frames = std::move(other.frames);
    spritesheetColumns = std::exchange(other.spritesheetColumns, 0);
    spritesheetRows = std::exchange(other.spritesheetRows, 0);
    spritesheetFrameCount = std::exchange(other.spritesheetFrameCount, 0);
    spritesheetDuration = std::exchange(other.spritesheetDuration, 0.0F);
    videoDecoder = std::exchange(other.videoDecoder, nullptr);
    lastUploadedVideoFrameSerial = std::exchange(
        other.lastUploadedVideoFrameSerial, 0
    );
    lastVideoUpdateFrame = std::exchange(other.lastVideoUpdateFrame, 0);
    video = std::exchange(other.video, false);
    other.images.clear();
    other.imageWidths.clear();
    other.imageHeights.clear();
    other.frames.clear();
    return *this;
}

Device::Session::Session(Device& device)
    : device_(device), lock_(device.mutex_), previous_(CGLGetCurrentContext()) {
    const CGLError result = CGLSetCurrentContext(device_.context_);
    if (result != kCGLNoError) {
        throw Error(
            ErrorCode::contextCreation,
            "Making the SceneGL context current failed: " + cglErrorMessage(result)
        );
    }
}

Device::Session::~Session() {
    const CGLError result = CGLSetCurrentContext(previous_);
    if (result != kCGLNoError) {
        std::fprintf(
            stderr,
            "SceneGL failed to restore the previous CGL context: %s\n",
            cglErrorMessage(result).c_str()
        );
    }
}

GLuint Device::Session::createProgram(
    std::string_view vertexSource,
    std::string_view fragmentSource
) {
    validateShaderSource(vertexSource, "Vertex");
    validateShaderSource(fragmentSource, "Fragment");
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    GLuint program = 0;
    try {
        vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource, "Vertex");
        fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource, "Fragment");
        program = linkProgram(vertexShader, fragmentShader);
        glDeleteShader(vertexShader);
        vertexShader = 0;
        glDeleteShader(fragmentShader);
        fragmentShader = 0;
        checkError(ErrorCode::programLink, "Creating a shader program");
        device_.programs_.insert(program);
        return program;
    } catch (...) {
        if (vertexShader != 0) {
            glDeleteShader(vertexShader);
        }
        if (fragmentShader != 0) {
            glDeleteShader(fragmentShader);
        }
        if (program != 0) {
            glDeleteProgram(program);
        }
        throw;
    }
}

void Device::Session::destroyProgram(GLuint& program) noexcept {
    if (program == 0) {
        return;
    }
    glDeleteProgram(program);
    device_.programs_.erase(program);
    program = 0;
}

GLuint Device::Session::createVertexArray() {
    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    checkError(ErrorCode::resourceValidation, "Creating a vertex array");
    if (vertexArray == 0) {
        throw Error(ErrorCode::resourceValidation, "OpenGL returned vertex array zero");
    }
    device_.vertexArrays_.insert(vertexArray);
    return vertexArray;
}

void Device::Session::destroyVertexArray(GLuint& vertexArray) noexcept {
    if (vertexArray == 0) {
        return;
    }
    glDeleteVertexArrays(1, &vertexArray);
    device_.vertexArrays_.erase(vertexArray);
    vertexArray = 0;
}

GLuint Device::Session::createBuffer() {
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    checkError(ErrorCode::resourceValidation, "Creating a buffer");
    if (buffer == 0) {
        throw Error(ErrorCode::resourceValidation, "OpenGL returned buffer zero");
    }
    device_.buffers_.insert(buffer);
    return buffer;
}

void Device::Session::destroyBuffer(GLuint& buffer) noexcept {
    if (buffer == 0) {
        return;
    }
    glDeleteBuffers(1, &buffer);
    device_.buffers_.erase(buffer);
    buffer = 0;
}

FramebufferResource Device::Session::createFramebuffer(
    PixelFormat format,
    std::uint32_t width,
    std::uint32_t height,
    TextureWrap wrap,
    bool depthAttachment
) {
    const GLPixelFormat pixelFormat = framebufferPixelFormat(format);
    validateResourceDimensions(width, height, pixelFormat.bytesPerPixel, "Framebuffer");
    validateMaximumTextureSize(width, height, "Framebuffer");

    FramebufferResource result(0, 0, 0, width, height, format);
    try {
        glGenTextures(1, &result.colorTexture);
        if (result.colorTexture != 0) {
            device_.textures_.insert(result.colorTexture);
        }
        glBindTexture(GL_TEXTURE_2D, result.colorTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrap(wrap));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrap(wrap));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            pixelFormat.internalFormat,
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            0,
            pixelFormat.externalFormat,
            pixelFormat.type,
            nullptr
        );

        glGenFramebuffers(1, &result.framebuffer);
        if (result.framebuffer != 0) {
            device_.framebuffers_.insert(result.framebuffer);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, result.framebuffer);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            result.colorTexture,
            0
        );
        const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);
        if (depthAttachment) {
            ensureDepthAttachment(result);
        }
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::ostringstream message;
            message << "Framebuffer is incomplete (status 0x" << std::hex
                    << status << ')';
            throw Error(ErrorCode::framebufferCreation, message.str());
        }
        checkError(ErrorCode::framebufferCreation, "Creating framebuffer resources");
        return result;
    } catch (...) {
        destroyFramebuffer(result);
        throw;
    }
}

void Device::Session::ensureDepthAttachment(FramebufferResource& framebuffer) {
    if (framebuffer.framebuffer == 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "A color framebuffer is required before adding depth storage"
        );
    }
    if (framebuffer.depthRenderbuffer != 0) {
        return;
    }
    GLuint candidate = 0;
    bool tracked = false;
    try {
        glGenRenderbuffers(1, &candidate);
        checkError(
            ErrorCode::framebufferCreation,
            "Generating framebuffer depth storage"
        );
        if (candidate == 0) {
            throw Error(
                ErrorCode::framebufferCreation,
                "OpenGL returned renderbuffer zero for framebuffer depth storage"
            );
        }
        device_.renderbuffers_.insert(candidate);
        tracked = true;
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.framebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, candidate);
        glRenderbufferStorage(
            GL_RENDERBUFFER,
            GL_DEPTH_COMPONENT24,
            static_cast<GLsizei>(framebuffer.width),
            static_cast<GLsizei>(framebuffer.height)
        );
        glFramebufferRenderbuffer(
            GL_FRAMEBUFFER,
            GL_DEPTH_ATTACHMENT,
            GL_RENDERBUFFER,
            candidate
        );
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::ostringstream message;
            message << "Framebuffer is incomplete after adding depth storage (status 0x"
                    << std::hex << status << ')';
            throw Error(ErrorCode::framebufferCreation, message.str());
        }
        checkError(
            ErrorCode::framebufferCreation,
            "Adding framebuffer depth storage"
        );
        framebuffer.depthRenderbuffer = candidate;
        candidate = 0;
    } catch (...) {
        if (candidate != 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.framebuffer);
            glFramebufferRenderbuffer(
                GL_FRAMEBUFFER,
                GL_DEPTH_ATTACHMENT,
                GL_RENDERBUFFER,
                0
            );
            glDeleteRenderbuffers(1, &candidate);
            if (tracked) {
                device_.renderbuffers_.erase(candidate);
            }
        }
        throw;
    }
}

void Device::Session::destroyFramebuffer(FramebufferResource& framebuffer) noexcept {
    if (framebuffer.depthRenderbuffer != 0) {
        glDeleteRenderbuffers(1, &framebuffer.depthRenderbuffer);
        device_.renderbuffers_.erase(framebuffer.depthRenderbuffer);
    }
    if (framebuffer.framebuffer != 0) {
        glDeleteFramebuffers(1, &framebuffer.framebuffer);
        device_.framebuffers_.erase(framebuffer.framebuffer);
    }
    if (framebuffer.colorTexture != 0) {
        glDeleteTextures(1, &framebuffer.colorTexture);
        device_.textures_.erase(framebuffer.colorTexture);
    }
    framebuffer.framebuffer = 0;
    framebuffer.colorTexture = 0;
    framebuffer.depthRenderbuffer = 0;
    framebuffer.width = 0;
    framebuffer.height = 0;
    framebuffer.format = PixelFormat::rgba8;
}

AssetTextureResource Device::Session::uploadTexture(
    const Texture& texture,
    std::string_view source
) {
    const bool isVideo = texture.isVideoMp4 || (texture.flags & textureFlagVideo) != 0;
    if (texture.imageCount == 0 || texture.images.size() != texture.imageCount) {
        throw Error(
            ErrorCode::textureUpload,
            "Texture image table is inconsistent in '" + std::string(source) + "'"
        );
    }
    if (texture.imageCount > static_cast<std::uint32_t>(
            std::numeric_limits<GLsizei>::max())) {
        throw Error(
            ErrorCode::textureUpload,
            "Texture image count exceeds OpenGL's signed range in '" +
                std::string(source) + "'"
        );
    }

    const TextureUploadFormat format = uploadFormat(texture, source);
    // Uploading is a resource operation, not a draw-state change. Preserve the
    // caller's active unit and binding so lazy uploads cannot overwrite slots
    // that were already prepared for the current pass.
    const TextureBindingGuard textureBinding;
    AssetTextureResource result;
    result.images.resize(texture.imageCount);
    result.imageWidths.resize(texture.imageCount);
    result.imageHeights.resize(texture.imageCount);
    result.format = texture.format;
    result.flags = texture.flags;
    result.frames = texture.frames;
    result.spritesheetColumns = texture.spritesheetColumns;
    result.spritesheetRows = texture.spritesheetRows;
    result.spritesheetFrameCount = texture.spritesheetFrameCount;
    result.spritesheetDuration = texture.spritesheetDuration;
    try {
        if (isVideo) {
            if (texture.images.empty() || texture.images.front().mipmaps.empty()) {
                throw Error(
                    ErrorCode::textureUpload,
                    "Video-backed TEX asset has no encoded media payload: '" +
                        std::string(source) + "'"
                );
            }
            const TextureMipmap& mipmap = texture.images.front().mipmaps.front();
            result.videoDecoder = createVideoDecoder(
                mipmap.bytes.data(), mipmap.bytes.size(), std::string(source).c_str()
            );
            if (result.videoDecoder == nullptr) {
                throw Error(
                    ErrorCode::textureDecode,
                    "Unable to initialize AVFoundation decoder for video texture '" +
                        std::string(source) + "'"
                );
            }
            VideoFrame frame;
            if (!copyLatestVideoFrame(result.videoDecoder, frame) ||
                frame.bytes == nullptr || frame.byteCount == 0 ||
                frame.bytesPerRow < static_cast<std::size_t>(frame.width) * 4 ||
                frame.bytesPerRow % 4 != 0) {
                throw Error(
                    ErrorCode::textureDecode,
                    "Unable to decode the first frame of video texture '" +
                        std::string(source) + "'"
                );
            }
            validateResourceDimensions(
                frame.width, frame.height, 4, "Video texture frame"
            );
            validateMaximumTextureSize(
                frame.width, frame.height, "Video texture frame"
            );
            if (frame.bytesPerRow / 4 > static_cast<std::size_t>(
                    std::numeric_limits<GLint>::max())) {
                throw Error(
                    ErrorCode::textureUpload,
                    "Decoded video row length exceeds OpenGL's signed range"
                );
            }
            result.images.resize(1);
            result.imageWidths.resize(1);
            result.imageHeights.resize(1);
            glGenTextures(1, result.images.data());
            if (result.images.front() == 0) {
                throw Error(ErrorCode::textureUpload, "OpenGL returned texture zero for video texture");
            }
            device_.textures_.insert(result.images.front());
            glBindTexture(GL_TEXTURE_2D, result.images.front());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            configureTextureParameters(texture, 1);
            const PixelStoreGuard pixelStore(
                GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH
            );
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(
                GL_UNPACK_ROW_LENGTH,
                static_cast<GLint>(frame.bytesPerRow / 4)
            );
            glTexImage2D(
                GL_TEXTURE_2D, 0, GL_RGBA8,
                static_cast<GLsizei>(frame.width), static_cast<GLsizei>(frame.height),
                0,
                frame.pixelFormat == VideoFramePixelFormat::bgra8
                    ? GL_BGRA : GL_RGBA,
                GL_UNSIGNED_BYTE,
                frame.bytes
            );
            checkError(ErrorCode::textureUpload, "Uploading the first video texture frame");
            result.imageWidths.front() = frame.width;
            result.imageHeights.front() = frame.height;
            result.resolution = {
                static_cast<float>(frame.width), static_cast<float>(frame.height),
                static_cast<float>(texture.width), static_cast<float>(texture.height),
            };
            result.lastUploadedVideoFrameSerial = frame.serial;
            result.video = true;
            return result;
        }
        glGenTextures(
            static_cast<GLsizei>(result.images.size()),
            result.images.data()
        );
        for (const GLuint identifier : result.images) {
            if (identifier == 0) {
                throw Error(
                    ErrorCode::textureUpload,
                    "OpenGL returned texture zero while uploading '" +
                        std::string(source) + "'"
                );
            }
            device_.textures_.insert(identifier);
        }

        const PixelStoreGuard pixelStore(GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        for (std::size_t imageIndex = 0;
             imageIndex < texture.images.size();
             ++imageIndex) {
            const TextureImage& image = texture.images[imageIndex];
            if (image.mipmaps.empty()) {
                throw Error(
                    ErrorCode::textureUpload,
                    "Texture image has no mipmaps in '" + std::string(source) + "'"
                );
            }
            glBindTexture(GL_TEXTURE_2D, result.images[imageIndex]);
            configureTextureParameters(texture, image.mipmaps.size());
            for (std::size_t level = 0; level < image.mipmaps.size(); ++level) {
                const TextureMipmap& mipmap = image.mipmaps[level];
                std::uint32_t width = mipmap.width;
                std::uint32_t height = mipmap.height;
                const void* pixels = mipmap.bytes.data();
                std::size_t byteCount = mipmap.bytes.size();
                std::optional<DecodedImage> decoded;
                if (texture.fileFormat != TextureFileFormat::unknown) {
                    decoded = decodeEmbeddedImage(mipmap.bytes, source);
                    width = decoded->width;
                    height = decoded->height;
                    pixels = decoded->rgba8.data();
                    byteCount = decoded->rgba8.size();
                }
                validateResourceDimensions(
                    width,
                    height,
                    format.compressed ? 1 : format.bytesPerPixel,
                    "Texture mipmap"
                );
                validateMaximumTextureSize(width, height, "Texture mipmap");
                if (level == 0) {
                    result.imageWidths[imageIndex] = width;
                    result.imageHeights[imageIndex] = height;
                }
                if (format.compressed) {
                    const std::size_t expectedByteCount =
                        TextureParser::expectedBlockCompressedSize(
                            texture.format,
                            width,
                            height
                        );
                    if (byteCount != expectedByteCount) {
                        throw Error(
                            ErrorCode::textureUpload,
                            "Compressed mipmap byte count mismatch in '" +
                                std::string(source) + "': expected " +
                                std::to_string(expectedByteCount) + ", provided " +
                                std::to_string(byteCount)
                        );
                    }
                    if (byteCount > static_cast<std::size_t>(
                            std::numeric_limits<GLsizei>::max())) {
                        throw Error(
                            ErrorCode::textureUpload,
                            "Compressed mipmap exceeds OpenGL's signed range in '" +
                                std::string(source) + "'"
                        );
                    }
                    glCompressedTexImage2D(
                        GL_TEXTURE_2D,
                        static_cast<GLint>(level),
                        static_cast<GLenum>(format.internalFormat),
                        static_cast<GLsizei>(width),
                        static_cast<GLsizei>(height),
                        0,
                        static_cast<GLsizei>(byteCount),
                        pixels
                    );
                } else {
                    glTexImage2D(
                        GL_TEXTURE_2D,
                        static_cast<GLint>(level),
                        format.internalFormat,
                        static_cast<GLsizei>(width),
                        static_cast<GLsizei>(height),
                        0,
                        format.externalFormat,
                        format.type,
                        pixels
                    );
                }
                checkError(ErrorCode::textureUpload, "Uploading a texture mipmap");
            }
        }
        if (texture.isAnimated()) {
            result.resolution = {
                static_cast<float>(texture.textureWidth),
                static_cast<float>(texture.textureHeight),
                static_cast<float>(texture.gifWidth),
                static_cast<float>(texture.gifHeight),
            };
        } else if (texture.fileFormat != TextureFileFormat::unknown) {
            result.resolution = {
                static_cast<float>(result.imageWidths.front()),
                static_cast<float>(result.imageHeights.front()),
                static_cast<float>(texture.width),
                static_cast<float>(texture.height),
            };
        } else {
            result.resolution = {
                static_cast<float>(texture.textureWidth),
                static_cast<float>(texture.textureHeight),
                static_cast<float>(texture.width),
                static_cast<float>(texture.height),
            };
        }
        return result;
    } catch (...) {
        destroyTexture(result);
        throw;
    }
}

void Device::Session::requestVideoTextureFrame(
    AssetTextureResource& texture,
    double timeSeconds
) {
    if (!texture.video || texture.videoDecoder == nullptr) return;
    if (!requestVideoFrame(texture.videoDecoder, timeSeconds)) {
        throw Error(ErrorCode::textureDecode, "Unable to decode a video texture frame");
    }
}

bool Device::Session::updateVideoTexture(
    AssetTextureResource& texture,
    std::uint64_t frameSequence
) {
    if (!texture.video || texture.videoDecoder == nullptr ||
        texture.images.empty()) {
        return false;
    }
    if (texture.lastVideoUpdateFrame == frameSequence) return false;
    texture.lastVideoUpdateFrame = frameSequence;

    VideoFrame frame;
    if (!copyLatestVideoFrame(texture.videoDecoder, frame) ||
        frame.bytes == nullptr || frame.byteCount == 0 ||
        frame.bytesPerRow < static_cast<std::size_t>(frame.width) * 4 ||
        frame.bytesPerRow % 4 != 0) {
        throw Error(ErrorCode::textureDecode, "Unable to decode a video texture frame");
    }
    if (frame.serial == texture.lastUploadedVideoFrameSerial) return false;
    validateResourceDimensions(
        frame.width, frame.height, 4, "Video texture frame"
    );
    validateMaximumTextureSize(
        frame.width, frame.height, "Video texture frame"
    );
    if (frame.bytesPerRow / 4 > static_cast<std::size_t>(
            std::numeric_limits<GLint>::max())) {
        throw Error(
            ErrorCode::textureUpload,
            "Decoded video row length exceeds OpenGL's signed range"
        );
    }
    const TextureBindingGuard textureBinding;
    const PixelStoreGuard pixelStore(
        GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH
    );
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(
        GL_UNPACK_ROW_LENGTH,
        static_cast<GLint>(frame.bytesPerRow / 4)
    );
    glBindTexture(GL_TEXTURE_2D, texture.images.front());
    if (texture.imageWidths.front() != frame.width || texture.imageHeights.front() != frame.height) {
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8,
            static_cast<GLsizei>(frame.width), static_cast<GLsizei>(frame.height),
            0,
            frame.pixelFormat == VideoFramePixelFormat::bgra8
                ? GL_BGRA : GL_RGBA,
            GL_UNSIGNED_BYTE,
            frame.bytes
        );
        texture.imageWidths.front() = frame.width;
        texture.imageHeights.front() = frame.height;
        texture.resolution[0] = static_cast<float>(frame.width);
        texture.resolution[1] = static_cast<float>(frame.height);
    } else {
        glTexSubImage2D(
            GL_TEXTURE_2D, 0, 0, 0,
            static_cast<GLsizei>(frame.width), static_cast<GLsizei>(frame.height),
            frame.pixelFormat == VideoFramePixelFormat::bgra8
                ? GL_BGRA : GL_RGBA,
            GL_UNSIGNED_BYTE,
            frame.bytes
        );
    }
    checkError(ErrorCode::textureUpload, "Updating a video texture frame");
    texture.lastUploadedVideoFrameSerial = frame.serial;
    return true;
}

void Device::Session::destroyTexture(AssetTextureResource& texture) noexcept {
    if (texture.videoDecoder != nullptr) {
        destroyVideoDecoder(texture.videoDecoder);
        texture.videoDecoder = nullptr;
    }
    if (!texture.images.empty()) {
        for (const GLuint identifier : texture.images) {
            device_.textures_.erase(identifier);
        }
        glDeleteTextures(
            static_cast<GLsizei>(texture.images.size()),
            texture.images.data()
        );
    }
    texture.images.clear();
    texture.imageWidths.clear();
    texture.imageHeights.clear();
    texture.resolution = {};
    texture.format = TextureFormat::unknown;
    texture.flags = textureFlagNone;
    texture.frames.clear();
    texture.spritesheetColumns = 0;
    texture.spritesheetRows = 0;
    texture.spritesheetFrameCount = 0;
    texture.spritesheetDuration = 0.0F;
    texture.lastUploadedVideoFrameSerial = 0;
    texture.lastVideoUpdateFrame = 0;
    texture.video = false;
}

GLuint Device::Session::uploadCoverageTexture(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t bytesPerRow,
    std::span<const std::uint8_t> coverage
) {
    validateResourceDimensions(width, height, 1, "Text coverage texture");
    validateMaximumTextureSize(width, height, "Text coverage texture");
    if (bytesPerRow < width ||
        static_cast<std::size_t>(bytesPerRow) * height != coverage.size()) {
        throw Error(ErrorCode::invalidArgument, "Text coverage row layout is invalid");
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        throw Error(ErrorCode::textureUpload, "Creating a text coverage texture failed");
    }
    device_.textures_.insert(texture);
    try {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const PixelStoreGuard pixelStore(GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bytesPerRow));
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_R8,
            static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0,
            GL_RED, GL_UNSIGNED_BYTE, coverage.data()
        );
        checkError(ErrorCode::textureUpload, "Uploading a text coverage texture");
        return texture;
    } catch (...) {
        destroyTexture(texture);
        throw;
    }
}

GLuint Device::Session::uploadRGBA8Texture(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> pixels
) {
    validateResourceDimensions(width, height, 4, "Host RGBA8 texture");
    validateMaximumTextureSize(width, height, "Host RGBA8 texture");
    if (static_cast<std::size_t>(width) >
        std::numeric_limits<std::size_t>::max() /
            static_cast<std::size_t>(height) / 4) {
        throw Error(
            ErrorCode::invalidArgument,
            "Host RGBA8 texture byte count overflows size_t"
        );
    }
    const std::size_t expected = static_cast<std::size_t>(width) * height * 4;
    if (pixels.size() != expected) {
        throw Error(
            ErrorCode::invalidArgument,
            "Host RGBA8 texture requires tightly packed pixel storage"
        );
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        throw Error(
            ErrorCode::textureUpload,
            "Creating a host RGBA8 texture failed"
        );
    }
    device_.textures_.insert(texture);
    try {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const PixelStoreGuard pixelStore(
            GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH
        );
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            pixels.data()
        );
        checkError(
            ErrorCode::textureUpload,
            "Uploading a host RGBA8 texture"
        );
        return texture;
    } catch (...) {
        destroyTexture(texture);
        throw;
    }
}

void Device::Session::destroyTexture(GLuint& texture) noexcept {
    if (texture == 0) return;
    device_.textures_.erase(texture);
    glDeleteTextures(1, &texture);
    texture = 0;
}

void Device::Session::readRGBA8(
    const FramebufferResource& framebuffer,
    std::span<std::uint8_t> output,
    ReadbackSourceOrientation sourceOrientation
) {
    validateResourceDimensions(
        framebuffer.width,
        framebuffer.height,
        4,
        "RGBA8 readback"
    );
    const std::size_t required =
        static_cast<std::size_t>(framebuffer.width) * framebuffer.height * 4;
    if (output.size() < required) {
        throw Error(
            ErrorCode::invalidArgument,
            "RGBA8 output buffer is too small: expected " +
                std::to_string(required) + " bytes, provided " +
                std::to_string(output.size())
        );
    }
    if (framebuffer.framebuffer == 0) {
        throw Error(ErrorCode::readback, "A framebuffer is required for readback");
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    const PixelStoreGuard pixelStore(GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(
        0,
        0,
        static_cast<GLsizei>(framebuffer.width),
        static_cast<GLsizei>(framebuffer.height),
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        output.data()
    );
    checkError(ErrorCode::readback, "Reading an offscreen framebuffer");

    if (sourceOrientation ==
        ReadbackSourceOrientation::wallpaperEngineTopLeft) {
        // Wallpaper Engine's first logical (top) row already lives at the
        // framebuffer's first OpenGL (bottom) row, which is also the first row
        // returned by glReadPixels.
        return;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(framebuffer.width) * 4;
    for (std::size_t topRow = 0; topRow < framebuffer.height / 2; ++topRow) {
        const std::size_t bottomRow = framebuffer.height - topRow - 1;
        std::swap_ranges(
            output.data() + topRow * rowBytes,
            output.data() + (topRow + 1) * rowBytes,
            output.data() + bottomRow * rowBytes
        );
    }
}

void Device::Session::checkError(ErrorCode code, const char* operation) const {
    const GLenum firstError = glGetError();
    if (firstError == GL_NO_ERROR) return;

    bool outOfMemory = firstError == GL_OUT_OF_MEMORY;
    for (GLenum error = glGetError(); error != GL_NO_ERROR;
         error = glGetError()) {
        outOfMemory = outOfMemory || error == GL_OUT_OF_MEMORY;
    }
    const GLenum reportedError = outOfMemory
        ? GL_OUT_OF_MEMORY : firstError;
    throw Error(
        outOfMemory ? ErrorCode::internalFailure : code,
        std::string(operation) + " failed: " +
            glErrorMessage(reportedError)
    );
}

Device::Device() {
    const std::array<CGLPixelFormatAttribute, 5> attributes = {
        kCGLPFAOpenGLProfile,
        static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core),
        kCGLPFAAccelerated,
        kCGLPFAAllowOfflineRenderers,
        static_cast<CGLPixelFormatAttribute>(0),
    };
    CGLPixelFormatObj pixelFormat = nullptr;
    GLint pixelFormatCount = 0;
    CGLError result = CGLChoosePixelFormat(
        attributes.data(),
        &pixelFormat,
        &pixelFormatCount
    );
    if (result != kCGLNoError || pixelFormat == nullptr || pixelFormatCount == 0) {
        if (pixelFormat != nullptr) {
            CGLReleasePixelFormat(pixelFormat);
        }
        throw Error(
            ErrorCode::contextCreation,
            result != kCGLNoError
                ? "Selecting an OpenGL 4.1 Core pixel format failed: " +
                    cglErrorMessage(result)
                : "No OpenGL 4.1 Core pixel format is available"
        );
    }
    result = CGLCreateContext(pixelFormat, nullptr, &context_);
    CGLReleasePixelFormat(pixelFormat);
    if (result != kCGLNoError || context_ == nullptr) {
        throw Error(
            ErrorCode::contextCreation,
            "Creating an OpenGL 4.1 Core context failed: " +
                cglErrorMessage(result)
        );
    }
    try {
        validateContext();
    } catch (...) {
        CGLDestroyContext(context_);
        context_ = nullptr;
        throw;
    }
}

Device::Device(CGLContextObj borrowedContext)
    : context_(borrowedContext), ownsContext_(false) {
    if (context_ == nullptr) {
        throw Error(ErrorCode::invalidArgument, "A borrowed CGL context is required");
    }
    validateContext();
}

Device::~Device() {
    if (context_ == nullptr) {
        return;
    }
    try {
        Session current(*this);
        destroyTrackedResources();
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "SceneGL failed to release context resources: %s\n",
            error.what()
        );
    }
    if (!ownsContext_) {
        context_ = nullptr;
        return;
    }
    if (CGLGetCurrentContext() == context_) {
        static_cast<void>(CGLSetCurrentContext(nullptr));
    }
    const CGLError result = CGLDestroyContext(context_);
    if (result != kCGLNoError) {
        std::fprintf(
            stderr,
            "SceneGL failed to destroy its CGL context: %s\n",
            cglErrorMessage(result).c_str()
        );
    }
    context_ = nullptr;
}

Device::Session Device::activate() {
    return Session(*this);
}

void Device::validateContext() {
    Session current(*this);
    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    current.checkError(ErrorCode::unsupportedContext, "Querying the OpenGL version");
    if (major < 4 || (major == 4 && minor < 1)) {
        throw Error(
            ErrorCode::unsupportedContext,
            "SceneGL requires OpenGL 4.1 Core, but the context is " +
                std::to_string(major) + "." + std::to_string(minor)
        );
    }
}

void Device::destroyTrackedResources() noexcept {
    if (!buffers_.empty()) {
        std::vector<GLuint> values(buffers_.begin(), buffers_.end());
        glDeleteBuffers(static_cast<GLsizei>(values.size()), values.data());
        buffers_.clear();
    }
    if (!vertexArrays_.empty()) {
        std::vector<GLuint> values(vertexArrays_.begin(), vertexArrays_.end());
        glDeleteVertexArrays(static_cast<GLsizei>(values.size()), values.data());
        vertexArrays_.clear();
    }
    for (const GLuint program : programs_) {
        glDeleteProgram(program);
    }
    programs_.clear();
    if (!renderbuffers_.empty()) {
        std::vector<GLuint> values(renderbuffers_.begin(), renderbuffers_.end());
        glDeleteRenderbuffers(static_cast<GLsizei>(values.size()), values.data());
        renderbuffers_.clear();
    }
    if (!framebuffers_.empty()) {
        std::vector<GLuint> values(framebuffers_.begin(), framebuffers_.end());
        glDeleteFramebuffers(static_cast<GLsizei>(values.size()), values.data());
        framebuffers_.clear();
    }
    if (!textures_.empty()) {
        std::vector<GLuint> values(textures_.begin(), textures_.end());
        glDeleteTextures(static_cast<GLsizei>(values.size()), values.data());
        textures_.clear();
    }
}

}  // namespace we::scene::gl
