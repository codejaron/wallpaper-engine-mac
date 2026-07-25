#include <SceneGL/FramePlanExecutor.hpp>

#include "SceneGLDevice.hpp"
#include "TextCoverageRenderer.hpp"

#include <SceneCore/FormatError.hpp>
#include <SceneCore/Runtime.hpp>
#include <SceneShader/ShaderCompiler.hpp>
#include <SceneShader/ShaderPreprocessor.hpp>
#include <SceneText/SceneText.hpp>

#include <OpenGL/gl3.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace we::scene::gl {
namespace {

struct ActiveUniform final {
    std::string name;
    GLenum type = 0;
    GLint size = 0;
    GLint blockIndex = -1;
    GLint location = -1;
    bool isArray = false;
};

struct ProgramResource final {
    GLuint program = 0;
    std::vector<ShaderParameterMetadata> parameters;
    std::vector<ActiveUniform> uniforms;
};

static_assert(std::is_nothrow_move_assignable_v<ProgramResource>);

struct Vertex final {
    float position[3];
    float texCoord[2];
};

struct ParticleVertex final {
    float position[3];
    float texCoordRotationSize[4];
    float color[4];
    float velocityLifetime[4];
    float rotationXY[2];
};

static_assert(sizeof(ParticleVertex) == sizeof(float) * 17);

struct ParticleAtlasMetadata final {
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    std::uint32_t frames = 0;
    float duration = 0.0F;
    float frameAspect = 1.0F;

    [[nodiscard]] bool enabled() const noexcept { return frames > 0; }
};

struct ParticleDrawBatch final {
    std::vector<ParticleVertex> vertices;
    std::vector<std::uint32_t> indices;
    ParticleAtlasMetadata atlas;
};

struct ResolvedFrameInputs final {
    FrameVector2 pointerPosition;
    FrameVector2 pointerPositionLast;
    double timeSeconds = 0.0;
    double frameTimeSeconds = 0.0;
    float daytime = 0.0F;
};

struct TextureAnimationSelection final {
    std::size_t imageIndex = 0;
    std::array<float, 2> translation{0.0F, 0.0F};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 0.0F};
    bool animated = false;
};

TextureAnimationSelection selectTextureAnimation(
    const AssetTextureResource& texture,
    double timeSeconds
) {
    TextureAnimationSelection selection;
    if (!texture.isAnimated() || texture.frames.empty()) return selection;

    double duration = 0.0;
    const TextureFrame* lastPositiveDurationFrame = nullptr;
    for (const auto& frame : texture.frames) {
        if (!std::isfinite(frame.frameTime) || frame.frameTime < 0.0F) {
            throw Error(
                ErrorCode::resourceValidation,
                "Animated texture contains a negative or non-finite frame duration"
            );
        }
        duration += static_cast<double>(frame.frameTime);
        if (frame.frameTime > 0.0F) lastPositiveDurationFrame = &frame;
    }
    if (!std::isfinite(duration) || duration <= 0.0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Animated texture has a non-positive or non-finite total duration"
        );
    }

    const double phase = std::fmod(timeSeconds, duration);
    const TextureFrame* selected = nullptr;
    double remaining = phase;
    for (const auto& frame : texture.frames) {
        remaining -= static_cast<double>(frame.frameTime);
        if (remaining <= 0.0) {
            selected = &frame;
            break;
        }
    }
    // fmod is strictly below duration, but accumulated floating-point error can
    // leave a positive remainder. The final positive-duration timeline entry
    // owns that tail; trailing zero-duration entries do not occupy any time.
    if (selected == nullptr) selected = lastPositiveDurationFrame;

    selection.imageIndex = selected->frameNumber;
    if (selection.imageIndex >= texture.images.size() ||
        selection.imageIndex >= texture.imageWidths.size() ||
        selection.imageIndex >= texture.imageHeights.size()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Animated texture frame references an image outside the uploaded resource"
        );
    }
    const std::uint32_t width = texture.imageWidths[selection.imageIndex];
    const std::uint32_t height = texture.imageHeights[selection.imageIndex];
    if (width == 0 || height == 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Animated texture frame references an image with zero dimensions"
        );
    }
    const float inverseWidth = 1.0F / static_cast<float>(width);
    const float inverseHeight = 1.0F / static_cast<float>(height);
    selection.translation = {
        selected->x * inverseWidth,
        selected->y * inverseHeight,
    };
    selection.rotation = {
        selected->width * inverseWidth,
        selected->widthAux * inverseWidth,
        selected->heightAux * inverseHeight,
        selected->height * inverseHeight,
    };
    selection.animated = true;
    return selection;
}

PixelFormat pixelFormat(FramebufferFormat format) {
    switch (format) {
        case FramebufferFormat::rgba8: return PixelFormat::rgba8;
        case FramebufferFormat::r8: return PixelFormat::r8;
        case FramebufferFormat::rg16f: return PixelFormat::rg16f;
        case FramebufferFormat::r16f: return PixelFormat::r16f;
    }
    throw Error(ErrorCode::resourceValidation, "Unknown frame-plan format");
}

TextureWrap textureWrap(FramebufferWrapMode wrap) {
    switch (wrap) {
        case FramebufferWrapMode::clampToEdge: return TextureWrap::clampToEdge;
        case FramebufferWrapMode::clampToBorder: return TextureWrap::clampToBorder;
        case FramebufferWrapMode::repeat: return TextureWrap::repeat;
    }
    throw Error(ErrorCode::resourceValidation, "Unknown frame-plan wrap mode");
}

bool sameDescriptor(
    const FramebufferDescriptor& lhs,
    const FramebufferDescriptor& rhs
) {
    return lhs.resource == rhs.resource && lhs.format == rhs.format &&
        lhs.wrapMode == rhs.wrapMode && lhs.width == rhs.width &&
        lhs.height == rhs.height && lhs.scale == rhs.scale &&
        lhs.unique == rhs.unique;
}

static_assert(std::is_nothrow_move_assignable_v<FramebufferResource>);
static_assert(std::is_nothrow_swappable_v<FramebufferDescriptor>);
static_assert(
    std::is_nothrow_swappable_v<std::map<std::string, std::string>>
);
static_assert(noexcept(std::declval<Device::Session&>().destroyFramebuffer(
    std::declval<FramebufferResource&>()
)));

struct PresentationRect final {
    GLint x = 0;
    GLint y = 0;
    GLsizei width = 0;
    GLsizei height = 0;
};

struct PresentationTransform final {
    GLsizei sourceWidth = 0;
    GLsizei sourceHeight = 0;
    GLsizei drawableWidth = 0;
    GLsizei drawableHeight = 0;
    PresentationRect source;
    PresentationRect destination;

    [[nodiscard]] FrameVector2 map(FrameVector2 drawablePoint) const {
        drawablePoint.x = std::clamp(drawablePoint.x, 0.0, 1.0);
        drawablePoint.y = std::clamp(drawablePoint.y, 0.0, 1.0);
        const double pixelX = std::clamp(
            drawablePoint.x * drawableWidth,
            static_cast<double>(destination.x),
            static_cast<double>(destination.x + destination.width)
        );
        const double pixelY = std::clamp(
            drawablePoint.y * drawableHeight,
            static_cast<double>(destination.y),
            static_cast<double>(destination.y + destination.height)
        );
        const double sourceX = source.x +
            (pixelX - destination.x) * source.width / destination.width;
        const double sourceY = source.y +
            (pixelY - destination.y) * source.height / destination.height;
        return {
            sourceX / sourceWidth,
            sourceY / sourceHeight,
        };
    }
};

PresentationTransform presentationTransform(
    GLsizei sourceWidth,
    GLsizei sourceHeight,
    GLsizei drawableWidth,
    GLsizei drawableHeight,
    PresentationScaling scaling
) {
    if (sourceWidth <= 0 || sourceHeight <= 0 ||
        drawableWidth <= 0 || drawableHeight <= 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation dimensions must be greater than zero"
        );
    }
    PresentationTransform result{
        .sourceWidth = sourceWidth,
        .sourceHeight = sourceHeight,
        .drawableWidth = drawableWidth,
        .drawableHeight = drawableHeight,
        .source = {.width = sourceWidth, .height = sourceHeight},
        .destination = {.width = drawableWidth, .height = drawableHeight},
    };
    if (scaling == PresentationScaling::aspectFit) {
        const double scaleX = static_cast<double>(drawableWidth) / sourceWidth;
        const double scaleY = static_cast<double>(drawableHeight) / sourceHeight;
        const double scale = std::min(scaleX, scaleY);
        result.destination.width = std::clamp<GLsizei>(
            static_cast<GLsizei>(std::lround(sourceWidth * scale)),
            1,
            drawableWidth
        );
        result.destination.height = std::clamp<GLsizei>(
            static_cast<GLsizei>(std::lround(sourceHeight * scale)),
            1,
            drawableHeight
        );
        result.destination.x = (drawableWidth - result.destination.width) / 2;
        result.destination.y = (drawableHeight - result.destination.height) / 2;
    } else if (scaling == PresentationScaling::aspectFill) {
        const double sourceAspect = static_cast<double>(sourceWidth) / sourceHeight;
        const double drawableAspect = static_cast<double>(drawableWidth) / drawableHeight;
        if (sourceAspect > drawableAspect) {
            result.source.width = std::clamp<GLsizei>(
                static_cast<GLsizei>(std::lround(sourceHeight * drawableAspect)),
                1,
                sourceWidth
            );
            result.source.x = (sourceWidth - result.source.width) / 2;
        } else if (sourceAspect < drawableAspect) {
            result.source.height = std::clamp<GLsizei>(
                static_cast<GLsizei>(std::lround(sourceWidth / drawableAspect)),
                1,
                sourceHeight
            );
            result.source.y = (sourceHeight - result.source.height) / 2;
        }
    }
    return result;
}

std::string planIssues(const FramePlan& plan) {
    std::ostringstream message;
    message << "Frame plan revision " << plan.modelRevision
            << " is not executable";
    for (const auto& issue : plan.issues) {
        message << "\n- " << issue.message;
        if (!issue.assetPath.empty()) message << " [" << issue.assetPath << ']';
        if (!issue.jsonPointer.empty()) message << " at " << issue.jsonPointer;
    }
    return message.str();
}

std::array<float, 16> identityMatrix() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

using Matrix = std::array<float, 16>;

Matrix multiply(const Matrix& lhs, const Matrix& rhs) {
    Matrix result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int index = 0; index < 4; ++index) {
                result[column * 4 + row] +=
                    lhs[index * 4 + row] * rhs[column * 4 + index];
            }
        }
    }
    return result;
}

Matrix inverse(const Matrix& matrix, std::string_view description) {
    std::array<std::array<double, 8>, 4> augmented{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            augmented[row][column] = matrix[column * 4 + row];
        }
        augmented[row][row + 4] = 1.0;
    }

    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row) {
            if (std::abs(augmented[row][column]) >
                std::abs(augmented[pivot][column])) {
                pivot = row;
            }
        }
        const double divisor = augmented[pivot][column];
        if (!std::isfinite(divisor) ||
            std::abs(divisor) <= std::numeric_limits<double>::epsilon()) {
            throw Error(
                ErrorCode::resourceValidation,
                std::string(description) + " is singular"
            );
        }
        if (pivot != column) {
            std::swap(augmented[pivot], augmented[column]);
        }
        for (double& value : augmented[column]) value /= divisor;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column) continue;
            const double factor = augmented[row][column];
            for (std::size_t index = 0; index < 8; ++index) {
                augmented[row][index] -= factor * augmented[column][index];
            }
        }
    }

    Matrix result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const double value = augmented[row][column + 4];
            if (!std::isfinite(value) ||
                value > std::numeric_limits<float>::max() ||
                value < -std::numeric_limits<float>::max()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    std::string(description) +
                        " inverse contains a non-finite or out-of-range value"
                );
            }
            result[column * 4 + row] = static_cast<float>(value);
        }
    }
    return result;
}

float localDaytime() {
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) {
        throw Error(
            ErrorCode::internalFailure,
            "Reading local wall-clock time failed"
        );
    }
    std::tm local{};
    if (::localtime_r(&now, &local) == nullptr) {
        throw Error(
            ErrorCode::internalFailure,
            "Converting local wall-clock time failed"
        );
    }
    return static_cast<float>(local.tm_hour * 60 + local.tm_min) /
        (24.0F * 60.0F);
}

Matrix translation(float x, float y, float z) {
    Matrix result = identityMatrix();
    result[12] = x;
    result[13] = y;
    result[14] = z;
    return result;
}

Matrix scaling(float x, float y, float z) {
    Matrix result{};
    result[0] = x;
    result[5] = y;
    result[10] = z;
    result[15] = 1.0F;
    return result;
}

Matrix rotationX(float radians) {
    Matrix result = identityMatrix();
    result[5] = std::cos(radians);
    result[6] = std::sin(radians);
    result[9] = -std::sin(radians);
    result[10] = std::cos(radians);
    return result;
}

Matrix rotationY(float radians) {
    Matrix result = identityMatrix();
    result[0] = std::cos(radians);
    result[2] = -std::sin(radians);
    result[8] = std::sin(radians);
    result[10] = std::cos(radians);
    return result;
}

Matrix rotationZ(float radians) {
    Matrix result = identityMatrix();
    result[0] = std::cos(radians);
    result[1] = std::sin(radians);
    result[4] = -std::sin(radians);
    result[5] = std::cos(radians);
    return result;
}

Matrix orthographic(float left, float right, float bottom, float top, float near, float far) {
    if (!std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(bottom) || !std::isfinite(top) ||
        !std::isfinite(near) || !std::isfinite(far) ||
        left == right || bottom == top || near == far) {
        throw Error(ErrorCode::resourceValidation, "Degenerate orthographic projection");
    }
    Matrix result{};
    result[0] = 2.0F / (right - left);
    result[5] = 2.0F / (top - bottom);
    result[10] = -2.0F / (far - near);
    result[12] = -(right + left) / (right - left);
    result[13] = -(top + bottom) / (top - bottom);
    result[14] = -(far + near) / (far - near);
    result[15] = 1.0F;
    return result;
}

Matrix perspective(float fieldOfViewRadians, float aspect, float near, float far) {
    if (!std::isfinite(fieldOfViewRadians) ||
        !std::isfinite(aspect) || !std::isfinite(near) ||
        !std::isfinite(far) || fieldOfViewRadians <= 0.0F ||
        fieldOfViewRadians >= 3.14159265358979323846F ||
        aspect <= 0.0F || near <= 0.0F || far <= near) {
        throw Error(
            ErrorCode::resourceValidation,
            "Degenerate particle perspective projection"
        );
    }
    const float tangent = std::tan(fieldOfViewRadians * 0.5F);
    if (!std::isfinite(tangent) || tangent <= 0.0F) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective field of view is invalid"
        );
    }
    Matrix result{};
    result[0] = 1.0F / (aspect * tangent);
    result[5] = 1.0F / tangent;
    result[10] = -(far + near) / (far - near);
    result[11] = -1.0F;
    result[14] = -(2.0F * far * near) / (far - near);
    return result;
}

Vector3 subtract(Vector3 lhs, Vector3 rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vector3 normalized(Vector3 value) {
    const double length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (!std::isfinite(length) || length <= std::numeric_limits<double>::epsilon()) {
        throw Error(ErrorCode::resourceValidation, "Camera contains a zero-length direction");
    }
    return {value.x / length, value.y / length, value.z / length};
}

Vector3 cross(Vector3 lhs, Vector3 rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

double dot(Vector3 lhs, Vector3 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Matrix lookAt(const FrameCameraDescriptor& camera) {
    const Vector3 forward = normalized(subtract(camera.center, camera.eye));
    const Vector3 side = normalized(cross(forward, camera.up));
    const Vector3 up = cross(side, forward);
    Matrix result = identityMatrix();
    result[0] = float(side.x); result[4] = float(side.y); result[8] = float(side.z);
    result[1] = float(up.x); result[5] = float(up.y); result[9] = float(up.z);
    result[2] = float(-forward.x); result[6] = float(-forward.y); result[10] = float(-forward.z);
    result[12] = float(-dot(side, camera.eye));
    result[13] = float(-dot(up, camera.eye));
    result[14] = float(dot(forward, camera.eye));
    return result;
}

void validateCameraVector(Vector3 value, std::string_view name) {
    const auto valid = [](double component) {
        return std::isfinite(component) &&
            component <= std::numeric_limits<float>::max() &&
            component >= -std::numeric_limits<float>::max();
    };
    if (!valid(value.x) || !valid(value.y) || !valid(value.z)) {
        throw Error(
            ErrorCode::resourceValidation,
            "Camera " + std::string(name) + " contains a non-finite or out-of-range component"
        );
    }
}

Matrix sceneOrthographicViewProjection(
    const FrameCameraDescriptor& camera,
    float width,
    float height
) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0F || height <= 0.0F) {
        throw Error(
            ErrorCode::resourceValidation,
            "Camera orthographic projection dimensions must be finite and greater than zero"
        );
    }
    validateCameraVector(camera.center, "center");
    validateCameraVector(camera.eye, "eye");
    validateCameraVector(camera.up, "up");
    if (!std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
        camera.nearPlane == camera.farPlane) {
        throw Error(
            ErrorCode::resourceValidation,
            "Camera near and far planes must be finite and distinct"
        );
    }

    // linux-wallpaperengine b016d7d Camera::setOrthogonalProjection composes
    // the eye translation into the projection before applying glm::lookAt.
    // Keep that ordering for Wallpaper Engine scene-camera parity.
    const Matrix projection = orthographic(
        -width * 0.5F, width * 0.5F,
        -height * 0.5F, height * 0.5F,
        static_cast<float>(camera.nearPlane),
        static_cast<float>(camera.farPlane)
    );
    return multiply(
        multiply(
            projection,
            translation(
                static_cast<float>(camera.eye.x),
                static_cast<float>(camera.eye.y),
                static_cast<float>(camera.eye.z)
            )
        ),
        lookAt(camera)
    );
}

struct ParticleView final {
    Matrix viewProjection;
    std::array<float, 3> eyePosition{};
};

ParticleView particlePerspectiveView(
    const FrameCameraDescriptor& camera,
    float width,
    float height
) {
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0F || height <= 0.0F ||
        !std::isfinite(camera.fieldOfView) ||
        camera.fieldOfView <= 0.0 || camera.fieldOfView >= 180.0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective dimensions or field of view are invalid"
        );
    }
    if (!std::isfinite(camera.nearPlane) ||
        !std::isfinite(camera.farPlane) ||
        camera.nearPlane < 0.0 || camera.farPlane <= 0.0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective clipping planes are invalid"
        );
    }
    constexpr double degreesToRadians =
        0.017453292519943295769236907684886;
    const double fieldOfViewRadians = camera.fieldOfView * degreesToRadians;
    const double tangent = std::tan(fieldOfViewRadians * 0.5);
    const double eyeDistance = (static_cast<double>(height) * 0.5) / tangent;
    if (!std::isfinite(eyeDistance) || eyeDistance <= 0.0 ||
        eyeDistance > std::numeric_limits<float>::max()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective eye distance is invalid"
        );
    }
    constexpr double minimumNearPlane = 0.01;
    const double nearPlane = camera.nearPlane > 0.0
        ? camera.nearPlane : minimumNearPlane;
    const double minimumFarPlane = eyeDistance +
        std::max(static_cast<double>(width), static_cast<double>(height));
    const double farPlane = std::max(camera.farPlane, minimumFarPlane);
    if (!std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
        farPlane <= nearPlane ||
        farPlane > std::numeric_limits<float>::max()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective clipping planes are invalid"
        );
    }
    FrameCameraDescriptor particleCamera = camera;
    particleCamera.eye = {0.0, 0.0, eyeDistance};
    particleCamera.center = {0.0, 0.0, 0.0};
    particleCamera.up = {0.0, 1.0, 0.0};
    const Matrix projection = perspective(
        static_cast<float>(fieldOfViewRadians),
        width / height,
        static_cast<float>(nearPlane),
        static_cast<float>(farPlane)
    );
    return {
        .viewProjection = multiply(projection, lookAt(particleCamera)),
        .eyePosition = {0.0F, 0.0F, static_cast<float>(eyeDistance)},
    };
}

FrameVector2 sceneParallaxOffset(
    const FramePlan& plan,
    const FrameVector2& depth,
    const FrameVector2& displacement,
    std::string_view subject
) {
    const double referenceSize = static_cast<double>(plan.width);
    const FrameVector2 offset{
        .x = (depth.x + plan.parallax.amount) *
            displacement.x * referenceSize,
        .y = (depth.y + plan.parallax.amount) *
            displacement.y * referenceSize,
    };
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y) ||
        std::abs(offset.x) > std::numeric_limits<float>::max() ||
        std::abs(offset.y) > std::numeric_limits<float>::max()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Camera parallax produced a non-finite or out-of-range " +
                std::string(subject) + " offset"
        );
    }
    return offset;
}

void bindMatrix(GLint location, const std::array<float, 16>& value) {
    if (location >= 0) glUniformMatrix4fv(location, 1, GL_FALSE, value.data());
}

void bindMatrix3(GLint location, const std::array<float, 9>& value) {
    if (location >= 0) glUniformMatrix3fv(location, 1, GL_FALSE, value.data());
}

void bindVector2(GLint location, const std::array<float, 2>& value) {
    if (location >= 0) glUniform2fv(location, 1, value.data());
}

void bindVector4(GLint location, const std::array<float, 4>& value) {
    if (location >= 0) glUniform4fv(location, 1, value.data());
}

void bindVector3(GLint location, const std::array<float, 3>& value) {
    if (location >= 0) glUniform3fv(location, 1, value.data());
}

std::vector<GLfloat> numericComponents(
    const RuntimeValue& value,
    std::size_t count,
    std::string_view name
) {
    if (count > value.vector().size()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Uniform '" + std::string(name) + "' requests too many components"
        );
    }
    std::vector<GLfloat> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double number = value.vector()[index];
        if (!std::isfinite(number) ||
            number > std::numeric_limits<GLfloat>::max() ||
            number < -std::numeric_limits<GLfloat>::max()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + std::string(name) + "' contains a non-finite or out-of-range number"
            );
        }
        result.push_back(static_cast<GLfloat>(number));
    }
    return result;
}

RuntimeValue metadataDefault(const ShaderParameterDefault& input) {
    return std::visit([](const auto& value) -> RuntimeValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::vector<double>>) {
            if (value.size() < 2 || value.size() > 4) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Shader vector default must have two to four components"
                );
            }
            std::array<double, 4> components{};
            std::copy(value.begin(), value.end(), components.begin());
            return RuntimeValue::vector(components, value.size());
        } else if constexpr (std::is_same_v<T, std::string>) {
            return RuntimeValue::initialString(value);
        } else if constexpr (std::is_same_v<T, bool>) {
            return RuntimeValue::boolean(value);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return RuntimeValue::integer(value);
        } else {
            return RuntimeValue::floating(value);
        }
    }, input);
}

bool isSamplerParameter(const ShaderParameterMetadata& metadata) {
    return metadata.type.starts_with("sampler") ||
        metadata.type.starts_with("isampler") ||
        metadata.type.starts_with("usampler");
}

std::vector<ActiveUniform> activeUniforms(GLuint program) {
    GLint count = 0;
    GLint maximumNameLength = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maximumNameLength);
    std::vector<GLchar> buffer(
        static_cast<std::size_t>(std::max(1, maximumNameLength))
    );
    std::vector<ActiveUniform> result;
    result.reserve(static_cast<std::size_t>(std::max(0, count)));
    for (GLint index = 0; index < count; ++index) {
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(
            program, static_cast<GLuint>(index), maximumNameLength, &length,
            &size, &type, buffer.data()
        );
        std::string name(buffer.data(), static_cast<std::size_t>(length));
        const bool isArray = name.ends_with("[0]");
        if (isArray) {
            name.resize(name.size() - 3);
        }
        const GLuint uniformIndex = static_cast<GLuint>(index);
        GLint blockIndex = -1;
        glGetActiveUniformsiv(
            program, 1, &uniformIndex, GL_UNIFORM_BLOCK_INDEX, &blockIndex
        );
        const GLint location = glGetUniformLocation(program, name.c_str());
        result.push_back({
            .name = std::move(name),
            .type = type,
            .size = size,
            .blockIndex = blockIndex,
            .location = location,
            .isArray = isArray,
        });
    }
    return result;
}

const ActiveUniform* activeUniform(
    const ProgramResource& program,
    std::string_view name
) {
    const auto found = std::find_if(
        program.uniforms.begin(), program.uniforms.end(),
        [name](const ActiveUniform& uniform) { return uniform.name == name; }
    );
    return found == program.uniforms.end() ? nullptr : &*found;
}

GLint prepareBuiltinUniform(
    const ProgramResource& program,
    const std::string& name,
    GLenum expectedType
) {
    const ActiveUniform* uniform = activeUniform(program, name);
    if (uniform == nullptr) return -1;
    if (uniform->blockIndex >= 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin uniform '" + name +
                "' must not be declared in a uniform block"
        );
    }
    if (uniform->isArray || uniform->size != 1) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin uniform '" + name + "' must not be an array"
        );
    }
    if (uniform->type != expectedType) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin uniform '" + name + "' has an incompatible type"
        );
    }
    if (uniform->location < 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin uniform '" + name + "' has no bindable location"
        );
    }
    return uniform->location;
}

std::optional<int> textureSlot(std::string_view name) {
    constexpr std::string_view prefix = "g_Texture";
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return std::nullopt;
    }
    int slot = 0;
    for (const char digit : name.substr(prefix.size())) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        slot = slot * 10 + (digit - '0');
        if (slot >= 32) {
            return std::nullopt;
        }
    }
    return slot;
}

const std::string* samplerDefaultTexture(
    const std::vector<ShaderParameterMetadata>& parameters,
    std::string_view name
) {
    for (const auto& parameter : parameters) {
        if (parameter.name != name || !isSamplerParameter(parameter) ||
            !parameter.defaultValue) {
            continue;
        }
        if (const auto* value =
                std::get_if<std::string>(&*parameter.defaultValue)) {
            return value;
        }
        throw Error(
            ErrorCode::resourceValidation,
            "Sampler metadata default for '" + std::string(name) +
                "' must be a texture name"
        );
    }
    return nullptr;
}

bool isOpenGLSamplerType(GLenum type) {
    switch (type) {
        case GL_SAMPLER_1D:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_1D_SHADOW:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_2D_RECT:
        case GL_SAMPLER_2D_RECT_SHADOW:
        case GL_SAMPLER_1D_ARRAY:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_1D_ARRAY_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_BUFFER:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
        case GL_INT_SAMPLER_1D:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_2D_RECT:
        case GL_INT_SAMPLER_1D_ARRAY:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_1D:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
        case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
            return true;
        default:
            return false;
    }
}

void configureState(const FrameRenderPass& pass) {
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    switch (pass.blending) {
        case BlendingMode::normal:
            glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
            break;
        case BlendingMode::translucent:
            glBlendFuncSeparate(
                GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
            );
            break;
        case BlendingMode::additive:
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_SRC_ALPHA, GL_ONE);
            break;
    }
    pass.depthTest == DepthMode::enabled ? glEnable(GL_DEPTH_TEST)
                                         : glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(pass.depthWrite == DepthMode::enabled ? GL_TRUE : GL_FALSE);
    glFrontFace(GL_CCW);
    if (pass.culling == CullingMode::disabled) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, pass.writeAlpha ? GL_TRUE : GL_FALSE);
    // Image layers are 2D scene planes and commonly sit at z=0 even when a
    // project declares a small positive nearz. Clamping keeps that plane
    // visible without discarding the camera's near/far range for depth tests.
    const bool sceneGeometry =
        pass.geometry == FrameGeometryKind::imageScene ||
        pass.geometry == FrameGeometryKind::passthroughCapture;
    sceneGeometry ? glEnable(GL_DEPTH_CLAMP) : glDisable(GL_DEPTH_CLAMP);
}

}  // namespace

struct FramePlanExecutor::Impl final {
    struct CachedFramebuffer final {
        FramebufferDescriptor descriptor;
        FramebufferResource resource;
    };

    struct ParticleState final {
        std::string assetIdentity;
        particle::ParticleSimulation simulation;
    };

    struct PreparedUniform final {
        enum class Kind { integer, float1, float2, float3, float4 };

        GLint location = -1;
        Kind kind = Kind::float1;
        GLint integer = 0;
        std::array<GLfloat, 4> floating{};
    };

    struct PreparedTextureBinding final {
        int slot = 0;
        FrameResourceRef resource;
        GLuint assetTexture = 0;
        std::array<float, 4> resolution{};
        GLint samplerLocation = -1;
        GLint resolutionLocation = -1;
    };

    struct PreparedCommonUniforms final {
        GLint textureReductionScale = -1;
        GLint lightAmbientColor = -1;
        GLint lightSkylightColor = -1;
        GLint brightness = -1;
        GLint userAlpha = -1;
        GLint alpha = -1;
        GLint color = -1;
        GLint color4 = -1;
        GLint compositeColor = -1;
        GLint time = -1;
        GLint daytime = -1;
        GLint modelViewProjection = -1;
        GLint modelViewProjectionInverse = -1;
        GLint effectModelViewProjection = -1;
        GLint model = -1;
        GLint effectModel = -1;
        GLint normalModel = -1;
        GLint viewProjection = -1;
        GLint pointer = -1;
        GLint pointerLast = -1;
        GLint effectTextureProjection = -1;
        GLint effectTextureProjectionInverse = -1;
        GLint texelSize = -1;
        GLint texelSizeHalf = -1;
        bool compositeColorProvidedByShader = false;
        Matrix modelViewProjectionInverseValue = identityMatrix();
        std::array<float, 3> ambientColorValue{};
        std::array<float, 3> skylightColorValue{};
        float brightnessValue = 1.0F;
        float alphaValue = 1.0F;
        std::array<float, 3> colorValue{};
        std::array<float, 4> color4Value{};
        float timeValue = 0.0F;
        float daytimeValue = 0.0F;
        std::array<float, 2> pointerValue{};
        std::array<float, 2> pointerLastValue{};
        std::array<float, 2> texelSizeValue{};
        std::array<float, 2> texelSizeHalfValue{};
    };

    struct CommonRenderableValues final {
        float brightness = 0.0F;
        float alpha = 0.0F;
        std::array<float, 4> color{};
    };

    struct PreparedImageUniforms final {
        GLint texture0Translation = -1;
        GLint texture0Rotation = -1;
        GLint alternateModel = -1;
        GLint alternateViewProjection = -1;
    };

    struct PreparedDraw final {
        FrameRenderPass pass;
        GLuint program = 0;
        std::vector<PreparedTextureBinding> textures;
        TextureAnimationSelection texture0Animation;
        PreparedImageUniforms uniforms;
        PreparedCommonUniforms commonUniforms;
        Matrix model = identityMatrix();
        Matrix viewProjection = identityMatrix();
        Matrix modelViewProjection = identityMatrix();
        std::vector<PreparedUniform> materialUniforms;
        std::array<Vertex, 6> vertices{};
        GLint positionLocation = -1;
        GLint texCoordLocation = -1;
    };

    struct PreparedClear final {
        FrameResourceRef destination;
        std::array<GLfloat, 4> color{};
    };

    struct PreparedText final {
        FrameResourceRef destination;
        PreparedTextCoverage coverage;
        TextDrawRequest request;
    };

    struct PreparedParticleUniforms final {
        GLint texture0 = -1;
        GLint texture0Resolution = -1;
        GLint modelInverse = -1;
        GLint orientationUp = -1;
        GLint orientationRight = -1;
        GLint orientationForward = -1;
        GLint viewUp = -1;
        GLint viewRight = -1;
        GLint eyePosition = -1;
        GLint renderVar0 = -1;
        GLint renderVar1 = -1;
    };

    struct PreparedParticle final {
        FrameResourceRef destination;
        BlendingMode blending = BlendingMode::normal;
        CullingMode culling = CullingMode::disabled;
        DepthMode depthTest = DepthMode::disabled;
        DepthMode depthWrite = DepthMode::disabled;
        GLuint program = 0;
        GLuint texture = 0;
        std::array<float, 4> textureResolution{};
        PreparedParticleUniforms uniforms;
        PreparedCommonUniforms commonUniforms;
        Matrix model = identityMatrix();
        Matrix modelInverse = identityMatrix();
        Matrix viewProjection = identityMatrix();
        Matrix modelViewProjection = identityMatrix();
        std::array<float, 3> eyePosition{};
        std::array<float, 4> renderVar1{};
        std::vector<PreparedUniform> materialUniforms;
        std::array<GLint, 5> attributeLocations{};
        std::size_t operationIndex = 0;
    };

    struct PreparedCamera final {
        std::optional<Matrix> orthographicViewProjection;
        std::optional<ParticleView> particlePerspective;
        std::array<float, 3> eyePosition{};
    };

    struct ParticlePreparation final {
        PreparedParticle operation;
        std::optional<ParticleDrawBatch> batch;
        std::optional<ParticleState> state;
    };

    using PreparedOperation = std::variant<
        PreparedDraw,
        FrameSwapCommand,
        PreparedClear,
        PreparedText,
        PreparedParticle
    >;

    struct PreparedFrame final {
        std::vector<std::optional<PreparedOperation>> operations;
        std::map<std::size_t, ParticleDrawBatch> particleBatches;
        const std::map<std::size_t, ParticleDrawBatch>* frozenParticleBatches =
            nullptr;
        std::map<std::size_t, ParticleState> particleStates;
        std::map<std::string, std::string> finalAliases;
        std::vector<FrameExecutionIssue> issues;
    };

    struct ObjectOperationGroup final {
        std::size_t objectIndex = 0;
        int objectId = 0;
        std::vector<std::size_t> operationIndexes;
    };

    struct LastFrameState final {
        FramePlan sourcePlan;
        FrameEvaluationState evaluation;
        ResolvedFrameInputs inputs;
        std::map<std::size_t, ParticleDrawBatch> particleBatches;
        std::vector<FrameExecutionIssue> issues;
    };

    explicit Impl(std::shared_ptr<SceneFrameGraph> graph)
        : frameGraph(std::move(graph)) {
        validateGraph();
        initializeDimensions();
    }
    Impl(std::shared_ptr<SceneFrameGraph> graph, CGLContextObj context)
        : frameGraph(std::move(graph)), borrowedContext(context) {
        validateGraph();
        if (borrowedContext == nullptr) {
            throw Error(ErrorCode::invalidArgument, "Borrowed OpenGL context is null");
        }
        initializeDimensions();
    }

    ~Impl() {
        if (!device) return;
        try {
            auto session = device->activate();
            textRenderer.release(session);
            releaseParticleGeometry(session);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FramePlanExecutor failed to release renderer resources: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "FramePlanExecutor failed to release renderer resources with an unknown error\n");
        }
    }

    void validateGraph() const {
        if (!frameGraph) {
            throw Error(ErrorCode::invalidArgument, "FramePlanExecutor requires a frame graph");
        }
    }

    void initializeDimensions() {
        if (frameGraph->requiresDrawableProjectionFallback()) return;
        const FramePlan initial = frameGraph->snapshot();
        updateDimensions(initial.width, initial.height);
    }

    [[nodiscard]] static std::size_t checkedRGBA8ByteCount(
        std::uint32_t newWidth,
        std::uint32_t newHeight
    ) {
        if (newHeight != 0 && static_cast<std::size_t>(newWidth) >
                std::numeric_limits<std::size_t>::max() / newHeight / 4) {
            throw Error(ErrorCode::resourceValidation, "RGBA8 output byte count overflows size_t");
        }
        return static_cast<std::size_t>(newWidth) * newHeight * 4;
    }

    void updateDimensions(std::uint32_t newWidth, std::uint32_t newHeight) {
        const std::size_t newByteCount = checkedRGBA8ByteCount(
            newWidth, newHeight
        );
        width = newWidth;
        height = newHeight;
        byteCount = newByteCount;
    }

    Device& ensureDevice() {
        if (!device) {
            device = borrowedContext == nullptr
                ? std::make_unique<Device>()
                : std::make_unique<Device>(borrowedContext);
        }
        return *device;
    }

    const AssetResolver& resolver() const {
        return frameGraph->graph()->model()->runtime()->assetResolver();
    }

    void clearNewFramebuffer(
        Device::Session& session,
        FramebufferResource& resource
    ) {
        glBindFramebuffer(GL_FRAMEBUFFER, resource.framebuffer);
        glViewport(
            0, 0,
            static_cast<GLsizei>(resource.width),
            static_cast<GLsizei>(resource.height)
        );
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        session.checkError(
            ErrorCode::framebufferCreation,
            "initializing a new framebuffer to transparent"
        );
    }

    void destroyCachedFramebuffers(
        Device::Session& session,
        std::map<std::string, CachedFramebuffer>& cached
    ) noexcept {
        for (auto& [id, framebuffer] : cached) {
            static_cast<void>(id);
            session.destroyFramebuffer(framebuffer.resource);
        }
    }

    void ensureFramebuffers(Device::Session& session, const FramePlan& plan) {
        // Validate and allocate every non-GL part of the next arena before
        // touching the live cache. A malformed plan therefore cannot evict a
        // previously usable framebuffer set.
        std::map<std::string, FramebufferDescriptor> required;
        for (const auto& descriptor : plan.framebuffers) {
            if (descriptor.resource.kind != FrameResourceKind::framebuffer ||
                descriptor.resource.id.empty()) {
                throw Error(ErrorCode::resourceValidation, "Invalid framebuffer descriptor identity");
            }
            if (descriptor.width == 0 || descriptor.height == 0 ||
                !std::isfinite(descriptor.scale) || descriptor.scale <= 0.0) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Invalid framebuffer descriptor dimensions or scale for '" +
                        descriptor.resource.id + "'"
                );
            }
            static_cast<void>(pixelFormat(descriptor.format));
            static_cast<void>(textureWrap(descriptor.wrapMode));
            if (!required.emplace(descriptor.resource.id, descriptor).second) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Duplicate framebuffer descriptor '" +
                        descriptor.resource.id + "'"
                );
            }
        }
        if (plan.output.kind != FrameResourceKind::framebuffer ||
            plan.output.id.empty() || !required.contains(plan.output.id)) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame plan output has no framebuffer descriptor"
            );
        }
        const FramebufferDescriptor& outputDescriptor =
            required.at(plan.output.id);
        if (outputDescriptor.width != plan.width ||
            outputDescriptor.height != plan.height) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame plan output descriptor dimensions do not match the plan"
            );
        }
        const std::size_t nextByteCount = checkedRGBA8ByteCount(
            plan.width, plan.height
        );
        std::map<std::string, std::string> nextAliases;
        for (const auto& [id, descriptor] : required) {
            static_cast<void>(descriptor);
            nextAliases.emplace(id, id);
        }
        std::string nextOutputId = plan.output.id;

        // Allocate every staging node before creating GL objects. A map
        // allocation must never occur after FramebufferResource ownership has
        // moved because the resource intentionally has no RAII destructor.
        std::map<std::string, CachedFramebuffer> staged;
        for (const auto& [id, descriptor] : required) {
            const auto existing = framebuffers.find(id);
            if (existing != framebuffers.end() &&
                sameDescriptor(existing->second.descriptor, descriptor)) {
                continue;
            }
            staged.emplace(
                id,
                CachedFramebuffer{descriptor, FramebufferResource{}}
            );
        }

        // Create and initialize replacements while the live cache remains
        // untouched. FramebufferResource is not RAII-owned, so every failure
        // path explicitly destroys both the current temporary and all
        // successfully staged GL objects.
        try {
            for (auto& [id, replacement] : staged) {
                static_cast<void>(id);
                const FramebufferDescriptor& descriptor =
                    replacement.descriptor;
                FramebufferResource resource = session.createFramebuffer(
                    pixelFormat(descriptor.format),
                    descriptor.width,
                    descriptor.height,
                    textureWrap(descriptor.wrapMode),
                    true
                );
                try {
                    clearNewFramebuffer(session, resource);
                } catch (...) {
                    session.destroyFramebuffer(resource);
                    throw;
                }
                replacement.resource = std::move(resource);
            }
        } catch (...) {
            destroyCachedFramebuffers(session, staged);
            throw;
        }

        using CacheIterator = decltype(framebuffers)::iterator;
        std::size_t missingCount = 0;
        for (const auto& [id, descriptor] : required) {
            static_cast<void>(descriptor);
            if (!framebuffers.contains(id)) {
                ++missingCount;
            }
        }
        std::vector<CacheIterator> insertedPlaceholders;
        try {
            insertedPlaceholders.reserve(missingCount);
            for (const auto& [id, descriptor] : required) {
                if (framebuffers.contains(id)) {
                    continue;
                }
                const auto [inserted, didInsert] = framebuffers.emplace(
                    id,
                    CachedFramebuffer{descriptor, FramebufferResource{}}
                );
                if (!didInsert) {
                    throw Error(
                        ErrorCode::resourceValidation,
                        "Unable to reserve framebuffer cache entry '" + id + "'"
                    );
                }
                insertedPlaceholders.push_back(inserted);
            }
        } catch (...) {
            for (const CacheIterator inserted : insertedPlaceholders) {
                framebuffers.erase(inserted);
            }
            destroyCachedFramebuffers(session, staged);
            throw;
        }

        // All potentially throwing work is complete. Commit with only
        // noexcept descriptor swaps, GL deletion, resource moves, and map
        // erasure so a creation/initialization failure can never damage the
        // old arena.
        for (const auto& [id, descriptor] : required) {
            auto replacement = staged.find(id);
            if (replacement == staged.end()) {
                continue;
            }
            auto live = framebuffers.find(id);
            if (!sameDescriptor(live->second.descriptor, descriptor)) {
                std::swap(
                    live->second.descriptor,
                    replacement->second.descriptor
                );
            }
            session.destroyFramebuffer(live->second.resource);
            live->second.resource = std::move(replacement->second.resource);
        }
        destroyCachedFramebuffers(session, staged);

        // Cache entries absent from this frame remain in the arena. Visibility
        // changes must not destroy effect resources; a later plan that
        // references the same descriptor reuses the initialized backing.
        // Only aliases for the current plan are exposed to execution.
        framebufferAliases.swap(nextAliases);
        outputId.swap(nextOutputId);
        width = plan.width;
        height = plan.height;
        byteCount = nextByteCount;
    }

    FramebufferResource& framebuffer(
        const FrameResourceRef& ref,
        const std::map<std::string, std::string>& aliases
    ) {
        if (ref.kind != FrameResourceKind::framebuffer) {
            throw Error(ErrorCode::resourceValidation, "Unknown framebuffer resource '" + ref.id + "'");
        }
        const auto alias = aliases.find(ref.id);
        if (alias == aliases.end()) {
            throw Error(
                ErrorCode::internalFailure,
                "Framebuffer alias arena has no logical resource '" +
                    ref.id + "'"
            );
        }
        const auto found = framebuffers.find(alias->second);
        if (found == framebuffers.end()) {
            throw Error(
                ErrorCode::internalFailure,
                "Framebuffer alias has no backing resource '" + ref.id + "'"
            );
        }
        return found->second.resource;
    }

    FramebufferResource& framebuffer(const FrameResourceRef& ref) {
        return framebuffer(ref, framebufferAliases);
    }

    AssetTextureResource& assetTexture(
        Device::Session& session,
        const FrameResourceRef& ref
    ) {
        if (ref.kind != FrameResourceKind::assetTexture) {
            throw Error(ErrorCode::resourceValidation, "Resource is not an asset texture");
        }
        auto found = assets.find(ref.id);
        if (found == assets.end()) {
            const std::string path = ref.logicalName.empty() ? ref.id : ref.logicalName;
            AssetTextureResource uploaded = session.uploadTexture(
                resolver().parseTexture(path), path
            );
            try {
                const auto [inserted, didInsert] = assets.try_emplace(ref.id);
                if (!didInsert) {
                    session.destroyTexture(uploaded);
                    found = inserted;
                } else {
                    inserted->second = std::move(uploaded);
                    found = inserted;
                }
            } catch (...) {
                session.destroyTexture(uploaded);
                throw;
            }
        }
        if (found->second.images.empty()) {
            throw Error(ErrorCode::resourceValidation, "Texture has no uploaded image: '" + ref.id + "'");
        }
        return found->second;
    }

    GLuint texture(
        Device::Session& session,
        const FrameResourceRef& ref,
        std::size_t imageIndex = 0
    ) {
        if (ref.kind == FrameResourceKind::framebuffer) return framebuffer(ref).colorTexture;
        auto& resource = assetTexture(session, ref);
        if (imageIndex >= resource.images.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Texture image index is outside the uploaded resource: '" + ref.id + "'"
            );
        }
        return resource.images[imageIndex];
    }

    std::array<float, 4> textureResolution(
        const FrameResourceRef& ref,
        const std::map<std::string, std::string>& aliases
    ) const {
        if (ref.kind == FrameResourceKind::framebuffer) {
            const auto alias = aliases.find(ref.id);
            if (alias == aliases.end()) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Framebuffer alias arena has no logical resource '" +
                        ref.id + "'"
                );
            }
            const auto backing = framebuffers.find(alias->second);
            if (backing == framebuffers.end()) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Framebuffer alias has no backing resource '" +
                        ref.id + "'"
                );
            }
            const auto& resource = backing->second.resource;
            return {float(resource.width), float(resource.height), float(resource.width), float(resource.height)};
        }
        const auto found = assets.find(ref.id);
        return found == assets.end() ? std::array<float, 4>{} : found->second.resolution;
    }

    ProgramResource& program(
        Device::Session& session,
        const std::string& vertexShaderPath,
        const std::string& fragmentShaderPath,
        const ComboMap& combos,
        std::string_view description
    ) {
        std::ostringstream keyBuilder;
        keyBuilder << vertexShaderPath << '|' << fragmentShaderPath;
        for (const auto& [name, value] : combos) keyBuilder << '|' << name << '=' << value;
        const std::string key = keyBuilder.str();
        if (const auto found = programs.find(key); found != programs.end()) return found->second;

        if (vertexShaderPath.empty() || fragmentShaderPath.empty()) {
            throw Error(
                ErrorCode::invalidArgument,
                std::string(description) + " has no resolved shader paths"
            );
        }
        ShaderPreprocessOptions options;
        options.combos = combos;
        const ShaderPreprocessor preprocessor(resolver());
        const auto preprocessed = preprocessor.preprocessFiles(
            vertexShaderPath, fragmentShaderPath, options
        );
        const auto translated = ShaderCompiler::translate(
            preprocessed.vertex.source, preprocessed.fragment.source,
            preprocessed.vertex.name, preprocessed.fragment.name
        );
        std::vector<ShaderParameterMetadata> parameters = preprocessed.vertex.parameters;
        parameters.insert(
            parameters.end(), preprocessed.fragment.parameters.begin(),
            preprocessed.fragment.parameters.end()
        );
        const GLuint value = session.createProgram(
            translated.vertex, translated.fragment
        );
        ProgramResource candidate{
            .program = value,
            .parameters = std::move(parameters),
            .uniforms = {},
        };
        try {
            candidate.uniforms = activeUniforms(candidate.program);
            session.checkError(
                ErrorCode::programLink,
                "Inspecting active shader uniforms"
            );
            const auto [inserted, didInsert] = programs.try_emplace(key);
            if (!didInsert) {
                session.destroyProgram(candidate.program);
                return inserted->second;
            }
            inserted->second = std::move(candidate);
            candidate.program = 0;
            return inserted->second;
        } catch (...) {
            session.destroyProgram(candidate.program);
            throw;
        }
    }

    ProgramResource& program(Device::Session& session, const FrameRenderPass& pass) {
        return program(
            session,
            pass.vertexShaderPath,
            pass.fragmentShaderPath,
            pass.combos,
            "Frame render pass"
        );
    }

    FrameResourceRef samplerDefaultResource(
        const FramePlan& plan,
        const FrameRenderPass& pass,
        std::string_view name
    ) const {
        const auto matches = [name](const FrameResourceRef& resource) {
            return resource.logicalName == name;
        };
        if (name == "_rt_FullFrameBuffer" ||
            name == "_rt_MipMappedFrameBuffer") {
            return plan.output;
        }
        if (matches(pass.input)) {
            return pass.input;
        }
        if (pass.previousInput && matches(*pass.previousInput)) {
            return *pass.previousInput;
        }
        for (const auto& [slot, resource] : pass.textures) {
            static_cast<void>(slot);
            if (matches(resource)) {
                return resource;
            }
        }
        if (matches(pass.destination)) {
            return pass.destination;
        }
        for (const FramebufferDescriptor& descriptor : plan.framebuffers) {
            if (matches(descriptor.resource)) {
                return descriptor.resource;
            }
        }
        if (name.starts_with("_rt_") || name.starts_with("_alias_")) {
            throw Error(
                ErrorCode::resourceValidation,
                "Sampler metadata default references unavailable runtime texture '" +
                    std::string(name) + "'"
            );
        }
        return frameAssetTextureResource(name);
    }

    [[nodiscard]] static GLfloat checkedFloat(
        double value,
        std::string_view description
    ) {
        if (!std::isfinite(value) ||
            value > static_cast<double>(std::numeric_limits<GLfloat>::max()) ||
            value < -static_cast<double>(std::numeric_limits<GLfloat>::max())) {
            throw Error(
                ErrorCode::resourceValidation,
                std::string(description) +
                    " contains a non-finite or out-of-range value"
            );
        }
        return static_cast<GLfloat>(value);
    }

    static void validateMatrix(
        const Matrix& matrix,
        std::string_view description
    ) {
        for (const float value : matrix) {
            if (!std::isfinite(value)) {
                throw Error(
                    ErrorCode::resourceValidation,
                    std::string(description) + " contains a non-finite value"
                );
            }
        }
    }

    [[nodiscard]] static PreparedCommonUniforms prepareCommonUniforms(
        const ProgramResource& program,
        const FramePlan& plan,
        const ResolvedFrameInputs& inputs,
        const Matrix& model,
        const Matrix& viewProjection,
        const Matrix& modelViewProjection,
        const CommonRenderableValues& renderable
    ) {
        static constexpr std::array<std::string_view, 6> audioUniforms{
            "g_AudioSpectrum16Left",
            "g_AudioSpectrum16Right",
            "g_AudioSpectrum32Left",
            "g_AudioSpectrum32Right",
            "g_AudioSpectrum64Left",
            "g_AudioSpectrum64Right",
        };
        for (const std::string_view name : audioUniforms) {
            if (activeUniform(program, name) != nullptr) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active builtin uniform '" + std::string(name) +
                        "' requires unavailable system audio spectrum input"
                );
            }
        }
        if (plan.width == 0 || plan.height == 0) {
            throw Error(
                ErrorCode::internalFailure,
                "Frame dimensions are unavailable for common shader uniforms"
            );
        }

        PreparedCommonUniforms result;
        result.textureReductionScale = prepareBuiltinUniform(
            program, "g_TextureReductionScale", GL_FLOAT
        );
        result.lightAmbientColor = prepareBuiltinUniform(
            program, "g_LightAmbientColor", GL_FLOAT_VEC3
        );
        result.lightSkylightColor = prepareBuiltinUniform(
            program, "g_LightSkylightColor", GL_FLOAT_VEC3
        );
        result.brightness = prepareBuiltinUniform(
            program, "g_Brightness", GL_FLOAT
        );
        result.userAlpha = prepareBuiltinUniform(
            program, "g_UserAlpha", GL_FLOAT
        );
        result.alpha = prepareBuiltinUniform(program, "g_Alpha", GL_FLOAT);
        result.color = prepareBuiltinUniform(
            program, "g_Color", GL_FLOAT_VEC3
        );
        result.color4 = prepareBuiltinUniform(
            program, "g_Color4", GL_FLOAT_VEC4
        );
        result.compositeColor = prepareBuiltinUniform(
            program, "g_CompositeColor", GL_FLOAT_VEC3
        );
        result.compositeColorProvidedByShader = std::any_of(
            program.parameters.begin(), program.parameters.end(),
            [](const ShaderParameterMetadata& parameter) {
                return parameter.name == "g_CompositeColor";
            }
        );
        result.time = prepareBuiltinUniform(program, "g_Time", GL_FLOAT);
        result.daytime = prepareBuiltinUniform(
            program, "g_Daytime", GL_FLOAT
        );
        result.modelViewProjection = prepareBuiltinUniform(
            program, "g_ModelViewProjectionMatrix", GL_FLOAT_MAT4
        );
        result.modelViewProjectionInverse = prepareBuiltinUniform(
            program, "g_ModelViewProjectionMatrixInverse", GL_FLOAT_MAT4
        );
        result.effectModelViewProjection = prepareBuiltinUniform(
            program, "g_EffectModelViewProjectionMatrix", GL_FLOAT_MAT4
        );
        result.model = prepareBuiltinUniform(
            program, "g_ModelMatrix", GL_FLOAT_MAT4
        );
        result.effectModel = prepareBuiltinUniform(
            program, "g_EffectModelMatrix", GL_FLOAT_MAT4
        );
        result.normalModel = prepareBuiltinUniform(
            program, "g_NormalModelMatrix", GL_FLOAT_MAT3
        );
        result.viewProjection = prepareBuiltinUniform(
            program, "g_ViewProjectionMatrix", GL_FLOAT_MAT4
        );
        result.pointer = prepareBuiltinUniform(
            program, "g_PointerPosition", GL_FLOAT_VEC2
        );
        result.pointerLast = prepareBuiltinUniform(
            program, "g_PointerPositionLast", GL_FLOAT_VEC2
        );
        result.effectTextureProjection = prepareBuiltinUniform(
            program, "g_EffectTextureProjectionMatrix", GL_FLOAT_MAT4
        );
        result.effectTextureProjectionInverse = prepareBuiltinUniform(
            program, "g_EffectTextureProjectionMatrixInverse", GL_FLOAT_MAT4
        );
        result.texelSize = prepareBuiltinUniform(
            program, "g_TexelSize", GL_FLOAT_VEC2
        );
        result.texelSizeHalf = prepareBuiltinUniform(
            program, "g_TexelSizeHalf", GL_FLOAT_VEC2
        );

        if (result.modelViewProjectionInverse >= 0) {
            result.modelViewProjectionInverseValue = inverse(
                modelViewProjection, "Model-view-projection matrix"
            );
        }
        result.ambientColorValue = {
            checkedFloat(plan.ambientColor.red, "Ambient light color"),
            checkedFloat(plan.ambientColor.green, "Ambient light color"),
            checkedFloat(plan.ambientColor.blue, "Ambient light color"),
        };
        result.skylightColorValue = {
            checkedFloat(plan.skylightColor.red, "Skylight color"),
            checkedFloat(plan.skylightColor.green, "Skylight color"),
            checkedFloat(plan.skylightColor.blue, "Skylight color"),
        };
        result.brightnessValue = checkedFloat(
            renderable.brightness, "Renderable brightness"
        );
        result.alphaValue = checkedFloat(
            renderable.alpha, "Renderable alpha"
        );
        result.color4Value = renderable.color;
        for (std::size_t index = 0; index < renderable.color.size(); ++index) {
            result.color4Value[index] = checkedFloat(
                renderable.color[index], "Renderable color"
            );
        }
        std::copy_n(
            result.color4Value.begin(), 3, result.colorValue.begin()
        );
        result.timeValue = checkedFloat(inputs.timeSeconds, "Frame time");
        result.daytimeValue = checkedFloat(inputs.daytime, "Local daytime");
        result.pointerValue = {
            checkedFloat(inputs.pointerPosition.x, "Pointer position"),
            checkedFloat(inputs.pointerPosition.y, "Pointer position"),
        };
        result.pointerLastValue = {
            checkedFloat(inputs.pointerPositionLast.x, "Previous pointer position"),
            checkedFloat(inputs.pointerPositionLast.y, "Previous pointer position"),
        };
        result.texelSizeValue = {
            1.0F / static_cast<float>(plan.width),
            1.0F / static_cast<float>(plan.height),
        };
        result.texelSizeHalfValue = {
            result.texelSizeValue[0] * 0.5F,
            result.texelSizeValue[1] * 0.5F,
        };
        validateMatrix(model, "Model matrix");
        validateMatrix(viewProjection, "View-projection matrix");
        validateMatrix(modelViewProjection, "Model-view-projection matrix");
        return result;
    }

    static void bindCommonUniforms(
        const PreparedCommonUniforms& uniforms,
        const Matrix& model,
        const Matrix& viewProjection,
        const Matrix& modelViewProjection
    ) {
        static constexpr std::array<float, 9> identity3{
            1.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 1.0F,
        };
        const Matrix identity4 = identityMatrix();
        if (uniforms.textureReductionScale >= 0) {
            glUniform1f(uniforms.textureReductionScale, 1.0F);
        }
        bindVector3(uniforms.lightAmbientColor, uniforms.ambientColorValue);
        bindVector3(uniforms.lightSkylightColor, uniforms.skylightColorValue);
        if (uniforms.brightness >= 0) {
            glUniform1f(uniforms.brightness, uniforms.brightnessValue);
        }
        if (uniforms.userAlpha >= 0) {
            glUniform1f(uniforms.userAlpha, uniforms.alphaValue);
        }
        if (uniforms.alpha >= 0) {
            glUniform1f(uniforms.alpha, uniforms.alphaValue);
        }
        bindVector3(uniforms.color, uniforms.colorValue);
        bindVector4(uniforms.color4, uniforms.color4Value);
        if (!uniforms.compositeColorProvidedByShader) {
            bindVector3(uniforms.compositeColor, uniforms.colorValue);
        }
        if (uniforms.time >= 0) {
            glUniform1f(uniforms.time, uniforms.timeValue);
        }
        if (uniforms.daytime >= 0) {
            glUniform1f(uniforms.daytime, uniforms.daytimeValue);
        }
        bindMatrix(uniforms.modelViewProjection, modelViewProjection);
        bindMatrix(
            uniforms.modelViewProjectionInverse,
            uniforms.modelViewProjectionInverseValue
        );
        bindMatrix(uniforms.effectModelViewProjection, modelViewProjection);
        bindMatrix(uniforms.model, model);
        bindMatrix(uniforms.effectModel, model);
        bindMatrix3(uniforms.normalModel, identity3);
        bindMatrix(uniforms.viewProjection, viewProjection);
        bindVector2(uniforms.pointer, uniforms.pointerValue);
        bindVector2(uniforms.pointerLast, uniforms.pointerLastValue);
        bindMatrix(uniforms.effectTextureProjection, identity4);
        bindMatrix(uniforms.effectTextureProjectionInverse, identity4);
        bindVector2(uniforms.texelSize, uniforms.texelSizeValue);
        bindVector2(uniforms.texelSizeHalf, uniforms.texelSizeHalfValue);
    }

    [[nodiscard]] static std::optional<PreparedUniform> prepareRuntimeUniform(
        const ProgramResource& program,
        const std::string& name,
        const RuntimeValue& value
    ) {
        const ActiveUniform* uniform = activeUniform(program, name);
        if (uniform == nullptr) return std::nullopt;
        if (uniform->blockIndex >= 0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + name +
                    "' uses an unsupported uniform-block binding"
            );
        }
        if (uniform->isArray || uniform->size != 1) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + name + "' uses an unsupported array binding"
            );
        }
        if (uniform->location < 0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + name + "' has no bindable location"
            );
        }
        const GLenum type = uniform->type;
        PreparedUniform result{.location = uniform->location};
        if (type == GL_BOOL) {
            result.kind = PreparedUniform::Kind::integer;
            result.integer = value.boolean() ? 1 : 0;
            return result;
        }
        if (type == GL_INT) {
            const std::int64_t integer = value.integer();
            if (integer < std::numeric_limits<GLint>::min() ||
                integer > std::numeric_limits<GLint>::max()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Uniform '" + name + "' exceeds GLint range"
                );
            }
            result.kind = PreparedUniform::Kind::integer;
            result.integer = static_cast<GLint>(integer);
            return result;
        }
        const std::size_t expected = type == GL_FLOAT ? 1
            : type == GL_FLOAT_VEC2 ? 2
            : type == GL_FLOAT_VEC3 ? 3
            : type == GL_FLOAT_VEC4 ? 4 : 0;
        if (expected == 0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + name + "' has an unsupported metadata type"
            );
        }
        const auto components = numericComponents(value, expected, name);
        std::copy(
            components.begin(), components.end(), result.floating.begin()
        );
        result.kind = expected == 1 ? PreparedUniform::Kind::float1
            : expected == 2 ? PreparedUniform::Kind::float2
            : expected == 3 ? PreparedUniform::Kind::float3
                            : PreparedUniform::Kind::float4;
        return result;
    }

    static void bindPreparedUniform(const PreparedUniform& uniform) {
        switch (uniform.kind) {
            case PreparedUniform::Kind::integer:
                glUniform1i(uniform.location, uniform.integer);
                break;
            case PreparedUniform::Kind::float1:
                glUniform1fv(uniform.location, 1, uniform.floating.data());
                break;
            case PreparedUniform::Kind::float2:
                glUniform2fv(uniform.location, 1, uniform.floating.data());
                break;
            case PreparedUniform::Kind::float3:
                glUniform3fv(uniform.location, 1, uniform.floating.data());
                break;
            case PreparedUniform::Kind::float4:
                glUniform4fv(uniform.location, 1, uniform.floating.data());
                break;
        }
    }

    [[nodiscard]] GLuint preparedTexture(
        const PreparedTextureBinding& binding
    ) {
        if (binding.assetTexture != 0) return binding.assetTexture;
        return framebuffer(binding.resource).colorTexture;
    }

    void ensureGeometry(Device::Session& session) {
        if (vertexArray != 0 || vertexBuffer != 0) {
            if (vertexArray == 0 || vertexBuffer == 0) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Image OpenGL geometry is only partially initialized"
                );
            }
            return;
        }

        GLuint candidateVertexArray = 0;
        GLuint candidateVertexBuffer = 0;
        try {
            candidateVertexArray = session.createVertexArray();
            candidateVertexBuffer = session.createBuffer();
            glBindVertexArray(candidateVertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, candidateVertexBuffer);
            glBufferData(
                GL_ARRAY_BUFFER,
                sizeof(Vertex) * 6,
                nullptr,
                GL_DYNAMIC_DRAW
            );
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindVertexArray(0);
            session.checkError(
                ErrorCode::internalFailure,
                "initializing shared image geometry"
            );
            vertexArray = candidateVertexArray;
            vertexBuffer = candidateVertexBuffer;
        } catch (...) {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindVertexArray(0);
            session.destroyBuffer(candidateVertexBuffer);
            session.destroyVertexArray(candidateVertexArray);
            throw;
        }
    }

    void ensureParticleGeometry(Device::Session& session) {
        if (particleVertexArray != 0 || particleVertexBuffer != 0 ||
            particleElementBuffer != 0) {
            if (particleVertexArray == 0 || particleVertexBuffer == 0 ||
                particleElementBuffer == 0) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Particle OpenGL geometry is only partially initialized"
                );
            }
            return;
        }

        GLuint vertexArray = 0;
        GLuint vertexBuffer = 0;
        GLuint elementBuffer = 0;
        try {
            vertexArray = session.createVertexArray();
            vertexBuffer = session.createBuffer();
            elementBuffer = session.createBuffer();
            glBindVertexArray(vertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementBuffer);
            session.checkError(
                ErrorCode::internalFailure,
                "initializing shared particle geometry"
            );
            particleVertexArray = vertexArray;
            particleVertexBuffer = vertexBuffer;
            particleElementBuffer = elementBuffer;
        } catch (...) {
            session.destroyBuffer(elementBuffer);
            session.destroyBuffer(vertexBuffer);
            session.destroyVertexArray(vertexArray);
            throw;
        }
    }

    void releaseParticleGeometry(Device::Session& session) noexcept {
        session.destroyBuffer(particleElementBuffer);
        session.destroyBuffer(particleVertexBuffer);
        session.destroyVertexArray(particleVertexArray);
    }

    [[nodiscard]] PreparedDraw prepareDraw(
        Device::Session& session,
        const FramePlan& plan,
        const FrameRenderPass& pass,
        const ResolvedFrameInputs& inputs,
        const FrameVector2& frameParallax,
        const PreparedCamera& camera,
        const std::map<std::string, std::string>& aliases
    ) {
        if (pass.origin.imageIndex >= plan.images.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Render pass image index is invalid"
            );
        }
        auto& destination = framebuffer(pass.destination, aliases);
        ProgramResource& programResource = program(session, pass);
        const GLuint activeProgram = programResource.program;
        PreparedDraw result{
            .pass = pass,
            .program = activeProgram,
        };

        std::array<bool, 32> boundTextureSlots{};
        for (const auto& [slot, ref] : pass.textures) {
            if (slot < 0 || slot >= 32) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Texture slot is outside the supported range"
                );
            }
            if (ref.kind == FrameResourceKind::framebuffer &&
                framebuffer(ref, aliases).framebuffer == destination.framebuffer) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Texture slot " + std::to_string(slot) +
                        " resolves to the render destination '" +
                        pass.destination.logicalName + "'"
                );
            }
            std::size_t imageIndex = 0;
            GLuint assetTextureValue = 0;
            if (slot == 0 && ref.kind == FrameResourceKind::assetTexture) {
                result.texture0Animation = selectTextureAnimation(
                    assetTexture(session, ref), inputs.timeSeconds
                );
                imageIndex = result.texture0Animation.imageIndex;
            }
            if (ref.kind == FrameResourceKind::assetTexture) {
                assetTextureValue = texture(session, ref, imageIndex);
            } else {
                static_cast<void>(framebuffer(ref, aliases));
            }
            const std::string samplerName = "g_Texture" + std::to_string(slot);
            result.textures.push_back({
                .slot = slot,
                .resource = ref,
                .assetTexture = assetTextureValue,
                .resolution = textureResolution(ref, aliases),
                .samplerLocation = prepareBuiltinUniform(
                    programResource, samplerName, GL_SAMPLER_2D
                ),
                .resolutionLocation = prepareBuiltinUniform(
                    programResource, samplerName + "Resolution", GL_FLOAT_VEC4
                ),
            });
            boundTextureSlots[static_cast<std::size_t>(slot)] = true;
        }
        for (const ActiveUniform& uniform : programResource.uniforms) {
            if (!isOpenGLSamplerType(uniform.type)) {
                continue;
            }
            if (uniform.type != GL_SAMPLER_2D) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active sampler '" + uniform.name +
                        "' uses an unsupported non-2D texture type"
                );
            }
            const std::optional<int> slot = textureSlot(uniform.name);
            if (!slot) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active sampler '" + uniform.name +
                        "' does not follow the g_TextureN binding contract"
                );
            }
            if (boundTextureSlots[static_cast<std::size_t>(*slot)]) {
                continue;
            }
            const std::string* defaultTexture = samplerDefaultTexture(
                programResource.parameters, uniform.name
            );
            if (defaultTexture == nullptr || defaultTexture->empty()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active sampler '" + uniform.name + "' requires texture slot " +
                        std::to_string(*slot) +
                        ", but the frame pass provides no texture or metadata default"
                );
            }
            const FrameResourceRef defaultResource = samplerDefaultResource(
                plan, pass, *defaultTexture
            );
            if (defaultResource.kind == FrameResourceKind::framebuffer &&
                framebuffer(defaultResource, aliases).framebuffer ==
                    destination.framebuffer) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active sampler '" + uniform.name +
                        "' resolves to the render destination '" +
                        defaultResource.logicalName + "'"
                );
            }
            std::size_t imageIndex = 0;
            GLuint assetTextureValue = 0;
            if (*slot == 0 &&
                defaultResource.kind == FrameResourceKind::assetTexture) {
                result.texture0Animation = selectTextureAnimation(
                    assetTexture(session, defaultResource), inputs.timeSeconds
                );
                imageIndex = result.texture0Animation.imageIndex;
            }
            if (defaultResource.kind == FrameResourceKind::assetTexture) {
                assetTextureValue = texture(
                    session, defaultResource, imageIndex
                );
            } else {
                static_cast<void>(framebuffer(defaultResource, aliases));
            }
            result.textures.push_back({
                .slot = *slot,
                .resource = defaultResource,
                .assetTexture = assetTextureValue,
                .resolution = textureResolution(defaultResource, aliases),
                .samplerLocation = prepareBuiltinUniform(
                    programResource, uniform.name, GL_SAMPLER_2D
                ),
                .resolutionLocation = prepareBuiltinUniform(
                    programResource, uniform.name + "Resolution", GL_FLOAT_VEC4
                ),
            });
            boundTextureSlots[static_cast<std::size_t>(*slot)] = true;
        }
        result.uniforms.texture0Translation = prepareBuiltinUniform(
            programResource, "g_Texture0Translation", GL_FLOAT_VEC2
        );
        result.uniforms.texture0Rotation = prepareBuiltinUniform(
            programResource, "g_Texture0Rotation", GL_FLOAT_VEC4
        );
        result.uniforms.alternateModel = prepareBuiltinUniform(
            programResource, "g_AltModelMatrix", GL_FLOAT_MAT4
        );
        result.uniforms.alternateViewProjection = prepareBuiltinUniform(
            programResource, "g_AltViewProjectionMatrix", GL_FLOAT_MAT4
        );
        const FrameImageDescriptor& image =
            plan.images[pass.origin.imageIndex];
        const auto identity = identityMatrix();
        const float imageWidth = checkedFloat(image.size.x, "Image width");
        const float imageHeight = checkedFloat(image.size.y, "Image height");
        const Matrix localProjection = orthographic(
            0.0F, imageWidth, 0.0F, imageHeight, -1.0F, 1.0F
        );
        result.model = localProjection;
        result.viewProjection = identity;

        float left = -1.0F;
        float right = 1.0F;
        float bottom = -1.0F;
        float top = 1.0F;
        if (pass.geometry == FrameGeometryKind::imageLocal) {
            left = 0.0F;
            right = imageWidth;
            bottom = 0.0F;
            top = imageHeight;
            result.modelViewProjection = localProjection;
        } else if (pass.geometry == FrameGeometryKind::imageScene ||
                   pass.geometry == FrameGeometryKind::passthroughCapture) {
            const auto& transform = image.worldTransform;
            // Wallpaper Engine stores image origins in top-left scene pixels,
            // while its orthographic camera is centered with Y pointing up.
            // CImage::updateScenePosition bakes origin, alignment, and XY scale
            // into scene-space geometry. Keep the same decomposition because
            // shaders are allowed to inspect each common matrix separately.
            const double scaledWidth = image.size.x * transform.scale.x;
            const double scaledHeight = image.size.y * transform.scale.y;
            double centerX =
                transform.origin.x - static_cast<double>(plan.width) * 0.5;
            double centerY =
                static_cast<double>(plan.height) * 0.5 - transform.origin.y;
            if (image.horizontalAlignment.find("left") != std::string::npos) {
                centerX += scaledWidth * 0.5;
            } else if (
                image.horizontalAlignment.find("right") != std::string::npos
            ) {
                centerX -= scaledWidth * 0.5;
            }
            if (image.horizontalAlignment.find("top") != std::string::npos) {
                centerY += scaledHeight * 0.5;
            } else if (
                image.horizontalAlignment.find("bottom") != std::string::npos
            ) {
                centerY -= scaledHeight * 0.5;
            }
            left = checkedFloat(
                centerX - scaledWidth * 0.5, "Image scene left edge"
            );
            right = checkedFloat(
                centerX + scaledWidth * 0.5, "Image scene right edge"
            );
            bottom = checkedFloat(
                centerY - scaledHeight * 0.5, "Image scene bottom edge"
            );
            top = checkedFloat(
                centerY + scaledHeight * 0.5, "Image scene top edge"
            );
            const float pivotX = checkedFloat(centerX, "Image rotation pivot");
            const float pivotY = checkedFloat(centerY, "Image rotation pivot");
            const float angle = checkedFloat(
                transform.angles.z, "Image Z angle"
            );
            Matrix screenTransform = multiply(
                translation(pivotX, pivotY, 0.0F),
                multiply(
                    rotationZ(-angle),
                    translation(-pivotX, -pivotY, 0.0F)
                )
            );
            if (plan.parallax.enabled) {
                const auto depth = numericComponents(
                    image.parallaxDepth.value,
                    2,
                    "Image parallax depth"
                );
                const FrameVector2 parallaxOffset = sceneParallaxOffset(
                    plan,
                    {
                        .x = static_cast<double>(depth[0]),
                        .y = static_cast<double>(depth[1]),
                    },
                    frameParallax,
                    "image"
                );
                // Linux post-multiplies this translation into the rotated
                // screen MVP. This order matters when the image is rotated.
                screenTransform = multiply(
                    screenTransform,
                    translation(
                        checkedFloat(parallaxOffset.x, "Image parallax X"),
                        checkedFloat(parallaxOffset.y, "Image parallax Y"),
                        0.0F
                    )
                );
            }
            if (!camera.orthographicViewProjection) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Scene orthographic camera was not prepared"
                );
            }
            result.modelViewProjection = multiply(
                *camera.orthographicViewProjection,
                screenTransform
            );
        } else {
            result.modelViewProjection = identity;
        }
        validateMatrix(result.model, "Image model matrix");
        validateMatrix(result.viewProjection, "Image view-projection matrix");
        validateMatrix(
            result.modelViewProjection,
            "Image model-view-projection matrix"
        );
        const auto color = numericComponents(
            image.color.value, 4, "Renderable color"
        );
        const auto alpha = numericComponents(
            image.alpha.value, 1, "Renderable alpha"
        );
        const auto brightness = numericComponents(
            image.brightness.value, 1, "Renderable brightness"
        );
        result.commonUniforms = prepareCommonUniforms(
            programResource,
            plan,
            inputs,
            result.model,
            result.viewProjection,
            result.modelViewProjection,
            {
                .brightness = brightness[0],
                .alpha = alpha[0],
                .color = {color[0], color[1], color[2], color[3]},
            }
        );
        for (const auto& metadata : programResource.parameters) {
            if (!metadata.material || isSamplerParameter(metadata)) continue;
            if (const auto value = pass.constants.find(*metadata.material); value != pass.constants.end()) {
                if (auto uniform = prepareRuntimeUniform(
                        programResource, metadata.name, value->second.value
                    )) {
                    result.materialUniforms.push_back(*uniform);
                }
            } else if (metadata.defaultValue) {
                if (auto uniform = prepareRuntimeUniform(
                        programResource, metadata.name,
                        metadataDefault(*metadata.defaultValue)
                    )) {
                    result.materialUniforms.push_back(*uniform);
                }
            } else if (activeUniform(programResource, metadata.name) != nullptr) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active material uniform '" + metadata.name +
                        "' has neither a frame constant nor metadata default"
                );
            }
        }
        float uMax = 1.0F;
        float vMax = 1.0F;
        if (pass.textureCoordinates == FrameTexCoordKind::image) {
            const auto resolution = textureResolution(pass.input, aliases);
            if (resolution[0] > 0 && resolution[1] > 0) {
                uMax = resolution[2] / resolution[0];
                vMax = resolution[3] / resolution[1];
            }
        }
        result.vertices = {{
            {{left, bottom, 0}, {0, 0}},
            {{right, bottom, 0}, {uMax, 0}},
            {{right, top, 0}, {uMax, vMax}},
            {{left, bottom, 0}, {0, 0}},
            {{right, top, 0}, {uMax, vMax}},
            {{left, top, 0}, {0, vMax}},
        }};
        ensureGeometry(session);
        result.positionLocation = glGetAttribLocation(
            activeProgram, "a_Position"
        );
        result.texCoordLocation = glGetAttribLocation(
            activeProgram, "a_TexCoord"
        );
        session.checkError(
            ErrorCode::draw,
            "preparing a frame render pass"
        );
        return result;
    }

    void draw(Device::Session& session, const PreparedDraw& prepared) {
        auto& destination = framebuffer(prepared.pass.destination);
        glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
        glViewport(
            0, 0, static_cast<GLsizei>(destination.width),
            static_cast<GLsizei>(destination.height)
        );
        configureState(prepared.pass);
        glUseProgram(prepared.program);

        for (const PreparedTextureBinding& binding : prepared.textures) {
            glActiveTexture(GL_TEXTURE0 + binding.slot);
            glBindTexture(GL_TEXTURE_2D, preparedTexture(binding));
            if (binding.samplerLocation >= 0) {
                glUniform1i(binding.samplerLocation, binding.slot);
            }
            if (binding.resolutionLocation >= 0) {
                glUniform4fv(
                    binding.resolutionLocation, 1,
                    binding.resolution.data()
                );
            }
        }
        if (prepared.uniforms.texture0Translation >= 0) {
            glUniform2fv(
                prepared.uniforms.texture0Translation, 1,
                prepared.texture0Animation.translation.data()
            );
        }
        if (prepared.uniforms.texture0Rotation >= 0) {
            glUniform4fv(
                prepared.uniforms.texture0Rotation, 1,
                prepared.texture0Animation.rotation.data()
            );
        }

        for (const PreparedUniform& uniform : prepared.materialUniforms) {
            bindPreparedUniform(uniform);
        }
        bindCommonUniforms(
            prepared.commonUniforms,
            prepared.model,
            prepared.viewProjection,
            prepared.modelViewProjection
        );
        bindMatrix(prepared.uniforms.alternateModel, prepared.model);
        bindMatrix(
            prepared.uniforms.alternateViewProjection,
            prepared.viewProjection
        );

        glBindVertexArray(vertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        disableVertexAttributes();
        glBufferSubData(
            GL_ARRAY_BUFFER, 0, sizeof(prepared.vertices),
            prepared.vertices.data()
        );
        if (prepared.positionLocation >= 0) {
            glEnableVertexAttribArray(
                static_cast<GLuint>(prepared.positionLocation)
            );
            glVertexAttribPointer(
                static_cast<GLuint>(prepared.positionLocation),
                3, GL_FLOAT, GL_FALSE,
                sizeof(Vertex), nullptr
            );
        }
        if (prepared.texCoordLocation >= 0) {
            glEnableVertexAttribArray(
                static_cast<GLuint>(prepared.texCoordLocation)
            );
            glVertexAttribPointer(
                static_cast<GLuint>(prepared.texCoordLocation),
                2, GL_FLOAT, GL_FALSE,
                sizeof(Vertex),
                reinterpret_cast<const void*>(offsetof(Vertex, texCoord))
            );
        }
        glDrawArrays(GL_TRIANGLES, 0, 6);
        session.checkError(ErrorCode::draw, "executing frame render pass");
    }

    [[nodiscard]] PreparedDraw prepareCopy(
        Device::Session& session,
        const FramePlan& plan,
        const FrameCopyCommand& command,
        const ResolvedFrameInputs& inputs,
        const FrameVector2& frameParallax,
        const PreparedCamera& camera,
        const std::map<std::string, std::string>& aliases
    ) {
        if (command.source.kind == FrameResourceKind::framebuffer &&
            framebuffer(command.source, aliases).framebuffer ==
                framebuffer(command.destination, aliases).framebuffer) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame copy source and destination resolve to the same backing resource"
            );
        }
        FrameRenderPass pass{
            .origin = command.origin,
            .shader = "commands/copy",
            .vertexShaderPath = "shaders/commands/copy.vert",
            .fragmentShaderPath = "shaders/commands/copy.frag",
            .blending = BlendingMode::normal,
            .culling = CullingMode::disabled,
            .depthTest = DepthMode::disabled,
            .depthWrite = DepthMode::disabled,
            .geometry = FrameGeometryKind::fullscreenLocal,
            .textureCoordinates = FrameTexCoordKind::full,
            .input = command.source,
            .destination = command.destination,
            .textures = {{0, command.source}},
            .writeAlpha = true,
        };
        return prepareDraw(
            session, plan, pass, inputs, frameParallax, camera, aliases
        );
    }

    void prepareSwap(
        const FrameSwapCommand& command,
        std::map<std::string, std::string>& aliases
    ) const {
        if (command.source.kind != FrameResourceKind::framebuffer ||
            command.destination.kind != FrameResourceKind::framebuffer) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame swap requires framebuffer resources"
            );
        }
        auto source = aliases.find(command.source.id);
        auto destination = aliases.find(command.destination.id);
        if (source == aliases.end() || destination == aliases.end()) {
            throw Error(
                ErrorCode::internalFailure,
                "Frame swap cannot resolve its prepared framebuffer aliases"
            );
        }
        std::swap(source->second, destination->second);
    }

    void swap(const FrameSwapCommand& command) {
        if (command.source.kind != FrameResourceKind::framebuffer ||
            command.destination.kind != FrameResourceKind::framebuffer) {
            throw Error(ErrorCode::resourceValidation, "Frame swap requires framebuffer resources");
        }
        auto source = framebufferAliases.find(command.source.id);
        auto destination = framebufferAliases.find(command.destination.id);
        if (source == framebufferAliases.end() || destination == framebufferAliases.end()) {
            throw Error(
                ErrorCode::internalFailure,
                "Frame swap cannot resolve its execution framebuffer aliases"
            );
        }
        std::swap(source->second, destination->second);
    }

    [[nodiscard]] PreparedClear prepareClear(
        const FrameClearCommand& command,
        const std::map<std::string, std::string>& aliases
    ) {
        static_cast<void>(framebuffer(command.destination, aliases));
        return {
            .destination = command.destination,
            .color = {
                checkedFloat(command.color.red, "Clear color"),
                checkedFloat(command.color.green, "Clear color"),
                checkedFloat(command.color.blue, "Clear color"),
                checkedFloat(command.color.alpha, "Clear color"),
            },
        };
    }

    void clear(Device::Session& session, const PreparedClear& prepared) {
        auto& destination = framebuffer(prepared.destination);
        glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
        glViewport(
            0, 0, static_cast<GLsizei>(destination.width),
            static_cast<GLsizei>(destination.height)
        );
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(
            prepared.color[0], prepared.color[1],
            prepared.color[2], prepared.color[3]
        );
        glClear(GL_COLOR_BUFFER_BIT);
        session.checkError(ErrorCode::draw, "executing frame clear command");
    }

    [[nodiscard]] PreparedText prepareText(
        Device::Session& session,
        const FramePlan& plan,
        const FrameTextCommand& command,
        const PreparedCamera& camera,
        const std::map<std::string, std::string>& aliases
    ) {
        if (command.textIndex >= plan.texts.size()) {
            throw Error(ErrorCode::resourceValidation, "Frame text command index is invalid");
        }
        const auto& descriptor = plan.texts[command.textIndex];
        if (descriptor.objectId != command.objectId) {
            throw Error(ErrorCode::resourceValidation, "Frame text command object identity is inconsistent");
        }
        static_cast<void>(framebuffer(command.destination, aliases));
        if (descriptor.horizontalAlignment != "center" ||
            descriptor.verticalAlignment != "center") {
            throw Error(
                ErrorCode::resourceValidation,
                "Text rendering currently requires center horizontal and vertical alignment"
            );
        }

        text::FontSource font;
        std::optional<ResolvedAsset> fontAsset;
        if (descriptor.font == "systemfont_arial") {
            font = text::FontSource::system("Arial");
        } else if (descriptor.font.starts_with("systemfont_")) {
            throw Error(
                ErrorCode::resourceValidation,
                "Unsupported system font identifier '" + descriptor.font + "'"
            );
        } else {
            fontAsset = resolver().resolve(descriptor.font);
            font = text::FontSource::bytes(fontAsset->bytes);
        }
        const auto rasterized = text::rasterize({
            .utf8 = descriptor.text,
            .pointSize = descriptor.pointSize,
            .font = font,
        });
        const std::array<double, 4> layoutComponents{
            descriptor.size.x, descriptor.size.y,
            descriptor.padding.x, descriptor.padding.y,
        };
        for (const double component : layoutComponents) {
            if (!std::isfinite(component) || component < 0.0) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Text size and padding must be finite non-negative values"
                );
            }
        }
        if (!descriptor.text.empty() &&
            (static_cast<double>(rasterized.width) + descriptor.padding.x * 2.0 >
                 descriptor.size.x ||
             static_cast<double>(rasterized.height) + descriptor.padding.y * 2.0 >
                 descriptor.size.y)) {
            throw Error(
                ErrorCode::resourceValidation,
                "Rasterized text plus padding exceeds its authored layout size"
            );
        }

        const double effectiveAlpha = descriptor.color.alpha * descriptor.alpha;
        const std::array<double, 4> colorComponents{
            descriptor.color.red,
            descriptor.color.green,
            descriptor.color.blue,
            effectiveAlpha,
        };
        std::array<float, 4> color{};
        for (std::size_t index = 0; index < color.size(); ++index) {
            const double component = colorComponents[index];
            if (!std::isfinite(component) || component < 0.0 || component > 1.0) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Text color and opacity components must be finite values in [0, 1]"
                );
            }
            color[index] = static_cast<float>(component);
        }

        const auto& transform = descriptor.worldTransform;
        const float originX = static_cast<float>(
            transform.origin.x - static_cast<double>(plan.width) * 0.5
        );
        const float originY = static_cast<float>(
            static_cast<double>(plan.height) * 0.5 - transform.origin.y
        );
        const Matrix world = multiply(
            translation(originX, originY, float(transform.origin.z)),
            multiply(
                rotationZ(float(transform.angles.z)),
                multiply(
                    rotationY(float(transform.angles.y)),
                    multiply(
                        rotationX(float(transform.angles.x)),
                        scaling(float(transform.scale.x), float(transform.scale.y), float(transform.scale.z))
                    )
                )
            )
        );
        const Matrix alignment = translation(
            -static_cast<float>(rasterized.width) * 0.5F,
            -static_cast<float>(rasterized.height) * 0.5F,
            0.0F
        );
        if (!camera.orthographicViewProjection) {
            throw Error(
                ErrorCode::internalFailure,
                "Scene orthographic camera was not prepared"
            );
        }
        const Matrix modelViewProjection = multiply(
            *camera.orthographicViewProjection,
            multiply(world, alignment)
        );
        validateMatrix(modelViewProjection, "Text model-view-projection matrix");
        return {
            .destination = command.destination,
            .coverage = textRenderer.prepare(session, rasterized),
            .request = {
                .modelViewProjection = modelViewProjection,
                .color = color,
            },
        };
    }

    void drawText(
        Device::Session& session,
        const PreparedText& prepared
    ) {
        textRenderer.drawPrepared(
            session,
            framebuffer(prepared.destination),
            prepared.coverage,
            prepared.request
        );
    }

    [[nodiscard]] static float particleFloat(double value, std::string_view name) {
        if (!std::isfinite(value) ||
            value > static_cast<double>(std::numeric_limits<float>::max()) ||
            value < -static_cast<double>(std::numeric_limits<float>::max())) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle " + std::string(name) +
                    " contains a non-finite or out-of-range value"
            );
        }
        return static_cast<float>(value);
    }

    [[nodiscard]] static ParticleAtlasMetadata particleAtlasMetadata(
        const AssetTextureResource& texture
    ) {
        if (texture.isAnimated() || texture.images.size() != 1) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rendering does not support multi-image animated textures"
            );
        }
        const bool hasColumns = texture.spritesheetColumns > 0;
        const bool hasRows = texture.spritesheetRows > 0;
        const bool hasFrames = texture.spritesheetFrameCount > 0;
        if (!hasColumns && !hasRows && !hasFrames) return {};
        if (!hasColumns || !hasRows || !hasFrames ||
            !std::isfinite(texture.spritesheetDuration) ||
            texture.spritesheetDuration < 0.0F ||
            static_cast<std::uint64_t>(texture.spritesheetColumns) *
                    texture.spritesheetRows <
                texture.spritesheetFrameCount ||
            !std::isfinite(texture.resolution[0]) ||
            !std::isfinite(texture.resolution[1]) ||
            texture.resolution[0] <= 0.0F || texture.resolution[1] <= 0.0F) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle spritesheet metadata is incomplete or invalid"
            );
        }
        const float frameWidth = 1.0F / float(texture.spritesheetColumns);
        const float frameHeight = 1.0F / float(texture.spritesheetRows);
        const float frameAspect =
            (texture.resolution[1] * frameHeight) /
            (texture.resolution[0] * frameWidth);
        if (!std::isfinite(frameAspect) || frameAspect <= 0.0F) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle spritesheet frame aspect ratio is invalid"
            );
        }
        return {
            .columns = texture.spritesheetColumns,
            .rows = texture.spritesheetRows,
            .frames = texture.spritesheetFrameCount,
            .duration = texture.spritesheetDuration,
            .frameAspect = frameAspect,
        };
    }

    [[nodiscard]] static double particleLifetimeAttribute(
        const particle::ParticleInstance& particle,
        const FrameParticleDescriptor& descriptor,
        const ParticleAtlasMetadata& atlas
    ) {
        const double life = particle.lifetimePosition();
        if (!std::isfinite(life) || life < 0.0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle normalized lifetime is invalid"
            );
        }
        if (!atlas.enabled()) return life;
        if (!std::isfinite(descriptor.sequenceMultiplier) ||
            descriptor.sequenceMultiplier <= 0.0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle sequence multiplier must be finite and greater than zero"
            );
        }

        const double frameCount = static_cast<double>(atlas.frames);
        if (descriptor.animationMode == "randomframe") {
            if (!std::isfinite(particle.randomFrameUnit) ||
                particle.randomFrameUnit < 0.0 ||
                particle.randomFrameUnit >= 1.0) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle random frame unit must be finite and in [0, 1)"
                );
            }
            const double frame = std::floor(
                particle.randomFrameUnit * frameCount
            );
            return (frame + 0.5) / frameCount;
        }
        if (descriptor.animationMode == "once") {
            const double frame = std::min(
                life * frameCount * descriptor.sequenceMultiplier,
                frameCount - 1.0
            );
            return frame / frameCount;
        }
        if (descriptor.animationMode != "sequence") {
            throw Error(
                ErrorCode::resourceValidation,
                "Unknown particle animation mode '" +
                    descriptor.animationMode + "'"
            );
        }
        if (atlas.duration > 0.0F) {
            const double timeInCycle = std::fmod(
                particle.age * descriptor.sequenceMultiplier,
                static_cast<double>(atlas.duration)
            );
            return timeInCycle / static_cast<double>(atlas.duration);
        }
        const double frame = std::fmod(
            life * frameCount * descriptor.sequenceMultiplier,
            frameCount
        );
        return frame / frameCount;
    }

    [[nodiscard]] static ParticleDrawBatch particleBatch(
        const particle::ParticleSimulation& simulation,
        const FrameParticleDescriptor& descriptor,
        ParticleAtlasMetadata atlas
    ) {
        const auto& particles = simulation.particles();
        if (particles.size() >
            std::numeric_limits<std::uint32_t>::max() / 4U) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle vertex index count exceeds the uint32 range"
            );
        }
        if (particles.size() >
                std::numeric_limits<std::size_t>::max() / 4U ||
            particles.size() >
                std::numeric_limits<std::size_t>::max() / 6U) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle geometry count overflows size_t"
            );
        }

        ParticleDrawBatch batch;
        batch.atlas = atlas;
        batch.vertices.reserve(particles.size() * 4U);
        batch.indices.reserve(particles.size() * 6U);
        for (const auto& particle : particles) {
            const std::array<std::array<float, 2>, 4> coordinates{{
                {{0.0F, 1.0F}},
                {{1.0F, 1.0F}},
                {{1.0F, 0.0F}},
                {{0.0F, 0.0F}},
            }};
            const auto position = std::array{
                particleFloat(particle.position.x, "position"),
                particleFloat(particle.position.y, "position"),
                particleFloat(particle.position.z, "position"),
            };
            const auto velocity = std::array{
                particleFloat(particle.velocity.x, "velocity"),
                particleFloat(particle.velocity.y, "velocity"),
                particleFloat(particle.velocity.z, "velocity"),
            };
            const auto rotation = std::array{
                particleFloat(particle.rotation.x, "rotation"),
                particleFloat(particle.rotation.y, "rotation"),
                particleFloat(particle.rotation.z, "rotation"),
            };
            const auto color = std::array{
                particleFloat(particle.color.x, "color"),
                particleFloat(particle.color.y, "color"),
                particleFloat(particle.color.z, "color"),
                particleFloat(particle.alpha, "alpha"),
            };
            const float size = particleFloat(particle.size, "size");
            const float lifetime = particleFloat(
                particleLifetimeAttribute(particle, descriptor, atlas),
                "encoded lifetime"
            );
            const std::uint32_t base = static_cast<std::uint32_t>(
                batch.vertices.size()
            );
            for (const auto& coordinate : coordinates) {
                batch.vertices.push_back({
                    .position = {position[0], position[1], position[2]},
                    .texCoordRotationSize = {
                        coordinate[0], coordinate[1], rotation[2], size,
                    },
                    .color = {color[0], color[1], color[2], color[3]},
                    .velocityLifetime = {
                        velocity[0], velocity[1], velocity[2], lifetime,
                    },
                    .rotationXY = {rotation[0], rotation[1]},
                });
            }
            batch.indices.insert(
                batch.indices.end(),
                {base, base + 1U, base + 2U, base + 2U, base + 3U, base}
            );
        }
        return batch;
    }

    [[nodiscard]] std::string particleAssetIdentity(
        const FrameParticleDescriptor& descriptor
    ) const {
        return frameGraph->graph()->model()->project().assetPath +
            "\x1f" + descriptor.definitionIdentity;
    }

    void initializeParticleStates(
        const FramePlan& plan,
        std::map<std::size_t, ParticleState>& working
    ) const {
        std::set<std::size_t> descriptorIndexes;
        for (const auto& descriptor : plan.particles) {
            if (!descriptorIndexes.emplace(descriptor.objectIndex).second) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame plan contains duplicate particle object indexes"
                );
            }
        }
        for (auto iterator = working.begin(); iterator != working.end();) {
            if (!descriptorIndexes.contains(iterator->first)) {
                iterator = working.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    [[nodiscard]] std::pair<ParticleState, ParticleDrawBatch>
    advanceParticleState(
        const FrameParticleDescriptor& descriptor,
        const ParticleAtlasMetadata& atlas,
        const ResolvedFrameInputs& inputs,
        const ParticleState* previous
    ) {
        const std::string identity = particleAssetIdentity(descriptor);
        try {
            ParticleState state = previous != nullptr &&
                    previous->assetIdentity == identity
                ? *previous
                : ParticleState{
                      .assetIdentity = identity,
                      .simulation = particle::ParticleSimulation(
                          descriptor.configuration,
                          descriptor.objectId,
                          identity
                      ),
                  };
            state.simulation.advance(
                inputs.frameTimeSeconds,
                descriptor.configuration
            );
            ParticleDrawBatch batch = particleBatch(
                state.simulation, descriptor, atlas
            );
            return {std::move(state), std::move(batch)};
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::invalid_argument& error) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle object " + std::to_string(descriptor.objectId) +
                    " simulation failed: " + error.what()
            );
        } catch (const std::overflow_error& error) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle object " + std::to_string(descriptor.objectId) +
                    " simulation failed: " + error.what()
            );
        }
    }

    [[nodiscard]] ParticlePreparation prepareParticle(
        Device::Session& session,
        const FramePlan& plan,
        const FrameParticleCommand& command,
        std::size_t operationIndex,
        const ResolvedFrameInputs& inputs,
        const FrameVector2& frameParallax,
        const PreparedCamera& camera,
        const std::map<std::string, std::string>& aliases,
        const ParticleState* previousState,
        const ParticleDrawBatch* frozenBatch
    ) {
        if (command.particleIndex >= plan.particles.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame particle command index is invalid"
            );
        }
        const auto& descriptor = plan.particles[command.particleIndex];
        if (descriptor.objectId != command.objectId) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame particle command object identity is inconsistent"
            );
        }
        if (command.destination != plan.output) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle phase one requires the scene output as its destination"
            );
        }
        if (descriptor.shader != "genericparticle") {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle phase one requires the genericparticle shader"
            );
        }
        if (descriptor.texture0.kind != FrameResourceKind::assetTexture ||
            descriptor.texture0.id.empty()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle texture slot zero must reference a real asset texture"
            );
        }
        static_cast<void>(framebuffer(command.destination, aliases));

        AssetTextureResource& textureResource = assetTexture(
            session, descriptor.texture0
        );
        const ParticleAtlasMetadata atlas = particleAtlasMetadata(
            textureResource
        );
        std::optional<ParticleDrawBatch> nextBatch;
        std::optional<ParticleState> nextState;
        const ParticleDrawBatch* batch = frozenBatch;
        if (batch == nullptr) {
            auto advanced = advanceParticleState(
                descriptor, atlas, inputs, previousState
            );
            nextState.emplace(std::move(advanced.first));
            nextBatch.emplace(std::move(advanced.second));
            batch = &*nextBatch;
        }

        ComboMap effectiveCombos = descriptor.combos;
        const auto requireCombo = [&](const char* name, int expected) {
            const auto authored = effectiveCombos.find(name);
            if (authored != effectiveCombos.end() &&
                authored->second != expected) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle shader combination '" + std::string(name) +
                        "' must be " + std::to_string(expected)
                );
            }
            effectiveCombos[name] = expected;
        };
        requireCombo("GS_ENABLED", 0);
        requireCombo("THICKFORMAT", 1);
        requireCombo("TRAILRENDERER", 0);
        effectiveCombos["SPRITESHEET"] = batch->atlas.enabled() ? 1 : 0;
        ProgramResource& programResource = program(
            session,
            descriptor.vertexShaderPath,
            descriptor.fragmentShaderPath,
            effectiveCombos,
            "Particle render pass"
        );
        const GLuint activeProgram = programResource.program;
        if (textureResource.isAnimated() || textureResource.images.size() != 1) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle phase one requires one static texture image in slot zero"
            );
        }

        PreparedParticle prepared{
            .destination = command.destination,
            .blending = descriptor.blending,
            .culling = descriptor.culling,
            .depthTest = descriptor.depthTest,
            .depthWrite = descriptor.depthWrite,
            .program = activeProgram,
            .texture = textureResource.images.front(),
            .textureResolution = textureResource.resolution,
            .operationIndex = operationIndex,
        };
        prepared.attributeLocations.fill(-1);
        prepared.uniforms.texture0 = prepareBuiltinUniform(
            programResource, "g_Texture0", GL_SAMPLER_2D
        );
        prepared.uniforms.texture0Resolution = prepareBuiltinUniform(
            programResource, "g_Texture0Resolution", GL_FLOAT_VEC4
        );
        prepared.uniforms.modelInverse = prepareBuiltinUniform(
            programResource, "g_ModelMatrixInverse", GL_FLOAT_MAT4
        );
        prepared.uniforms.orientationUp = prepareBuiltinUniform(
            programResource, "g_OrientationUp", GL_FLOAT_VEC3
        );
        prepared.uniforms.orientationRight = prepareBuiltinUniform(
            programResource, "g_OrientationRight", GL_FLOAT_VEC3
        );
        prepared.uniforms.orientationForward = prepareBuiltinUniform(
            programResource, "g_OrientationForward", GL_FLOAT_VEC3
        );
        prepared.uniforms.viewUp = prepareBuiltinUniform(
            programResource, "g_ViewUp", GL_FLOAT_VEC3
        );
        prepared.uniforms.viewRight = prepareBuiltinUniform(
            programResource, "g_ViewRight", GL_FLOAT_VEC3
        );
        prepared.uniforms.eyePosition = prepareBuiltinUniform(
            programResource, "g_EyePosition", GL_FLOAT_VEC3
        );
        prepared.uniforms.renderVar0 = prepareBuiltinUniform(
            programResource, "g_RenderVar0", GL_FLOAT_VEC4
        );
        prepared.uniforms.renderVar1 = prepareBuiltinUniform(
            programResource, "g_RenderVar1", GL_FLOAT_VEC4
        );
        const auto& transform = descriptor.worldTransform;
        const float originX = particleFloat(
            transform.origin.x - static_cast<double>(plan.width) * 0.5,
            "object origin"
        );
        const float originY = particleFloat(
            static_cast<double>(plan.height) * 0.5 - transform.origin.y,
            "object origin"
        );
        const float originZ = particleFloat(transform.origin.z, "object origin");
        Matrix model = multiply(
            translation(originX, originY, originZ),
            multiply(
                rotationZ(-particleFloat(transform.angles.z, "object angle")),
                multiply(
                    rotationY(particleFloat(transform.angles.y, "object angle")),
                    multiply(
                        rotationX(-particleFloat(transform.angles.x, "object angle")),
                        scaling(
                            particleFloat(transform.scale.x, "object scale"),
                            particleFloat(transform.scale.y, "object scale"),
                            particleFloat(transform.scale.z, "object scale")
                        )
                    )
                )
            )
        );
        if (plan.parallax.enabled) {
            double depthX = descriptor.parallaxDepth.x;
            double depthY = descriptor.parallaxDepth.y;
            constexpr double minimumParticleDepth = 0.65;
            if (std::abs(depthX) < minimumParticleDepth) {
                depthX = depthX < 0.0
                    ? -minimumParticleDepth : minimumParticleDepth;
            }
            if (std::abs(depthY) < minimumParticleDepth) {
                depthY = depthY < 0.0
                    ? -minimumParticleDepth : minimumParticleDepth;
            }
            const FrameVector2 parallaxOffset = sceneParallaxOffset(
                plan,
                {.x = depthX, .y = depthY},
                frameParallax,
                "particle"
            );
            // Linux inserts particle parallax after the object origin and
            // before rotation and scale. Since translations commute, left
            // multiplying the completed model preserves that world-space order.
            model = multiply(
                translation(
                    static_cast<float>(parallaxOffset.x),
                    static_cast<float>(parallaxOffset.y),
                    0.0F
                ),
                model
            );
        }
        Matrix viewProjection;
        std::array<float, 3> eyePosition = camera.eyePosition;
        if (descriptor.perspective) {
            if (!camera.particlePerspective) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Particle perspective camera was not prepared"
                );
            }
            viewProjection = camera.particlePerspective->viewProjection;
            eyePosition = camera.particlePerspective->eyePosition;
        } else {
            if (!camera.orthographicViewProjection) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Scene orthographic camera was not prepared"
                );
            }
            viewProjection = *camera.orthographicViewProjection;
        }
        prepared.model = model;
        prepared.viewProjection = viewProjection;
        prepared.modelViewProjection = multiply(viewProjection, model);
        validateMatrix(prepared.model, "Particle model matrix");
        validateMatrix(
            prepared.viewProjection,
            "Particle view-projection matrix"
        );
        validateMatrix(
            prepared.modelViewProjection,
            "Particle model-view-projection matrix"
        );
        if (prepared.uniforms.modelInverse >= 0) {
            prepared.modelInverse = inverse(
                prepared.model, "Particle model matrix"
            );
        }
        prepared.eyePosition = eyePosition;
        if (batch->atlas.enabled()) {
            prepared.renderVar1 = {
                1.0F / float(batch->atlas.columns),
                1.0F / float(batch->atlas.rows),
                float(batch->atlas.frames),
                batch->atlas.frameAspect,
            };
        } else {
            const float textureWidth = textureResource.resolution[2];
            const float textureHeight = textureResource.resolution[3];
            if (!std::isfinite(textureWidth) || !std::isfinite(textureHeight) ||
                textureWidth <= 0.0F || textureHeight <= 0.0F) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle texture has an invalid real resolution"
                );
            }
            // Linux exposes the non-atlas texture's real height/width ratio
            // through g_RenderVar1.w for genericparticle shaders.
            prepared.renderVar1 = {
                0.0F, 0.0F, 0.0F, textureHeight / textureWidth,
            };
        }
        static_cast<void>(particleFloat(inputs.timeSeconds, "frame time"));
        const auto& overrides = descriptor.configuration.overrides;
        prepared.commonUniforms = prepareCommonUniforms(
            programResource,
            plan,
            inputs,
            prepared.model,
            prepared.viewProjection,
            prepared.modelViewProjection,
            {
                .brightness = 1.0F,
                .alpha = particleFloat(
                    overrides.alpha, "instance alpha override"
                ),
                .color = {
                    particleFloat(overrides.color.x, "instance color override"),
                    particleFloat(overrides.color.y, "instance color override"),
                    particleFloat(overrides.color.z, "instance color override"),
                    1.0F,
                },
            }
        );
        for (const auto& metadata : programResource.parameters) {
            if (!metadata.material || isSamplerParameter(metadata)) continue;
            if (metadata.defaultValue) {
                if (auto uniform = prepareRuntimeUniform(
                        programResource,
                        metadata.name,
                        metadataDefault(*metadata.defaultValue))) {
                    prepared.materialUniforms.push_back(std::move(*uniform));
                }
            } else if (activeUniform(
                           programResource, metadata.name) != nullptr) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active particle material uniform '" + metadata.name +
                        "' has no metadata default"
                );
            }
        }

        if (batch->vertices.size() >
                static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max()) /
                    sizeof(ParticleVertex) ||
            batch->indices.size() >
                static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max()) /
                    sizeof(std::uint32_t) ||
            batch->indices.size() >
                static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle geometry exceeds OpenGL buffer or draw-count limits"
            );
        }

        ensureParticleGeometry(session);
        const auto prepareAttribute = [&](std::size_t index,
                                          const char* name) {
            prepared.attributeLocations[index] = glGetAttribLocation(
                activeProgram, name
            );
        };
        prepareAttribute(0, "a_Position");
        prepareAttribute(1, "a_TexCoordVec4");
        prepareAttribute(2, "a_Color");
        prepareAttribute(3, "a_TexCoordVec4C1");
        prepareAttribute(4, "a_TexCoordC2");
        session.checkError(
            ErrorCode::draw,
            "preparing particle rendering"
        );
        return {
            .operation = std::move(prepared),
            .batch = std::move(nextBatch),
            .state = std::move(nextState),
        };
    }

    void drawParticle(
        Device::Session& session,
        const PreparedParticle& prepared,
        const ParticleDrawBatch& batch
    ) {
        if (batch.vertices.empty()) {
            glDisable(GL_DEPTH_CLAMP);
            return;
        }
        auto& destination = framebuffer(prepared.destination);
        glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
        glViewport(
            0, 0,
            static_cast<GLsizei>(destination.width),
            static_cast<GLsizei>(destination.height)
        );
        configureState({
            .blending = prepared.blending,
            .culling = prepared.culling,
            .depthTest = prepared.depthTest,
            .depthWrite = prepared.depthWrite,
            .writeAlpha = true,
        });
        glEnable(GL_DEPTH_CLAMP);
        glUseProgram(prepared.program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, prepared.texture);
        if (prepared.uniforms.texture0 >= 0) {
            glUniform1i(prepared.uniforms.texture0, 0);
        }
        bindVector4(
            prepared.uniforms.texture0Resolution,
            prepared.textureResolution
        );
        for (const PreparedUniform& uniform : prepared.materialUniforms) {
            bindPreparedUniform(uniform);
        }
        bindCommonUniforms(
            prepared.commonUniforms,
            prepared.model,
            prepared.viewProjection,
            prepared.modelViewProjection
        );
        bindMatrix(prepared.uniforms.modelInverse, prepared.modelInverse);
        bindVector3(
            prepared.uniforms.orientationUp, {0.0F, 1.0F, 0.0F}
        );
        bindVector3(
            prepared.uniforms.orientationRight, {1.0F, 0.0F, 0.0F}
        );
        bindVector3(
            prepared.uniforms.orientationForward, {0.0F, 0.0F, 1.0F}
        );
        bindVector3(prepared.uniforms.viewUp, {0.0F, 1.0F, 0.0F});
        bindVector3(prepared.uniforms.viewRight, {1.0F, 0.0F, 0.0F});
        bindVector3(prepared.uniforms.eyePosition, prepared.eyePosition);
        bindVector4(
            prepared.uniforms.renderVar0, {0.0F, 0.0F, 0.0F, 0.0F}
        );
        bindVector4(prepared.uniforms.renderVar1, prepared.renderVar1);
        glBindVertexArray(particleVertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, particleVertexBuffer);
        disableVertexAttributes();
        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(batch.vertices.size() * sizeof(ParticleVertex)),
            batch.vertices.data(),
            GL_DYNAMIC_DRAW
        );
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, particleElementBuffer);
        glBufferData(
            GL_ELEMENT_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(batch.indices.size() * sizeof(std::uint32_t)),
            batch.indices.data(),
            GL_DYNAMIC_DRAW
        );

        const GLsizei stride = static_cast<GLsizei>(sizeof(ParticleVertex));
        const auto bindAttribute = [&](GLint location, GLint components,
                                       std::size_t offset) {
            if (location < 0) return;
            glEnableVertexAttribArray(static_cast<GLuint>(location));
            glVertexAttribPointer(
                static_cast<GLuint>(location),
                components,
                GL_FLOAT,
                GL_FALSE,
                stride,
                reinterpret_cast<const void*>(offset)
            );
        };
        bindAttribute(
            prepared.attributeLocations[0],
            3,
            offsetof(ParticleVertex, position)
        );
        bindAttribute(
            prepared.attributeLocations[1],
            4,
            offsetof(ParticleVertex, texCoordRotationSize)
        );
        bindAttribute(
            prepared.attributeLocations[2],
            4,
            offsetof(ParticleVertex, color)
        );
        bindAttribute(
            prepared.attributeLocations[3],
            4,
            offsetof(ParticleVertex, velocityLifetime)
        );
        bindAttribute(
            prepared.attributeLocations[4],
            2,
            offsetof(ParticleVertex, rotationXY)
        );
        glDrawElements(
            GL_TRIANGLES,
            static_cast<GLsizei>(batch.indices.size()),
            GL_UNSIGNED_INT,
            nullptr
        );
        glDisable(GL_DEPTH_CLAMP);
        session.checkError(ErrorCode::draw, "drawing particle sprites");
    }

    static void normalizeGLState() {
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_DEPTH_CLAMP);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_SAMPLE_COVERAGE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DITHER);
        glDisable(GL_MULTISAMPLE);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    static void disableVertexAttributes() {
        GLint count = 0;
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &count);
        for (GLint index = 0; index < count; ++index) {
            glDisableVertexAttribArray(static_cast<GLuint>(index));
        }
    }

    [[nodiscard]] ObjectOperationGroup operationGroup(
        const FramePlan& plan,
        const FrameOperation& operation,
        std::size_t operationIndex
    ) const {
        const auto imageIdentity = [&](const FramePassOrigin& origin) {
            if (origin.imageIndex >= plan.images.size()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " references an invalid image descriptor index"
                );
            }
            const FrameImageDescriptor& descriptor =
                plan.images[origin.imageIndex];
            if (descriptor.objectId != origin.objectId) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " has an inconsistent image object identity"
                );
            }
            return ObjectOperationGroup{
                .objectIndex = descriptor.objectIndex,
                .objectId = descriptor.objectId,
            };
        };

        if (const auto* pass = std::get_if<FrameRenderPass>(&operation)) {
            return imageIdentity(pass->origin);
        }
        if (const auto* command = std::get_if<FrameCopyCommand>(&operation)) {
            return imageIdentity(command->origin);
        }
        if (const auto* command = std::get_if<FrameSwapCommand>(&operation)) {
            return imageIdentity(command->origin);
        }
        if (const auto* command = std::get_if<FrameClearCommand>(&operation)) {
            return imageIdentity(command->origin);
        }
        if (const auto* command = std::get_if<FrameTextCommand>(&operation)) {
            if (command->textIndex >= plan.texts.size()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " references an invalid text descriptor index"
                );
            }
            const FrameTextDescriptor& descriptor =
                plan.texts[command->textIndex];
            if (descriptor.objectId != command->objectId) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " has an inconsistent text object identity"
                );
            }
            return {
                .objectIndex = descriptor.objectIndex,
                .objectId = descriptor.objectId,
            };
        }
        if (const auto* command =
                std::get_if<FrameParticleCommand>(&operation)) {
            if (command->particleIndex >= plan.particles.size()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " references an invalid particle descriptor index"
                );
            }
            const FrameParticleDescriptor& descriptor =
                plan.particles[command->particleIndex];
            if (descriptor.objectId != command->objectId) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " has an inconsistent particle object identity"
                );
            }
            if (command->destination != plan.output) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame particle operation destination is inconsistent with the plan output"
                );
            }
            return {
                .objectIndex = descriptor.objectIndex,
                .objectId = descriptor.objectId,
            };
        }
        throw Error(
            ErrorCode::internalFailure,
            "Frame plan contains an unknown operation variant"
        );
    }

    void validatePlanFramebufferReferences(const FramePlan& plan) const {
        std::set<std::string> framebufferIds;
        for (const FramebufferDescriptor& descriptor : plan.framebuffers) {
            if (descriptor.resource.kind != FrameResourceKind::framebuffer ||
                descriptor.resource.id.empty() ||
                !framebufferIds.emplace(descriptor.resource.id).second) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame plan contains an invalid or duplicate framebuffer descriptor"
                );
            }
        }
        const auto requireFramebuffer = [&](const FrameResourceRef& resource,
                                            std::string_view description) {
            if (resource.kind != FrameResourceKind::framebuffer ||
                !framebufferIds.contains(resource.id)) {
                throw Error(
                    ErrorCode::resourceValidation,
                    std::string(description) +
                        " references an unknown framebuffer '" +
                        resource.id + "'"
                );
            }
        };
        const auto validateResource = [&](const FrameResourceRef& resource,
                                          std::string_view description) {
            if (resource.kind == FrameResourceKind::framebuffer) {
                requireFramebuffer(resource, description);
            }
        };

        requireFramebuffer(plan.output, "Frame plan output");
        for (const FrameOperation& operation : plan.operations) {
            if (const auto* pass = std::get_if<FrameRenderPass>(&operation)) {
                requireFramebuffer(pass->destination, "Frame render destination");
                validateResource(pass->input, "Frame render input");
                if (pass->previousInput) {
                    validateResource(*pass->previousInput, "Frame render previous input");
                }
                for (const auto& [slot, resource] : pass->textures) {
                    static_cast<void>(slot);
                    validateResource(resource, "Frame render texture");
                }
            } else if (const auto* command =
                           std::get_if<FrameCopyCommand>(&operation)) {
                validateResource(command->source, "Frame copy source");
                requireFramebuffer(command->destination, "Frame copy destination");
            } else if (const auto* command =
                           std::get_if<FrameSwapCommand>(&operation)) {
                requireFramebuffer(command->source, "Frame swap source");
                requireFramebuffer(command->destination, "Frame swap destination");
            } else if (const auto* command =
                           std::get_if<FrameClearCommand>(&operation)) {
                requireFramebuffer(command->destination, "Frame clear destination");
            } else if (const auto* command =
                           std::get_if<FrameTextCommand>(&operation)) {
                requireFramebuffer(command->destination, "Frame text destination");
            } else if (const auto* command =
                           std::get_if<FrameParticleCommand>(&operation)) {
                requireFramebuffer(command->destination, "Frame particle destination");
            }
        }
    }

    [[nodiscard]] std::vector<ObjectOperationGroup> objectOperationGroups(
        const FramePlan& plan
    ) const {
        if (!plan.isExecutable()) {
            throw Error(ErrorCode::resourceValidation, planIssues(plan));
        }
        validatePlanFramebufferReferences(plan);

        std::vector<ObjectOperationGroup> groups;
        std::set<std::size_t> closedObjects;
        std::set<std::size_t> scheduledParticles;
        for (std::size_t operationIndex = 0;
             operationIndex < plan.operations.size(); ++operationIndex) {
            ObjectOperationGroup identity = operationGroup(
                plan, plan.operations[operationIndex], operationIndex
            );
            if (groups.empty() ||
                groups.back().objectIndex != identity.objectIndex) {
                if (!groups.empty()) {
                    closedObjects.emplace(groups.back().objectIndex);
                }
                if (closedObjects.contains(identity.objectIndex)) {
                    throw Error(
                        ErrorCode::resourceValidation,
                        "Frame plan operations for object index " +
                            std::to_string(identity.objectIndex) +
                            " are not contiguous"
                    );
                }
                groups.push_back(std::move(identity));
            } else if (groups.back().objectId != identity.objectId) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame plan reuses an object index with inconsistent object identities"
                );
            }
            groups.back().operationIndexes.push_back(operationIndex);

            if (std::holds_alternative<FrameParticleCommand>(
                    plan.operations[operationIndex]) &&
                !scheduledParticles.emplace(groups.back().objectIndex).second) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame plan schedules a particle object more than once"
                );
            }
        }
        return groups;
    }

    [[nodiscard]] PreparedCamera prepareCamera(const FramePlan& plan) const {
        bool needsOrthographic = false;
        bool needsParticlePerspective = false;
        bool needsOrthographicEye = false;
        for (const FrameOperation& operation : plan.operations) {
            if (const auto* pass = std::get_if<FrameRenderPass>(&operation)) {
                needsOrthographic = needsOrthographic ||
                    pass->geometry == FrameGeometryKind::imageScene ||
                    pass->geometry == FrameGeometryKind::passthroughCapture;
            } else if (std::holds_alternative<FrameTextCommand>(operation)) {
                needsOrthographic = true;
            } else if (const auto* command =
                           std::get_if<FrameParticleCommand>(&operation)) {
                const FrameParticleDescriptor& descriptor =
                    plan.particles.at(command->particleIndex);
                if (descriptor.perspective) {
                    needsParticlePerspective = true;
                } else {
                    needsOrthographic = true;
                    needsOrthographicEye = true;
                }
            }
        }

        const float cameraWidth = plan.camera.orthogonalProjectionAuto
            ? static_cast<float>(plan.width)
            : static_cast<float>(plan.camera.orthogonalProjectionWidth);
        const float cameraHeight = plan.camera.orthogonalProjectionAuto
            ? static_cast<float>(plan.height)
            : static_cast<float>(plan.camera.orthogonalProjectionHeight);
        PreparedCamera result;
        if (needsOrthographic) {
            result.orthographicViewProjection.emplace(
                sceneOrthographicViewProjection(
                    plan.camera, cameraWidth, cameraHeight
                )
            );
            validateMatrix(
                *result.orthographicViewProjection,
                "Scene orthographic view-projection matrix"
            );
        }
        if (needsParticlePerspective) {
            result.particlePerspective.emplace(
                particlePerspectiveView(plan.camera, cameraWidth, cameraHeight)
            );
            validateMatrix(
                result.particlePerspective->viewProjection,
                "Particle perspective view-projection matrix"
            );
        }
        if (needsOrthographicEye) {
            result.eyePosition = {
                checkedFloat(plan.camera.eye.x, "Camera eye"),
                checkedFloat(plan.camera.eye.y, "Camera eye"),
                checkedFloat(plan.camera.eye.z, "Camera eye"),
            };
        }
        return result;
    }

    void prepareFrameArena(Device::Session& session, const FramePlan& plan) {
        normalizeGLState();
        session.checkError(
            ErrorCode::unsupportedContext,
            "normalizing the OpenGL context before frame preparation"
        );
        ensureFramebuffers(session, plan);
        static_cast<void>(framebuffer(plan.output));

        bool needsGeometry = false;
        bool needsParticleGeometry = false;
        for (const FrameOperation& operation : plan.operations) {
            needsGeometry = needsGeometry ||
                std::holds_alternative<FrameRenderPass>(operation) ||
                std::holds_alternative<FrameCopyCommand>(operation);
            needsParticleGeometry = needsParticleGeometry ||
                std::holds_alternative<FrameParticleCommand>(operation);
        }
        if (needsGeometry) ensureGeometry(session);
        if (needsParticleGeometry) ensureParticleGeometry(session);
    }

    void beginFrameOutput(Device::Session& session, const FramePlan& plan) {
        std::array<GLfloat, 4> authoredClear{};
        if (plan.clearEnabled) {
            authoredClear = {
                checkedFloat(plan.clearColor.red, "Frame clear color"),
                checkedFloat(plan.clearColor.green, "Frame clear color"),
                checkedFloat(plan.clearColor.blue, "Frame clear color"),
                checkedFloat(plan.clearColor.alpha, "Frame clear color"),
            };
        }
        normalizeGLState();
        session.checkError(
            ErrorCode::unsupportedContext,
            "normalizing the OpenGL context before frame execution"
        );
        auto& output = framebuffer(plan.output);
        glBindFramebuffer(GL_FRAMEBUFFER, output.framebuffer);
        glViewport(0, 0, output.width, output.height);
        glClearColor(0, 0, 0, 0);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (plan.clearEnabled) {
            glClearColor(
                authoredClear[0], authoredClear[1],
                authoredClear[2], authoredClear[3]
            );
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        session.checkError(ErrorCode::draw, "clearing the frame output");
    }

    [[nodiscard]] static bool isFrameFatalPreparationError(
        const Error& error
    ) noexcept {
        switch (error.code()) {
            case ErrorCode::contextCreation:
            case ErrorCode::unsupportedContext:
            case ErrorCode::framebufferCreation:
            case ErrorCode::readback:
            case ErrorCode::internalFailure:
                return true;
            case ErrorCode::invalidArgument:
            case ErrorCode::shaderCompilation:
            case ErrorCode::programLink:
            case ErrorCode::draw:
            case ErrorCode::textureDecode:
            case ErrorCode::textureUpload:
            case ErrorCode::resourceValidation:
                return false;
        }
        return true;
    }

    static void validateIsolatedPreparationState() {
        normalizeGLState();
        const GLenum residualError = glGetError();
        if (residualError != GL_NO_ERROR) {
            throw Error(
                ErrorCode::internalFailure,
                "Object preparation left residual OpenGL error " +
                    std::to_string(residualError)
            );
        }
    }

    [[nodiscard]] PreparedFrame preflightFrameObjects(
        Device::Session& session,
        const FramePlan& plan,
        const std::vector<ObjectOperationGroup>& groups,
        const ResolvedFrameInputs& inputs,
        const FrameVector2& frameParallax,
        const PreparedCamera& camera,
        const std::map<std::size_t, ParticleDrawBatch>* frozenParticleBatches =
            nullptr,
        const std::vector<FrameExecutionIssue>* frozenIssues = nullptr
    ) {
        PreparedFrame result;
        result.operations.resize(plan.operations.size());
        result.frozenParticleBatches = frozenParticleBatches;
        if (frozenParticleBatches == nullptr) {
            result.particleStates = particles;
            initializeParticleStates(plan, result.particleStates);
        }
        std::map<std::string, std::string> aliases = framebufferAliases;

        for (const ObjectOperationGroup& group : groups) {
            if (frozenIssues != nullptr) {
                const auto frozenIssue = std::find_if(
                    frozenIssues->begin(), frozenIssues->end(),
                    [&](const FrameExecutionIssue& issue) {
                        return issue.severity ==
                                FramePlanIssueSeverity::skipObject &&
                            issue.objectIndex == group.objectIndex;
                    }
                );
                if (frozenIssue != frozenIssues->end()) {
                    result.issues.push_back(*frozenIssue);
                    continue;
                }
            }
            std::map<std::string, std::string> candidateAliases = aliases;
            std::vector<std::pair<std::size_t, PreparedOperation>>
                candidateOperations;
            candidateOperations.reserve(group.operationIndexes.size());
            std::optional<std::pair<std::size_t, ParticleDrawBatch>>
                candidateParticleBatch;
            std::optional<ParticleState> candidateParticleState;
            std::size_t failingOperation = group.operationIndexes.front();
            const auto recordSkippedObject = [&](std::string message) {
                validateIsolatedPreparationState();
                result.issues.push_back({
                    .severity = FramePlanIssueSeverity::skipObject,
                    .objectIndex = group.objectIndex,
                    .objectId = group.objectId,
                    .operationIndex = failingOperation,
                    .message = std::move(message),
                });
            };

            try {
                for (const std::size_t operationIndex :
                     group.operationIndexes) {
                    failingOperation = operationIndex;
                    const FrameOperation& operation =
                        plan.operations[operationIndex];
                    if (const auto* pass =
                            std::get_if<FrameRenderPass>(&operation)) {
                        candidateOperations.emplace_back(
                            operationIndex,
                            prepareDraw(
                                session, plan, *pass, inputs, frameParallax,
                                camera, candidateAliases
                            )
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameCopyCommand>(&operation)) {
                        candidateOperations.emplace_back(
                            operationIndex,
                            prepareCopy(
                                session, plan, *command, inputs,
                                frameParallax, camera, candidateAliases
                            )
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameSwapCommand>(&operation)) {
                        prepareSwap(*command, candidateAliases);
                        candidateOperations.emplace_back(
                            operationIndex, *command
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameClearCommand>(&operation)) {
                        candidateOperations.emplace_back(
                            operationIndex,
                            prepareClear(*command, candidateAliases)
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameTextCommand>(&operation)) {
                        candidateOperations.emplace_back(
                            operationIndex,
                            prepareText(
                                session, plan, *command, camera,
                                candidateAliases
                            )
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameParticleCommand>(&operation)) {
                        const ParticleState* previousState = nullptr;
                        if (candidateParticleState) {
                            previousState = &*candidateParticleState;
                        } else if (frozenParticleBatches == nullptr) {
                            const auto previous = result.particleStates.find(
                                group.objectIndex
                            );
                            if (previous != result.particleStates.end()) {
                                previousState = &previous->second;
                            }
                        }
                        const ParticleDrawBatch* frozenBatch = nullptr;
                        if (frozenParticleBatches != nullptr) {
                            const auto found = frozenParticleBatches->find(
                                operationIndex
                            );
                            if (found == frozenParticleBatches->end()) {
                                throw Error(
                                    ErrorCode::internalFailure,
                                    "Replay particle operation has no frozen simulation batch"
                                );
                            }
                            frozenBatch = &found->second;
                        }
                        ParticlePreparation prepared = prepareParticle(
                            session, plan, *command, operationIndex, inputs,
                            frameParallax, camera, candidateAliases,
                            previousState, frozenBatch
                        );
                        candidateOperations.emplace_back(
                            operationIndex, std::move(prepared.operation)
                        );
                        if (prepared.batch) {
                            candidateParticleBatch.emplace(
                                operationIndex, std::move(*prepared.batch)
                            );
                        }
                        if (prepared.state) {
                            candidateParticleState.emplace(
                                std::move(*prepared.state)
                            );
                        }
                    } else {
                        throw Error(
                            ErrorCode::internalFailure,
                            "Frame plan contains an unknown operation variant"
                        );
                    }
                }
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const Error& error) {
                if (isFrameFatalPreparationError(error)) throw;
                recordSkippedObject(error.what());
                continue;
            } catch (const ShaderCompileError& error) {
                recordSkippedObject(error.what());
                continue;
            } catch (const FormatError& error) {
                recordSkippedObject(error.what());
                continue;
            } catch (const text::Error& error) {
                recordSkippedObject(error.what());
                continue;
            } catch (const std::exception& error) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Unexpected object preparation failure: " +
                        std::string(error.what())
                );
            }

            aliases.swap(candidateAliases);
            for (auto& [operationIndex, prepared] : candidateOperations) {
                result.operations[operationIndex].emplace(
                    std::move(prepared)
                );
            }
            if (candidateParticleBatch) {
                const auto [inserted, didInsert] =
                    result.particleBatches.emplace(
                        candidateParticleBatch->first,
                        std::move(candidateParticleBatch->second)
                    );
                static_cast<void>(inserted);
                if (!didInsert) {
                    throw Error(
                        ErrorCode::internalFailure,
                        "Prepared particle batch operation index is duplicated"
                    );
                }
            }
            if (candidateParticleState) {
                result.particleStates.insert_or_assign(
                    group.objectIndex,
                    std::move(*candidateParticleState)
                );
            }
        }
        result.finalAliases = std::move(aliases);
        return result;
    }

    void executePreparedOperations(
        Device::Session& session,
        const PreparedFrame& prepared
    ) {
        const auto& particleBatches = prepared.frozenParticleBatches != nullptr
            ? *prepared.frozenParticleBatches
            : prepared.particleBatches;
        for (const auto& operation : prepared.operations) {
            if (!operation) continue;
            if (const auto* drawOperation =
                    std::get_if<PreparedDraw>(&*operation)) {
                draw(session, *drawOperation);
            } else if (const auto* swapOperation =
                           std::get_if<FrameSwapCommand>(&*operation)) {
                swap(*swapOperation);
            } else if (const auto* clearOperation =
                           std::get_if<PreparedClear>(&*operation)) {
                clear(session, *clearOperation);
            } else if (const auto* textOperation =
                           std::get_if<PreparedText>(&*operation)) {
                drawText(session, *textOperation);
            } else if (const auto* particleOperation =
                           std::get_if<PreparedParticle>(&*operation)) {
                const auto batch = particleBatches.find(
                    particleOperation->operationIndex
                );
                if (batch == particleBatches.end()) {
                    throw Error(
                        ErrorCode::internalFailure,
                        "Prepared particle operation has no simulation batch"
                    );
                }
                drawParticle(session, *particleOperation, batch->second);
            } else {
                throw Error(
                    ErrorCode::internalFailure,
                    "Prepared frame contains an unknown operation variant"
                );
            }
        }
        if (framebufferAliases != prepared.finalAliases) {
            throw Error(
                ErrorCode::internalFailure,
                "Prepared framebuffer alias simulation diverged during execution"
            );
        }
    }

    void clearCurrentOutput() {
        if (!device || outputId.empty()) return;
        auto session = device->activate();
        if (framebufferAliases.contains(outputId)) {
            normalizeGLState();
            auto& output = framebuffer({
                .kind = FrameResourceKind::framebuffer,
                .id = outputId,
            });
            glBindFramebuffer(GL_FRAMEBUFFER, output.framebuffer);
            glViewport(0, 0, output.width, output.height);
            glClearColor(0, 0, 0, 0);
            glClearDepth(1.0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            session.checkError(
                ErrorCode::draw,
                "clearing the previous frame after failure"
            );
        }
    }

    [[nodiscard]] FrameVector2 nextParallax(
        const FrameVector2& previous,
        const FramePlan& plan,
        const ResolvedFrameInputs& inputs
    ) const {
        // Target and delayed interpolation formulas are adapted from
        // Almamu/linux-wallpaperengine Render/Wallpapers/CScene.cpp at
        // b016d7d1fdcf4e5fd2f9c9fa420a8aaa07fee02d (GPL-3.0).
        if (!plan.parallax.enabled) {
            return {};
        }
        const FrameVector2 target = {
            (inputs.pointerPosition.x - 0.5) * plan.parallax.amount *
                plan.parallax.mouseInfluence,
            (inputs.pointerPosition.y - 0.5) * plan.parallax.amount *
                plan.parallax.mouseInfluence,
        };
        if (!std::isfinite(target.x) || !std::isfinite(target.y)) {
            throw Error(
                ErrorCode::resourceValidation,
                "Camera parallax target is non-finite"
            );
        }
        const double interpolation = std::clamp(
            plan.parallax.delay * inputs.frameTimeSeconds, 0.0, 1.0
        );
        FrameVector2 result = previous;
        result.x += (target.x - result.x) * interpolation;
        result.y += (target.y - result.y) * interpolation;
        if (!std::isfinite(result.x) || !std::isfinite(result.y)) {
            throw Error(
                ErrorCode::resourceValidation,
                "Camera parallax displacement is non-finite"
            );
        }
        return result;
    }

    [[nodiscard]] ResolvedFrameInputs resolveInputs(
        const FrameInputs& inputs,
        std::optional<FrameProjectionSize> drawableFallback,
        std::optional<PresentationScaling> scaling
    ) const {
        ResolvedFrameInputs result{
            .timeSeconds = inputs.timeSeconds,
            .frameTimeSeconds = inputs.frameTimeSeconds,
            .daytime = localDaytime(),
        };
        const FrameVector2 drawablePointer = inputs.pointerPosition;
        if (!drawableFallback) {
            result.pointerPosition = {
                std::clamp(drawablePointer.x, 0.0, 1.0),
                std::clamp(drawablePointer.y, 0.0, 1.0),
            };
        } else {
            if (!scaling) {
                throw Error(
                    ErrorCode::invalidArgument,
                    "Drawable pointer mapping requires a presentation scaling mode"
                );
            }
            const std::uint32_t sourceWidth =
                frameGraph->requiresDrawableProjectionFallback()
                    ? drawableFallback->width
                    : width;
            const std::uint32_t sourceHeight =
                frameGraph->requiresDrawableProjectionFallback()
                    ? drawableFallback->height
                    : height;
            if (sourceWidth == 0 || sourceHeight == 0) {
                throw Error(
                    ErrorCode::invalidArgument,
                    "Scene projection dimensions are unavailable for pointer mapping"
                );
            }
            const auto transform = presentationTransform(
                static_cast<GLsizei>(sourceWidth),
                static_cast<GLsizei>(sourceHeight),
                static_cast<GLsizei>(drawableFallback->width),
                static_cast<GLsizei>(drawableFallback->height),
                *scaling
            );
            // Host, script, shader, and parallax inputs share one canonical
            // bottom-left normalized scene coordinate. Stock effect shaders apply
            // their own texture-space adaptation where required; flipping here as
            // well reverses cursor-driven effects such as xray. Resolve the visible
            // crop once and share that result with every consumer.
            result.pointerPosition = transform.map(drawablePointer);
        }
        result.pointerPositionLast = hasPublishedPointer
            ? lastPublishedPointer
            : FrameVector2{};
        return result;
    }

    void invalidateFrame() noexcept {
        lastFrame.reset();
        if (device) {
            try {
                auto session = device->activate();
                textRenderer.trimCache(session);
            } catch (const std::exception& cleanupError) {
                std::fprintf(
                    stderr,
                    "FramePlanExecutor failed to trim the text cache after frame failure: %s\n",
                    cleanupError.what()
                );
            } catch (...) {
                std::fprintf(
                    stderr,
                    "FramePlanExecutor failed to trim the text cache after frame failure with an unknown error\n"
                );
            }
        }
        try {
            clearCurrentOutput();
        } catch (const std::exception& cleanupError) {
            std::fprintf(
                stderr,
                "FramePlanExecutor failed to clear the previous output: %s\n",
                cleanupError.what()
            );
        } catch (...) {
            std::fprintf(
                stderr,
                "FramePlanExecutor failed to clear the previous output with an unknown error\n"
            );
        }
    }

    [[noreturn]] void failFrame(std::exception_ptr original) {
        invalidateFrame();
        std::rethrow_exception(original);
    }

    void render(
        const FrameInputs& inputs,
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt,
        std::optional<PresentationScaling> scaling = std::nullopt
    ) {
        try {
            if (!std::isfinite(inputs.pointerPosition.x) ||
                !std::isfinite(inputs.pointerPosition.y) ||
                !std::isfinite(inputs.timeSeconds) || inputs.timeSeconds < 0.0 ||
                !std::isfinite(inputs.frameTimeSeconds) || inputs.frameTimeSeconds < 0.0) {
                throw Error(
                    ErrorCode::invalidArgument,
                    "Frame inputs must be finite and time values must be non-negative"
                );
            }
            const ResolvedFrameInputs resolvedInputs = resolveInputs(
                inputs, drawableFallback, scaling
            );
            EvaluatedFramePlan evaluated = frameGraph->evaluate(
                {
                    .runtimeSeconds = resolvedInputs.timeSeconds,
                    .frameTimeSeconds = resolvedInputs.frameTimeSeconds,
                    .pointerX = resolvedInputs.pointerPosition.x,
                    .pointerY = resolvedInputs.pointerPosition.y,
                },
                drawableFallback
            );
            const FramePlan& plan = evaluated.plan;
            const std::vector<ObjectOperationGroup> groups =
                objectOperationGroups(plan);
            const PreparedCamera camera = prepareCamera(plan);
            const FrameVector2 workingParallax = nextParallax(
                parallaxDisplacement, plan, resolvedInputs
            );
            auto session = ensureDevice().activate();
            prepareFrameArena(session, plan);
            PreparedFrame prepared = preflightFrameObjects(
                session,
                plan,
                groups,
                resolvedInputs,
                workingParallax,
                camera
            );
            beginFrameOutput(session, plan);
            executePreparedOperations(session, prepared);
            textRenderer.trimCache(session);

            LastFrameState published{
                .sourcePlan = std::move(evaluated.plan),
                .evaluation = std::move(evaluated.evaluation),
                .inputs = resolvedInputs,
                .particleBatches = std::move(prepared.particleBatches),
                .issues = std::move(prepared.issues),
            };
            lastFrame.emplace(std::move(published));
            particles.swap(prepared.particleStates);
            parallaxDisplacement = workingParallax;
            lastPublishedPointer = resolvedInputs.pointerPosition;
            hasPublishedPointer = true;
        } catch (...) {
            failFrame(std::current_exception());
        }
    }

    void replay(std::uint32_t drawableWidth, std::uint32_t drawableHeight) {
        try {
            if (!lastFrame) {
                throw Error(
                    ErrorCode::invalidArgument,
                    "No successful evaluated scene frame is available to replay"
                );
            }
            if (!frameGraph->requiresDrawableProjectionFallback() ||
                (width == drawableWidth && height == drawableHeight)) {
                return;
            }
            const FramePlan replayPlan = frameGraph->reproject(
                lastFrame->evaluation,
                FrameProjectionSize{
                    .width = drawableWidth,
                    .height = drawableHeight,
                }
            );
            const std::vector<ObjectOperationGroup> groups =
                objectOperationGroups(replayPlan);
            const PreparedCamera camera = prepareCamera(replayPlan);
            auto session = ensureDevice().activate();
            prepareFrameArena(session, replayPlan);
            PreparedFrame prepared = preflightFrameObjects(
                session,
                replayPlan,
                groups,
                lastFrame->inputs,
                parallaxDisplacement,
                camera,
                &lastFrame->particleBatches,
                &lastFrame->issues
            );
            beginFrameOutput(session, replayPlan);
            executePreparedOperations(session, prepared);
            textRenderer.trimCache(session);
            lastFrame->issues = std::move(prepared.issues);
        } catch (...) {
            failFrame(std::current_exception());
        }
    }

    void present(
        std::uint32_t drawableWidth,
        std::uint32_t drawableHeight,
        PresentationScaling scaling
    ) {
        if (borrowedContext == nullptr) {
            throw Error(
                ErrorCode::invalidArgument,
                "Presenting requires an executor created with a borrowed CGL context"
            );
        }
        if (drawableWidth == 0 || drawableHeight == 0) {
            throw Error(ErrorCode::invalidArgument, "Drawable dimensions must be greater than zero");
        }
        if (outputId.empty() || !lastFrame.has_value()) {
            throw Error(ErrorCode::resourceValidation, "No successful scene frame is available to present");
        }
        if (drawableWidth > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max()) ||
            drawableHeight > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())) {
            throw Error(ErrorCode::invalidArgument, "Drawable dimensions exceed OpenGL's signed range");
        }

        auto session = ensureDevice().activate();
        auto& output = framebuffer({.kind = FrameResourceKind::framebuffer, .id = outputId});
        const auto transform = presentationTransform(
            output.width,
            output.height,
            static_cast<GLsizei>(drawableWidth),
            static_cast<GLsizei>(drawableHeight),
            scaling
        );

        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_DEPTH_CLAMP);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_SAMPLE_COVERAGE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DITHER);
        glDisable(GL_MULTISAMPLE);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        glViewport(0, 0, static_cast<GLsizei>(drawableWidth), static_cast<GLsizei>(drawableHeight));
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, output.framebuffer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(
            transform.source.x,
            transform.source.y,
            transform.source.x + transform.source.width,
            transform.source.y + transform.source.height,
            transform.destination.x,
            transform.destination.y,
            transform.destination.x + transform.destination.width,
            transform.destination.y + transform.destination.height,
            GL_COLOR_BUFFER_BIT, GL_LINEAR
        );
        session.checkError(ErrorCode::draw, "presenting the scene frame to the drawable");
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }

    std::shared_ptr<SceneFrameGraph> frameGraph;
    CGLContextObj borrowedContext = nullptr;
    std::unique_ptr<Device> device;
    std::map<std::string, CachedFramebuffer> framebuffers;
    std::map<std::string, std::string> framebufferAliases;
    std::map<std::string, AssetTextureResource> assets;
    std::map<std::string, ProgramResource> programs;
    TextCoverageRenderer textRenderer;
    GLuint vertexArray = 0;
    GLuint vertexBuffer = 0;
    GLuint particleVertexArray = 0;
    GLuint particleVertexBuffer = 0;
    GLuint particleElementBuffer = 0;
    std::map<std::size_t, ParticleState> particles;
    std::string outputId;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t byteCount = 0;
    std::optional<LastFrameState> lastFrame;
    FrameVector2 parallaxDisplacement;
    FrameVector2 lastPublishedPointer;
    bool hasPublishedPointer = false;
};

FramePlanExecutor::FramePlanExecutor(std::shared_ptr<SceneFrameGraph> frameGraph)
    : impl_(std::make_unique<Impl>(std::move(frameGraph))) {}

FramePlanExecutor::FramePlanExecutor(
    std::shared_ptr<SceneFrameGraph> frameGraph,
    CGLContextObj borrowedContext
) : impl_(std::make_unique<Impl>(std::move(frameGraph), borrowedContext)) {}

FramePlanExecutor::~FramePlanExecutor() = default;
void FramePlanExecutor::render(const FrameInputs& inputs) { impl_->render(inputs); }
void FramePlanExecutor::render(
    const FrameInputs& inputs,
    std::uint32_t drawableWidth,
    std::uint32_t drawableHeight,
    PresentationScaling scaling
) {
    if (drawableWidth == 0 || drawableHeight == 0 ||
        drawableWidth > static_cast<std::uint32_t>(
            std::numeric_limits<GLsizei>::max()
        ) ||
        drawableHeight > static_cast<std::uint32_t>(
            std::numeric_limits<GLsizei>::max()
        )) {
        impl_->invalidateFrame();
        throw Error(
            ErrorCode::invalidArgument,
            "Drawable dimensions must be non-zero and fit OpenGL's signed range"
        );
    }
    impl_->render(inputs, FrameProjectionSize{
        .width = drawableWidth,
        .height = drawableHeight,
    }, scaling);
}
void FramePlanExecutor::replay(
    std::uint32_t drawableWidth,
    std::uint32_t drawableHeight
) {
    if (drawableWidth == 0 || drawableHeight == 0 ||
        drawableWidth > static_cast<std::uint32_t>(
            std::numeric_limits<GLsizei>::max()
        ) ||
        drawableHeight > static_cast<std::uint32_t>(
            std::numeric_limits<GLsizei>::max()
        )) {
        impl_->invalidateFrame();
        throw Error(
            ErrorCode::invalidArgument,
            "Drawable dimensions must be non-zero and fit OpenGL's signed range"
        );
    }
    impl_->replay(drawableWidth, drawableHeight);
}
void FramePlanExecutor::present(
    std::uint32_t drawableWidth,
    std::uint32_t drawableHeight,
    PresentationScaling scaling
) {
    impl_->present(drawableWidth, drawableHeight, scaling);
}
void FramePlanExecutor::invalidateFrame() noexcept { impl_->invalidateFrame(); }
std::uint32_t FramePlanExecutor::width() const noexcept { return impl_->width; }
std::uint32_t FramePlanExecutor::height() const noexcept { return impl_->height; }
std::size_t FramePlanExecutor::rgba8ByteCount() const noexcept {
    return impl_->byteCount;
}
std::optional<std::uint64_t> FramePlanExecutor::lastModelRevision() const noexcept {
    return impl_->lastFrame
        ? std::optional<std::uint64_t>(impl_->lastFrame->sourcePlan.modelRevision)
        : std::nullopt;
}
const std::vector<FrameSoundDescriptor>* FramePlanExecutor::lastSounds() const noexcept {
    return impl_->lastFrame ? &impl_->lastFrame->sourcePlan.sounds : nullptr;
}
const std::vector<FrameExecutionIssue>* FramePlanExecutor::lastIssues() const noexcept {
    return impl_->lastFrame ? &impl_->lastFrame->issues : nullptr;
}
void FramePlanExecutor::readRGBA8(std::span<std::uint8_t> output) {
    if (impl_->outputId.empty()) {
        throw Error(ErrorCode::readback, "No frame has been rendered");
    }
    auto session = impl_->ensureDevice().activate();
    session.readRGBA8(impl_->framebuffer({
        .kind = FrameResourceKind::framebuffer,
        .id = impl_->outputId,
    }), output);
}

}  // namespace we::scene::gl
