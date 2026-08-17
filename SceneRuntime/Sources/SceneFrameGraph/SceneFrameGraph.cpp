#include <SceneFrameGraph/SceneFrameGraph.hpp>

#include <SceneCore/AssetResolver.hpp>
#include <SceneCore/FormatError.hpp>
#include <SceneCore/Package.hpp>
#include <SceneCore/PuppetMesh.hpp>
#include <SceneCore/Runtime.hpp>
#include <SceneCore/Texture.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace we::scene {

FrameResourceRef frameAssetTextureResource(std::string_view name) {
    std::string path;
    if (name.starts_with("materials/")) {
        path = name;
    } else {
        path = "materials/";
        path += name;
    }
    if (!name.ends_with(".tex")) {
        path += ".tex";
    }
    return {
        .kind = FrameResourceKind::assetTexture,
        .id = path,
        .logicalName = path,
    };
}

namespace {

using FrameGraphTraceClock = std::chrono::steady_clock;

[[nodiscard]] bool frameGraphTraceEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("WE_SCENE_FRAME_TRACE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

void frameGraphTraceLog(const char* format, ...) noexcept {
    if (!frameGraphTraceEnabled()) return;
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "[SceneFrameTrace] %s\n", message);
}

[[nodiscard]] double frameGraphTraceMilliseconds(
    FrameGraphTraceClock::time_point start,
    FrameGraphTraceClock::time_point end = FrameGraphTraceClock::now()
) noexcept {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

FrameResourceRef frameHostTextureResource(std::string_view name) {
    return {
        .kind = FrameResourceKind::hostTexture,
        .id = std::string(name),
        .logicalName = std::string(name),
    };
}

using FramebufferMap = std::map<std::string, FrameResourceRef>;
constexpr int wallpaperTextureSlotCount = 8;
constexpr int linuxBloomLegacyObjectId = -1;
constexpr int bloomRuntimeObjectId = std::numeric_limits<int>::min();

using FrameMatrix = std::array<double, 16>;

FrameMatrix frameIdentityMatrix() {
    return {1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0};
}

FrameMatrix frameMatrixMultiply(
    const FrameMatrix& lhs,
    const FrameMatrix& rhs
) {
    FrameMatrix result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t index = 0; index < 4; ++index) {
                result[column * 4 + row] +=
                    lhs[index * 4 + row] * rhs[column * 4 + index];
            }
        }
    }
    return result;
}

FrameMatrix frameTranslation(double x, double y, double z) {
    FrameMatrix result = frameIdentityMatrix();
    result[12] = x;
    result[13] = y;
    result[14] = z;
    return result;
}

FrameMatrix frameRotationX(double radians) {
    FrameMatrix result = frameIdentityMatrix();
    result[5] = std::cos(radians);
    result[6] = std::sin(radians);
    result[9] = -std::sin(radians);
    result[10] = std::cos(radians);
    return result;
}

FrameMatrix frameRotationY(double radians) {
    FrameMatrix result = frameIdentityMatrix();
    result[0] = std::cos(radians);
    result[2] = -std::sin(radians);
    result[8] = std::sin(radians);
    result[10] = std::cos(radians);
    return result;
}

FrameMatrix frameRotationZ(double radians) {
    FrameMatrix result = frameIdentityMatrix();
    result[0] = std::cos(radians);
    result[1] = std::sin(radians);
    result[4] = -std::sin(radians);
    result[5] = std::cos(radians);
    return result;
}

FrameMatrix frameOrthographic(
    double left,
    double right,
    double bottom,
    double top,
    double nearPlane,
    double farPlane
) {
    if (!std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(bottom) || !std::isfinite(top) ||
        !std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
        left == right || bottom == top || nearPlane == farPlane) {
        throw std::invalid_argument(
            "Scene camera has a degenerate orthographic projection"
        );
    }
    FrameMatrix result{};
    result[0] = 2.0 / (right - left);
    result[5] = 2.0 / (top - bottom);
    result[10] = -1.0 / (farPlane - nearPlane);
    result[12] = -(right + left) / (right - left);
    result[13] = -(top + bottom) / (top - bottom);
    result[14] = -nearPlane / (farPlane - nearPlane);
    result[15] = 1.0;
    return result;
}

FrameMatrix framePerspective(
    double fieldOfViewRadians,
    double aspect,
    double nearPlane,
    double farPlane
) {
    if (!std::isfinite(fieldOfViewRadians) ||
        !std::isfinite(aspect) || !std::isfinite(nearPlane) ||
        !std::isfinite(farPlane) || fieldOfViewRadians <= 0.0 ||
        fieldOfViewRadians >= 3.14159265358979323846264338327950288 ||
        aspect <= 0.0 || nearPlane == 0.0 || farPlane == nearPlane) {
        throw std::invalid_argument(
            "Scene perspective projection is degenerate"
        );
    }
    const double tangent = std::tan(fieldOfViewRadians * 0.5);
    if (!std::isfinite(tangent) || tangent <= 0.0) {
        throw std::invalid_argument(
            "Scene perspective field of view is invalid"
        );
    }
    FrameMatrix result{};
    result[0] = 1.0 / (aspect * tangent);
    result[5] = 1.0 / tangent;
    result[10] = -farPlane / (farPlane - nearPlane);
    result[11] = -1.0;
    result[14] = -(nearPlane * farPlane) / (farPlane - nearPlane);
    return result;
}

Vector3 frameSubtract(Vector3 lhs, Vector3 rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

double frameDot(Vector3 lhs, Vector3 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vector3 frameCross(Vector3 lhs, Vector3 rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

Vector3 frameNormalized(Vector3 value) {
    const double length = std::sqrt(frameDot(value, value));
    if (!std::isfinite(length) ||
        length <= std::numeric_limits<double>::epsilon()) {
        throw std::invalid_argument(
            "Scene camera contains a zero-length direction"
        );
    }
    return {value.x / length, value.y / length, value.z / length};
}

FrameMatrix frameLightVolumeTransform(const FrameLightDescriptor& light) {
    return frameMatrixMultiply(
        frameTranslation(
            light.worldTransform.origin.x,
            light.worldTransform.origin.y,
            light.worldTransform.origin.z
        ),
        frameMatrixMultiply(
            frameRotationZ(light.worldTransform.angles.z),
            frameMatrixMultiply(
                frameRotationY(light.worldTransform.angles.y),
                frameRotationX(light.worldTransform.angles.x)
            )
        )
    );
}

Vector3 frameLightForward(const FrameLightDescriptor& light) {
    const FrameMatrix transform = frameLightVolumeTransform(light);
    return {transform[0], transform[1], transform[2]};
}

bool frameCameraInsideLightVolume(
    const FrameCameraDescriptor& camera,
    const FrameLightDescriptor& light
) {
    const Vector3 delta = frameSubtract(
        camera.eye,
        Vector3{
            light.worldTransform.origin.x,
            light.worldTransform.origin.y,
            light.worldTransform.origin.z,
        }
    );
    const double radius = light.radius;
    const double distanceSquared = frameDot(delta, delta);
    if (!std::isfinite(radius) || radius <= 0.0 ||
        !std::isfinite(distanceSquared) ||
        distanceSquared > radius * radius) {
        return false;
    }
    if (light.type == FrameLightType::point || distanceSquared <= 1e-18) {
        return true;
    }
    const Vector3 direction = frameNormalized(delta);
    const Vector3 forward = frameNormalized(frameLightForward(light));
    const double cosine = frameDot(direction, forward);
    const double outerRadians = light.outerCone *
        3.14159265358979323846264338327950288 / 180.0;
    return std::isfinite(cosine) && std::isfinite(outerRadians) &&
        cosine >= std::cos(outerRadians);
}

FrameMatrix frameLookAt(const FrameCameraDescriptor& camera) {
    const Vector3 forward = frameNormalized(
        frameSubtract(camera.center, camera.eye)
    );
    const Vector3 side = frameNormalized(frameCross(forward, camera.up));
    const Vector3 up = frameCross(side, forward);
    FrameMatrix result = frameIdentityMatrix();
    result[0] = side.x;
    result[4] = side.y;
    result[8] = side.z;
    result[1] = up.x;
    result[5] = up.y;
    result[9] = up.z;
    result[2] = -forward.x;
    result[6] = -forward.y;
    result[10] = -forward.z;
    result[12] = -frameDot(side, camera.eye);
    result[13] = -frameDot(up, camera.eye);
    result[14] = frameDot(forward, camera.eye);
    return result;
}

FrameMatrix frameMatrixInverse(const FrameMatrix& matrix) {
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
            throw std::invalid_argument(
                "Scene camera view-projection matrix is singular"
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

    FrameMatrix result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const double value = augmented[row][column + 4];
            if (!std::isfinite(value)) {
                throw std::invalid_argument(
                    "Scene camera inverse view-projection matrix is non-finite"
                );
            }
            result[column * 4 + row] = value;
        }
    }
    return result;
}

FrameMatrix frameSceneViewProjection(
    const FrameCameraDescriptor& camera,
    double width,
    double height
) {
    if (!camera.orthographic) {
        const double fieldOfView = camera.perspectiveOverrideFieldOfView > 0.0
            ? camera.perspectiveOverrideFieldOfView
            : camera.fieldOfView;
        if (!std::isfinite(fieldOfView) || fieldOfView <= 0.0 ||
            fieldOfView >= 180.0 || !std::isfinite(camera.nearPlane) ||
            !std::isfinite(camera.farPlane) || camera.nearPlane <= 0.0 ||
            camera.farPlane <= camera.nearPlane) {
            throw std::invalid_argument(
                "Scene camera has an invalid perspective projection"
            );
        }
        constexpr double degreesToRadians =
            3.14159265358979323846264338327950288 / 180.0;
        return frameMatrixMultiply(
            framePerspective(
                fieldOfView * degreesToRadians,
                width / height,
                camera.nearPlane,
                camera.farPlane
            ),
            frameLookAt(camera)
        );
    }
    const FrameMatrix projection = frameOrthographic(
        -width * 0.5,
        width * 0.5,
        -height * 0.5,
        height * 0.5,
        camera.nearPlane,
        camera.farPlane
    );
    return frameMatrixMultiply(
        frameMatrixMultiply(
            projection,
            frameTranslation(camera.eye.x, camera.eye.y, camera.eye.z)
        ),
        frameLookAt(camera)
    );
}

FramePlanIssueSeverity defaultIssueSeverity(FramePlanIssueCode code) noexcept {
    switch (code) {
        case FramePlanIssueCode::soundRuntimeUnavailable:
        case FramePlanIssueCode::scriptRuntimeUnavailable:
        case FramePlanIssueCode::composeUnavailable:
        case FramePlanIssueCode::textRenderingUnavailable:
        case FramePlanIssueCode::perspectiveProjectionUnavailable:
        case FramePlanIssueCode::framebufferReadBeforeWrite:
            return FramePlanIssueSeverity::warning;
        case FramePlanIssueCode::effectPassPlanningFailed:
        case FramePlanIssueCode::framebufferDescriptorMissing:
        case FramePlanIssueCode::framebufferFeedbackLoop:
            return FramePlanIssueSeverity::skipPass;
        case FramePlanIssueCode::passthroughUnavailable:
        case FramePlanIssueCode::puppetUnavailable:
        case FramePlanIssueCode::imageMaterialUnavailable:
        case FramePlanIssueCode::objectPlanningFailed:
            return FramePlanIssueSeverity::skipObject;
        case FramePlanIssueCode::audioInputUnavailable:
            return FramePlanIssueSeverity::frameFatal;
    }
    return FramePlanIssueSeverity::frameFatal;
}

std::string objectPointer(std::size_t index, std::string_view field = {}) {
    std::string result = "/objects/" + std::to_string(index);
    if (!field.empty()) {
        result += '/';
        result += field;
    }
    return result;
}

[[noreturn]] void frameError(
    const SceneModel& model,
    SceneModelErrorCode code,
    std::string pointer,
    std::string message
) {
    throw SceneModelError(
        code,
        model.project().scene.assetPath,
        std::move(pointer),
        {model.project().scene.assetPath},
        std::move(message)
    );
}

bool isFramebufferName(std::string_view name) {
    return name.starts_with("_rt_") || name.starts_with("_alias_");
}

bool isPassRelativeResourceName(std::string_view name) {
    return name == "previous" || name == "original";
}

FrameResourceRef imageCompositeResource(int objectId, char suffix) {
    const std::string logicalName = "_rt_imageLayerComposite_" +
        std::to_string(objectId) + '_' + suffix;
    return {
        .kind = FrameResourceKind::framebuffer,
        .id = "object:" + std::to_string(objectId) + ':' + logicalName,
        .logicalName = logicalName,
    };
}

FramebufferFormat framebufferFormat(
    const SceneModel& model,
    std::string_view value,
    std::string pointer
) {
    if (value == "rgba8888" || value == "rgba_backbuffer") {
        return FramebufferFormat::rgba8;
    }
    if (value == "r8") {
        return FramebufferFormat::r8;
    }
    if (value == "rg1616f") {
        return FramebufferFormat::rg16f;
    }
    if (value == "r16f") {
        return FramebufferFormat::r16f;
    }
    frameError(
        model,
        SceneModelErrorCode::invalidValue,
        std::move(pointer),
        "Unsupported framebuffer format '" + std::string(value) + "'"
    );
}

FramebufferWrapMode framebufferWrapMode(
    const SceneModel& model,
    const std::optional<std::string>& value,
    std::string pointer
) {
    if (!value || *value == "clamp") {
        return FramebufferWrapMode::clampToEdge;
    }
    if (*value == "border") {
        return FramebufferWrapMode::clampToBorder;
    }
    if (*value == "repeat") {
        return FramebufferWrapMode::repeat;
    }
    frameError(
        model,
        SceneModelErrorCode::invalidValue,
        std::move(pointer),
        "Unsupported framebuffer UV wrap mode '" + *value +
            "'; expected 'clamp', 'border', or 'repeat'"
    );
}

std::uint32_t checkedDimension(
    const SceneModel& model,
    double value,
    std::string pointer,
    std::string_view description
) {
    if (!std::isfinite(value) || value < 1.0 ||
        value > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        frameError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            std::string(description) + " must be a finite positive 32-bit value"
        );
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t checkedScaledFramebufferDimension(
    const SceneModel& model,
    double value,
    std::string pointer,
    std::string_view description
) {
    if (!std::isfinite(value) || value <= 0.0 ||
        value > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        frameError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            std::string(description) +
                " must resolve to a finite positive 32-bit value"
        );
    }
    // Wallpaper Engine effects routinely downsample small layers. Integer
    // division can reach zero, but GPU framebuffers cannot; retain the
    // authored scale while allocating the smallest valid backing texture.
    return std::max<std::uint32_t>(
        1,
        static_cast<std::uint32_t>(std::floor(value))
    );
}

double numberValue(
    const SceneModel& model,
    const EvaluatedValue& evaluated,
    std::string pointer,
    std::string_view description
);

FrameVector2 vector2Value(
    const SceneModel& model,
    const EvaluatedValue& evaluated,
    std::string pointer,
    std::string_view description
) {
    const auto& projected = evaluated.value.vector();
    const FrameVector2 result{projected[0], projected[1]};
    if (!std::isfinite(result.x) || !std::isfinite(result.y)) {
        frameError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            std::string(description) + " must project to two finite numbers"
        );
    }
    return result;
}

Vector3 vector3Value(
    const SceneModel& model,
    const EvaluatedValue& evaluated,
    std::string pointer,
    std::string_view description
) {
    const auto& projected = evaluated.value.vector();
    const Vector3 result{
        projected[0], projected[1], projected[2],
    };
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z)) {
        frameError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            std::string(description) +
                " must project to three finite numbers"
        );
    }
    return result;
}

Vector3 cameraVector3Value(
    const SceneModel& model,
    EvaluatedValue evaluated,
    std::string pointer,
    std::string_view description
) {
    if (evaluated.value.type() == RuntimeValueType::string) {
        try {
            evaluated.value = RuntimeValue::initialString(
                evaluated.value.string()
            );
        } catch (const std::exception& error) {
            frameError(
                model,
                SceneModelErrorCode::invalidValue,
                std::move(pointer),
                std::string(description) + " is invalid: " + error.what()
            );
        }
    }
    return vector3Value(
        model, evaluated, std::move(pointer), description
    );
}

double numberValue(
    const SceneModel& model,
    const EvaluatedValue& evaluated,
    std::string pointer,
    std::string_view description
) {
    const double result = evaluated.value.number();
    if (!std::isfinite(result)) {
        frameError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            std::string(description) + " must be finite"
        );
    }
    return result;
}

FrameColor colorValue(
    const SceneModel& model,
    const EvaluatedValue& evaluated,
    std::string pointer
) {
    const auto& projected = evaluated.value.vector();
    const FrameColor result{
        .red = projected[0],
        .green = projected[1],
        .blue = projected[2],
        .alpha = projected[3],
    };
    if (!std::isfinite(result.red) ||
        !std::isfinite(result.green) || !std::isfinite(result.blue) ||
        !std::isfinite(result.alpha)) {
        frameError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            "Scene color must project to finite components"
        );
    }
    return result;
}

bool booleanValue(
    const SceneModel& model,
    const EvaluatedValue& evaluated,
    std::string pointer,
    std::string_view description
) {
    (void)model;
    (void)pointer;
    (void)description;
    return evaluated.value.boolean();
}

std::map<int, std::string> namedSlots(const TextureSlots& slots) {
    std::map<int, std::string> result;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].name && !slots[index].name->empty()) {
            result.emplace(static_cast<int>(index), *slots[index].name);
        }
    }
    return result;
}

void mergeSlots(TextureSlots& target, const TextureSlots& additional) {
    if (target.size() < additional.size()) {
        target.resize(additional.size());
    }
    for (std::size_t index = 0; index < additional.size(); ++index) {
        // The Linux implementation parses sparse arrays into a map and uses
        // insert for image instance values: an instance fills an absent slot,
        // but never replaces a material-owned slot.
        if (!target[index].name && additional[index].name) {
            target[index] = additional[index];
        }
    }
}

MaterialPass effectiveBasePass(
    const MaterialPass& material,
    const ImageObject& image,
    bool firstPass
) {
    MaterialPass result = material;
    if (firstPass) {
        mergeSlots(result.textures, image.instanceTextures);
        mergeSlots(result.userTextures, image.instanceUserTextures);
    }
    return result;
}

struct ShaderAssetPaths {
    std::string vertex;
    std::string fragment;
};

std::string normalizedShaderPath(
    const SceneModel& model,
    std::string_view root,
    std::string_view shader,
    std::string_view extension,
    std::string pointer
) {
    try {
        return normalizeAssetPath(
            std::string(root) + std::string(shader) + std::string(extension)
        );
    } catch (const FormatError& error) {
        frameError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            "Shader path cannot be resolved from its asset context: " +
                std::string(error.what())
        );
    }
}

ShaderAssetPaths shaderPaths(
    const SceneModel& model,
    std::string root,
    std::string_view shader,
    std::string pointer
) {
    return {
        .vertex = normalizedShaderPath(
            model, root, shader, ".vert", pointer + "/vertex"
        ),
        .fragment = normalizedShaderPath(
            model, root, shader, ".frag", pointer + "/fragment"
        ),
    };
}

ShaderAssetPaths materialShaderPaths(
    const SceneModel& model,
    const Material& material,
    std::string_view shader,
    std::string pointer
) {
    constexpr std::string_view rootComponent = "materials/";
    std::string root;
    if (material.assetPath.starts_with(rootComponent)) {
        root = "shaders/";
    } else {
        const std::size_t position = material.assetPath.find("/materials/");
        if (position == std::string::npos) {
            frameError(
                model,
                SceneModelErrorCode::invalidValue,
                std::move(pointer),
                "Material asset path '" + material.assetPath +
                    "' does not identify a shader namespace"
            );
        }
        root = material.assetPath.substr(0, position + 1) + "shaders/";
    }
    return shaderPaths(model, std::move(root), shader, std::move(pointer));
}

struct PendingRender {
    FramePassOrigin origin;
    MaterialPass material;
    std::string vertexShaderPath;
    std::string fragmentShaderPath;
    std::string materialObjectId;
    std::string constantPointerPrefix;
    const EffectPassOverride* overridePass = nullptr;
    std::map<int, std::string> binds;
    FramebufferMap localFramebuffers;
    std::optional<FrameResourceRef> target;
};

struct PendingCopy {
    FramePassOrigin origin;
    FrameResourceRef source;
    FrameResourceRef target;
};

struct PendingSwap {
    FramePassOrigin origin;
    FrameResourceRef source;
    FrameResourceRef target;
};

using PendingOperation = std::variant<PendingRender, PendingCopy, PendingSwap>;

struct ImageContext {
    std::size_t planImageIndex = 0;
    std::size_t objectIndex = 0;
    const ImageObject* image = nullptr;
    const SceneGraphNodeSnapshot* node = nullptr;
    FrameResourceRef currentMain;
    FrameResourceRef currentSub;
    FramebufferMap localFramebuffers;
};

struct PlanCheckpoint {
    std::size_t framebufferCount = 0;
    std::size_t imageCount = 0;
    std::size_t textCount = 0;
    std::size_t textEffectCount = 0;
    std::size_t particleCount = 0;
    std::size_t soundCount = 0;
    std::size_t operationCount = 0;
};

class PlanBuilder final {
public:
    PlanBuilder(
        std::shared_ptr<const SceneModel> model,
        const SceneGraphSnapshot& graphSnapshot,
        FrameProjectionSize projectionSize,
        const SceneFrameInputs& inputs,
        FrameRenderQuality renderQuality,
        SceneGraph::EvaluationFrame* evaluationFrame = nullptr,
        const std::map<std::string, EvaluatedValue>* scriptedValues = nullptr,
        const std::vector<int>* cursorInteractiveLayerIds = nullptr
    )
        : model_(std::move(model)), graphSnapshot_(graphSnapshot),
          projectionSize_(projectionSize), inputs_(inputs),
          renderQuality_(renderQuality),
          evaluationFrame_(evaluationFrame), scriptedValues_(scriptedValues),
          cursorInteractiveLayerIds_(cursorInteractiveLayerIds) {
        if (!model_) {
            throw SceneModelError(
                SceneModelErrorCode::invalidValue,
                {},
                {},
                {},
                "Scene model is required to build a frame plan"
            );
        }
    }

    [[nodiscard]] FramePlan build() {
        const SceneProject& project = model_->project();
        const Scene& scene = project.scene;

        plan_.modelRevision = graphSnapshot_.modelRevision;
        plan_.width = projectionSize_.width;
        plan_.height = projectionSize_.height;
        plan_.renderQuality = renderQuality_;
        plan_.camera = snapshotCamera(scene.camera);
        plan_.parallax = snapshotParallax(scene);
        const auto generalDynamic = [&](std::string_view key)
            -> const DynamicValue& {
            const auto value = scene.generalValues.find(std::string(key));
            if (value == scene.generalValues.end()) {
                frameError(
                    *model_, SceneModelErrorCode::missingField,
                    "/general/" + std::string(key),
                    "Scene general value is required"
                );
            }
            return value->second;
        };
        const auto generalColor = [&](std::string_view key) {
            return colorValue(
                *model_,
                evaluate(
                    generalDynamic(key),
                    "/general/" + std::string(key),
                    std::nullopt
                ),
                "/general/" + std::string(key)
            );
        };
        const auto generalNumber = [&](std::string_view key) {
            const std::string pointer = "/general/" + std::string(key);
            return numberValue(
                *model_,
                evaluate(generalDynamic(key), pointer, std::nullopt),
                pointer,
                "Scene general number"
            );
        };
        const auto generalBoolean = [&](std::string_view key) {
            const std::string pointer = "/general/" + std::string(key);
            return booleanValue(
                *model_,
                evaluate(generalDynamic(key), pointer, std::nullopt),
                pointer,
                "Scene general boolean"
            );
        };
        plan_.ambientColor = generalColor("ambientcolor");
        plan_.skylightColor = generalColor("skylightcolor");
        plan_.distanceFog = {
            .enabled = generalBoolean("fogdistance"),
            .color = generalColor("fogdistancecolor"),
            .start = generalNumber("fogdistancestart"),
            .end = generalNumber("fogdistanceend"),
            .startDensity = generalNumber("fogdistancestartdensity"),
            .endDensity = generalNumber("fogdistanceenddensity"),
        };
        plan_.heightFog = {
            .enabled = generalBoolean("fogheight"),
            .color = generalColor("fogheightcolor"),
            .start = generalNumber("fogheightstart"),
            .end = generalNumber("fogheightend"),
            .startDensity = generalNumber("fogheightstartdensity"),
            .endDensity = generalNumber("fogheightenddensity"),
        };
        plan_.output = {
            .kind = FrameResourceKind::framebuffer,
            .id = "scene:_rt_FullFrameBuffer",
            .logicalName = "_rt_FullFrameBuffer",
        };
        plan_.framebuffers.push_back({
            .resource = plan_.output,
            .format = FramebufferFormat::rgba8,
            .width = plan_.width,
            .height = plan_.height,
            .scale = 1.0,
            .unique = true,
        });
        sceneFramebuffers_.emplace("_rt_FullFrameBuffer", plan_.output);
        sceneFramebuffers_.emplace("_rt_MipMappedFrameBuffer", plan_.output);

        // These runtime targets are created by the Linux renderer for every
        // scene, even when bloom is disabled. Keeping them in the frame arena
        // makes authored effects that bind the standard aliases resolve to a
        // real resource instead of failing during planning.
        const auto registerSceneFramebuffer = [&](
            std::string logicalName,
            std::uint32_t width,
            std::uint32_t height
        ) {
            const FramebufferDescriptor descriptor = createFramebuffer(
                "scene:" + logicalName,
                logicalName,
                FramebufferFormat::rgba8,
                std::max<std::uint32_t>(1, width),
                std::max<std::uint32_t>(1, height),
                1.0,
                true
            );
            sceneFramebuffers_.emplace(logicalName, descriptor.resource);
            return descriptor.resource;
        };
        registerSceneFramebuffer("_rt_4FrameBuffer", plan_.width / 4, plan_.height / 4);
        registerSceneFramebuffer("_rt_8FrameBuffer", plan_.width / 8, plan_.height / 8);
        registerSceneFramebuffer("_rt_Bloom", plan_.width / 8, plan_.height / 8);
        // Shadow comparison samples outside an entry must receive the border
        // value instead of repeating a neighboring light's depth tile.
        const FramebufferDescriptor shadowAtlasDescriptor = createFramebuffer(
            "scene:_rt_shadowAtlas",
            "_rt_shadowAtlas",
            FramebufferFormat::rgba8,
            plan_.width,
            plan_.height,
            1.0,
            true,
            FramebufferWrapMode::clampToBorder
        );
        sceneFramebuffers_.emplace(
            "_rt_shadowAtlas", shadowAtlasDescriptor.resource
        );
        // Volumetric materials use these runtime targets as pass-local
        // providers. Keep them in the frame arena even when no light is
        // active so a later scripted visibility change does not alter the
        // resource namespace halfway through a frame.
        const auto registerVolumetricFramebuffer = [this](
            std::string logicalName,
            FramebufferFormat format,
            std::uint32_t width,
            std::uint32_t height
        ) {
            const FramebufferDescriptor descriptor = createFramebuffer(
                "scene:" + logicalName,
                logicalName,
                format,
                std::max<std::uint32_t>(1, width),
                std::max<std::uint32_t>(1, height),
                1.0,
                true
            );
            sceneFramebuffers_.emplace(logicalName, descriptor.resource);
            return descriptor.resource;
        };
        registerVolumetricFramebuffer(
            "_rt_volumetricsBack", FramebufferFormat::r16f,
            plan_.width, plan_.height
        );
        const bool lowQuality = static_cast<std::uint8_t>(renderQuality_) < 3;
        const std::uint32_t lightBufferDivisor = lowQuality ? 8 : 4;
        // The official volumetric ray-limit buffer is quarter resolution for
        // quality 3+ and eighth resolution below that threshold.  It is
        // integer-addressed by texLoad2D, so both the allocation and the
        // shader's Resolution uniform must describe the same reduced target.
        registerVolumetricFramebuffer(
            "_rt_volumetricsSingle", FramebufferFormat::r16f,
            plan_.width / lightBufferDivisor,
            plan_.height / lightBufferDivisor
        );
        registerVolumetricFramebuffer(
            "_rt_volumetricsLightBuffer", FramebufferFormat::rgba8,
            plan_.width / lightBufferDivisor,
            plan_.height / lightBufferDivisor
        );
        if (lowQuality) {
            registerVolumetricFramebuffer(
                "_rt_volumetricsLightBufferB", FramebufferFormat::rgba8,
                plan_.width / 8,
                plan_.height / 8
            );
        }
        // The official cookie sampler is a real asset texture, not the
        // shadow depth atlas. The default is resolved again in planLightObjects
        // when a visible light selects an authored cookie.
        plan_.lightCookie = frameAssetTextureResource("cookie/flashlight1");
        sceneFramebuffers_.emplace("_alias_lightCookie", plan_.lightCookie);

        const auto clear = scene.generalValues.find("clearcolor");
        if (clear == scene.generalValues.end()) {
            frameError(
                *model_, SceneModelErrorCode::missingField,
                "/general/clearcolor", "Scene clear color is required"
            );
        }
        const EvaluatedValue clearValue = evaluate(
            clear->second, "/general/clearcolor", std::nullopt
        );
        plan_.clearColor = colorValue(*model_, clearValue, "/general/clearcolor");
        // Linux consumes clearcolor as Vec3 and always clears the scene with
        // an opaque alpha, even when the authored DynamicValue is Vec4.
        plan_.clearColor.alpha = 1.0;

        collectUnsupportedObjects();
        planLightObjects();
        collectDependencyObjectIds();
        registerImageCompositeResources();
        planRenderableObjects();
        planShadowCasters();
        planVolumetricObjects();
        planBloomObject();
        planSoundObjects();
        finalizeScriptLayerStates();
        validateFramePlan();
        return std::move(plan_);
    }

    void initializeMaterialScripts() {
        if (!evaluationFrame_ ||
            evaluationFrame_->scriptPropertyObjectsCurrent()) {
            return;
        }
        const auto& objects = model_->project().scene.objects;
        for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
            const auto* image = std::get_if<ImageObject>(&objects[objectIndex].data);
            if (image == nullptr) continue;
            const int layerId = objects[objectIndex].base.id;

            if (image->model && image->model->material) {
                for (std::size_t passIndex = 0;
                     passIndex < image->model->material->passes.size();
                     ++passIndex) {
                    const std::string id =
                        "layer:" + std::to_string(layerId) +
                        "/image/material/pass:" + std::to_string(passIndex);
                    const std::string pointer =
                        objectPointer(objectIndex, "image") +
                        "/material/passes/" + std::to_string(passIndex) +
                        "/constants";
                    initializeMaterialConstants(
                        id,
                        image->model->material->assetPath,
                        image->model->material->passes[passIndex],
                        nullptr,
                        pointer,
                        layerId
                    );
                }
            }

            for (std::size_t effectIndex = 0;
                 effectIndex < image->effects.size();
                 ++effectIndex) {
                const ImageEffect& instance = image->effects[effectIndex];
                if (!instance.effect) continue;
                const std::string effectPointer =
                    objectPointer(objectIndex, "effects") + '/' +
                    std::to_string(effectIndex);
                const std::string effectId =
                    "layer:" + std::to_string(layerId) + "/effect:" +
                    std::to_string(effectIndex);
                std::vector<std::string> materialIds;
                for (std::size_t passIndex = 0;
                     passIndex < instance.effect->passes.size();
                     ++passIndex) {
                    const EffectPass& effectPass =
                        instance.effect->passes[passIndex];
                    if (!effectPass.material) continue;
                    for (std::size_t materialPassIndex = 0;
                         materialPassIndex < effectPass.material->passes.size();
                         ++materialPassIndex) {
                        materialIds.push_back(
                            effectId + "/pass:" + std::to_string(passIndex) +
                            "/material-pass:" +
                            std::to_string(materialPassIndex)
                        );
                    }
                }
                evaluationFrame_->registerScriptPropertyObject({
                    .id = effectId,
                    .type = script::ScriptPropertyObjectType::effect,
                    .name = instance.name.empty()
                        ? instance.effect->name
                        : instance.name,
                    .properties = {{
                        "visible",
                        evaluateDynamicValue(
                            *model_,
                            instance.visible,
                            *graphSnapshot_.propertyValues,
                            effectPointer + "/visible"
                        ).value,
                    }},
                    .propertyAnimations = instance.visible.animation
                        ? std::map<std::string, TimelineAnimation>{{
                            "visible", *instance.visible.animation,
                        }}
                        : std::map<std::string, TimelineAnimation>{},
                    .materialIds = std::move(materialIds),
                });
                evaluationFrame_->initialize(
                    instance.visible,
                    effectPointer + "/visible",
                    script::ScriptPropertyOwner{
                        .layerId = layerId,
                        .type = script::ScriptPropertyOwnerType::effect,
                        .objectId = effectId,
                        .property = "visible",
                    }
                );

                std::size_t overrideIndex = 0;
                for (std::size_t passIndex = 0;
                     passIndex < instance.effect->passes.size();
                     ++passIndex) {
                    const EffectPass& effectPass =
                        instance.effect->passes[passIndex];
                    if (!effectPass.material) continue;
                    const EffectPassOverride* overridePass =
                        overrideIndex < instance.passOverrides.size()
                            ? &instance.passOverrides[overrideIndex]
                            : nullptr;
                    ++overrideIndex;
                    for (std::size_t materialPassIndex = 0;
                         materialPassIndex < effectPass.material->passes.size();
                         ++materialPassIndex) {
                        const std::string id =
                            effectId + "/pass:" + std::to_string(passIndex) +
                            "/material-pass:" +
                            std::to_string(materialPassIndex);
                        const std::string pointer =
                            effectPointer + "/passes/" +
                            std::to_string(passIndex) + "/material/passes/" +
                            std::to_string(materialPassIndex) + "/constants";
                        initializeMaterialConstants(
                            id,
                            effectPass.material->assetPath,
                            effectPass.material->passes[materialPassIndex],
                            overridePass,
                            pointer,
                            layerId
                        );
                    }
                }
            }
        }
        // Material/effect base descriptors are a function of the immutable
        // model plus the coherent user-property revision. Keep the registry's
        // existing overlays and timeline controllers between frames instead
        // of rebuilding every descriptor at presentation frequency.
        evaluationFrame_->commitScriptPropertyObjects();
    }

private:
    [[nodiscard]] EvaluatedValue evaluate(
        const DynamicValue& value,
        std::string pointer,
        std::optional<int> objectId,
        script::ScriptPropertyOwner owner = {}
    ) {
        if (!owner.layerId && objectId) owner.layerId = objectId;
        if (evaluationFrame_ && objectId &&
            owner.type == script::ScriptPropertyOwnerType::none) {
            const SceneGraphNodeSnapshot* node = graphSnapshot_.node(*objectId);
            if (node != nullptr && node->dynamic) {
                const std::string prefix = objectPointer(node->objectIndex) + '/';
                if (pointer.starts_with(prefix)) {
                    std::string property = pointer.substr(prefix.size());
                    constexpr std::string_view particleOverride =
                        "particle/instanceoverride/";
                    if (property.starts_with(particleOverride)) {
                        property.erase(0, particleOverride.size());
                    }
                    if (property.find('/') == std::string::npos) {
                        const auto fold = [](std::string_view name) {
                            std::string result;
                            result.reserve(name.size());
                            for (const unsigned char character : name) {
                                result.push_back(static_cast<char>(
                                    std::tolower(character)
                                ));
                            }
                            return result;
                        };
                        const std::string folded = fold(property);
                        const auto found = std::find_if(
                            node->layerProperties.begin(),
                            node->layerProperties.end(),
                            [&](const auto& entry) {
                                return fold(entry.first) == folded;
                            }
                        );
                        if (found != node->layerProperties.end()) {
                            return {
                                .value = found->second,
                                .source = DynamicValueSource::literal,
                            };
                        }
                    }
                }
            }
        }
        EvaluatedValue result;
        if (evaluationFrame_) {
            result = evaluationFrame_->evaluate(value, pointer, owner);
        } else if (value.script && scriptedValues_) {
            const auto found = scriptedValues_->find(pointer);
            if (found == scriptedValues_->end()) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue, pointer,
                    "Evaluated frame state is missing a scripted dynamic value"
                );
            }
            result = found->second;
        } else {
            result = evaluateDynamicValue(
                *model_, value, *graphSnapshot_.propertyValues, pointer
            );
        }
        if (result.source == DynamicValueSource::scriptInitial) {
            addIssue(
                FramePlanIssueCode::scriptRuntimeUnavailable,
                objectId,
                std::move(pointer),
                "QuickJS has not executed this dynamic value; FrameGraph only has its connected input state"
            );
        }
        if (result.source == DynamicValueSource::scriptUnavailable) {
            const FramePlanIssueSeverity severity = objectId
                ? FramePlanIssueSeverity::skipObject
                : FramePlanIssueSeverity::frameFatal;
            addIssue(
                FramePlanIssueCode::audioInputUnavailable,
                objectId,
                std::move(pointer),
                "Dynamic script requires unavailable system audio input",
                severity
            );
            if (objectId) {
                skippedObjectIds_.emplace(*objectId);
            }
        }
        return result;
    }

    template <typename Callback>
    static void forEachMergedConstant(
        const MaterialPass& material,
        const EffectPassOverride* overridePass,
        Callback&& callback
    ) {
        const ConstantMap empty;
        const ConstantMap& overrides = overridePass == nullptr
            ? empty
            : overridePass->constants;
        auto base = material.constants.begin();
        auto replacement = overrides.begin();
        while (base != material.constants.end() ||
               replacement != overrides.end()) {
            if (replacement == overrides.end() ||
                (base != material.constants.end() &&
                 base->first < replacement->first)) {
                callback(base->first, base->second);
                ++base;
                continue;
            }
            if (base == material.constants.end() ||
                replacement->first < base->first) {
                callback(replacement->first, replacement->second);
                ++replacement;
                continue;
            }
            callback(replacement->first, replacement->second);
            ++base;
            ++replacement;
        }
    }

    void registerMaterialObject(
        std::string id,
        std::string name,
        const MaterialPass& material,
        const EffectPassOverride* overridePass,
        const std::string& pointerPrefix
    ) {
        if (!evaluationFrame_) return;
        script::ScriptPropertyObjectDescriptor descriptor{
            .id = std::move(id),
            .type = script::ScriptPropertyObjectType::material,
            .name = std::move(name),
        };
        forEachMergedConstant(material, overridePass, [&](
            const std::string& property,
            const DynamicValue& value
        ) {
            descriptor.properties.emplace(
                property,
                evaluateDynamicValue(
                    *model_,
                    value,
                    *graphSnapshot_.propertyValues,
                    pointerPrefix + '/' + property
                ).value
            );
            if (value.animation) {
                descriptor.propertyAnimations.emplace(
                    property, *value.animation
                );
            }
        });
        evaluationFrame_->registerScriptPropertyObject(std::move(descriptor));
    }

    void initializeMaterialConstants(
        const std::string& id,
        const std::string& name,
        const MaterialPass& material,
        const EffectPassOverride* overridePass,
        const std::string& pointerPrefix,
        int layerId
    ) {
        if (!evaluationFrame_) return;
        registerMaterialObject(
            id, name, material, overridePass, pointerPrefix
        );
        forEachMergedConstant(material, overridePass, [&](
            const std::string& property,
            const DynamicValue& value
        ) {
            evaluationFrame_->initialize(
                value,
                pointerPrefix + '/' + property,
                script::ScriptPropertyOwner{
                    .layerId = layerId,
                    .type = script::ScriptPropertyOwnerType::material,
                    .objectId = id,
                    .property = property,
                }
            );
        });
    }

    [[nodiscard]] FrameCameraDescriptor snapshotCamera(
        const SceneCamera& camera
    ) {
        FrameCameraDescriptor result;
        result.center = cameraVector3Value(
            *model_, evaluate(camera.center, "/camera/center", std::nullopt),
            "/camera/center", "Camera center"
        );
        result.eye = cameraVector3Value(
            *model_, evaluate(camera.eye, "/camera/eye", std::nullopt),
            "/camera/eye", "Camera eye"
        );
        result.up = cameraVector3Value(
            *model_, evaluate(camera.up, "/camera/up", std::nullopt),
            "/camera/up", "Camera up"
        );
        result.nearPlane = numberValue(
            *model_, evaluate(camera.nearPlane, "/camera/nearz", std::nullopt),
            "/camera/nearz", "Camera near plane"
        );
        result.farPlane = numberValue(
            *model_, evaluate(camera.farPlane, "/camera/farz", std::nullopt),
            "/camera/farz", "Camera far plane"
        );
        result.fieldOfView = numberValue(
            *model_, evaluate(camera.fieldOfView, "/camera/fov", std::nullopt),
            "/camera/fov", "Camera field of view"
        );
        result.perspectiveOverrideFieldOfView = numberValue(
            *model_,
            evaluate(
                camera.perspectiveOverrideFieldOfView,
                "/general/perspectiveoverridefov",
                std::nullopt
            ),
            "/general/perspectiveoverridefov",
            "Perspective override field of view"
        );
        result.orthographic = camera.orthographic;
        result.orthogonalProjectionAuto = camera.projectionAuto;
        result.orthogonalProjectionWidth = plan_.width;
        result.orthogonalProjectionHeight = plan_.height;
        return result;
    }

    [[nodiscard]] FrameParallaxDescriptor snapshotParallax(
        const Scene& scene
    ) {
        FrameParallaxDescriptor result;
        const auto dynamic = [&](std::string_view key) -> const DynamicValue& {
            const auto value = scene.generalValues.find(std::string(key));
            if (value == scene.generalValues.end()) {
                frameError(
                    *model_, SceneModelErrorCode::missingField,
                    "/general/" + std::string(key),
                    "Parsed scene is missing a normalized general value"
                );
            }
            return value->second;
        };
        const auto boolean = [&](std::string_view key) {
            const std::string pointer = "/general/" + std::string(key);
            return booleanValue(
                *model_, evaluate(dynamic(key), pointer, std::nullopt),
                pointer, "Camera parallax " + std::string(key)
            );
        };
        const auto number = [&](std::string_view key) {
            const std::string pointer = "/general/" + std::string(key);
            return numberValue(
                *model_, evaluate(dynamic(key), pointer, std::nullopt),
                pointer, "Camera parallax " + std::string(key)
            );
        };
        result.enabled = boolean("cameraparallax");
        result.amount = number("cameraparallaxamount");
        result.delay = number("cameraparallaxdelay");
        result.mouseInfluence = number("cameraparallaxmouseinfluence");
        if (result.delay < 0.0) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue,
                "/general/cameraparallaxdelay",
                "Camera parallax delay must not be negative"
            );
        }
        return result;
    }

    void addIssue(
        FramePlanIssueCode code,
        std::optional<int> objectId,
        std::string pointer,
        std::string message,
        std::optional<FramePlanIssueSeverity> severity = std::nullopt
    ) {
        const FramePlanIssueSeverity resolvedSeverity =
            severity.value_or(defaultIssueSeverity(code));
        const auto existing = std::find_if(
            plan_.issues.begin(), plan_.issues.end(),
            [&](const FramePlanIssue& issue) {
                return issue.code == code && issue.objectId == objectId &&
                    issue.jsonPointer == pointer;
            }
        );
        if (existing != plan_.issues.end()) {
            FramePlanIssue& issue = *existing;
            if (static_cast<int>(resolvedSeverity) >
                static_cast<int>(issue.severity)) {
                issue.severity = resolvedSeverity;
            }
            return;
        }
        plan_.issues.push_back({
            .code = code,
            .severity = resolvedSeverity,
            .objectId = objectId,
            .assetPath = model_->project().scene.assetPath,
            .jsonPointer = std::move(pointer),
            .message = std::move(message),
        });
    }

    void addPlanningIssue(
        FramePlanIssueCode code,
        std::optional<int> objectId,
        std::string pointer,
        const std::exception& error,
        FramePlanIssueSeverity severity
    ) {
        addIssue(
            code,
            objectId,
            std::move(pointer),
            std::string("Scene object was skipped: ") + error.what(),
            severity
        );
    }

    [[nodiscard]] PlanCheckpoint checkpoint() const noexcept {
        return {
            .framebufferCount = plan_.framebuffers.size(),
            .imageCount = plan_.images.size(),
            .textCount = plan_.texts.size(),
            .textEffectCount = plan_.textEffects.size(),
            .particleCount = plan_.particles.size(),
            .soundCount = plan_.sounds.size(),
            .operationCount = plan_.operations.size(),
        };
    }

    void rollback(
        const PlanCheckpoint& value,
        std::optional<std::size_t> retainedFramebufferCount = std::nullopt
    ) {
        plan_.framebuffers.resize(
            retainedFramebufferCount.value_or(value.framebufferCount)
        );
        plan_.images.resize(value.imageCount);
        plan_.texts.resize(value.textCount);
        plan_.textEffects.resize(value.textEffectCount);
        plan_.particles.resize(value.particleCount);
        plan_.sounds.resize(value.soundCount);
        plan_.operations.resize(value.operationCount);
    }

    void recordObjectPlanningFailure(
        std::size_t objectIndex,
        int objectId,
        const std::exception& error
    ) {
        skippedObjectIds_.emplace(objectId);
        addPlanningIssue(
            FramePlanIssueCode::objectPlanningFailed,
            objectId,
            objectPointer(objectIndex),
            error,
            FramePlanIssueSeverity::skipObject
        );
    }

    template <typename Callback>
    void isolateObjectPlanning(
        std::size_t objectIndex,
        int objectId,
        Callback&& callback
    ) {
        if (skippedObjectIds_.contains(objectId)) {
            return;
        }
        const PlanCheckpoint before = checkpoint();
        try {
            callback();
            if (skippedObjectIds_.contains(objectId)) {
                rollback(before);
            }
        } catch (const std::bad_alloc&) {
            rollback(before);
            throw;
        } catch (const std::exception& error) {
            rollback(before);
            recordObjectPlanningFailure(objectIndex, objectId, error);
        }
    }

    void collectUnsupportedObjects() {
        const auto& objects = model_->project().scene.objects;
        for (const SceneGraphNodeSnapshot& node : graphSnapshot_.nodes) {
            for (const auto* value : {&node.origin, &node.scale, &node.angles, &node.visible}) {
                if (value->source == DynamicValueSource::scriptInitial ||
                    value->source == DynamicValueSource::scriptUnavailable) {
                    const bool unavailable =
                        value->source == DynamicValueSource::scriptUnavailable;
                    addIssue(
                        unavailable
                            ? FramePlanIssueCode::audioInputUnavailable
                            : FramePlanIssueCode::scriptRuntimeUnavailable,
                        node.id, objectPointer(node.objectIndex),
                        "QuickJS has not executed an object dynamic value",
                        unavailable
                            ? FramePlanIssueSeverity::skipObject
                            : FramePlanIssueSeverity::warning
                    );
                    if (unavailable) {
                        skippedObjectIds_.emplace(node.id);
                    }
                    break;
                }
            }
        }
    }

    void planLightObjects() {
        const Scene& scene = model_->project().scene;
        FrameLightConfiguration actual;
        const auto increment = [&actual](LightType type) -> std::size_t& {
            switch (type) {
                case LightType::point: return actual.point;
                case LightType::spot: return actual.spot;
                case LightType::tube: return actual.tube;
                case LightType::directional: return actual.directional;
            }
            std::terminate();
        };
        for (const SceneObject& object : scene.objects) {
            if (const auto* light = std::get_if<LightObject>(&object.data)) {
                ++increment(light->type);
            }
        }

        const auto configured = [&](std::optional<std::size_t> authored,
                                    std::size_t discovered,
                                    std::string_view name) {
            const std::size_t result = authored.value_or(discovered);
            if (result < discovered) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue,
                    "/general/lightconfig/" + std::string(name),
                    "Scene light configuration reserves " +
                        std::to_string(result) + " " + std::string(name) +
                        " slots but the scene contains " +
                        std::to_string(discovered) + " objects"
                );
            }
            return result;
        };
        plan_.lightConfiguration = {
            .point = configured(
                scene.lightConfiguration.point, actual.point, "point"
            ),
            .spot = configured(
                scene.lightConfiguration.spot, actual.spot, "spot"
            ),
            .tube = configured(
                scene.lightConfiguration.tube, actual.tube, "tube"
            ),
            .directional = configured(
                scene.lightConfiguration.directional,
                actual.directional,
                "directional"
            ),
        };
        const std::size_t slotCount = plan_.lightConfiguration.point +
            plan_.lightConfiguration.spot + plan_.lightConfiguration.tube +
            plan_.lightConfiguration.directional;
        if (slotCount > 4) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue,
                "/general/lightconfig",
                "Wallpaper Engine scenes support at most four configured lights"
            );
        }

        for (const std::size_t nodeIndex : graphSnapshot_.renderOrder) {
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(nodeIndex);
            const SceneObject& object = scene.objects.at(node.objectIndex);
            const auto* light = std::get_if<LightObject>(&object.data);
            if (light == nullptr) continue;
            const std::string base = objectPointer(node.objectIndex);
            const auto scalar = [&](const DynamicValue& value,
                                    std::string_view field) {
                const std::string pointer = base + '/' + std::string(field);
                return numberValue(
                    *model_, evaluate(value, pointer, node.id), pointer,
                    "Light " + std::string(field)
                );
            };
            const auto flag = [&](const DynamicValue& value,
                                  std::string_view field) {
                const std::string pointer = base + '/' + std::string(field);
                return booleanValue(
                    *model_, evaluate(value, pointer, node.id), pointer,
                    "Light " + std::string(field)
                );
            };
            const std::string colorPointer = base + "/color";
            const std::string controlPointer = base + "/controlpoint";
            FrameLightDescriptor descriptor{
                .objectIndex = node.objectIndex,
                .objectId = node.id,
                .visible = node.isVisible,
                .worldTransform = node.worldTransform,
                .color = colorValue(
                    *model_,
                    evaluate(light->color, colorPointer, node.id),
                    colorPointer
                ),
                .intensity = scalar(light->intensity, "intensity"),
                .radius = scalar(light->radius, "radius"),
                .exponent = scalar(light->exponent, "exponent"),
                .innerCone = scalar(light->innerCone, "innercone"),
                .outerCone = scalar(light->outerCone, "outercone"),
                .controlPoint = vector3Value(
                    *model_,
                    evaluate(light->controlPoint, controlPointer, node.id),
                    controlPointer,
                    "Light control point"
                ),
                .castShadow = flag(light->castShadow, "castshadow"),
                .cookie = textValue(
                    light->cookie,
                    base + "/cookie",
                    node.id,
                    "Light cookie"
                ),
                .useCookie = flag(light->useCookie, "usecookie"),
                .castVolumetrics = flag(
                    light->castVolumetrics, "castvolumetrics"
                ),
                .density = scalar(light->density, "density"),
                .volumetricsExponent = scalar(
                    light->volumetricsExponent, "volumetricsexponent"
                ),
                .lightSourceSize = scalar(
                    light->lightSourceSize, "lightsourcesize"
                ),
            };
            for (std::size_t index = 0;
                 index < descriptor.cascadeDistances.size(); ++index) {
                descriptor.cascadeDistances[index] = scalar(
                    light->cascadeDistances[index],
                    "cascadedistance" + std::to_string(index)
                );
            }
            switch (light->type) {
                case LightType::point:
                    descriptor.type = FrameLightType::point;
                    break;
                case LightType::spot:
                    descriptor.type = FrameLightType::spot;
                    break;
                case LightType::tube:
                    descriptor.type = FrameLightType::tube;
                    break;
                case LightType::directional:
                    descriptor.type = FrameLightType::directional;
                    break;
            }
            if (descriptor.intensity < 0.0 || descriptor.radius < 0.0) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue, base,
                    "Light intensity and radius must not be negative"
                );
            }
            if (descriptor.type == FrameLightType::spot &&
                (descriptor.innerCone < 0.0 || descriptor.outerCone < 0.0 ||
                 descriptor.innerCone > descriptor.outerCone ||
                 descriptor.outerCone > 180.0)) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue, base,
                    "Spot light cones must satisfy 0 <= innercone <= outercone <= 180 degrees"
                );
            }
            plan_.lights.push_back(std::move(descriptor));
        }

        FrameLightConfiguration classified;
        for (const FrameLightDescriptor& light : plan_.lights) {
            if (!light.visible) continue;
            switch (light.type) {
                case FrameLightType::point:
                    classified.pointShadow += light.castShadow ? 1U : 0U;
                    break;
                case FrameLightType::spot:
                    if (light.castShadow && light.useCookie) {
                        ++classified.spotShadowCookie;
                    } else if (light.castShadow) {
                        ++classified.spotShadow;
                    } else if (light.useCookie) {
                        ++classified.spotCookie;
                    }
                    break;
                case FrameLightType::directional:
                    classified.directionalShadow += light.castShadow ? 1U : 0U;
                    break;
                case FrameLightType::tube:
                    break;
            }
        }
        plan_.lightConfiguration.pointShadow = configured(
            scene.lightConfiguration.pointShadow,
            classified.pointShadow,
            "pointshadow"
        );
        plan_.lightConfiguration.spotCookie = configured(
            scene.lightConfiguration.spotCookie,
            classified.spotCookie,
            "spotcookie"
        );
        plan_.lightConfiguration.spotShadow = configured(
            scene.lightConfiguration.spotShadow,
            classified.spotShadow,
            "spotshadow"
        );
        plan_.lightConfiguration.spotShadowCookie = configured(
            scene.lightConfiguration.spotShadowCookie,
            classified.spotShadowCookie,
            "spotshadowcookie"
        );
        plan_.lightConfiguration.directionalShadow = configured(
            scene.lightConfiguration.directionalShadow,
            classified.directionalShadow,
            "directionalshadow"
        );
        if (plan_.lightConfiguration.pointShadow >
                plan_.lightConfiguration.point ||
            plan_.lightConfiguration.spotCookie +
                    plan_.lightConfiguration.spotShadow +
                    plan_.lightConfiguration.spotShadowCookie >
                plan_.lightConfiguration.spot ||
            plan_.lightConfiguration.directionalShadow >
                plan_.lightConfiguration.directional) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue,
                "/general/lightconfig",
                "Scene light feature counts exceed their configured light totals"
            );
        }
        const std::uint32_t shadowResolution = [&] {
            switch (renderQuality_) {
                case FrameRenderQuality::powerSaving:
                case FrameRenderQuality::balanced:
                    return 256U;
                case FrameRenderQuality::high:
                    return 512U;
                case FrameRenderQuality::ultra:
                    return 1024U;
            }
            std::terminate();
        }();
        if (plan_.lightConfiguration.directionalShadow > 1) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue,
                "/general/lightconfig",
                "Wallpaper Engine supports at most one shadow-casting directional light"
            );
        }
        plan_.shadowAtlasResolution = shadowResolution;
        const auto featureRank = [](const FrameLightDescriptor& light) {
            switch (light.type) {
                case FrameLightType::point:
                    return light.castShadow ? 0 : 1;
                case FrameLightType::spot:
                    if (light.castShadow && light.useCookie) return 0;
                    if (light.useCookie) return 1;
                    if (light.castShadow) return 2;
                    return 3;
                case FrameLightType::directional:
                    return light.castShadow ? 0 : 1;
                case FrameLightType::tube:
                    return 0;
            }
            std::terminate();
        };
        std::vector<std::size_t> orderedShadowLights;
        for (std::size_t index = 0; index < plan_.lights.size(); ++index) {
            const FrameLightDescriptor& light = plan_.lights[index];
            if (light.visible && light.castShadow &&
                light.type != FrameLightType::tube) {
                orderedShadowLights.push_back(index);
            }
        }
        std::stable_sort(
            orderedShadowLights.begin(), orderedShadowLights.end(),
            [&](std::size_t lhs, std::size_t rhs) {
                const FrameLightDescriptor& left = plan_.lights[lhs];
                const FrameLightDescriptor& right = plan_.lights[rhs];
                if (left.type != right.type) {
                    return static_cast<int>(left.type) <
                        static_cast<int>(right.type);
                }
                return featureRank(left) < featureRank(right);
            }
        );
        struct FreeShadowRect final {
            std::uint32_t left = 0;
            std::uint32_t right = 0;
            std::uint32_t top = 0;
            std::uint32_t bottom = 0;
        };
        std::vector<FreeShadowRect> freeRects{{
            .left = 0, .right = 0x2000U, .top = 0, .bottom = 0x2000U,
        }};
        std::uint32_t atlasWidth = 0;
        std::uint32_t atlasHeight = 0;
        for (const std::size_t lightIndex : orderedShadowLights) {
            const FrameLightDescriptor& light = plan_.lights[lightIndex];
            const std::size_t entryCount =
                light.type == FrameLightType::directional ? 3U : 1U;
            for (std::size_t cascade = 0; cascade < entryCount; ++cascade) {
                const auto available = std::find_if(
                    freeRects.begin(), freeRects.end(),
                    [&](const FreeShadowRect& rect) {
                        return shadowResolution <= rect.right - rect.left &&
                            shadowResolution <= rect.bottom - rect.top;
                    }
                );
                if (available == freeRects.end()) {
                    frameError(
                        *model_, SceneModelErrorCode::invalidValue,
                        "/general/lightconfig",
                        "Shadow atlas exceeds Wallpaper Engine's 8192x8192 allocator boundary"
                    );
                }
                const std::uint32_t x = available->left;
                const std::uint32_t y = available->top;
                const std::uint32_t newLeft = x + shadowResolution;
                const std::uint32_t newTop = y + shadowResolution;
                const std::uint32_t oldBottom = available->bottom;
                available->left = newLeft;
                if (newTop < oldBottom) {
                    freeRects.push_back({
                        .left = x,
                        .right = newLeft,
                        .top = newTop,
                        .bottom = oldBottom,
                    });
                }
                plan_.shadowAtlasEntries.push_back({
                    .lightIndex = lightIndex,
                    .cascade = cascade,
                    .x = x,
                    .y = y,
                    .size = shadowResolution,
                });
                atlasWidth = std::max(atlasWidth, newLeft);
                atlasHeight = std::max(atlasHeight, newTop);
            }
        }
        plan_.shadowAtlasWidth = std::max<std::uint32_t>(2U, atlasWidth);
        plan_.shadowAtlasHeight = std::max<std::uint32_t>(2U, atlasHeight);
        resizeSceneFramebuffer(
            "_rt_shadowAtlas",
            plan_.shadowAtlasWidth,
            plan_.shadowAtlasHeight
        );

        // Wallpaper Engine exposes one 2D cookie sampler to the generated
        // lighting module. Select the first visible cookie light in scene
        // order, matching the renderer's single provider contract. The
        // reverse-engineered loader resolves missing/unknown authored names
        // to cookie/flashlight1, so keep that behavior while still failing
        // explicitly if the official fallback asset itself is unavailable.
        const FrameResourceRef defaultCookie =
            frameAssetTextureResource("cookie/flashlight1");
        const AssetResolver& resolver = model_->runtime()->assetResolver();
        for (const FrameLightDescriptor& light : plan_.lights) {
            if (!light.visible || !light.useCookie) continue;
            std::string cookiePath = light.cookie;
            if (cookiePath.empty()) {
                cookiePath = "cookie/flashlight1";
            } else if (!cookiePath.starts_with("cookie/") &&
                       !cookiePath.starts_with("materials/")) {
                cookiePath = "cookie/" + cookiePath;
            }
            FrameResourceRef selected = frameAssetTextureResource(cookiePath);
            if (!resolver.contains(selected.logicalName)) {
                selected = defaultCookie;
            }
            if (!resolver.contains(selected.logicalName)) {
                addIssue(
                    FramePlanIssueCode::objectPlanningFailed,
                    light.objectId,
                    objectPointer(light.objectIndex, "cookie"),
                    "Light cookie provider is unavailable: '" +
                        selected.logicalName + "'",
                    FramePlanIssueSeverity::frameFatal
                );
                break;
            }
            plan_.lightCookie = selected;
            sceneFramebuffers_.at("_alias_lightCookie") = selected;
            break;
        }
    }

    [[nodiscard]] FrameResourceRef resolveTexture(
        std::string_view name,
        const FramebufferMap& localFramebuffers,
        std::string pointer
    ) const {
        if (name.empty()) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue, std::move(pointer),
                "Texture name must not be empty"
            );
        }
        if (isPassRelativeResourceName(name)) {
            frameError(
                *model_, SceneModelErrorCode::unsupportedObject,
                std::move(pointer),
                "Effect runtime resource '" + std::string(name) +
                    "' requires pass-relative resolution in this position"
            );
        }
        const std::string key(name);
        if (const auto local = localFramebuffers.find(key);
            local != localFramebuffers.end()) {
            return local->second;
        }
        if (const auto scene = sceneFramebuffers_.find(key);
            scene != sceneFramebuffers_.end()) {
            return scene->second;
        }
        if (isFramebufferName(name)) {
            frameError(
                *model_, SceneModelErrorCode::danglingReference, std::move(pointer),
                "Texture references an unknown framebuffer '" + key + "'"
            );
        }
        if (name == "$mediaThumbnail" ||
            name == "$mediaPreviousThumbnail") {
            return frameHostTextureResource(name);
        }
        return frameAssetTextureResource(name);
    }

    [[nodiscard]] FrameResourceRef resolveUserTexture(
        const TextureSlot& slot,
        const FramebufferMap& localFramebuffers,
        std::string pointer
    ) const {
        if (!slot.name || slot.name->empty()) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue,
                std::move(pointer),
                "User texture binding must name a property or texture"
            );
        }
        const std::string& name = *slot.name;
        const auto metadataType = [&]() -> std::optional<std::string> {
            if (!slot.metadata) return std::nullopt;
            const auto type = slot.metadata->find("type");
            if (type == slot.metadata->end()) return std::nullopt;
            if (const auto* value = std::get_if<std::string>(
                    &type->second.storage)) {
                return *value;
            }
            return std::nullopt;
        }();
        const auto property = model_->project().properties.find(name);
        const bool propertyBinding = property != model_->project().properties.end() ||
            metadataType == "scenetexture" ||
            metadataType == "usershortcut" || metadataType == "file";
        if (!propertyBinding) {
            // Linux accepts string entries in usertextures as ordinary texture
            // providers. Object entries with a user-property type take the
            // Windows host-binding path above.
            return resolveTexture(name, localFramebuffers, std::move(pointer));
        }

        std::string selectedAsset;
        if (property != model_->project().properties.end() &&
            property->second.type == PropertyType::sceneTexture) {
            const auto selected = graphSnapshot_.propertyValues->find(name);
            if (selected != graphSnapshot_.propertyValues->end()) {
                if (const auto* value = std::get_if<std::string>(
                        &selected->second.storage)) {
                    selectedAsset = *value;
                } else {
                    frameError(
                        *model_, SceneModelErrorCode::typeMismatch,
                        std::move(pointer),
                        "Scene-texture property '" + name +
                            "' must resolve to a string asset name"
                    );
                }
            }
        }
        return {
            .kind = FrameResourceKind::userPropertyTexture,
            .id = "user-property:" + name,
            .logicalName = name,
            .resolvedAssetName = std::move(selectedAsset),
        };
    }

    [[nodiscard]] std::optional<FrameResourceRef> primarySource(
        const ImageObject& image,
        std::size_t objectIndex,
        int objectId
    ) {
        if (!image.model || !image.model->material ||
            image.model->material->passes.empty()) {
            frameError(
                *model_, SceneModelErrorCode::assetFailure,
                objectPointer(objectIndex, "image"),
                "Image model has no material pass to provide its primary texture"
            );
        }
        // A solid layer is a procedural transparent image source. Its material
        // may intentionally omit slot zero; any authored slot zero is not the
        // object source and must not replace this contract.
        if (image.model->solidLayer) {
            return std::nullopt;
        }
        const MaterialPass pass = effectiveBasePass(
            image.model->material->passes.front(), image, true
        );
        const auto textures = namedSlots(pass.textures);
        std::optional<std::string> name;
        if (const auto base = textures.find(0); base != textures.end()) {
            name = base->second;
        }
        if (!name) {
            addIssue(
                FramePlanIssueCode::imageMaterialUnavailable,
                objectId,
                objectPointer(objectIndex, "image"),
                "Image material has no primary texture in slot zero; using the transparent source defined by the Linux runtime",
                FramePlanIssueSeverity::warning
            );
            return std::nullopt;
        }
        return resolveTexture(*name, {}, objectPointer(objectIndex, "image"));
    }

    [[nodiscard]] FrameVector2 imageSize(
        const ImageObject& image,
        const std::optional<FrameResourceRef>& source,
        std::size_t objectIndex,
        int objectId
    ) {
        const std::string pointer = objectPointer(objectIndex, "size");
        if (image.model && image.model->fullscreen) {
            return {
                .x = static_cast<double>(plan_.width),
                .y = static_cast<double>(plan_.height),
            };
        }
        FrameVector2 result = vector2Value(
            *model_, evaluate(image.size, pointer, objectId), pointer,
            "Image size"
        );
        if (result.x < 0.0 || result.y < 0.0) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue, pointer,
                "Image size must not contain a negative component"
            );
        }
        if (result.x > 0.0 && result.y > 0.0) {
            return result;
        }
        if (result.x != 0.0 || result.y != 0.0) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue, pointer,
                "Image size fallback requires both components to be zero"
            );
        }
        if (!source && image.model && image.model->solidLayer) {
            return {
                .x = static_cast<double>(plan_.width),
                .y = static_cast<double>(plan_.height),
            };
        }
        if (source && source->kind == FrameResourceKind::framebuffer) {
            const auto descriptor = std::find_if(
                plan_.framebuffers.rbegin(), plan_.framebuffers.rend(),
                [&](const FramebufferDescriptor& candidate) {
                    return candidate.resource.id == source->id;
                }
            );
            if (descriptor != plan_.framebuffers.rend()) {
                return {
                    .x = static_cast<double>(descriptor->width),
                    .y = static_cast<double>(descriptor->height),
                };
            }
        }
        if (!source && image.model && image.model->width && image.model->height) {
            result.x = static_cast<double>(*image.model->width);
            result.y = static_cast<double>(*image.model->height);
            if (result.x > 0.0 && result.y > 0.0) {
                return result;
            }
        }
        if (!source || source->kind != FrameResourceKind::assetTexture) {
            frameError(
                *model_, SceneModelErrorCode::unsupportedObject, pointer,
                "Image size fallback has no authored texture or model dimensions"
            );
        }
        const Texture texture = model_->runtime()->assetResolver().parseTexture(source->id);
        result.x = static_cast<double>(texture.width);
        result.y = static_cast<double>(texture.height);
        if (result.x <= 0.0 || result.y <= 0.0) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue, pointer,
                "Image size and primary texture dimensions are invalid"
            );
        }
        return result;
    }

    [[nodiscard]] FramebufferDescriptor createFramebuffer(
        std::string id,
        std::string logicalName,
        FramebufferFormat format,
        std::uint32_t width,
        std::uint32_t height,
        double scale,
        bool unique,
        FramebufferWrapMode wrapMode = FramebufferWrapMode::clampToEdge
    ) {
        FramebufferDescriptor descriptor{
            .resource = {
                .kind = FrameResourceKind::framebuffer,
                .id = std::move(id),
                .logicalName = std::move(logicalName),
            },
            .format = format,
            .wrapMode = wrapMode,
            .width = width,
            .height = height,
            .scale = scale,
            .unique = unique,
        };
        plan_.framebuffers.push_back(descriptor);
        return descriptor;
    }

    void resizeSceneFramebuffer(
        std::string_view logicalName,
        std::uint32_t width,
        std::uint32_t height
    ) {
        const auto resource = sceneFramebuffers_.find(std::string(logicalName));
        if (resource == sceneFramebuffers_.end()) {
            throw std::logic_error(
                "Scene framebuffer alias is not registered: " +
                std::string(logicalName)
            );
        }
        bool found = false;
        for (FramebufferDescriptor& descriptor : plan_.framebuffers) {
            if (descriptor.resource.id != resource->second.id) continue;
            descriptor.width = std::max<std::uint32_t>(1, width);
            descriptor.height = std::max<std::uint32_t>(1, height);
            found = true;
        }
        if (!found) {
            throw std::logic_error(
                "Scene framebuffer descriptor is not present: " +
                std::string(logicalName)
            );
        }
    }

    void registerImageCompositeResources() {
        const auto& objects = model_->project().scene.objects;
        for (const SceneObject& object : objects) {
            if (!std::holds_alternative<ImageObject>(object.data)) {
                continue;
            }
            for (const char suffix : {'a', 'b'}) {
                FrameResourceRef resource = imageCompositeResource(
                    object.base.id, suffix
                );
                sceneFramebuffers_.emplace(resource.logicalName, std::move(resource));
            }
        }
    }

    void collectDependencyObjectIds() {
        std::set<int> graphObjectIds;
        for (const SceneGraphNodeSnapshot& node : graphSnapshot_.nodes) {
            graphObjectIds.insert(node.id);
        }
        const auto& objects = model_->project().scene.objects;
        for (const SceneGraphNodeSnapshot& node : graphSnapshot_.nodes) {
            const SceneObject& sourceObject = objects.at(node.objectIndex);
            for (const ObjectDependency& dependency :
                 sourceObject.base.dependencies) {
                if (dependency.id == sourceObject.base.id) continue;
                if (graphObjectIds.contains(dependency.id)) {
                    dependencyObjectIds_.insert(dependency.id);
                }
            }
        }
    }

    [[nodiscard]] std::optional<ImageContext> createImageContext(
        std::size_t objectIndex,
        std::size_t nodeIndex,
        std::optional<std::size_t>& retainedFramebufferCount
    ) {
        const auto& objects = model_->project().scene.objects;
        const auto* image = std::get_if<ImageObject>(&objects.at(objectIndex).data);
        if (image == nullptr) {
            return std::nullopt;
        }
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(nodeIndex);
            const std::optional<FrameResourceRef> source = primarySource(
                *image, objectIndex, node.id
            );
            const bool solidLayer = image->model && image->model->solidLayer;
            const FrameVector2 size = imageSize(
                *image, source, objectIndex, node.id
            );
            const std::uint32_t width = checkedDimension(
                *model_, size.x, objectPointer(objectIndex, "size"), "Image width"
            );
            const std::uint32_t height = checkedDimension(
                *model_, size.y, objectPointer(objectIndex, "size"), "Image height"
            );
            const FrameResourceRef resourceA = imageCompositeResource(node.id, 'a');
            const FrameResourceRef resourceB = imageCompositeResource(node.id, 'b');
            sceneFramebuffers_.insert_or_assign(resourceA.logicalName, resourceA);
            sceneFramebuffers_.insert_or_assign(resourceB.logicalName, resourceB);
            const FramebufferDescriptor compositeA = createFramebuffer(
                resourceA.id, resourceA.logicalName, FramebufferFormat::rgba8,
                width, height, 1.0, true
            );
            const FramebufferDescriptor compositeB = createFramebuffer(
                resourceB.id, resourceB.logicalName, FramebufferFormat::rgba8,
                width, height, 1.0, true
            );
            retainedFramebufferCount = plan_.framebuffers.size();

            std::optional<FramebufferDescriptor> proceduralSource;
            if (!source) {
                const std::string logicalName = solidLayer
                    ? "_rt_solidLayerSource_" + std::to_string(node.id)
                    : "_rt_missingTextureSource_" + std::to_string(node.id);
                proceduralSource = createFramebuffer(
                    "object:" + std::to_string(node.id) + ':' + logicalName,
                    logicalName, FramebufferFormat::rgba8,
                    width, height, 1.0, true
                );
            }
            const std::size_t imageIndex = plan_.images.size();
            const FrameResourceRef resolvedSource = source
                ? *source
                : proceduralSource->resource;
            ObjectTransform worldTransform = node.worldTransform;
            if (image->model && image->model->fullscreen) {
                worldTransform.origin = {
                    static_cast<double>(plan_.width) * 0.5,
                    static_cast<double>(plan_.height) * 0.5,
                    0.0,
                };
            }
            std::vector<FramePuppetAnimationLayer> puppetAnimationLayers;
            puppetAnimationLayers.reserve(image->animationLayers.size());
            for (std::size_t layerIndex = 0;
                 layerIndex < image->animationLayers.size();
                 ++layerIndex) {
                const PuppetAnimationLayer& layer =
                    image->animationLayers[layerIndex];
                const std::string layerPointer = objectPointer(
                    objectIndex,
                    "animationlayers/" + std::to_string(layerIndex)
                );
                const bool visible = booleanValue(
                    *model_,
                    evaluate(
                        layer.visible,
                        layerPointer + "/visible",
                        node.id
                    ),
                    layerPointer + "/visible",
                    "Puppet animation visibility"
                );
                if (!visible) continue;
                const double rate = numberValue(
                    *model_,
                    evaluate(layer.rate, layerPointer + "/rate", node.id),
                    layerPointer + "/rate",
                    "Puppet animation rate"
                );
                const double blend = numberValue(
                    *model_,
                    evaluate(layer.blend, layerPointer + "/blend", node.id),
                    layerPointer + "/blend",
                    "Puppet animation blend"
                );
                const double animationNumber = numberValue(
                    *model_,
                    evaluate(
                        layer.animation,
                        layerPointer + "/animation",
                        node.id
                    ),
                    layerPointer + "/animation",
                    "Puppet animation id"
                );
                if (blend < 0.0 || blend > 1.0) {
                    frameError(
                        *model_,
                        SceneModelErrorCode::invalidValue,
                        layerPointer + "/blend",
                        "Puppet animation blend must resolve inside 0...1"
                    );
                }
                if (std::floor(animationNumber) != animationNumber ||
                    animationNumber < std::numeric_limits<int>::min() ||
                    animationNumber > std::numeric_limits<int>::max()) {
                    frameError(
                        *model_,
                        SceneModelErrorCode::invalidValue,
                        layerPointer + "/animation",
                        "Puppet animation id must resolve to an integer"
                    );
                }
                const int animationId = static_cast<int>(animationNumber);
                if (!image->model || !image->model->puppetMesh ||
                    image->model->puppetMesh->animation(animationId) == nullptr) {
                    frameError(
                        *model_,
                        SceneModelErrorCode::danglingReference,
                        layerPointer + "/animation",
                        "Puppet animation layer references unknown animation id " +
                            std::to_string(animationId)
                    );
                }
                if (blend == 0.0) continue;
                puppetAnimationLayers.push_back({
                    .animationId = animationId,
                    .rate = rate,
                    .blend = blend,
                    .additive = layer.additive,
                });
            }
            plan_.images.push_back({
                .objectIndex = objectIndex,
                .objectId = node.id,
                .visible = node.isVisible,
                .solid = objects.at(objectIndex).base.solid,
                .castShadow = image->castShadow,
                .passthrough = image->model && image->model->passthrough,
                .fullscreen = image->model && image->model->fullscreen,
                .perspective = image->perspective,
                .size = size,
                .worldTransform = worldTransform,
                .source = resolvedSource,
                .compositeA = compositeA.resource,
                .compositeB = compositeB.resource,
                .puppetMesh = image->model ? image->model->puppetMesh : nullptr,
                .puppetAnimationLayers = std::move(puppetAnimationLayers),
                .alpha = evaluate(image->alpha, objectPointer(objectIndex, "alpha"), node.id),
                .color = evaluate(image->color, objectPointer(objectIndex, "color"), node.id),
                .brightness = evaluate(image->brightness, objectPointer(objectIndex, "brightness"), node.id),
                .colorBlendMode = evaluate(image->colorBlendMode, objectPointer(objectIndex, "colorBlendMode"), node.id),
                .parallaxDepth = evaluate(image->parallaxDepth, objectPointer(objectIndex, "parallaxDepth"), node.id),
                .horizontalAlignment = image->horizontalAlignment,
            });
            if (proceduralSource) {
                plan_.operations.push_back(FrameClearCommand{
                    .origin = {
                        .imageIndex = imageIndex,
                        .objectId = node.id,
                    },
                    .destination = proceduralSource->resource,
                    .color = {
                        .red = 0.0,
                        .green = 0.0,
                        .blue = 0.0,
                        .alpha = 0.0,
                    },
                });
            }
            return ImageContext{
                .planImageIndex = imageIndex,
                .objectIndex = objectIndex,
                .image = image,
                .node = &node,
                .currentMain = compositeA.resource,
                .currentSub = compositeB.resource,
            };
    }

    [[nodiscard]] std::string textValue(
        const DynamicValue& value,
        std::string pointer,
        int objectId,
        std::string_view description
    ) {
        const EvaluatedValue evaluated = evaluate(value, pointer, objectId);
        (void)pointer;
        (void)description;
        return evaluated.value.string();
    }

    [[nodiscard]] std::optional<std::size_t> createTextDescriptor(
        std::size_t objectIndex,
        std::size_t nodeIndex
    ) {
        const auto& objects = model_->project().scene.objects;
        const auto* text = std::get_if<TextObject>(&objects.at(objectIndex).data);
        if (text == nullptr) {
            return std::nullopt;
        }
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(nodeIndex);
            if (skippedObjectIds_.contains(node.id)) return std::nullopt;
            const std::string base = objectPointer(objectIndex);
            const double pointSize = numberValue(
                *model_, evaluate(text->pointSize, base + "/pointsize", node.id),
                base + "/pointsize", "Text point size"
            );
            if (!std::isfinite(pointSize) || pointSize <= 0.0) {
                frameError(*model_, SceneModelErrorCode::invalidValue,
                           base + "/pointsize",
                           "Text point size must be finite and greater than zero");
            }
            const double alpha = numberValue(
                *model_, evaluate(text->alpha, base + "/alpha", node.id),
                base + "/alpha", "Text alpha"
            );
            if (!std::isfinite(alpha)) {
                frameError(*model_, SceneModelErrorCode::invalidValue,
                           base + "/alpha", "Text alpha must be finite");
            }
            const FrameVector2 spacing = vector2Value(
                *model_, evaluate(text->spacing, base + "/spacing", node.id),
                base + "/spacing", "Text spacing"
            );
            const double maxWidth = numberValue(
                *model_, evaluate(text->maxWidth, base + "/maxwidth", node.id),
                base + "/maxwidth", "Text maximum width"
            );
            const double blurSize = numberValue(
                *model_, evaluate(text->blurSize, base + "/blursize", node.id),
                base + "/blursize", "Text blur size"
            );
            const FrameColor dropShadowColor = colorValue(
                *model_,
                evaluate(
                    text->dropShadowColor,
                    base + "/dropshadowcolor",
                    node.id
                ),
                base + "/dropshadowcolor"
            );
            const FrameVector2 dropShadowOffset = vector2Value(
                *model_,
                evaluate(
                    text->dropShadowOffset,
                    base + "/dropshadowoffset",
                    node.id
                ),
                base + "/dropshadowoffset",
                "Text drop-shadow offset"
            );
            const double dropShadowOpacity = numberValue(
                *model_,
                evaluate(
                    text->dropShadowOpacity,
                    base + "/dropshadowopacity",
                    node.id
                ),
                base + "/dropshadowopacity",
                "Text drop-shadow opacity"
            );
            const double dropShadowSize = numberValue(
                *model_,
                evaluate(
                    text->dropShadowSize,
                    base + "/dropshadowsize",
                    node.id
                ),
                base + "/dropshadowsize",
                "Text drop-shadow size"
            );
            const FrameColor outlineColor = colorValue(
                *model_,
                evaluate(
                    text->outlineColor,
                    base + "/outlinecolor",
                    node.id
                ),
                base + "/outlinecolor"
            );
            const double outlineThickness = numberValue(
                *model_,
                evaluate(
                    text->outlineThickness,
                    base + "/outlinethickness",
                    node.id
                ),
                base + "/outlinethickness",
                "Text outline thickness"
            );
            const auto validateTextEffectScalar = [&] (
                double value,
                std::string_view name
            ) {
                if (std::isfinite(value) && value >= 0.0) return;
                frameError(
                    *model_, SceneModelErrorCode::invalidValue,
                    base + '/' + std::string(name),
                    "Text effect size and opacity must be finite and non-negative"
                );
            };
            validateTextEffectScalar(
                dropShadowOpacity, "dropshadowopacity"
            );
            validateTextEffectScalar(dropShadowSize, "dropshadowsize");
            validateTextEffectScalar(blurSize, "blursize");
            validateTextEffectScalar(
                outlineThickness, "outlinethickness"
            );
            if (text->limitRows && text->maxRows <= 0) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue,
                    base + "/maxrows",
                    "Text maximum rows must be greater than zero when row limiting is enabled"
                );
            }
            if (text->limitWidth &&
                (!std::isfinite(maxWidth) || maxWidth <= 0.0)) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue,
                    base + "/maxwidth",
                    "Text maximum width must be finite and greater than zero when width limiting is enabled"
                );
            }
            const std::size_t descriptorIndex = plan_.texts.size();
            plan_.texts.push_back({
                .objectIndex = objectIndex,
                .objectId = node.id,
                .visible = node.isVisible,
                .perspective = text->perspective,
                .text = textValue(text->text, base + "/text", node.id, "Text content"),
                .font = text->font,
                .pointSize = pointSize,
                .size = vector2Value(*model_, evaluate(text->size, base + "/size", node.id),
                                     base + "/size", "Text size"),
                .color = colorValue(*model_, evaluate(text->color, base + "/color", node.id),
                                    base + "/color"),
                .alpha = alpha,
                .padding = vector2Value(*model_, evaluate(text->padding, base + "/padding", node.id),
                                        base + "/padding", "Text padding"),
                .spacing = spacing,
                .worldTransform = node.worldTransform,
                .horizontalAlignment = text->horizontalAlignment,
                .verticalAlignment = text->verticalAlignment,
                .limitRows = text->limitRows,
                .limitUseEllipsis = text->limitUseEllipsis,
                .limitWidth = text->limitWidth,
                .maxRows = text->maxRows,
                .maxWidth = maxWidth,
                .msdf = text->msdf,
                .blur = text->blur,
                .blurSize = blurSize,
                .dropShadow = text->dropShadow,
                .dropShadowColor = dropShadowColor,
                .dropShadowOffset = dropShadowOffset,
                .dropShadowOpacity = dropShadowOpacity,
                .dropShadowSize = dropShadowSize,
                .outline = text->outline,
                .outlineColor = outlineColor,
                .outlineThickness = outlineThickness,
            });
            return descriptorIndex;
    }

    void scheduleTextEffects(
        std::size_t objectIndex,
        std::size_t nodeIndex,
        std::size_t textIndex
    ) {
        const auto& objects = model_->project().scene.objects;
        const auto* text = std::get_if<TextObject>(&objects.at(objectIndex).data);
        if (text == nullptr || text->effects.empty()) return;
        const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(nodeIndex);
        if (!node.isVisible) return;
        if (!text->effectSourceModel ||
            !text->effectSourceModel->material ||
            text->effectSourceModel->material->passes.empty()) {
            frameError(
                *model_,
                SceneModelErrorCode::assetFailure,
                objectPointer(objectIndex, "effects"),
                "Text effects require the official passthrough material"
            );
        }

        const FrameTextDescriptor& descriptor = plan_.texts.at(textIndex);
        const std::size_t firstFramebufferIndex = plan_.framebuffers.size();
        const double layerWidth = descriptor.size.x > 0.0
            ? descriptor.size.x
            : static_cast<double>(plan_.width);
        const double layerHeight = descriptor.size.y > 0.0
            ? descriptor.size.y
            : static_cast<double>(plan_.height);
        const std::uint32_t width = checkedDimension(
            *model_, layerWidth, objectPointer(objectIndex, "size"),
            "Text effect width"
        );
        const std::uint32_t height = checkedDimension(
            *model_, layerHeight, objectPointer(objectIndex, "size"),
            "Text effect height"
        );

        const std::string sourceLogicalName =
            "_rt_textLayerSource_" + std::to_string(node.id);
        const FramebufferDescriptor source = createFramebuffer(
            "object:" + std::to_string(node.id) + ":text-source",
            sourceLogicalName,
            FramebufferFormat::rgba8,
            width,
            height,
            1.0,
            true
        );
        const FrameResourceRef resourceA = imageCompositeResource(node.id, 'a');
        const FrameResourceRef resourceB = imageCompositeResource(node.id, 'b');
        sceneFramebuffers_.insert_or_assign(resourceA.logicalName, resourceA);
        sceneFramebuffers_.insert_or_assign(resourceB.logicalName, resourceB);
        const FramebufferDescriptor compositeA = createFramebuffer(
            resourceA.id,
            resourceA.logicalName,
            FramebufferFormat::rgba8,
            width,
            height,
            1.0,
            true
        );
        const FramebufferDescriptor compositeB = createFramebuffer(
            resourceB.id,
            resourceB.logicalName,
            FramebufferFormat::rgba8,
            width,
            height,
            1.0,
            true
        );

        const auto dynamicLiteral = [](RuntimeValue value) {
            DynamicValue result;
            result.value = std::move(value);
            return result;
        };
        const auto evaluatedLiteral = [](RuntimeValue value) {
            return EvaluatedValue{
                .value = std::move(value),
                .source = DynamicValueSource::literal,
            };
        };
        ImageObject effectImage;
        effectImage.model = text->effectSourceModel;
        effectImage.magentaCompositeTintMaterial =
            text->magentaCompositeTintMaterial;
        effectImage.alpha = dynamicLiteral(RuntimeValue::floating(1.0));
        effectImage.color = dynamicLiteral(
            RuntimeValue::color({1.0, 1.0, 1.0, 1.0})
        );
        effectImage.size = dynamicLiteral(RuntimeValue::vector(
            {layerWidth, layerHeight, 0.0, 0.0}, 2
        ));
        effectImage.parallaxDepth = dynamicLiteral(RuntimeValue::vector(
            {0.0, 0.0, 0.0, 0.0}, 2
        ));
        effectImage.brightness = dynamicLiteral(RuntimeValue::floating(1.0));
        effectImage.colorBlendMode = dynamicLiteral(RuntimeValue::integer(0));
        effectImage.horizontalAlignment = "center";
        effectImage.perspective = text->perspective;
        effectImage.effects = text->effects;

        const std::size_t imageIndex = plan_.images.size();
        plan_.images.push_back({
            .objectIndex = objectIndex,
            .objectId = node.id,
            .visible = true,
            .solid = false,
            .passthrough = false,
            .fullscreen = false,
            .perspective = text->perspective,
            .size = {layerWidth, layerHeight},
            .worldTransform = node.worldTransform,
            .source = source.resource,
            .compositeA = compositeA.resource,
            .compositeB = compositeB.resource,
            .puppetMesh = nullptr,
            .alpha = evaluatedLiteral(RuntimeValue::floating(1.0)),
            .color = evaluatedLiteral(
                RuntimeValue::color({1.0, 1.0, 1.0, 1.0})
            ),
            .brightness = evaluatedLiteral(RuntimeValue::floating(1.0)),
            .colorBlendMode = evaluatedLiteral(RuntimeValue::integer(0)),
            .parallaxDepth = evaluatedLiteral(RuntimeValue::vector(
                {0.0, 0.0, 0.0, 0.0}, 2
            )),
            .horizontalAlignment = "center",
        });
        plan_.operations.emplace_back(FrameClearCommand{
            .origin = {
                .imageIndex = imageIndex,
                .objectId = node.id,
            },
            .destination = source.resource,
            .color = {
                .red = 0.0,
                .green = 0.0,
                .blue = 0.0,
                .alpha = 0.0,
            },
        });
        plan_.operations.emplace_back(FrameTextCommand{
            .textIndex = textIndex,
            .objectId = node.id,
            .destination = source.resource,
            .localSpace = true,
        });
        scheduleImage(ImageContext{
            .planImageIndex = imageIndex,
            .objectIndex = objectIndex,
            .image = &effectImage,
            .node = &node,
            .currentMain = compositeA.resource,
            .currentSub = compositeB.resource,
        });

        std::map<std::string, FrameTextEffectFramebufferDescriptor>
            effectFramebufferSizing;
        for (std::size_t effectIndex = 0;
             effectIndex < text->effects.size(); ++effectIndex) {
            const ImageEffect& effectInstance = text->effects[effectIndex];
            if (!effectInstance.effect) {
                throw std::logic_error(
                    "Text effect definition disappeared after scheduling"
                );
            }
            const Effect& effect = *effectInstance.effect;
            std::map<std::string, std::size_t> finalDefinitionIndexes;
            for (std::size_t index = 0;
                 index < effect.framebuffers.size(); ++index) {
                finalDefinitionIndexes.insert_or_assign(
                    effect.framebuffers[index].name, index
                );
            }
            for (std::size_t index = 0;
                 index < effect.framebuffers.size(); ++index) {
                const FramebufferDefinition& definition =
                    effect.framebuffers[index];
                if (finalDefinitionIndexes.at(definition.name) != index) {
                    continue;
                }
                FrameTextEffectFramebufferDescriptor sizing;
                if (definition.width || definition.height) {
                    sizing.sizing = FrameTextEffectFramebufferSizing::fixed;
                } else if (definition.fit) {
                    sizing.sizing = FrameTextEffectFramebufferSizing::fit;
                    sizing.value = static_cast<double>(*definition.fit);
                } else {
                    sizing.sizing = FrameTextEffectFramebufferSizing::relative;
                    sizing.value = definition.scale;
                }
                const std::string id =
                    "object:" + std::to_string(node.id) + ":effect:" +
                    std::to_string(effectIndex) + ':' + definition.name;
                effectFramebufferSizing.insert_or_assign(
                    id, std::move(sizing)
                );
            }
        }

        FrameTextEffectDescriptor effect{
            .textIndex = textIndex,
            .imageIndex = imageIndex,
        };
        effect.framebuffers.reserve(
            plan_.framebuffers.size() - firstFramebufferIndex
        );
        for (std::size_t index = firstFramebufferIndex;
             index < plan_.framebuffers.size(); ++index) {
            FrameTextEffectFramebufferDescriptor sizing{
                .framebufferIndex = index,
            };
            if (index >= firstFramebufferIndex + 3) {
                const auto found = effectFramebufferSizing.find(
                    plan_.framebuffers[index].resource.id
                );
                if (found == effectFramebufferSizing.end()) {
                    throw std::logic_error(
                        "Text effect framebuffer has no dynamic sizing contract"
                    );
                }
                sizing = found->second;
                sizing.framebufferIndex = index;
            }
            effect.framebuffers.push_back(std::move(sizing));
        }
        plan_.textEffects.push_back(std::move(effect));
    }

    [[nodiscard]] particle::Vector3 concreteParticleVector(
        ParticleVector3 value
    ) const noexcept {
        return {.x = value.x, .y = value.y, .z = value.z};
    }

    [[nodiscard]] particle::Vector3 evaluatedParticleVector(
        const DynamicValue& value,
        std::string pointer,
        int objectId,
        std::string_view description
    ) {
        const Vector3 result = vector3Value(
            *model_, evaluate(value, pointer, objectId), pointer, description
        );
        return {.x = result.x, .y = result.y, .z = result.z};
    }

    [[nodiscard]] double evaluatedParticleNumber(
        const DynamicValue& value,
        std::string pointer,
        int objectId,
        std::string_view description
    ) {
        return numberValue(
            *model_, evaluate(value, pointer, objectId), pointer, description
        );
    }

    [[nodiscard]] int evaluatedParticleInteger(
        const DynamicValue& value,
        std::string pointer,
        int objectId,
        std::string_view description
    ) {
        const double result = evaluatedParticleNumber(
            value, pointer, objectId, description
        );
        if (!std::isfinite(result) || std::floor(result) != result ||
            result < static_cast<double>(std::numeric_limits<int>::min()) ||
            result > static_cast<double>(std::numeric_limits<int>::max())) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue, pointer,
                std::string(description) + " must be a finite integer"
            );
        }
        return static_cast<int>(result);
    }

    [[nodiscard]] bool evaluatedParticleBoolean(
        const DynamicValue& value,
        std::string pointer,
        int objectId,
        std::string_view description
    ) {
        return booleanValue(
            *model_, evaluate(value, pointer, objectId), pointer, description
        );
    }

    [[nodiscard]] static particle::Vector3 addParticleVectors(
        particle::Vector3 lhs,
        particle::Vector3 rhs
    ) noexcept {
        return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
    }

    [[nodiscard]] static particle::Vector3 rotateParticleX(
        particle::Vector3 value,
        double angle
    ) noexcept {
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        return {
            value.x,
            value.y * cosine - value.z * sine,
            value.y * sine + value.z * cosine,
        };
    }

    [[nodiscard]] static particle::Vector3 rotateParticleY(
        particle::Vector3 value,
        double angle
    ) noexcept {
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        return {
            value.x * cosine + value.z * sine,
            value.y,
            -value.x * sine + value.z * cosine,
        };
    }

    [[nodiscard]] static particle::Vector3 rotateParticleZ(
        particle::Vector3 value,
        double angle
    ) noexcept {
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        return {
            value.x * cosine - value.y * sine,
            value.x * sine + value.y * cosine,
            value.z,
        };
    }

    [[nodiscard]] particle::Vector3 particleLocalPoint(
        particle::Vector3 scenePoint,
        const ObjectTransform& transform,
        std::size_t objectIndex
    ) const {
        const auto validScale = [](double value) {
            return std::isfinite(value) && value != 0.0;
        };
        if (!validScale(transform.scale.x) || !validScale(transform.scale.y) ||
            !validScale(transform.scale.z)) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue,
                objectPointer(objectIndex, "scale"),
                "Pointer and world-space particle control points require finite non-zero object scale"
            );
        }

        particle::Vector3 value{
            scenePoint.x - (transform.origin.x - static_cast<double>(plan_.width) * 0.5),
            scenePoint.y - (static_cast<double>(plan_.height) * 0.5 - transform.origin.y),
            scenePoint.z - transform.origin.z,
        };
        // Inverse of T * Rz(-z) * Ry(y) * Rx(-x) * S, matching the
        // particle model matrix built by FramePlanExecutor.
        value = rotateParticleZ(value, transform.angles.z);
        value = rotateParticleY(value, -transform.angles.y);
        value = rotateParticleX(value, transform.angles.x);
        return {
            value.x / transform.scale.x,
            value.y / transform.scale.y,
            value.z / transform.scale.z,
        };
    }

    [[nodiscard]] particle::Vector3 authoredWorldParticlePoint(
        ParticleVector3 point
    ) const noexcept {
        // Particle definition world-space control points are authored in the
        // same centered scene coordinate system consumed by the particle
        // model matrix. Do not apply the image top-left conversion here.
        return concreteParticleVector(point);
    }

    [[nodiscard]] particle::ControlPoint concreteParticleControlPoint(
        const ParticleControlPoint& source,
        const ParticleInstanceOverride& instanceOverride,
        const SceneGraphNodeSnapshot& node,
        std::size_t objectIndex,
        std::size_t controlPointIndex,
        int objectId
    ) {
        const std::string pointer = objectPointer(objectIndex, "particle") +
            "/controlpoint/" + std::to_string(controlPointIndex);
        particle::Vector3 authoredOffset = concreteParticleVector(source.offset);
        if (const auto found = instanceOverride.controlPoints.find(source.id);
            found != instanceOverride.controlPoints.end()) {
            const std::string overridePointer =
                objectPointer(objectIndex, "instanceoverride") +
                "/controlpoint" + std::to_string(source.id);
            authoredOffset = evaluatedParticleVector(
                found->second,
                overridePointer,
                objectId,
                "Particle instance control point"
            );
        }
        particle::Vector3 position = authoredOffset;
        if (source.lockToPointer) {
            const particle::Vector3 pointerScene{
                inputs_.pointerX * static_cast<double>(plan_.width) -
                    static_cast<double>(plan_.width) * 0.5,
                inputs_.pointerY * static_cast<double>(plan_.height) -
                    static_cast<double>(plan_.height) * 0.5,
                0.0,
            };
            position = addParticleVectors(
                pointerScene,
                authoredOffset
            );
            position = particleLocalPoint(
                position, node.worldTransform, objectIndex
            );
        } else if ((source.flags & 2U) != 0U) {
            position = particleLocalPoint(
                authoredOffset,
                node.worldTransform,
                objectIndex
            );
        }
        return {
            .id = source.id,
            .position = position,
            .linkedToPointer = (source.flags & 1U) != 0U,
        };
    }

    [[nodiscard]] particle::ParticleInstanceOverrides concreteParticleOverrides(
        const ParticleInstanceOverride& source,
        std::size_t objectIndex,
        int objectId
    ) {
        const std::string pointer = objectPointer(objectIndex, "instanceoverride");
        const bool enabled = evaluatedParticleBoolean(
            source.enabled, pointer + "/enabled", objectId,
            "Particle instance override enabled"
        );
        particle::ParticleInstanceOverrides result{
            .enabled = enabled,
            .alpha = evaluatedParticleNumber(
                source.alpha, pointer + "/alpha", objectId,
                "Particle alpha override"
            ),
            .size = evaluatedParticleNumber(
                source.size, pointer + "/size", objectId,
                "Particle size override"
            ),
            .lifetime = evaluatedParticleNumber(
                source.lifetime, pointer + "/lifetime", objectId,
                "Particle lifetime override"
            ),
            .rate = evaluatedParticleNumber(
                source.rate, pointer + "/rate", objectId,
                "Particle rate override"
            ),
            .speed = evaluatedParticleNumber(
                source.speed, pointer + "/speed", objectId,
                "Particle speed override"
            ),
            .count = evaluatedParticleNumber(
                source.count, pointer + "/count", objectId,
                "Particle count override"
            ),
            .color = evaluatedParticleVector(
                source.color, pointer + "/color", objectId,
                "Particle color override"
            ),
            .colorMultiplier = evaluatedParticleVector(
                source.colorMultiplier, pointer + "/colorn", objectId,
                "Particle color multiplier override"
            ),
        };
        if (result.count < 0.0) {
            frameError(
                *model_, SceneModelErrorCode::invalidValue, pointer,
                "Particle count override must be non-negative"
            );
        }
        return result;
    }

    [[nodiscard]] particle::Emitter concreteParticleEmitter(
        const ParticleEmitter& emitter,
        std::size_t objectIndex,
        std::size_t emitterIndex
    ) const {
        const std::string pointer = objectPointer(objectIndex, "particle") +
            "/emitter/" + std::to_string(emitterIndex);
        return std::visit([&](const auto& source) -> particle::Emitter {
            const ParticleEmitterBase& sourceBase = source.base;
            particle::EmitterBase base{
                .directions = concreteParticleVector(sourceBase.directions),
                .distanceMin = concreteParticleVector(sourceBase.distanceMin),
                .distanceMax = concreteParticleVector(sourceBase.distanceMax),
                .origin = concreteParticleVector(sourceBase.origin),
                .instantaneous = sourceBase.instantaneous,
                .rate = sourceBase.rate,
                .controlPoint = sourceBase.controlPoint,
                .flags = sourceBase.flags,
                .delay = sourceBase.delay,
                .duration = sourceBase.duration,
                .minPeriodicDelay = sourceBase.minimumPeriodicDelay,
                .maxPeriodicDelay = sourceBase.maximumPeriodicDelay,
                .minPeriodicDuration = sourceBase.minimumPeriodicDuration,
                .maxPeriodicDuration = sourceBase.maximumPeriodicDuration,
                .maxToEmitPerPeriod = sourceBase.maximumToEmitPerPeriod,
            };
            if constexpr (std::is_same_v<
                              std::decay_t<decltype(source)>,
                              ParticleBoxRandomEmitter>) {
                return particle::BoxRandomEmitter{.base = std::move(base)};
            } else {
                particle::SphereRandomEmitter result{
                    .base = std::move(base),
                    .sign = concreteParticleVector(source.sign),
                    .speedMin = source.speedMin,
                    .speedMax = source.speedMax,
                };
                return result;
            }
        }, emitter);
    }

    [[nodiscard]] particle::Initializer concreteParticleInitializer(
        const ParticleInitializer& initializer,
        std::size_t objectIndex,
        std::size_t initializerIndex,
        int objectId
    ) {
        const std::string pointer = objectPointer(objectIndex, "particle") +
            "/initializer/" + std::to_string(initializerIndex);
        return std::visit([&](const auto& source) -> particle::Initializer {
            using Source = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<Source, ParticleLifetimeRandomInitializer>) {
                const double minimum = evaluatedParticleNumber(
                    source.minimum, pointer + "/min", objectId,
                    "Particle lifetime minimum"
                );
                const double maximum = evaluatedParticleNumber(
                    source.maximum, pointer + "/max", objectId,
                    "Particle lifetime maximum"
                );
                return particle::LifetimeRandomInitializer{minimum, maximum};
            } else if constexpr (std::is_same_v<Source, ParticleSizeRandomInitializer>) {
                const double minimum = evaluatedParticleNumber(
                    source.minimum, pointer + "/min", objectId,
                    "Particle size minimum"
                );
                const double maximum = evaluatedParticleNumber(
                    source.maximum, pointer + "/max", objectId,
                    "Particle size maximum"
                );
                const double exponent = evaluatedParticleNumber(
                    source.exponent, pointer + "/exponent", objectId,
                    "Particle size exponent"
                );
                return particle::SizeRandomInitializer{
                    minimum, maximum, exponent,
                };
            } else if constexpr (std::is_same_v<Source, ParticleColorRandomInitializer>) {
                const particle::Vector3 minimum = evaluatedParticleVector(
                    source.minimum, pointer + "/min", objectId,
                    "Particle color minimum"
                );
                const particle::Vector3 maximum = evaluatedParticleVector(
                    source.maximum, pointer + "/max", objectId,
                    "Particle color maximum"
                );
                return particle::ColorRandomInitializer{minimum, maximum};
            } else if constexpr (std::is_same_v<Source, ParticleAlphaRandomInitializer>) {
                const double minimum = evaluatedParticleNumber(
                    source.minimum, pointer + "/min", objectId,
                    "Particle alpha minimum"
                );
                const double maximum = evaluatedParticleNumber(
                    source.maximum, pointer + "/max", objectId,
                    "Particle alpha maximum"
                );
                return particle::AlphaRandomInitializer{minimum, maximum};
            } else if constexpr (std::is_same_v<Source, ParticleVelocityRandomInitializer>) {
                const particle::Vector3 minimum = evaluatedParticleVector(
                    source.minimum, pointer + "/min", objectId,
                    "Particle velocity minimum"
                );
                const particle::Vector3 maximum = evaluatedParticleVector(
                    source.maximum, pointer + "/max", objectId,
                    "Particle velocity maximum"
                );
                return particle::VelocityRandomInitializer{minimum, maximum};
            } else if constexpr (std::is_same_v<Source, ParticleRotationRandomInitializer>) {
                const particle::Vector3 minimum = evaluatedParticleVector(
                    source.minimum, pointer + "/min", objectId,
                    "Particle rotation minimum"
                );
                const particle::Vector3 maximum = evaluatedParticleVector(
                    source.maximum, pointer + "/max", objectId,
                    "Particle rotation maximum"
                );
                return particle::RotationRandomInitializer{minimum, maximum};
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleAngularVelocityRandomInitializer>) {
                const particle::Vector3 minimum = evaluatedParticleVector(
                    source.minimum, pointer + "/min", objectId,
                    "Particle angular velocity minimum"
                );
                const particle::Vector3 maximum = evaluatedParticleVector(
                    source.maximum, pointer + "/max", objectId,
                    "Particle angular velocity maximum"
                );
                const double exponent = evaluatedParticleNumber(
                    source.exponent, pointer + "/exponent", objectId,
                    "Particle angular velocity exponent"
                );
                return particle::AngularVelocityRandomInitializer{
                    .minimum = minimum,
                    .maximum = maximum,
                    .exponent = exponent,
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleTurbulentVelocityRandomInitializer>) {
                particle::TurbulentVelocityRandomInitializer result{
                    .speedMinimum = evaluatedParticleNumber(
                        source.speedMinimum, pointer + "/speedmin", objectId,
                        "Particle turbulent velocity minimum speed"
                    ),
                    .speedMaximum = evaluatedParticleNumber(
                        source.speedMaximum, pointer + "/speedmax", objectId,
                        "Particle turbulent velocity maximum speed"
                    ),
                    .scale = evaluatedParticleNumber(
                        source.scale, pointer + "/scale", objectId,
                        "Particle turbulent velocity scale"
                    ),
                    .offset = evaluatedParticleNumber(
                        source.offset, pointer + "/offset", objectId,
                        "Particle turbulent velocity offset"
                    ),
                    .forward = evaluatedParticleVector(
                        source.forward, pointer + "/forward", objectId,
                        "Particle turbulent velocity forward direction"
                    ),
                    .timeScale = evaluatedParticleNumber(
                        source.timeScale, pointer + "/timescale", objectId,
                        "Particle turbulent velocity time scale"
                    ),
                    .phaseMinimum = evaluatedParticleNumber(
                        source.phaseMinimum, pointer + "/phasemin", objectId,
                        "Particle turbulent velocity minimum phase"
                    ),
                    .phaseMaximum = evaluatedParticleNumber(
                        source.phaseMaximum, pointer + "/phasemax", objectId,
                        "Particle turbulent velocity maximum phase"
                    ),
                    .right = evaluatedParticleVector(
                        source.right, pointer + "/right", objectId,
                        "Particle turbulent velocity right direction"
                    ),
                };
                return result;
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleMapSequenceAroundControlPointInitializer>) {
                return particle::MapSequenceAroundControlPointInitializer{
                    .controlPoint = evaluatedParticleInteger(
                        source.controlPoint, pointer + "/controlpoint", objectId,
                        "Particle map-sequence control point"
                    ),
                    .count = evaluatedParticleInteger(
                        source.count, pointer + "/count", objectId,
                        "Particle map-sequence count"
                    ),
                    .speedMinimum = evaluatedParticleVector(
                        source.speedMinimum, pointer + "/speedmin", objectId,
                        "Particle map-sequence minimum speed"
                    ),
                    .speedMaximum = evaluatedParticleVector(
                        source.speedMaximum, pointer + "/speedmax", objectId,
                        "Particle map-sequence maximum speed"
                    ),
                };
            }
        }, initializer);
    }

    [[nodiscard]] particle::Operator concreteParticleOperator(
        const ParticleOperator& operation,
        std::size_t objectIndex,
        std::size_t operatorIndex,
        int objectId
    ) {
        const std::string pointer = objectPointer(objectIndex, "particle") +
            "/operator/" + std::to_string(operatorIndex);
        return std::visit([&](const auto& source) -> particle::Operator {
            using Source = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<Source, ParticleMovementOperator>) {
                const double drag = evaluatedParticleNumber(
                    source.drag, pointer + "/drag", objectId,
                    "Particle movement drag"
                );
                return particle::MovementOperator{
                    .drag = drag,
                    .gravity = evaluatedParticleVector(
                        source.gravity, pointer + "/gravity", objectId,
                        "Particle movement gravity"
                    ),
                };
            } else if constexpr (std::is_same_v<Source, ParticleAlphaFadeOperator>) {
                const double fadeIn = evaluatedParticleNumber(
                    source.fadeInTime, pointer + "/fadeintime", objectId,
                    "Particle alpha fade-in time"
                );
                const double fadeOut = evaluatedParticleNumber(
                    source.fadeOutTime, pointer + "/fadeouttime", objectId,
                    "Particle alpha fade-out time"
                );
                return particle::AlphaFadeOperator{
                    .fadeInTime = fadeIn,
                    .fadeOutTime = fadeOut,
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleAngularMovementOperator>) {
                const double drag = evaluatedParticleNumber(
                    source.drag, pointer + "/drag", objectId,
                    "Particle angular movement drag"
                );
                return particle::AngularMovementOperator{
                    .drag = drag,
                    .force = evaluatedParticleVector(
                        source.force, pointer + "/force", objectId,
                        "Particle angular movement force"
                    ),
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleOscillatePositionOperator>) {
                return particle::OscillatePositionOperator{
                    .frequencyMinimum = evaluatedParticleNumber(
                        source.frequencyMinimum, pointer + "/frequencymin",
                        objectId, "Particle position oscillator minimum frequency"
                    ),
                    .frequencyMaximum = evaluatedParticleNumber(
                        source.frequencyMaximum, pointer + "/frequencymax",
                        objectId, "Particle position oscillator maximum frequency"
                    ),
                    .scaleMinimum = evaluatedParticleNumber(
                        source.scaleMinimum, pointer + "/scalemin", objectId,
                        "Particle position oscillator minimum scale"
                    ),
                    .scaleMaximum = evaluatedParticleNumber(
                        source.scaleMaximum, pointer + "/scalemax", objectId,
                        "Particle position oscillator maximum scale"
                    ),
                    .phaseMinimum = evaluatedParticleNumber(
                        source.phaseMinimum, pointer + "/phasemin", objectId,
                        "Particle position oscillator minimum phase"
                    ),
                    .phaseMaximum = evaluatedParticleNumber(
                        source.phaseMaximum, pointer + "/phasemax", objectId,
                        "Particle position oscillator maximum phase"
                    ),
                    .mask = evaluatedParticleVector(
                        source.mask, pointer + "/mask", objectId,
                        "Particle position oscillator mask"
                    ),
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleOscillateAlphaOperator>) {
                return particle::OscillateAlphaOperator{
                    .frequencyMinimum = evaluatedParticleNumber(
                        source.frequencyMinimum, pointer + "/frequencymin",
                        objectId, "Particle alpha oscillator minimum frequency"
                    ),
                    .frequencyMaximum = evaluatedParticleNumber(
                        source.frequencyMaximum, pointer + "/frequencymax",
                        objectId, "Particle alpha oscillator maximum frequency"
                    ),
                    .scaleMinimum = evaluatedParticleNumber(
                        source.scaleMinimum, pointer + "/scalemin", objectId,
                        "Particle alpha oscillator minimum scale"
                    ),
                    .scaleMaximum = evaluatedParticleNumber(
                        source.scaleMaximum, pointer + "/scalemax", objectId,
                        "Particle alpha oscillator maximum scale"
                    ),
                    .phaseMinimum = evaluatedParticleNumber(
                        source.phaseMinimum, pointer + "/phasemin", objectId,
                        "Particle alpha oscillator minimum phase"
                    ),
                    .phaseMaximum = evaluatedParticleNumber(
                        source.phaseMaximum, pointer + "/phasemax", objectId,
                        "Particle alpha oscillator maximum phase"
                    ),
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleOscillateSizeOperator>) {
                return particle::OscillateSizeOperator{
                    .frequencyMinimum = evaluatedParticleNumber(
                        source.frequencyMinimum, pointer + "/frequencymin",
                        objectId, "Particle size oscillator minimum frequency"
                    ),
                    .frequencyMaximum = evaluatedParticleNumber(
                        source.frequencyMaximum, pointer + "/frequencymax",
                        objectId, "Particle size oscillator maximum frequency"
                    ),
                    .scaleMinimum = evaluatedParticleNumber(
                        source.scaleMinimum, pointer + "/scalemin", objectId,
                        "Particle size oscillator minimum scale"
                    ),
                    .scaleMaximum = evaluatedParticleNumber(
                        source.scaleMaximum, pointer + "/scalemax", objectId,
                        "Particle size oscillator maximum scale"
                    ),
                    .phaseMinimum = evaluatedParticleNumber(
                        source.phaseMinimum, pointer + "/phasemin", objectId,
                        "Particle size oscillator minimum phase"
                    ),
                    .phaseMaximum = evaluatedParticleNumber(
                        source.phaseMaximum, pointer + "/phasemax", objectId,
                        "Particle size oscillator maximum phase"
                    ),
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleControlPointAttractOperator>) {
                const double threshold = evaluatedParticleNumber(
                    source.threshold, pointer + "/threshold", objectId,
                    "Particle control-point attract threshold"
                );
                return particle::ControlPointAttractOperator{
                    .controlPoint = source.controlPoint,
                    .origin = evaluatedParticleVector(
                        source.origin, pointer + "/origin", objectId,
                        "Particle control-point attract origin"
                    ),
                    .scale = evaluatedParticleNumber(
                        source.scale, pointer + "/scale", objectId,
                        "Particle control-point attract scale"
                    ),
                    .threshold = threshold,
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleSizeChangeOperator>) {
                return particle::SizeChangeOperator{
                    .startTime = evaluatedParticleNumber(
                        source.startTime, pointer + "/starttime", objectId,
                        "Particle size-change start time"
                    ),
                    .endTime = evaluatedParticleNumber(
                        source.endTime, pointer + "/endtime", objectId,
                        "Particle size-change end time"
                    ),
                    .startValue = evaluatedParticleNumber(
                        source.startValue, pointer + "/startvalue", objectId,
                        "Particle size-change start value"
                    ),
                    .endValue = evaluatedParticleNumber(
                        source.endValue, pointer + "/endvalue", objectId,
                        "Particle size-change end value"
                    ),
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleAlphaChangeOperator>) {
                return particle::AlphaChangeOperator{
                    .startTime = evaluatedParticleNumber(
                        source.startTime, pointer + "/starttime", objectId,
                        "Particle alpha-change start time"
                    ),
                    .endTime = evaluatedParticleNumber(
                        source.endTime, pointer + "/endtime", objectId,
                        "Particle alpha-change end time"
                    ),
                    .startValue = evaluatedParticleNumber(
                        source.startValue, pointer + "/startvalue", objectId,
                        "Particle alpha-change start value"
                    ),
                    .endValue = evaluatedParticleNumber(
                        source.endValue, pointer + "/endvalue", objectId,
                        "Particle alpha-change end value"
                    ),
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleColorChangeOperator>) {
                return particle::ColorChangeOperator{
                    .startTime = evaluatedParticleNumber(
                        source.startTime, pointer + "/starttime", objectId,
                        "Particle color-change start time"
                    ),
                    .endTime = evaluatedParticleNumber(
                        source.endTime, pointer + "/endtime", objectId,
                        "Particle color-change end time"
                    ),
                    .startValue = evaluatedParticleVector(
                        source.startValue, pointer + "/startvalue", objectId,
                        "Particle color-change start value"
                    ),
                    .endValue = evaluatedParticleVector(
                        source.endValue, pointer + "/endvalue", objectId,
                        "Particle color-change end value"
                    ),
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleTurbulenceOperator>) {
                return particle::TurbulenceOperator{
                    .scale = evaluatedParticleNumber(
                        source.scale, pointer + "/scale", objectId,
                        "Particle turbulence scale"
                    ),
                    .speedMinimum = evaluatedParticleNumber(
                        source.speedMinimum, pointer + "/speedmin", objectId,
                        "Particle turbulence minimum speed"
                    ),
                    .speedMaximum = evaluatedParticleNumber(
                        source.speedMaximum, pointer + "/speedmax", objectId,
                        "Particle turbulence maximum speed"
                    ),
                    .timeScale = evaluatedParticleNumber(
                        source.timeScale, pointer + "/timescale", objectId,
                        "Particle turbulence time scale"
                    ),
                    .mask = evaluatedParticleVector(
                        source.mask, pointer + "/mask", objectId,
                        "Particle turbulence mask"
                    ),
                    .phaseMinimum = evaluatedParticleNumber(
                        source.phaseMinimum, pointer + "/phasemin", objectId,
                        "Particle turbulence minimum phase"
                    ),
                    .phaseMaximum = evaluatedParticleNumber(
                        source.phaseMaximum, pointer + "/phasemax", objectId,
                        "Particle turbulence maximum phase"
                    ),
                    .audioProcessingMode = evaluatedParticleInteger(
                        source.audioProcessingMode,
                        pointer + "/audioprocessingmode", objectId,
                        "Particle turbulence audio processing mode"
                    ),
                };
            } else if constexpr (std::is_same_v<
                                     Source,
                                     ParticleVortexOperator>) {
                return particle::VortexOperator{
                    .controlPoint = source.controlPoint,
                    .flags = source.flags,
                    .axis = evaluatedParticleVector(
                        source.axis, pointer + "/axis", objectId,
                        "Particle vortex axis"
                    ),
                    .offset = evaluatedParticleVector(
                        source.offset, pointer + "/offset", objectId,
                        "Particle vortex offset"
                    ),
                    .distanceInner = evaluatedParticleNumber(
                        source.distanceInner, pointer + "/distanceinner", objectId,
                        "Particle vortex inner distance"
                    ),
                    .distanceOuter = evaluatedParticleNumber(
                        source.distanceOuter, pointer + "/distanceouter", objectId,
                        "Particle vortex outer distance"
                    ),
                    .speedInner = evaluatedParticleNumber(
                        source.speedInner, pointer + "/speedinner", objectId,
                        "Particle vortex inner speed"
                    ),
                    .speedOuter = evaluatedParticleNumber(
                        source.speedOuter, pointer + "/speedouter", objectId,
                        "Particle vortex outer speed"
                    ),
                    .centerForce = evaluatedParticleNumber(
                        source.centerForce, pointer + "/centerforce", objectId,
                        "Particle vortex center force"
                    ),
                    .ringRadius = evaluatedParticleNumber(
                        source.ringRadius, pointer + "/ringradius", objectId,
                        "Particle vortex ring radius"
                    ),
                    .ringWidth = evaluatedParticleNumber(
                        source.ringWidth, pointer + "/ringwidth", objectId,
                        "Particle vortex ring width"
                    ),
                    .ringPullDistance = evaluatedParticleNumber(
                        source.ringPullDistance,
                        pointer + "/ringpulldistance", objectId,
                        "Particle vortex ring pull distance"
                    ),
                    .ringPullForce = evaluatedParticleNumber(
                        source.ringPullForce, pointer + "/ringpullforce", objectId,
                        "Particle vortex ring pull force"
                    ),
                    .audioProcessingMode = evaluatedParticleInteger(
                        source.audioProcessingMode,
                        pointer + "/audioprocessingmode", objectId,
                        "Particle vortex audio processing mode"
                    ),
                };
            }
        }, operation);
    }

    [[nodiscard]] std::pair<
        FrameResourceRef,
        std::map<int, FrameTextureBinding>
    > particleTextures(
        const MaterialPass& pass,
        std::size_t objectIndex
    ) const {
        const std::string pointer = objectPointer(objectIndex, "particle") +
            "/material/passes/0";
        // CParticle::detectTexture obtains the particle source from the
        // ordinary material texture map. User textures participate in the
        // pass provider chain, but may not replace this simulation/atlas
        // source.
        if (pass.textures.empty() || !pass.textures.front().name ||
            pass.textures.front().name->empty()) {
            frameError(
                *model_, SceneModelErrorCode::missingField,
                pointer + "/textures/0",
                "Particle material requires a texture in slot zero"
            );
        }
        FrameResourceRef primary = resolveTexture(
            *pass.textures.front().name,
            {},
            pointer + "/textures/0"
        );

        std::map<int, FrameTextureBinding> bindings;
        const auto append = [&bindings, &pointer, this](
            const TextureSlots& slots,
            FrameTextureCandidateSource source,
            bool userTexture
        ) {
            for (std::size_t index = 0; index < slots.size(); ++index) {
                const TextureSlot& slot = slots[index];
                if (!slot.name || slot.name->empty()) continue;
                FrameResourceRef resource = userTexture
                    ? resolveUserTexture(
                          slot,
                          {},
                          pointer + "/usertextures/" +
                              std::to_string(index)
                      )
                    : resolveTexture(
                          *slot.name,
                          {},
                          pointer + "/textures/" +
                              std::to_string(index)
                      );
                bindings[static_cast<int>(index)].candidates.push_back({
                    .source = source,
                    .resource = std::move(resource),
                });
            }
        };
        append(
            pass.textures,
            FrameTextureCandidateSource::materialTexture,
            false
        );
        append(
            pass.userTextures,
            FrameTextureCandidateSource::materialUserTexture,
            true
        );
        // Linux's CParticle adds a final `previous` bind for slot zero so the
        // particle's ordinary primary texture wins over shader defaults and
        // user providers while all other slots retain their full chain.
        bindings[0].candidates.push_back({
            .source = FrameTextureCandidateSource::bind,
            .resource = primary,
        });
        return {std::move(primary), std::move(bindings)};
    }

    [[nodiscard]] static bool particleTrailRenderer(
        FrameParticleRendererKind kind
    ) noexcept {
        return kind == FrameParticleRendererKind::spriteTrail ||
            kind == FrameParticleRendererKind::ropeTrail;
    }

    [[nodiscard]] FrameParticleRendererDescriptor particleRenderer(
        const ParticleSpriteRenderer& source,
        std::size_t objectIndex
    ) const {
        FrameParticleRendererDescriptor result;
        if (source.name == "sprite") {
            result.kind = FrameParticleRendererKind::sprite;
        } else if (source.name == "spritetrail") {
            result.kind = FrameParticleRendererKind::spriteTrail;
        } else if (source.name == "rope") {
            result.kind = FrameParticleRendererKind::rope;
        } else if (source.name == "ropetrail") {
            result.kind = FrameParticleRendererKind::ropeTrail;
        } else {
            frameError(
                *model_, SceneModelErrorCode::unsupportedObject,
                objectPointer(objectIndex, "particle") + "/renderer/0/name",
                "Unsupported particle renderer '" + source.name + "'"
            );
        }
        const auto finite = [&](double value, std::string_view field) {
            if (!std::isfinite(value)) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue,
                    objectPointer(objectIndex, "particle") +
                        "/renderer/0/" + std::string(field),
                    "Particle renderer field '" + std::string(field) +
                        "' must be finite"
                );
            }
            return value;
        };
        result.length = finite(source.length, "length");
        result.maxLength = finite(source.maxLength, "maxlength");
        result.minLength = finite(source.minLength, "minlength");
        result.subdivision = finite(source.subdivision, "subdivision");
        result.segments = finite(source.segments, "segments");
        result.uvScale = finite(source.uvScale, "uvscale");
        result.uvScrolling = source.uvScrolling;
        result.uvSmoothing = source.uvSmoothing;
        result.fadeAlpha = source.fadeAlpha;
        result.fadeSize = source.fadeSize;
        return result;
    }

    [[nodiscard]] ComboMap resolvedParticleCombos(
        const ComboMap& combos,
        FrameParticleRendererKind renderer
    ) const {
        ComboMap result = combos;
        result.insert_or_assign("GS_ENABLED", 0);
        result.insert_or_assign("SPRITESHEET", 0);
        result.insert_or_assign("THICKFORMAT", 1);
        result.insert_or_assign(
            "TRAILRENDERER",
            particleTrailRenderer(renderer) ? 1 : 0
        );
        return result;
    }

    [[nodiscard]] std::optional<std::size_t> createParticleDescriptor(
        std::size_t objectIndex,
        std::size_t nodeIndex
    ) {
        const auto& objects = model_->project().scene.objects;
        const auto* object = std::get_if<ParticleObject>(&objects.at(objectIndex).data);
        if (object == nullptr) {
            return std::nullopt;
        }
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(nodeIndex);
            if (skippedObjectIds_.contains(node.id)) return std::nullopt;
            const std::string pointer = objectPointer(objectIndex, "particle");
            if (!object->definition || !object->definition->material) {
                frameError(
                    *model_, SceneModelErrorCode::assetFailure, pointer,
                    "Particle object has no loaded definition and material"
                );
            }
            const ParticleDefinition& definition = *object->definition;
            const Material& material = *definition.material;
            if (material.passes.empty()) {
                frameError(
                    *model_, SceneModelErrorCode::unsupportedObject,
                    pointer + "/material/passes",
                    "Particle material must contain at least one pass"
                );
            }
            // CParticle::setupPass() in the pinned Linux renderer consumes the
            // first authored pass and ignores any later passes. Keep that
            // contract instead of rejecting an otherwise valid definition.
            const MaterialPass& pass = material.passes.front();
            const FrameParticleRendererDescriptor renderer = particleRenderer(
                definition.renderer, objectIndex
            );
            const bool ropeRenderer =
                renderer.kind == FrameParticleRendererKind::rope ||
                renderer.kind == FrameParticleRendererKind::ropeTrail;
            // Linux keeps the authored first-pass shader for sprite and
            // spritetrail renderers. Rope variants alone force the dedicated
            // genericropeparticle program.
            const std::string shader = ropeRenderer
                ? "genericropeparticle"
                : pass.shader;
            const ComboMap combos = resolvedParticleCombos(
                pass.combos, renderer.kind
            );
            auto [texture0, textures] = particleTextures(pass, objectIndex);
            if (texture0.kind != FrameResourceKind::assetTexture) {
                frameError(
                    *model_, SceneModelErrorCode::unsupportedObject,
                    pointer + "/material/passes/0/textures/0",
                    "Particle texture slot zero must reference an asset texture"
                );
            }
            const ShaderAssetPaths paths = materialShaderPaths(
                *model_, material, shader,
                pointer + "/material/passes/0"
            );
            const std::string materialObjectId =
                "layer:" + std::to_string(node.id) +
                "/particle/material/pass:0";
            const std::string constantPointerPrefix =
                pointer + "/material/passes/0/constants";
            registerMaterialObject(
                materialObjectId,
                material.assetPath,
                pass,
                nullptr,
                constantPointerPrefix
            );
            std::map<std::string, EvaluatedValue> constants;
            for (const auto& [name, value] : pass.constants) {
                constants.emplace(
                    name,
                    evaluate(
                        value,
                        constantPointerPrefix + '/' + name,
                        node.id,
                        script::ScriptPropertyOwner{
                            .layerId = node.id,
                            .type =
                                script::ScriptPropertyOwnerType::material,
                            .objectId = materialObjectId,
                            .property = name,
                        }
                    )
                );
            }
            const FrameVector2 parallaxDepth = vector2Value(
                *model_,
                evaluate(
                    object->parallaxDepth,
                    objectPointer(objectIndex, "parallaxDepth"),
                    node.id
                ),
                objectPointer(objectIndex, "parallaxDepth"),
                "Particle parallax depth"
            );

            particle::Configuration configuration;
            configuration.maxCount = definition.maxCount;
            configuration.fixedStepSeconds = 1.0 / 120.0;
            configuration.startTime = definition.startTime;
            configuration.flags = definition.flags;
            configuration.overrides = concreteParticleOverrides(
                object->instanceOverride, objectIndex, node.id
            );
            configuration.emitters.reserve(definition.emitters.size());
            for (std::size_t index = 0; index < definition.emitters.size(); ++index) {
                configuration.emitters.push_back(concreteParticleEmitter(
                    definition.emitters[index], objectIndex, index
                ));
            }
            configuration.initializers.reserve(definition.initializers.size());
            for (std::size_t index = 0; index < definition.initializers.size(); ++index) {
                configuration.initializers.push_back(concreteParticleInitializer(
                    definition.initializers[index], objectIndex, index, node.id
                ));
            }
            configuration.operators.reserve(definition.operators.size());
            for (std::size_t index = 0; index < definition.operators.size(); ++index) {
                configuration.operators.push_back(concreteParticleOperator(
                    definition.operators[index], objectIndex, index, node.id
                ));
            }
            std::map<int, particle::ControlPoint> controlPoints;
            for (std::size_t index = 0;
                 index < definition.controlPoints.size(); ++index) {
                particle::ControlPoint controlPoint =
                    concreteParticleControlPoint(
                        definition.controlPoints[index],
                        object->instanceOverride,
                        node,
                        objectIndex,
                        index,
                        node.id
                    );
                controlPoints.insert_or_assign(
                    controlPoint.id, std::move(controlPoint)
                );
            }
            configuration.controlPoints.reserve(controlPoints.size());
            for (auto& entry : controlPoints) {
                configuration.controlPoints.push_back(
                    std::move(entry.second)
                );
            }

            const std::size_t descriptorIndex = plan_.particles.size();
            plan_.particles.push_back({
                .objectIndex = objectIndex,
                .objectId = node.id,
                .visible = node.isVisible,
                .worldTransform = node.worldTransform,
                .definitionIdentity = definition.assetPath,
                .shader = shader,
                .vertexShaderPath = paths.vertex,
                .fragmentShaderPath = paths.fragment,
                .blending = pass.blending,
                .culling = pass.culling,
                .depthTest = pass.depthTest,
                .depthWrite = pass.depthWrite,
                .texture0 = texture0,
                .textures = textures,
                .combos = combos,
                .constants = std::move(constants),
                .parallaxDepth = parallaxDepth,
                .perspective = (definition.flags & 4U) != 0U,
                .animationMode = definition.animationMode,
                .sequenceMultiplier = definition.sequenceMultiplier,
                .renderer = renderer,
                .configuration = std::move(configuration),
            });
            return descriptorIndex;
    }

    void createSoundDescriptor(
        std::size_t objectIndex,
        std::size_t nodeIndex
    ) {
        const auto& objects = model_->project().scene.objects;
        const auto* sound = std::get_if<SoundObject>(&objects.at(objectIndex).data);
        if (sound == nullptr) {
            return;
        }
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(nodeIndex);
            if (skippedObjectIds_.contains(node.id)) return;
            const std::string base = objectPointer(objectIndex);
            const double volume = numberValue(
                *model_, evaluate(sound->volume, base + "/volume", node.id),
                base + "/volume", "Sound volume"
            );
            if (!std::isfinite(volume) || volume < 0.0 || volume > 1.0) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue, base + "/volume",
                    "Sound volume must be finite and in the range 0...1"
                );
            }
            FrameSoundPlaybackMode playbackMode;
            switch (sound->playbackMode) {
                case SoundPlaybackMode::loop:
                    playbackMode = FrameSoundPlaybackMode::loop;
                    break;
                case SoundPlaybackMode::random:
                    playbackMode = FrameSoundPlaybackMode::random;
                    break;
                case SoundPlaybackMode::single:
                    playbackMode = FrameSoundPlaybackMode::single;
                    break;
            }
            if (!std::isfinite(sound->minimumTime) ||
                !std::isfinite(sound->maximumTime) ||
                sound->minimumTime < 0.0 || sound->maximumTime < 0.0) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue, base,
                    "Sound timing bounds must be finite and non-negative"
                );
            }
            plan_.sounds.push_back({
                .objectIndex = objectIndex,
                .objectId = node.id,
                .visible = node.isVisible,
                .sources = sound->sounds,
                .playbackMode = playbackMode,
                .volume = volume,
                .startSilent = sound->startSilent,
                .muteInEditor = sound->muteInEditor,
                .minimumTime = sound->minimumTime,
                .maximumTime = sound->maximumTime,
            });
    }

    [[nodiscard]] FramebufferMap effectFramebuffers(
        const ImageContext& imageContext,
        std::size_t effectIndex,
        const Effect& effect
    ) {
        FramebufferMap result = imageContext.localFramebuffers;
        const FrameImageDescriptor& image = plan_.images.at(imageContext.planImageIndex);
        std::map<std::string, std::size_t> finalDefinitionIndexes;
        for (std::size_t index = 0; index < effect.framebuffers.size(); ++index) {
            finalDefinitionIndexes.insert_or_assign(
                effect.framebuffers[index].name, index
            );
        }
        for (std::size_t index = 0; index < effect.framebuffers.size(); ++index) {
            const FramebufferDefinition& definition = effect.framebuffers[index];
            if (finalDefinitionIndexes.at(definition.name) != index) {
                continue;
            }
            const auto [width, height] = framebufferDimensions(image, definition, imageContext.objectIndex, index);
            const std::string id = "object:" + std::to_string(image.objectId) +
                ":effect:" + std::to_string(effectIndex) + ":" + definition.name;
            const FramebufferDescriptor framebuffer = createFramebuffer(
                id,
                definition.name,
                framebufferFormat(
                    *model_, definition.format,
                    objectPointer(imageContext.objectIndex, "effects") + '/' +
                        std::to_string(effectIndex) + "/fbos/" + std::to_string(index) + "/format"
                ),
                width,
                height,
                definition.scale,
                definition.unique,
                framebufferWrapMode(
                    *model_, definition.uvs,
                    objectPointer(imageContext.objectIndex, "effects") + '/' +
                        std::to_string(effectIndex) + "/fbos/" +
                        std::to_string(index) + "/uvs"
                )
            );
            result.insert_or_assign(definition.name, framebuffer.resource);
        }
        return result;
    }

    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> framebufferDimensions(
        const FrameImageDescriptor& image,
        const FramebufferDefinition& definition,
        std::size_t objectIndex,
        std::size_t framebufferIndex
    ) const {
        const std::string pointer = objectPointer(objectIndex, "effects") + "/fbos/" +
            std::to_string(framebufferIndex);
        if (definition.width || definition.height) {
            if (!definition.width || !definition.height) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue, pointer,
                    "Framebuffer width and height must be provided together"
                );
            }
            return {
                checkedDimension(*model_, *definition.width, pointer + "/width", "Framebuffer width"),
                checkedDimension(*model_, *definition.height, pointer + "/height", "Framebuffer height"),
            };
        }
        if (definition.fit) {
            const double maximum = std::max(image.size.x, image.size.y);
            const double scale = static_cast<double>(*definition.fit) / maximum;
            return {
                checkedScaledFramebufferDimension(
                    *model_, image.size.x * scale, pointer + "/fit",
                    "Framebuffer width"
                ),
                checkedScaledFramebufferDimension(
                    *model_, image.size.y * scale, pointer + "/fit",
                    "Framebuffer height"
                ),
            };
        }
        return {
            checkedScaledFramebufferDimension(
                *model_, image.size.x / definition.scale, pointer + "/scale",
                "Framebuffer width"
            ),
            checkedScaledFramebufferDimension(
                *model_, image.size.y / definition.scale, pointer + "/scale",
                "Framebuffer height"
            ),
        };
    }

    [[nodiscard]] std::vector<PendingOperation> pendingOperations(
        const ImageContext& imageContext
    ) {
        const ImageObject& image = *imageContext.image;
        std::vector<PendingOperation> result;
        if (!image.model || !image.model->material) {
            frameError(
                *model_, SceneModelErrorCode::assetFailure,
                objectPointer(imageContext.objectIndex, "image"),
                "Image model has no material"
            );
        }
        for (std::size_t passIndex = 0;
             passIndex < image.model->material->passes.size(); ++passIndex) {
            const std::string materialPointer =
                objectPointer(imageContext.objectIndex, "image") +
                "/material/passes/" + std::to_string(passIndex);
            const std::string materialObjectId =
                "layer:" + std::to_string(imageContext.node->id) +
                "/image/material/pass:" + std::to_string(passIndex);
            MaterialPass material = effectiveBasePass(
                image.model->material->passes[passIndex], image, passIndex == 0
            );
            const ShaderAssetPaths paths = materialShaderPaths(
                *model_, *image.model->material, material.shader,
                materialPointer
            );
            result.emplace_back(PendingRender{
                .origin = {
                    .imageIndex = imageContext.planImageIndex,
                    .objectId = imageContext.node->id,
                    .materialPassIndex = passIndex,
                },
                .material = std::move(material),
                .vertexShaderPath = paths.vertex,
                .fragmentShaderPath = paths.fragment,
                .materialObjectId = materialObjectId,
                .constantPointerPrefix = materialPointer + "/constants",
                .localFramebuffers = imageContext.localFramebuffers,
            });
        }

        std::optional<Vector3> magentaCompositeTint;
        std::string magentaCompositePointer;
        for (std::size_t effectIndex = 0; effectIndex < image.effects.size(); ++effectIndex) {
            const ImageEffect& effectInstance = image.effects[effectIndex];
            const std::string effectPointer = objectPointer(imageContext.objectIndex, "effects") +
                '/' + std::to_string(effectIndex);
            if (!effectInstance.effect) {
                frameError(
                    *model_, SceneModelErrorCode::assetFailure, effectPointer,
                    "Image effect definition is missing"
                );
            }
            const Effect& effect = *effectInstance.effect;
            const std::string effectObjectId =
                "layer:" + std::to_string(imageContext.node->id) +
                "/effect:" + std::to_string(effectIndex);
            std::vector<std::string> effectMaterialIds;
            std::size_t descriptorOverrideIndex = 0;
            for (std::size_t passIndex = 0;
                 passIndex < effect.passes.size(); ++passIndex) {
                const EffectPass& effectPass = effect.passes[passIndex];
                if (!effectPass.material) continue;
                const EffectPassOverride* overridePass = nullptr;
                if (descriptorOverrideIndex <
                    effectInstance.passOverrides.size()) {
                    overridePass = &effectInstance.passOverrides[
                        descriptorOverrideIndex
                    ];
                }
                ++descriptorOverrideIndex;
                for (std::size_t materialPassIndex = 0;
                     materialPassIndex < effectPass.material->passes.size();
                     ++materialPassIndex) {
                    const std::string materialObjectId =
                        effectObjectId + "/pass:" +
                        std::to_string(passIndex) + "/material-pass:" +
                        std::to_string(materialPassIndex);
                    const std::string constantPointerPrefix =
                        effectPointer + "/passes/" +
                        std::to_string(passIndex) + "/material/passes/" +
                        std::to_string(materialPassIndex) + "/constants";
                    registerMaterialObject(
                        materialObjectId,
                        effectPass.material->assetPath,
                        effectPass.material->passes[materialPassIndex],
                        overridePass,
                        constantPointerPrefix
                    );
                    effectMaterialIds.push_back(materialObjectId);
                }
            }
            if (evaluationFrame_) {
                script::ScriptPropertyObjectDescriptor descriptor{
                    .id = effectObjectId,
                    .type = script::ScriptPropertyObjectType::effect,
                    .name = effectInstance.name.empty()
                        ? effect.name
                        : effectInstance.name,
                    .properties = {{
                        "visible",
                        evaluateDynamicValue(
                            *model_,
                            effectInstance.visible,
                            *graphSnapshot_.propertyValues,
                            effectPointer + "/visible"
                        ).value,
                    }},
                    .propertyAnimations = effectInstance.visible.animation
                        ? std::map<std::string, TimelineAnimation>{{
                            "visible", *effectInstance.visible.animation,
                        }}
                        : std::map<std::string, TimelineAnimation>{},
                    .materialIds = effectMaterialIds,
                };
                evaluationFrame_->registerScriptPropertyObject(
                    std::move(descriptor)
                );
            }
            script::ScriptPropertyOwner effectOwner{
                .layerId = imageContext.node->id,
                .type = script::ScriptPropertyOwnerType::effect,
                .objectId = effectObjectId,
                .property = "visible",
            };
            if (!booleanValue(
                    *model_,
                    evaluate(
                        effectInstance.visible,
                        effectPointer + "/visible",
                        imageContext.node->id,
                        std::move(effectOwner)
                    ),
                    effectPointer + "/visible", "Effect visibility"
                )) {
                continue;
            }
            if (!magentaCompositeTint) {
                for (std::size_t overrideIndex = 0;
                     overrideIndex < effectInstance.passOverrides.size();
                     ++overrideIndex) {
                    const EffectPassOverride& passOverride =
                        effectInstance.passOverrides[overrideIndex];
                    const auto composite = passOverride.combos.find("COMPOSITE");
                    if (composite == passOverride.combos.end() ||
                        composite->second != 2) {
                        continue;
                    }
                    const auto color =
                        passOverride.constants.find("compositecolor");
                    if (color == passOverride.constants.end()) {
                        continue;
                    }
                    const std::string colorPointer = effectPointer + "/passes/" +
                        std::to_string(overrideIndex) +
                        "/constantshadervalues/compositecolor";
                    const Vector3 candidate = vector3Value(
                        *model_,
                        evaluate(
                            color->second,
                            colorPointer,
                            imageContext.node->id
                        ),
                        colorPointer,
                        "Composite color"
                    );
                    if (candidate.x > 0.55F && candidate.y < 0.25F &&
                        candidate.z > 0.45F) {
                        magentaCompositeTint = candidate;
                        magentaCompositePointer = colorPointer;
                        break;
                    }
                }
            }
            const FramebufferMap localFramebuffers = effectFramebuffers(
                imageContext, effectIndex, effect
            );
            std::size_t overrideIndex = 0;
            for (std::size_t passIndex = 0; passIndex < effect.passes.size(); ++passIndex) {
                const EffectPass& effectPass = effect.passes[passIndex];
                const std::string passPointer = effectPointer + "/passes/" +
                    std::to_string(passIndex);
                const auto skipPass = [&](std::string message) {
                    addIssue(
                        FramePlanIssueCode::effectPassPlanningFailed,
                        imageContext.node->id,
                        passPointer,
                        std::move(message)
                    );
                };
                if (effectPass.compose) {
                    addIssue(
                        FramePlanIssueCode::composeUnavailable,
                        imageContext.node->id,
                        passPointer + "/compose",
                        "Effect compose is ignored and the authored pass is executed, matching the Linux runtime"
                    );
                }
                const std::optional<FrameResourceRef> target = effectPass.target
                    ? std::optional<FrameResourceRef>(resolveTexture(
                        *effectPass.target, localFramebuffers,
                        passPointer + "/target"
                    ))
                    : std::nullopt;
                if (target && target->kind != FrameResourceKind::framebuffer) {
                    skipPass("Effect target must resolve to a framebuffer");
                    if (effectPass.material) {
                        ++overrideIndex;
                    }
                    continue;
                }
                if (!effectPass.material && effectPass.command) {
                    if (!effectPass.source || !target) {
                        skipPass("Effect command requires both source and target");
                        continue;
                    }
                    const FrameResourceRef source = resolveTexture(
                        *effectPass.source, localFramebuffers,
                        passPointer + "/source"
                    );
                    FramePassOrigin origin{
                        .imageIndex = imageContext.planImageIndex,
                        .objectId = imageContext.node->id,
                        .effectIndex = effectIndex,
                        .effectPassIndex = passIndex,
                    };
                    if (*effectPass.command == EffectCommand::copy) {
                        result.emplace_back(PendingCopy{
                            .origin = std::move(origin), .source = source, .target = *target,
                        });
                    } else {
                        if (source.kind != FrameResourceKind::framebuffer) {
                            skipPass("Effect swap source must resolve to a framebuffer");
                            continue;
                        }
                        result.emplace_back(PendingSwap{
                            .origin = std::move(origin), .source = source, .target = *target,
                        });
                    }
                    continue;
                }
                if (!effectPass.material) {
                    skipPass("Effect pass has neither material nor command");
                    continue;
                }
                const EffectPassOverride* overridePass = nullptr;
                if (overrideIndex < effectInstance.passOverrides.size()) {
                    overridePass = &effectInstance.passOverrides[overrideIndex];
                }
                ++overrideIndex;
                std::map<int, std::string> binds;
                for (std::size_t bindIndex = 0;
                     bindIndex < effectPass.binds.size(); ++bindIndex) {
                    const EffectBind& bind = effectPass.binds[bindIndex];
                    if (bind.index < 0 ||
                        bind.index >= wallpaperTextureSlotCount) {
                        addIssue(
                            FramePlanIssueCode::effectPassPlanningFailed,
                            imageContext.node->id,
                            passPointer + "/bind/" +
                                std::to_string(bindIndex) + "/index",
                            "Effect bind index " +
                                std::to_string(bind.index) +
                                " is outside Wallpaper Engine texture slots 0...7 and was ignored",
                            FramePlanIssueSeverity::warning
                        );
                        continue;
                    }
                    binds.emplace(bind.index, bind.name);
                }
                for (std::size_t materialPassIndex = 0;
                     materialPassIndex < effectPass.material->passes.size();
                     ++materialPassIndex) {
                    const MaterialPass& material =
                        effectPass.material->passes[materialPassIndex];
                    const std::string materialPointer =
                        passPointer + "/material/passes/" +
                        std::to_string(materialPassIndex);
                    const std::string materialObjectId =
                        effectObjectId + "/pass:" +
                        std::to_string(passIndex) + "/material-pass:" +
                        std::to_string(materialPassIndex);
                    const ShaderAssetPaths paths = materialShaderPaths(
                        *model_, *effectPass.material, material.shader,
                        materialPointer
                    );
                    result.emplace_back(PendingRender{
                        .origin = {
                            .imageIndex = imageContext.planImageIndex,
                            .objectId = imageContext.node->id,
                            .effectIndex = effectIndex,
                            .effectPassIndex = passIndex,
                            .materialPassIndex = materialPassIndex,
                        },
                        .material = material,
                        .vertexShaderPath = paths.vertex,
                        .fragmentShaderPath = paths.fragment,
                        .materialObjectId = materialObjectId,
                        .constantPointerPrefix =
                            materialPointer + "/constants",
                        .overridePass = overridePass,
                        .binds = binds,
                        .localFramebuffers = localFramebuffers,
                        .target = target,
                    });
                }
            }
        }

        std::size_t compatibilityMaterialPassIndex =
            image.model->material->passes.size();
        if (magentaCompositeTint) {
            if (!image.magentaCompositeTintMaterial ||
                image.magentaCompositeTintMaterial->passes.empty()) {
                frameError(
                    *model_, SceneModelErrorCode::assetFailure,
                    magentaCompositePointer,
                    "Magenta COMPOSITE=2 requires materials/effects/tint.json"
                );
            }
            MaterialPass material =
                image.magentaCompositeTintMaterial->passes.front();
            material.combos.insert_or_assign("BLENDMODE", 30);
            DynamicValue color;
            color.value = RuntimeValue::vector(
                {
                    magentaCompositeTint->x,
                    magentaCompositeTint->y,
                    magentaCompositeTint->z,
                    0.0,
                },
                3
            );
            material.constants.insert_or_assign("color", std::move(color));
            DynamicValue alpha;
            alpha.value = RuntimeValue::floating(1.0);
            material.constants.insert_or_assign("alpha", std::move(alpha));
            const ShaderAssetPaths paths = materialShaderPaths(
                *model_, *image.magentaCompositeTintMaterial, material.shader,
                magentaCompositePointer
            );
            result.emplace_back(PendingRender{
                .origin = {
                    .imageIndex = imageContext.planImageIndex,
                    .objectId = imageContext.node->id,
                    .materialPassIndex = compatibilityMaterialPassIndex++,
                },
                .material = std::move(material),
                .vertexShaderPath = paths.vertex,
                .fragmentShaderPath = paths.fragment,
                .materialObjectId =
                    "layer:" + std::to_string(imageContext.node->id) +
                    "/compat/tint/pass:" +
                    std::to_string(compatibilityMaterialPassIndex - 1),
                .constantPointerPrefix =
                    magentaCompositePointer + "/compat-tint/constants",
            });
        }

        // CImage adds the utility passthrough as a final compatibility pass
        // when colorBlendMode is non-zero. The authored mode is a DynamicValue
        // and may change per frame, so resolve the combo from the frozen image
        // descriptor rather than from the loader's initial value.
        const FrameImageDescriptor& descriptor =
            plan_.images.at(imageContext.planImageIndex);
        const std::int64_t blendMode = descriptor.colorBlendMode.value.integer();
        if (blendMode > 0) {
            if (!image.colorBlendMaterial ||
                image.colorBlendMaterial->passes.empty()) {
                frameError(
                    *model_, SceneModelErrorCode::assetFailure,
                    objectPointer(imageContext.objectIndex, "colorBlendMode"),
                    "Non-zero colorBlendMode requires materials/util/effectpassthrough.json"
                );
            }
            MaterialPass material = image.colorBlendMaterial->passes.front();
            if (blendMode > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue,
                    objectPointer(imageContext.objectIndex, "colorBlendMode"),
                    "colorBlendMode exceeds the supported combo range"
                );
            }
            material.combos.insert_or_assign(
                "BLENDMODE", static_cast<int>(blendMode)
            );
            const ShaderAssetPaths paths = materialShaderPaths(
                *model_, *image.colorBlendMaterial, material.shader,
                objectPointer(imageContext.objectIndex, "colorBlendMode")
            );
            result.emplace_back(PendingRender{
                .origin = {
                    .imageIndex = imageContext.planImageIndex,
                    .objectId = imageContext.node->id,
                    .materialPassIndex = compatibilityMaterialPassIndex,
                },
                .material = std::move(material),
                .vertexShaderPath = paths.vertex,
                .fragmentShaderPath = paths.fragment,
                .materialObjectId =
                    "layer:" + std::to_string(imageContext.node->id) +
                    "/compat/color-blend/pass:" +
                    std::to_string(compatibilityMaterialPassIndex),
                .constantPointerPrefix =
                    objectPointer(imageContext.objectIndex, "colorBlendMode") +
                    "/compat-material/constants",
            });
        }
        return result;
    }

    [[nodiscard]] std::map<int, FrameTextureBinding> resolvePassTextures(
        const PendingRender& pending,
        const FrameResourceRef& input,
        const std::optional<FrameResourceRef>& previous,
        const FramebufferMap& localFramebuffers,
        std::string pointer
    ) const {
        std::map<int, FrameTextureBinding> result;
        const auto append = [&result, &localFramebuffers, &pointer, this](
            const TextureSlots& slots,
            FrameTextureCandidateSource source,
            bool userTexture
        ) {
            for (std::size_t index = 0; index < slots.size(); ++index) {
                const TextureSlot& slot = slots[index];
                if (!slot.name || slot.name->empty()) continue;
                FrameResourceRef resource = userTexture
                    ? resolveUserTexture(
                          slot,
                          localFramebuffers,
                          pointer + "/" + std::to_string(index)
                      )
                    : resolveTexture(
                          *slot.name,
                          localFramebuffers,
                          pointer + "/" + std::to_string(index)
                      );
                result[static_cast<int>(index)].candidates.push_back({
                    .source = source,
                    .resource = std::move(resource),
                });
            }
        };
        append(
            pending.material.textures,
            FrameTextureCandidateSource::materialTexture,
            false
        );
        append(
            pending.material.userTextures,
            FrameTextureCandidateSource::materialUserTexture,
            true
        );
        if (pending.overridePass != nullptr) {
            append(
                pending.overridePass->textures,
                FrameTextureCandidateSource::overrideTexture,
                false
            );
            append(
                pending.overridePass->userTextures,
                FrameTextureCandidateSource::overrideUserTexture,
                true
            );
        }
        for (const auto& [index, name] : pending.binds) {
            FrameResourceRef resource;
            if (name == "previous") {
                resource = previous.value_or(input);
            } else {
                resource = resolveTexture(
                    name, localFramebuffers,
                    pointer + "/bind/" + std::to_string(index)
                );
            }
            result[index].candidates.push_back({
                .source = FrameTextureCandidateSource::bind,
                .resource = std::move(resource),
            });
        }
        // Slot zero always has the renderable input as an explicit final
        // fallback, even when no authored/default candidates exist.
        result.try_emplace(0);
        return result;
    }

    [[nodiscard]] ComboMap resolveCombos(const PendingRender& pending) const {
        ComboMap result = pending.material.combos;
        if (pending.origin.imageIndex < plan_.images.size()) {
            const FrameImageDescriptor& image = plan_.images.at(
                pending.origin.imageIndex
            );
            // The official image parser selects this variant from the layer's
            // authored perspective flag, independently of the scene camera.
            result.insert_or_assign("SCENE_ORTHO", image.perspective ? 0 : 1);
        }
        if (pending.material.blending == BlendingMode::alphaToCoverage) {
            result.insert_or_assign("ALPHATOCOVERAGE", 1);
        }
        if (pending.overridePass != nullptr) {
            for (const auto& [name, value] : pending.overridePass->combos) {
                result.insert_or_assign(name, value);
            }
        }
        return result;
    }

    [[nodiscard]] std::map<std::string, EvaluatedValue> resolveConstants(
        const PendingRender& pending,
        int objectId
    ) {
        registerMaterialObject(
            pending.materialObjectId,
            pending.material.shader,
            pending.material,
            pending.overridePass,
            pending.constantPointerPrefix
        );
        std::map<std::string, EvaluatedValue> result;
        forEachMergedConstant(pending.material, pending.overridePass, [&] (
            const std::string& name,
            const DynamicValue& value
        ) {
            result.emplace(
                name,
                evaluate(
                    value,
                    pending.constantPointerPrefix + '/' + name,
                    objectId,
                    script::ScriptPropertyOwner{
                        .layerId = objectId,
                        .type = script::ScriptPropertyOwnerType::material,
                        .objectId = pending.materialObjectId,
                        .property = name,
                    }
                )
            );
        });
        return result;
    }

    void planImageObject(
        std::size_t objectIndex,
        std::size_t nodeIndex,
        int objectId
    ) {
        const PlanCheckpoint before = checkpoint();
        std::optional<std::size_t> retainedFramebufferCount;
        try {
            const std::optional<ImageContext> context = createImageContext(
                objectIndex, nodeIndex, retainedFramebufferCount
            );
            if (context && !skippedObjectIds_.contains(objectId)) {
                scheduleImage(*context);
            }
            if (skippedObjectIds_.contains(objectId)) {
                rollback(before, retainedFramebufferCount);
            }
        } catch (const std::bad_alloc&) {
            rollback(before, retainedFramebufferCount);
            throw;
        } catch (const std::exception& error) {
            rollback(before, retainedFramebufferCount);
            recordObjectPlanningFailure(objectIndex, objectId, error);
        }
    }

    void planRenderableObjects() {
        const auto& objects = model_->project().scene.objects;
        for (const std::size_t nodeIndex : graphSnapshot_.renderOrder) {
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(nodeIndex);
            const std::size_t objectIndex = node.objectIndex;
            const SceneObject& object = objects.at(objectIndex);
            if (std::holds_alternative<ImageObject>(object.data)) {
                planImageObject(objectIndex, nodeIndex, node.id);
                continue;
            }
            isolateObjectPlanning(
                objectIndex,
                node.id,
                [&] {
                    if (std::holds_alternative<TextObject>(object.data)) {
                        const std::optional<std::size_t> descriptor =
                            createTextDescriptor(objectIndex, nodeIndex);
                        if (descriptor &&
                            !skippedObjectIds_.contains(node.id) &&
                            plan_.texts.at(*descriptor).visible) {
                            const auto& text = std::get<TextObject>(object.data);
                            if (text.effects.empty()) {
                                plan_.operations.emplace_back(FrameTextCommand{
                                    .textIndex = *descriptor,
                                    .objectId = plan_.texts.at(*descriptor).objectId,
                                    .destination = plan_.output,
                                });
                            } else {
                                scheduleTextEffects(
                                    objectIndex, nodeIndex, *descriptor
                                );
                            }
                        }
                        return;
                    }
                    if (!std::holds_alternative<ParticleObject>(object.data)) {
                        return;
                    }
                    const std::optional<std::size_t> descriptor =
                        createParticleDescriptor(objectIndex, nodeIndex);
                    if (!descriptor ||
                        skippedObjectIds_.contains(node.id) ||
                        !plan_.particles.at(*descriptor).visible) {
                        return;
                    }
                    plan_.operations.emplace_back(FrameParticleCommand{
                        .particleIndex = *descriptor,
                        .objectId = plan_.particles.at(*descriptor).objectId,
                        .destination = plan_.output,
                    });
                }
            );
        }
    }

    // Shadow rendering is a graph concern, not a shader-name concern. The
    // authored image material supplies the real texture/alpha/morph bindings;
    // only the vertex/fragment pair and depth target change for the caster.
    // Build these passes after normal image scheduling so every caster can
    // clone a fully resolved base material operation.
    void planShadowCasters() {
        const FrameResourceRef atlas = sceneFramebuffers_.at(
            "_rt_shadowAtlas"
        );
        if (plan_.shadowAtlasResolution == 0 ||
            plan_.lightConfiguration.pointShadow == 0 &&
                plan_.lightConfiguration.spotShadow == 0 &&
                plan_.lightConfiguration.spotShadowCookie == 0 &&
                plan_.lightConfiguration.directionalShadow == 0) {
            return;
        }

        const auto featureRank = [](const FrameLightDescriptor& light) {
            switch (light.type) {
                case FrameLightType::point:
                    return light.castShadow ? 0 : 1;
                case FrameLightType::spot:
                    if (light.castShadow && light.useCookie) return 0;
                    if (light.useCookie) return 1;
                    if (light.castShadow) return 2;
                    return 3;
                case FrameLightType::directional:
                    return light.castShadow ? 0 : 1;
                case FrameLightType::tube:
                    return 0;
            }
            std::terminate();
        };
        std::vector<const FrameLightDescriptor*> shadowLights;
        for (const FrameLightDescriptor& light : plan_.lights) {
            if (!light.visible || !light.castShadow ||
                light.type == FrameLightType::tube) {
                continue;
            }
            shadowLights.push_back(&light);
        }
        std::stable_sort(
            shadowLights.begin(), shadowLights.end(),
            [&](const FrameLightDescriptor* lhs,
                const FrameLightDescriptor* rhs) {
                if (lhs->type != rhs->type) {
                    return static_cast<int>(lhs->type) <
                        static_cast<int>(rhs->type);
                }
                return featureRank(*lhs) < featureRank(*rhs);
            }
        );
        if (shadowLights.empty()) return;

        const auto lightProjections = [&] (
            const FrameLightDescriptor& light,
            const std::vector<const FrameShadowAtlasEntry*>& atlasEntries
        ) {
            struct Entry final {
                FrameMatrix matrix;
                FrameRenderRegion region;
            };
            std::vector<Entry> result;
            const double degreesToRadians =
                3.14159265358979323846264338327950288 / 180.0;
            const double radius = std::max(light.radius, 0.02);
            const double nearPlane = std::max(radius * 0.001, 0.01);
            const double farPlane = std::max(radius, nearPlane + 0.01);
            const FrameMatrix orientation = frameLightVolumeTransform(light);
            const Vector3 origin{
                light.worldTransform.origin.x,
                light.worldTransform.origin.y,
                light.worldTransform.origin.z,
            };
            const Vector3 forward{orientation[0], orientation[1], orientation[2]};
            const Vector3 up{orientation[4], orientation[5], orientation[6]};
            if (atlasEntries.empty()) {
                throw std::logic_error(
                    "Shadow light has no allocated atlas entry"
                );
            }
            const std::uint32_t size = atlasEntries.front()->size;
            if (light.type == FrameLightType::point) {
                const FrameMatrix projection = framePerspective(
                    90.0 * degreesToRadians, 1.0, farPlane, nearPlane
                );
                static constexpr std::array<Vector3, 6> directions{{
                    {1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0},
                    {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0},
                    {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0},
                }};
                static constexpr std::array<Vector3, 6> ups{{
                    {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
                    {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0},
                    {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
                }};
                const std::uint32_t cellWidth = size / 2U;
                const std::uint32_t cellHeight = size / 3U;
                for (std::size_t face = 0; face < directions.size(); ++face) {
                    const FrameCameraDescriptor camera{
                        .center = {
                            origin.x + directions[face].x,
                            origin.y + directions[face].y,
                            origin.z + directions[face].z,
                        },
                        .eye = origin,
                        .up = ups[face],
                    };
                    const std::uint32_t column = static_cast<std::uint32_t>(face % 2U);
                    const std::uint32_t row = static_cast<std::uint32_t>(face / 2U);
                    result.push_back({
                        .matrix = frameMatrixMultiply(
                            projection, frameLookAt(camera)
                        ),
                        .region = {
                            .x = atlasEntries.front()->x + column * cellWidth,
                            .y = atlasEntries.front()->y + row * cellHeight,
                            .width = cellWidth,
                            .height = cellHeight,
                        },
                    });
                }
                return result;
            }
            if (light.type == FrameLightType::spot) {
                const double fieldOfView = std::clamp(
                    light.outerCone * 2.0, 0.01, 179.0
                );
                const FrameCameraDescriptor camera{
                    .center = {
                        origin.x + forward.x,
                        origin.y + forward.y,
                        origin.z + forward.z,
                    },
                    .eye = origin,
                    .up = up,
                };
                FrameMatrix matrix = frameMatrixMultiply(
                        framePerspective(
                            fieldOfView * degreesToRadians,
                            1.0,
                            farPlane,
                            nearPlane
                        ),
                        frameLookAt(camera)
                    );
                matrix[14] -= 0.000500000024;
                result.push_back({
                    .matrix = matrix,
                    .region = {
                        .x = atlasEntries.front()->x,
                        .y = atlasEntries.front()->y,
                        .width = size,
                        .height = size,
                    },
                });
                return result;
            }

            const double defaultExtent = std::max(
                static_cast<double>(plan_.camera.orthogonalProjectionWidth),
                static_cast<double>(plan_.camera.orthogonalProjectionHeight)
            ) * 0.5;
            if (atlasEntries.size() != 3U) {
                throw std::logic_error(
                    "Directional shadow light must own three atlas entries"
                );
            }
            for (std::size_t cascade = 0; cascade < 3; ++cascade) {
                const double authoredDistance = light.cascadeDistances[cascade];
                const double extent = authoredDistance > 0.0
                    ? authoredDistance
                    : defaultExtent * static_cast<double>(cascade + 1) / 3.0;
                const Vector3 center{
                    plan_.camera.center.x,
                    plan_.camera.center.y,
                    plan_.camera.center.z,
                };
                const FrameCameraDescriptor camera{
                    .center = {center.x, center.y, center.z},
                    .eye = {
                        center.x - forward.x * extent * 2.0,
                        center.y - forward.y * extent * 2.0,
                        center.z - forward.z * extent * 2.0,
                    },
                    .up = up,
                };
                FrameMatrix matrix = frameMatrixMultiply(
                        frameOrthographic(
                            -extent, extent, -extent, extent,
                            -extent * 4.0, extent * 4.0
                        ),
                        frameLookAt(camera)
                    );
                matrix[14] -= 0.000500000024;
                result.push_back({
                    .matrix = matrix,
                    .region = {
                        .x = atlasEntries[cascade]->x,
                        .y = atlasEntries[cascade]->y,
                        .width = atlasEntries[cascade]->size,
                        .height = atlasEntries[cascade]->size,
                    },
                });
            }
            return result;
        };

        const auto& resolver = model_->runtime()->assetResolver();
        const auto casterName = [&](const FrameRenderPass& source) {
            std::string result = "shadowcaster";
            if (!resolver.contains(source.fragmentShaderPath)) {
                return result;
            }
            const std::string sourceText = resolver.readString(
                source.fragmentShaderPath
            );
            const std::string marker = "[PASS] shadow";
            const std::size_t markerPosition = sourceText.find(marker);
            if (markerPosition == std::string::npos) return result;
            std::size_t begin = markerPosition + marker.size();
            while (begin < sourceText.size() &&
                   std::isspace(static_cast<unsigned char>(sourceText[begin]))) {
                ++begin;
            }
            std::size_t end = begin;
            while (end < sourceText.size() &&
                   !std::isspace(static_cast<unsigned char>(sourceText[end]))) {
                ++end;
            }
            if (end > begin) result = sourceText.substr(begin, end - begin);
            return result;
        };
        const auto shaderPath = [&](std::string_view sourcePath,
                                    std::string_view shaderName,
                                    std::string_view extension) {
            const std::size_t slash = sourcePath.rfind('/');
            const std::string prefix = slash == std::string_view::npos
                ? std::string()
                : std::string(sourcePath.substr(0, slash + 1));
            std::string candidate = prefix + std::string(shaderName) +
                std::string(extension);
            if (resolver.contains(candidate)) return candidate;
            candidate = "shaders/" + std::string(shaderName) +
                std::string(extension);
            if (resolver.contains(candidate)) return candidate;
            return std::string();
        };

        std::map<std::size_t, std::vector<FrameRenderPass>> casterOperations;
        for (const FrameLightDescriptor* light : shadowLights) {
            const std::size_t lightIndex = static_cast<std::size_t>(
                light - plan_.lights.data()
            );
            std::vector<const FrameShadowAtlasEntry*> atlasEntries;
            for (const FrameShadowAtlasEntry& entry :
                 plan_.shadowAtlasEntries) {
                if (entry.lightIndex == lightIndex) {
                    atlasEntries.push_back(&entry);
                }
            }
            std::ranges::sort(
                atlasEntries,
                [](const FrameShadowAtlasEntry* lhs,
                   const FrameShadowAtlasEntry* rhs) {
                    return lhs->cascade < rhs->cascade;
                }
            );
            const auto projections = lightProjections(*light, atlasEntries);
            for (std::size_t imageIndex = 0;
                 imageIndex < plan_.images.size(); ++imageIndex) {
                const FrameImageDescriptor& image = plan_.images[imageIndex];
                if (!image.visible || !image.castShadow) continue;
                const FrameRenderPass* sourcePass = nullptr;
                for (const FrameOperation& operation : plan_.operations) {
                    const auto* pass = std::get_if<FrameRenderPass>(&operation);
                    if (pass == nullptr || pass->origin.imageIndex != imageIndex ||
                        pass->origin.effectIndex || pass->origin.effectPassIndex) {
                        continue;
                    }
                    sourcePass = pass;
                    break;
                }
                if (sourcePass == nullptr) {
                    addIssue(
                        FramePlanIssueCode::objectPlanningFailed,
                        image.objectId,
                        objectPointer(image.objectIndex, "castshadow"),
                        "Image requests shadow casting but has no base material draw",
                        FramePlanIssueSeverity::warning
                    );
                    continue;
                }
                const std::string name = casterName(*sourcePass);
                const std::string vertex = shaderPath(
                    sourcePass->vertexShaderPath, name, ".vert"
                );
                const std::string fragment = shaderPath(
                    sourcePass->fragmentShaderPath, name, ".frag"
                );
                if (vertex.empty() || fragment.empty()) {
                    addIssue(
                        FramePlanIssueCode::objectPlanningFailed,
                        image.objectId,
                        objectPointer(image.objectIndex, "castshadow"),
                        "Shadow caster shader pair is unavailable: " + name,
                        FramePlanIssueSeverity::warning
                    );
                    continue;
                }
                for (const auto& projection : projections) {
                    FrameRenderPass caster = *sourcePass;
                    caster.origin.materialPassIndex =
                        std::numeric_limits<std::size_t>::max() -
                        casterOperations[imageIndex].size();
                    caster.shader = name;
                    caster.vertexShaderPath = vertex;
                    caster.fragmentShaderPath = fragment;
                    // The official shadow variant keeps alpha-to-coverage so
                    // foliage and other cutout materials still reject their
                    // transparent texels in the depth pass.
                    const bool alphaToCoverage = sourcePass->blending ==
                        BlendingMode::alphaToCoverage;
                    caster.blending = alphaToCoverage
                        ? BlendingMode::alphaToCoverage
                        : BlendingMode::normal;
                    caster.culling = CullingMode::disabled;
                    caster.depthTest = DepthMode::greater;
                    caster.depthWrite = DepthMode::greater;
                    caster.geometry = image.puppetMesh
                        ? FrameGeometryKind::puppetMesh
                        : FrameGeometryKind::imageScene;
                    caster.textureCoordinates = FrameTexCoordKind::image;
                    caster.input = image.source;
                    caster.previousInput.reset();
                    caster.destination = atlas;
                    // SceneObjectParsers constructs shadow_material from the
                    // first two texture slots and only the skinning/morph
                    // combos. Carrying the full authored material here makes
                    // unrelated active uniforms part of a depth-only shader
                    // contract and is not equivalent to the official path.
                    std::map<int, FrameTextureBinding> shadowTextures;
                    for (const int slot : {0, 1}) {
                        const auto texture = sourcePass->textures.find(slot);
                        if (texture != sourcePass->textures.end()) {
                            shadowTextures.emplace(slot, texture->second);
                        }
                    }
                    caster.textures = std::move(shadowTextures);
                    static constexpr std::array<std::string_view, 4>
                        shadowComboNames{
                            "SKINNING", "MORPHING", "MORPHING_NORMALS",
                            "BONECOUNT",
                        };
                    ComboMap shadowCombos;
                    for (const std::string_view name : shadowComboNames) {
                        const auto combo = sourcePass->combos.find(
                            std::string(name)
                        );
                        if (combo != sourcePass->combos.end()) {
                            shadowCombos.emplace(combo->first, combo->second);
                        }
                    }
                    if (alphaToCoverage) {
                        shadowCombos.insert_or_assign(
                            "ALPHATOCOVERAGE", 1
                        );
                    }
                    caster.combos = std::move(shadowCombos);
                    caster.renderVariables = {};
                    caster.viewport = projection.region;
                    caster.scissor = projection.region;
                    caster.instanceCount = 1;
                    caster.depthBias = 0.0;
                    caster.depthSlopeScale = -4.0;
                    caster.depthBiasClamp = 0.0;
                    caster.matrixOverrides = {};
                    caster.matrixOverrides.viewportViewProjections.push_back(
                        projection.matrix
                    );
                    caster.shadowCaster = true;
                    caster.shadowSourceVertexShaderPath =
                        sourcePass->vertexShaderPath;
                    caster.shadowSourceFragmentShaderPath =
                        sourcePass->fragmentShaderPath;
                    caster.shadowSourceCombos = sourcePass->combos;
                    caster.writeAlpha = false;
                    caster.writeColor = false;
                    casterOperations[imageIndex].push_back(std::move(caster));
                }
            }
        }
        if (casterOperations.empty()) return;

        const int clearObjectId = std::numeric_limits<int>::min() + 1;
        const std::size_t clearImageIndex = plan_.images.size();
        plan_.images.push_back({
            .objectIndex = model_->project().scene.objects.size(),
            .objectId = clearObjectId,
            .visible = true,
            .source = atlas,
            .compositeA = atlas,
            .compositeB = atlas,
            .size = {
                static_cast<double>(plan_.shadowAtlasWidth),
                static_cast<double>(plan_.shadowAtlasHeight),
            },
        });
        std::vector<FrameOperation> rewritten;
        rewritten.reserve(plan_.operations.size() + casterOperations.size() * 2U);
        rewritten.emplace_back(FrameClearCommand{
            .origin = {
                .imageIndex = clearImageIndex,
                .objectId = clearObjectId,
            },
            .destination = atlas,
            .color = {
                .red = 1.0, .green = 1.0, .blue = 1.0, .alpha = 1.0,
            },
            .clearDepth = true,
            .depthValue = 0.0,
        });
        std::set<std::size_t> emitted;
        for (const FrameOperation& operation : plan_.operations) {
            std::optional<std::size_t> imageIndex;
            if (const auto* pass = std::get_if<FrameRenderPass>(&operation)) {
                imageIndex = pass->origin.imageIndex;
            } else if (const auto* copy = std::get_if<FrameCopyCommand>(&operation)) {
                imageIndex = copy->origin.imageIndex;
            } else if (const auto* swap = std::get_if<FrameSwapCommand>(&operation)) {
                imageIndex = swap->origin.imageIndex;
            } else if (const auto* clear = std::get_if<FrameClearCommand>(&operation)) {
                imageIndex = clear->origin.imageIndex;
            }
            if (imageIndex && casterOperations.contains(*imageIndex) &&
                emitted.emplace(*imageIndex).second) {
                for (FrameRenderPass& caster : casterOperations.at(*imageIndex)) {
                    rewritten.emplace_back(std::move(caster));
                }
            }
            rewritten.push_back(operation);
        }
        plan_.operations = std::move(rewritten);
    }

    void planVolumetricObjects() {
        const FrameResourceRef back = sceneFramebuffers_.at(
            "_rt_volumetricsBack"
        );
        const FrameResourceRef single = sceneFramebuffers_.at(
            "_rt_volumetricsSingle"
        );
        const FrameResourceRef lightBuffer = sceneFramebuffers_.at(
            "_rt_volumetricsLightBuffer"
        );

        std::vector<const FrameLightDescriptor*> lights;
        for (const FrameLightDescriptor& light : plan_.lights) {
            if (!light.visible || !light.castVolumetrics) continue;
            if (light.type != FrameLightType::point &&
                light.type != FrameLightType::spot) {
                addIssue(
                    FramePlanIssueCode::objectPlanningFailed,
                    light.objectId,
                    "/scene/objects/" + std::to_string(light.objectIndex),
                    "Volumetric rendering currently supports only point and spot lights",
                    FramePlanIssueSeverity::frameFatal
                );
                continue;
            }
            if (!std::isfinite(light.radius) || light.radius <= 0.0) {
                addIssue(
                    FramePlanIssueCode::objectPlanningFailed,
                    light.objectId,
                    "/scene/objects/" + std::to_string(light.objectIndex) +
                        "/light/radius",
                    "Volumetric light radius must be greater than zero",
                    FramePlanIssueSeverity::frameFatal
                );
                continue;
            }
            if (light.type == FrameLightType::spot &&
                (!std::isfinite(light.outerCone) ||
                 light.outerCone <= 0.0)) {
                addIssue(
                    FramePlanIssueCode::objectPlanningFailed,
                    light.objectId,
                    "/scene/objects/" + std::to_string(light.objectIndex) +
                        "/light/coneangle",
                    "Volumetric spot outer cone must be greater than zero",
                    FramePlanIssueSeverity::frameFatal
                );
                continue;
            }
            lights.push_back(&light);
        }
        if (lights.empty()) return;

        FrameMatrix sceneViewProjection;
        FrameMatrix sceneViewProjectionInverse;
        try {
            sceneViewProjection = frameSceneViewProjection(
                plan_.camera,
                static_cast<double>(plan_.camera.orthogonalProjectionWidth),
                static_cast<double>(plan_.camera.orthogonalProjectionHeight)
            );
            sceneViewProjectionInverse = frameMatrixInverse(
                sceneViewProjection
            );
        } catch (const std::invalid_argument& error) {
            frameError(
                *model_,
                SceneModelErrorCode::invalidValue,
                "/camera",
                "Volumetric world reconstruction requires a valid scene "
                "camera: " + std::string(error.what())
            );
        }

        // Clear commands carry an origin for diagnostics. Reserve one
        // synthetic image record before emitting those commands so the origin
        // remains valid even in a scene containing only lights.
        const std::size_t clearImageIndex = plan_.images.size();
        plan_.images.push_back({
            .objectIndex = lights.front()->objectIndex,
            .objectId = lights.front()->objectId,
            .visible = true,
            .size = {
                static_cast<double>(plan_.width),
                static_cast<double>(plan_.height),
            },
            .worldTransform = lights.front()->worldTransform,
            .source = single,
            .compositeA = lightBuffer,
            .compositeB = lightBuffer,
        });

        const FramePassOrigin clearOrigin{
            .imageIndex = clearImageIndex,
            .objectId = lights.front()->objectId,
            .materialPassIndex = 0,
        };
        const FrameColor farDepth{
            .red = 1.0,
            .green = 1.0,
            .blue = 1.0,
            .alpha = 1.0,
        };
        plan_.operations.emplace_back(FrameClearCommand{
            .origin = clearOrigin,
            .destination = single,
            .color = farDepth,
        });
        plan_.operations.emplace_back(FrameClearCommand{
            .origin = clearOrigin,
            .destination = lightBuffer,
            .color = {
                .red = 0.0,
                .green = 0.0,
                .blue = 0.0,
                .alpha = 0.0,
            },
        });

        const auto renderVariable = [](const FrameLightDescriptor& light) {
            constexpr double degreesToRadians =
                3.14159265358979323846264338327950288 / 180.0;
            const double radius = light.radius;
            const double inner = std::cos(
                light.innerCone * degreesToRadians
            );
            const double outer = std::cos(
                light.outerCone * degreesToRadians
            );
            const std::array<double, 3> origin = {
                light.worldTransform.origin.x,
                light.worldTransform.origin.y,
                light.worldTransform.origin.z,
            };
            const std::array<double, 3> color = {
                light.color.red,
                light.color.green,
                light.color.blue,
            };
            const Vector3 direction = frameLightForward(light);
            const double nearPlane = std::max(light.radius * 0.001, 0.01);
            const double farPlane = std::max(light.radius, nearPlane + 0.01);
            // Volumetric point-light depth is sampled with the same reverse-Z
            // comparison as the shadow atlas.  The point projection matrix
            // therefore uses the shadow caster's (far, near) argument order:
            // near radius maps to depth 1 and the light radius maps to 0.
            const double projectionZ = nearPlane /
                (farPlane - nearPlane);
            const double projectionW = (nearPlane * farPlane) /
                (farPlane - nearPlane);
            const FrameRenderVariablePayload directionOrProjection{
                light.type == FrameLightType::point
                    ? std::array<double, 4>{
                          projectionZ, projectionW, -1.0, 0.0,
                      }
                    : std::array<double, 4>{
                          direction.x, direction.y, direction.z, 0.0,
                      },
            };
            return std::array<FrameRenderVariablePayload, 5>{
                FrameRenderVariablePayload{{0.0, 0.0, 1.0, 1.0}},
                FrameRenderVariablePayload{{
                    radius, inner, outer, light.intensity,
                }},
                FrameRenderVariablePayload{{
                    origin[0], origin[1], origin[2], light.density,
                }},
                directionOrProjection,
                FrameRenderVariablePayload{{
                    color[0], color[1], color[2], light.volumetricsExponent,
                }},
            };
        };

        for (const FrameLightDescriptor* light : lights) {
            const std::size_t lightIndex = static_cast<std::size_t>(
                light - plan_.lights.data()
            );
            const std::size_t imageIndex = plan_.images.size();
            const FramePassOrigin backOrigin{
                .imageIndex = imageIndex,
                .objectId = light->objectId,
                .materialPassIndex = 0,
            };
            const FramePassOrigin frontOrigin{
                .imageIndex = imageIndex,
                .objectId = light->objectId,
                .materialPassIndex = 1,
            };
            plan_.images.push_back({
                .objectIndex = light->objectIndex,
                .objectId = light->objectId,
                .visible = true,
                .size = {
                    static_cast<double>(plan_.width),
                    static_cast<double>(plan_.height),
                },
                .worldTransform = light->worldTransform,
                .source = single,
                .compositeA = lightBuffer,
                .compositeB = lightBuffer,
                .alpha = EvaluatedValue{
                    .value = RuntimeValue::floating(1.0),
                    .source = DynamicValueSource::literal,
                },
                .color = EvaluatedValue{
                    .value = RuntimeValue::color({1.0, 1.0, 1.0, 1.0}),
                    .source = DynamicValueSource::literal,
                },
                .brightness = EvaluatedValue{
                    .value = RuntimeValue::floating(1.0),
                    .source = DynamicValueSource::literal,
                },
                .colorBlendMode = EvaluatedValue{
                    .value = RuntimeValue::integer(0),
                    .source = DynamicValueSource::literal,
                },
                .parallaxDepth = EvaluatedValue{
                    .value = RuntimeValue::vector({0.0, 0.0, 0.0, 0.0}, 2),
                    .source = DynamicValueSource::literal,
                },
                .horizontalAlignment = "center",
            });

            const FrameMatrix lightTransform = frameLightVolumeTransform(
                *light
            );
            std::optional<FrameMatrix> spotProjection;
            if (light->type == FrameLightType::spot) {
                const double degreesToRadians =
                    3.14159265358979323846264338327950288 / 180.0;
                const double radius = std::max(light->radius, 0.02);
                const double nearPlane = std::max(radius * 0.001, 0.01);
                const double farPlane = std::max(radius, nearPlane + 0.01);
                const FrameMatrix orientation = lightTransform;
                const Vector3 origin{
                    light->worldTransform.origin.x,
                    light->worldTransform.origin.y,
                    light->worldTransform.origin.z,
                };
                const Vector3 forward{
                    orientation[0], orientation[1], orientation[2],
                };
                const Vector3 up{
                    orientation[4], orientation[5], orientation[6],
                };
                const FrameCameraDescriptor camera{
                    .center = {
                        origin.x + forward.x,
                        origin.y + forward.y,
                        origin.z + forward.z,
                    },
                    .eye = origin,
                    .up = up,
                };
                spotProjection = frameMatrixMultiply(
                    framePerspective(
                        std::clamp(light->outerCone * 2.0, 0.01, 179.0) *
                            degreesToRadians,
                        1.0,
                        farPlane,
                        nearPlane
                    ),
                    frameLookAt(camera)
                );
                spotProjection->at(14) -= 0.000500000024;
            }
            plan_.operations.emplace_back(FrameClearCommand{
                .origin = backOrigin,
                .destination = back,
                .color = farDepth,
                .clearDepth = true,
            });
            plan_.operations.emplace_back(FrameRenderPass{
                .origin = backOrigin,
                .shader = "volumetricsback",
                .vertexShaderPath = "shaders/volumetricsback.vert",
                .fragmentShaderPath = "shaders/volumetricsback.frag",
                .blending = BlendingMode::normal,
                .culling = CullingMode::normal,
                .depthTest = DepthMode::enabled,
                .depthWrite = DepthMode::enabled,
                .geometry = FrameGeometryKind::lightVolume,
                .textureCoordinates = FrameTexCoordKind::full,
                .input = single,
                .destination = back,
                .matrixOverrides = {
                    .viewProjection = sceneViewProjection,
                    .alternateViewProjection = lightTransform,
                },
                .lightIndex = lightIndex,
            });

            const bool cameraInside = frameCameraInsideLightVolume(
                plan_.camera, *light
            );
            auto variables = renderVariable(*light);
            const auto atlas = sceneFramebuffers_.at("_rt_shadowAtlas");
            const auto atlasEntry = std::ranges::find_if(
                plan_.shadowAtlasEntries,
                [lightIndex](const FrameShadowAtlasEntry& entry) {
                    return entry.lightIndex == lightIndex && entry.cascade == 0;
                }
            );
            const bool shadow = light->castShadow;
            const bool cookie = light->useCookie;
            if (shadow && atlasEntry == plan_.shadowAtlasEntries.end()) {
                addIssue(
                    FramePlanIssueCode::objectPlanningFailed,
                    light->objectId,
                    objectPointer(light->objectIndex, "castshadow"),
                    "Volumetric shadow light has no shadow atlas entry",
                    FramePlanIssueSeverity::frameFatal
                );
                continue;
            }
            if (shadow) {
                const double atlasWidth = static_cast<double>(
                    std::max<std::uint32_t>(1U, plan_.shadowAtlasWidth)
                );
                const double atlasHeight = static_cast<double>(
                    std::max<std::uint32_t>(1U, plan_.shadowAtlasHeight)
                );
                variables[0] = FrameRenderVariablePayload{{
                    static_cast<double>(atlasEntry->x) / atlasWidth,
                    static_cast<double>(atlasEntry->y) / atlasHeight,
                    static_cast<double>(atlasEntry->size) / atlasWidth,
                    static_cast<double>(atlasEntry->size) / atlasHeight,
                }};
            }
            ComboMap combos{
                {"FULLSCREEN", cameraInside ? 1 : 0},
                {"POINTLIGHT", light->type == FrameLightType::point ? 1 : 0},
                {"QUALITY", static_cast<int>(renderQuality_)},
                {"SHADOW", shadow ? 1 : 0},
                {"COOKIE", cookie ? 1 : 0},
            };
            std::map<int, FrameTextureBinding> volumetricTextures{
                {
                    1,
                    FrameTextureBinding{.candidates = {{
                        .source = FrameTextureCandidateSource::bind,
                        .resource = back,
                    }}, .sampleDepth = true},
                },
                {
                    3,
                    FrameTextureBinding{.candidates = {{
                        .source = FrameTextureCandidateSource::bind,
                        .resource = single,
                    }}},
                },
            };
            if (shadow) {
                volumetricTextures.emplace(
                    0,
                    FrameTextureBinding{.candidates = {{
                        .source = FrameTextureCandidateSource::bind,
                        .resource = atlas,
                    }}, .sampleDepth = true}
                );
            }
            if (cookie) {
                volumetricTextures.emplace(
                    2,
                    FrameTextureBinding{.candidates = {{
                        .source = FrameTextureCandidateSource::bind,
                        .resource = plan_.lightCookie,
                    }}}
                );
            }
            plan_.operations.emplace_back(FrameRenderPass{
                .origin = frontOrigin,
                .shader = "volumetricsfront",
                .vertexShaderPath = "shaders/volumetricsfront.vert",
                .fragmentShaderPath = "shaders/volumetricsfront.frag",
                .blending = BlendingMode::additive,
                .culling = cameraInside
                    ? CullingMode::disabled
                    : CullingMode::normal,
                .depthTest = DepthMode::disabled,
                .depthWrite = DepthMode::disabled,
                .geometry = cameraInside
                    ? FrameGeometryKind::fullscreenLocal
                    : FrameGeometryKind::lightVolume,
                .textureCoordinates = FrameTexCoordKind::full,
                .input = single,
                .destination = lightBuffer,
                .textures = std::move(volumetricTextures),
                .combos = std::move(combos),
                .renderVariables = {{
                    variables[0], variables[1], variables[2], variables[3],
                    variables[4],
                }},
                .matrixOverrides = {
                    .viewProjection = sceneViewProjection,
                    .effectModel = sceneViewProjectionInverse,
                    .alternateModel = spotProjection,
                    .alternateViewProjection = lightTransform,
                },
                .lightIndex = lightIndex,
            });
        }

        const std::size_t combineImageIndex = plan_.images.size() - 1;
        const bool lowQuality = static_cast<std::uint8_t>(renderQuality_) < 3;
        if (lowQuality) {
            const FrameResourceRef lightBufferB = sceneFramebuffers_.at(
                "_rt_volumetricsLightBufferB"
            );
            const FramePassOrigin blurHorizontalOrigin{
                .imageIndex = combineImageIndex,
                .objectId = lights.front()->objectId,
                .materialPassIndex = 2,
            };
            plan_.operations.emplace_back(FrameRenderPass{
                .origin = blurHorizontalOrigin,
                .shader = "blur_k3",
                .vertexShaderPath = "shaders/blur_k3.vert",
                .fragmentShaderPath = "shaders/blur_k3.frag",
                .blending = BlendingMode::normal,
                .culling = CullingMode::disabled,
                .depthTest = DepthMode::disabled,
                .depthWrite = DepthMode::disabled,
                .geometry = FrameGeometryKind::fullscreenLocal,
                .textureCoordinates = FrameTexCoordKind::full,
                .input = lightBuffer,
                .destination = lightBufferB,
                .textures = {{
                    0,
                    FrameTextureBinding{.candidates = {{
                        .source = FrameTextureCandidateSource::bind,
                        .resource = lightBuffer,
                    }}},
                }},
                .combos = {{"VERTICAL", 0}},
            });

            const FramePassOrigin blurVerticalOrigin{
                .imageIndex = combineImageIndex,
                .objectId = lights.front()->objectId,
                .materialPassIndex = 3,
            };
            plan_.operations.emplace_back(FrameRenderPass{
                .origin = blurVerticalOrigin,
                .shader = "blur_k3",
                .vertexShaderPath = "shaders/blur_k3.vert",
                .fragmentShaderPath = "shaders/blur_k3.frag",
                .blending = BlendingMode::normal,
                .culling = CullingMode::disabled,
                .depthTest = DepthMode::disabled,
                .depthWrite = DepthMode::disabled,
                .geometry = FrameGeometryKind::fullscreenLocal,
                .textureCoordinates = FrameTexCoordKind::full,
                .input = lightBufferB,
                .destination = lightBuffer,
                .textures = {{
                    0,
                    FrameTextureBinding{.candidates = {{
                        .source = FrameTextureCandidateSource::bind,
                        .resource = lightBufferB,
                    }}},
                }},
                .combos = {{"VERTICAL", 1}},
            });
        }

        const FramePassOrigin combineOrigin{
            .imageIndex = combineImageIndex,
            .objectId = lights.front()->objectId,
            .materialPassIndex = lowQuality ? 4U : 2U,
        };
        plan_.operations.emplace_back(FrameRenderPass{
            .origin = combineOrigin,
            .shader = "passthrough",
            .vertexShaderPath = "shaders/passthrough.vert",
            .fragmentShaderPath = "shaders/passthrough.frag",
            .blending = BlendingMode::additive,
            .culling = CullingMode::disabled,
            .depthTest = DepthMode::disabled,
            .depthWrite = DepthMode::disabled,
            .geometry = FrameGeometryKind::fullscreenLocal,
            .textureCoordinates = FrameTexCoordKind::full,
            .input = lightBuffer,
            .destination = plan_.output,
            .textures = {{
                0,
                FrameTextureBinding{.candidates = {{
                    .source = FrameTextureCandidateSource::bind,
                    .resource = lightBuffer,
                }}},
            }},
        });
    }

    void planBloomObject() {
        const Scene& scene = model_->project().scene;
        const auto bloom = scene.generalValues.find("bloom");
        if (bloom == scene.generalValues.end()) {
            frameError(
                *model_, SceneModelErrorCode::missingField,
                "/general/bloom", "Scene bloom value is required"
            );
        }
        const bool enabled = booleanValue(
            *model_, evaluate(bloom->second, "/general/bloom", std::nullopt),
            "/general/bloom", "Bloom enabled"
        );
        if (!enabled) {
            return;
        }
        if (!scene.bloomModel || !scene.bloomEffect) {
            frameError(
                *model_, SceneModelErrorCode::assetFailure,
                "/general/bloom",
                "Bloom is enabled but its Linux compatibility resources are unavailable"
            );
        }

        const PlanCheckpoint before = checkpoint();
        const std::size_t syntheticIndex = model_->project().scene.objects.size();
        try {
            const FrameResourceRef compositeA = imageCompositeResource(
                bloomRuntimeObjectId, 'a'
            );
            const FrameResourceRef compositeB = imageCompositeResource(
                bloomRuntimeObjectId, 'b'
            );
            sceneFramebuffers_.emplace(compositeA.logicalName, compositeA);
            sceneFramebuffers_.emplace(compositeB.logicalName, compositeB);
            const FramebufferDescriptor descriptorA = createFramebuffer(
                compositeA.id, compositeA.logicalName, FramebufferFormat::rgba8,
                plan_.width, plan_.height, 1.0, true
            );
            const FramebufferDescriptor descriptorB = createFramebuffer(
                compositeB.id, compositeB.logicalName, FramebufferFormat::rgba8,
                plan_.width, plan_.height, 1.0, true
            );

            const auto dynamicLiteral = [](RuntimeValue value) {
                DynamicValue result;
                result.value = std::move(value);
                return result;
            };
            const auto evaluatedLiteral = [](RuntimeValue value) {
                return EvaluatedValue{
                    .value = std::move(value),
                    .source = DynamicValueSource::literal,
                };
            };

            ImageObject bloomImage;
            bloomImage.model = scene.bloomModel;
            bloomImage.alpha = dynamicLiteral(RuntimeValue::floating(1.0));
            bloomImage.color = dynamicLiteral(
                RuntimeValue::color({1.0, 1.0, 1.0, 1.0})
            );
            bloomImage.size = dynamicLiteral(RuntimeValue::vector(
                {static_cast<double>(plan_.width), static_cast<double>(plan_.height), 0.0, 0.0},
                2
            ));
            bloomImage.parallaxDepth = dynamicLiteral(RuntimeValue::vector(
                {0.0, 0.0, 0.0, 0.0}, 2
            ));
            bloomImage.brightness = dynamicLiteral(RuntimeValue::floating(1.0));
            bloomImage.colorBlendMode = dynamicLiteral(RuntimeValue::integer(0));

            const auto strength = scene.generalValues.find("bloomstrength");
            const auto threshold = scene.generalValues.find("bloomthreshold");
            if (strength == scene.generalValues.end() ||
                threshold == scene.generalValues.end()) {
                frameError(
                    *model_, SceneModelErrorCode::missingField,
                    "/general", "Bloom strength and threshold are required"
                );
            }
            const EvaluatedValue strengthValue = evaluate(
                strength->second, "/general/bloomstrength", std::nullopt
            );
            const EvaluatedValue thresholdValue = evaluate(
                threshold->second, "/general/bloomthreshold", std::nullopt
            );
            const double strengthNumber = numberValue(
                *model_, strengthValue, "/general/bloomstrength",
                "Bloom strength"
            );
            const double thresholdNumber = numberValue(
                *model_, thresholdValue, "/general/bloomthreshold",
                "Bloom threshold"
            );
            const auto makeOverride = [
                &dynamicLiteral,
                strengthNumber,
                thresholdNumber
            ]() {
                EffectPassOverride result;
                result.id = bloomRuntimeObjectId;
                result.constants.emplace(
                    "bloomstrength",
                    dynamicLiteral(RuntimeValue::floating(strengthNumber))
                );
                result.constants.emplace(
                    "bloomthreshold",
                    dynamicLiteral(RuntimeValue::floating(thresholdNumber))
                );
                return result;
            };
            ImageEffect bloomEffect;
            bloomEffect.visible = dynamicLiteral(RuntimeValue::boolean(true));
            bloomEffect.effect = scene.bloomEffect;
            // The Linux synthetic object supplies exactly three overrides for
            // its four material passes. Strength/threshold configure the three
            // downsample/blur stages; the final combine pass intentionally has
            // no override.
            constexpr std::size_t linuxBloomOverrideCount = 3;
            bloomEffect.passOverrides.reserve(linuxBloomOverrideCount);
            for (std::size_t index = 0;
                 index < linuxBloomOverrideCount; ++index) {
                bloomEffect.passOverrides.push_back(makeOverride());
            }
            bloomImage.effects.push_back(std::move(bloomEffect));

            const double bloomOriginX = static_cast<double>(plan_.width / 2);
            const double bloomOriginY = static_cast<double>(plan_.height / 2);
            SceneGraphNodeSnapshot node;
            node.objectIndex = syntheticIndex;
            node.id = bloomRuntimeObjectId;
            node.origin = evaluatedLiteral(RuntimeValue::vector({
                bloomOriginX,
                bloomOriginY,
                0.0,
                0.0,
            }, 3));
            node.scale = evaluatedLiteral(RuntimeValue::vector({1.0, 1.0, 1.0, 0.0}, 3));
            node.angles = evaluatedLiteral(RuntimeValue::vector({0.0, 0.0, 0.0, 0.0}, 3));
            node.visible = evaluatedLiteral(RuntimeValue::boolean(true));
            node.localTransform = {
                .origin = {bloomOriginX, bloomOriginY, 0.0},
                .scale = {1.0, 1.0, 1.0},
                .angles = {0.0, 0.0, 0.0},
            };
            node.worldTransform = node.localTransform;
            node.isVisible = true;

            const std::size_t imageIndex = plan_.images.size();
            plan_.images.push_back({
                .objectIndex = syntheticIndex,
                .objectId = bloomRuntimeObjectId,
                .visible = true,
                .solid = false,
                .passthrough = false,
                .fullscreen = false,
                .size = {
                    static_cast<double>(plan_.width),
                    static_cast<double>(plan_.height),
                },
                .worldTransform = node.worldTransform,
                .source = plan_.output,
                .compositeA = descriptorA.resource,
                .compositeB = descriptorB.resource,
                .puppetMesh = nullptr,
                .alpha = evaluatedLiteral(RuntimeValue::floating(1.0)),
                .color = evaluatedLiteral(RuntimeValue::color({1.0, 1.0, 1.0, 1.0})),
                .brightness = evaluatedLiteral(RuntimeValue::floating(1.0)),
                .colorBlendMode = evaluatedLiteral(RuntimeValue::integer(0)),
                .parallaxDepth = evaluatedLiteral(RuntimeValue::vector({0.0, 0.0, 0.0, 0.0}, 2)),
                .horizontalAlignment = "center",
            });
            scheduleImage(ImageContext{
                .planImageIndex = imageIndex,
                .objectIndex = syntheticIndex,
                .image = &bloomImage,
                .node = &node,
                .currentMain = descriptorA.resource,
                .currentSub = descriptorB.resource,
                .localFramebuffers = {{
                    imageCompositeResource(
                        linuxBloomLegacyObjectId, 'a'
                    ).logicalName,
                    descriptorA.resource,
                }},
            });
        } catch (const std::bad_alloc&) {
            rollback(before);
            throw;
        } catch (const std::exception& error) {
            rollback(before);
            addIssue(
                FramePlanIssueCode::objectPlanningFailed,
                bloomRuntimeObjectId,
                "/general/bloom",
                std::string("Bloom post-process planning failed: ") + error.what(),
                FramePlanIssueSeverity::frameFatal
            );
        }
    }

    void planSoundObjects() {
        const auto& objects = model_->project().scene.objects;
        for (const std::size_t nodeIndex : graphSnapshot_.renderOrder) {
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(nodeIndex);
            const std::size_t objectIndex = node.objectIndex;
            const SceneObject& object = objects[objectIndex];
            if (!std::holds_alternative<SoundObject>(object.data)) {
                continue;
            }
            isolateObjectPlanning(
                objectIndex,
                node.id,
                [&] {
                    createSoundDescriptor(objectIndex, nodeIndex);
                }
            );
        }
    }

    void finalizeScriptLayerStates() {
        std::vector<int> evaluatedCursorLayers;
        const std::vector<int>* cursorLayers = cursorInteractiveLayerIds_;
        if (evaluationFrame_) {
            evaluatedCursorLayers = evaluationFrame_->cursorInteractiveLayerIds();
            cursorLayers = &evaluatedCursorLayers;
        }
        const auto textureAnimations = evaluationFrame_
            ? evaluationFrame_->textureAnimationSnapshots()
            : graphSnapshot_.textureAnimations;
        std::map<int, script::ScriptTextureAnimationSnapshot> animationsByLayer;
        for (const auto& animation : textureAnimations) {
            animationsByLayer.insert_or_assign(animation.layerId, animation);
        }
        for (FrameImageDescriptor& image : plan_.images) {
            if (evaluationFrame_) {
                if (const auto solid = evaluationFrame_->layerProperty(
                        image.objectId, "solid"
                    )) {
                    image.solid = solid->boolean();
                }
            }
            image.cursorInteractive = image.solid ||
                (cursorLayers != nullptr && std::binary_search(
                    cursorLayers->begin(), cursorLayers->end(), image.objectId
                ));
            const auto animation = animationsByLayer.find(image.objectId);
            if (animation == animationsByLayer.end()) continue;
            if (image.source.kind != FrameResourceKind::assetTexture ||
                image.source.id != animation->second.assetIdentity) {
                frameError(
                    *model_,
                    SceneModelErrorCode::invalidValue,
                    objectPointer(image.objectIndex, "image"),
                    "Texture animation controller asset does not match the image source"
                );
            }
            image.textureAnimation = FrameTextureAnimationOverride{
                .assetIdentity = animation->second.assetIdentity,
                .frame = animation->second.frame,
            };
        }

        const auto sounds = evaluationFrame_
            ? evaluationFrame_->soundSnapshots()
            : graphSnapshot_.sounds;
        std::map<int, script::ScriptSoundSnapshot> soundsByLayer;
        for (const auto& sound : sounds) {
            soundsByLayer.insert_or_assign(sound.layerId, sound);
        }
        for (FrameSoundDescriptor& sound : plan_.sounds) {
            const auto state = soundsByLayer.find(sound.objectId);
            if (state == soundsByLayer.end()) continue;
            if (!state->second.command) continue;
            FrameSoundPlaybackCommandAction action;
            switch (state->second.command->action) {
                case script::ScriptSoundCommandAction::play:
                    action = FrameSoundPlaybackCommandAction::play;
                    break;
                case script::ScriptSoundCommandAction::pause:
                    action = FrameSoundPlaybackCommandAction::pause;
                    break;
                case script::ScriptSoundCommandAction::stop:
                    action = FrameSoundPlaybackCommandAction::stop;
                    break;
            }
            sound.playbackCommand = FrameSoundPlaybackCommand{
                .action = action,
                .generation = state->second.command->generation,
            };
        }
    }

    void scheduleImage(ImageContext context) {
        FrameImageDescriptor& image = plan_.images.at(context.planImageIndex);
        const bool offscreenDependency =
            !image.visible && dependencyObjectIds_.contains(image.objectId);
        std::vector<PendingOperation> pending = pendingOperations(context);
        if (!image.visible && !offscreenDependency) {
            // SceneScript is a scene-state system, not a draw-call callback.
            // Material scripts on hidden layers still initialize/update shared
            // values that later layer scripts may consume in the same frame.
            for (const PendingOperation& operation : pending) {
                if (const auto* render = std::get_if<PendingRender>(&operation)) {
                    (void)resolveConstants(*render, context.node->id);
                }
            }
            return;
        }
        if (pending.empty()) {
            return;
        }
        const std::size_t basePassCount = context.image->model->material->passes.size();
        if (image.passthrough && !offscreenDependency &&
            pending.size() <= basePassCount) {
            // Passthrough layers are effect containers over the scene content
            // accumulated so far. With no visible effect they are a no-op;
            // replaying their base material would blend the background into
            // itself and may create a framebuffer feedback loop.
            return;
        }

        FrameResourceRef drawTo = context.currentMain;
        FrameResourceRef asInput = image.source;
        std::optional<FrameResourceRef> effectInput;
        bool inTargetEffectSequence = false;
        bool firstDraw = true;

        std::vector<std::size_t> imageRenderOperations;
        const auto isLastDraw = [&pending](std::size_t index) {
            for (std::size_t next = index + 1; next < pending.size(); ++next) {
                if (!std::holds_alternative<PendingSwap>(pending[next])) {
                    return false;
                }
            }
            return true;
        };

        for (std::size_t pendingIndex = 0; pendingIndex < pending.size(); ++pendingIndex) {
            if (const auto* render = std::get_if<PendingRender>(&pending[pendingIndex])) {
                const FrameResourceRef previousDrawTo = drawTo;
                const bool writesToTarget = render->target.has_value();
                if (writesToTarget) {
                    if (!inTargetEffectSequence) {
                        effectInput = asInput;
                        inTargetEffectSequence = true;
                    }
                    drawTo = *render->target;
                }
                const bool finalPass =
                    image.visible && !writesToTarget && isLastDraw(pendingIndex);
                const FrameResourceRef destination = finalPass ? plan_.output : drawTo;
                const std::optional<FrameResourceRef> previous =
                    inTargetEffectSequence ? effectInput : std::nullopt;
                FrameGeometryKind geometry = FrameGeometryKind::imageLocal;
                FrameTexCoordKind texcoords = FrameTexCoordKind::image;
                const bool firstPuppetDraw =
                    firstDraw && image.puppetMesh != nullptr;
                if (firstPuppetDraw) {
                    geometry = FrameGeometryKind::puppetMesh;
                } else if (firstDraw && image.passthrough) {
                    geometry = image.fullscreen
                        ? FrameGeometryKind::fullscreenLocal
                        : FrameGeometryKind::passthroughCapture;
                    texcoords = FrameTexCoordKind::full;
                } else if (!firstDraw) {
                    geometry = FrameGeometryKind::fullscreenLocal;
                    texcoords = FrameTexCoordKind::full;
                }
                // A one-pass Puppet image is both the first and final draw;
                // keep the indexed geometry in that case. Effects after the
                // first draw intentionally remain fullscreen quads.
                if (finalPass && !firstPuppetDraw) {
                    geometry = FrameGeometryKind::imageScene;
                }
                const std::string pointer = objectPointer(context.objectIndex, "effects");
                FrameRenderPass operation{
                    .origin = render->origin,
                    .shader = render->material.shader,
                    .vertexShaderPath = render->vertexShaderPath,
                    .fragmentShaderPath = render->fragmentShaderPath,
                    .blending = render->material.blending,
                    .culling = render->material.culling,
                    .depthTest = render->material.depthTest,
                    .depthWrite = render->material.depthWrite,
                    .geometry = geometry,
                    .textureCoordinates = texcoords,
                    .input = asInput,
                    .previousInput = previous,
                    .destination = destination,
                    .textures = resolvePassTextures(
                        *render, asInput, previous, render->localFramebuffers, pointer
                    ),
                    .combos = resolveCombos(*render),
                    .constants = resolveConstants(*render, context.node->id),
                    .writeAlpha = !finalPass,
                };
                if (firstPuppetDraw && destination != plan_.output) {
                    // Linux clears an intermediate target to transparent
                    // immediately before drawing the partial Puppet mesh.
                    // Composite framebuffers persist across frames here, so
                    // omitting this clear would leak stale pixels outside the
                    // indexed triangles.
                    plan_.operations.emplace_back(FrameClearCommand{
                        .origin = render->origin,
                        .destination = destination,
                        .color = {
                            .red = 0.0,
                            .green = 0.0,
                            .blue = 0.0,
                            .alpha = 0.0,
                        },
                    });
                }
                plan_.operations.emplace_back(std::move(operation));
                imageRenderOperations.push_back(plan_.operations.size() - 1);
                firstDraw = false;

                if (writesToTarget) {
                    asInput = drawTo;
                    drawTo = previousDrawTo;
                } else {
                    drawTo = context.currentSub;
                    asInput = context.currentMain;
                    std::swap(context.currentMain, context.currentSub);
                    inTargetEffectSequence = false;
                    effectInput.reset();
                }
            } else if (const auto* copy = std::get_if<PendingCopy>(&pending[pendingIndex])) {
                if (!inTargetEffectSequence) {
                    effectInput = asInput;
                    inTargetEffectSequence = true;
                }
                plan_.operations.emplace_back(FrameCopyCommand{
                    .origin = copy->origin,
                    .source = copy->source,
                    .destination = copy->target,
                });
                asInput = copy->target;
                firstDraw = false;
            } else {
                const PendingSwap& swap = std::get<PendingSwap>(pending[pendingIndex]);
                plan_.operations.emplace_back(FrameSwapCommand{
                    .origin = swap.origin,
                    .source = swap.source,
                    .destination = swap.target,
                });
            }
        }

        if (imageRenderOperations.size() > 1) {
            auto& first = std::get<FrameRenderPass>(
                plan_.operations.at(imageRenderOperations.front())
            );
            auto& last = std::get<FrameRenderPass>(
                plan_.operations.at(imageRenderOperations.back())
            );
            last.blending = first.blending;
            first.blending = BlendingMode::normal;
        }
        if (!imageRenderOperations.empty() && image.puppetMesh) {
            auto& first = std::get<FrameRenderPass>(
                plan_.operations.at(imageRenderOperations.front())
            );
            if (first.geometry == FrameGeometryKind::puppetMesh) {
                // Linux forces the indexed Puppet pass to translucent after
                // moving the authored base blend mode to the final pass.
                first.blending = BlendingMode::translucent;
            }
        }
    }

    [[nodiscard]] std::string operationPointer(
        const FramePassOrigin& origin
    ) const {
        if (origin.imageIndex >= plan_.images.size()) {
            return "/framePlan/operations";
        }
        const FrameImageDescriptor& image = plan_.images[origin.imageIndex];
        std::string pointer = objectPointer(image.objectIndex);
        if (origin.effectIndex) {
            pointer += "/effects/" + std::to_string(*origin.effectIndex);
            if (origin.effectPassIndex) {
                pointer += "/passes/" + std::to_string(*origin.effectPassIndex);
            }
        } else {
            pointer += "/image";
        }
        return pointer;
    }

    void validateFramePlan() {
        std::set<std::string> descriptorIds;
        for (std::size_t index = 0; index < plan_.framebuffers.size(); ++index) {
            const FramebufferDescriptor& descriptor = plan_.framebuffers[index];
            if (descriptor.resource.kind != FrameResourceKind::framebuffer ||
                descriptor.resource.id.empty()) {
                frameError(
                    *model_,
                    SceneModelErrorCode::invalidValue,
                    "/framePlan/framebuffers/" + std::to_string(index),
                    "Framebuffer descriptor has an invalid resource identity"
                );
            }
            if (!descriptorIds.emplace(descriptor.resource.id).second) {
                frameError(
                    *model_,
                    SceneModelErrorCode::duplicateId,
                    "/framePlan/framebuffers/" + std::to_string(index),
                    "Frame plan contains duplicate framebuffer descriptor '" +
                        descriptor.resource.id + "'"
                );
            }
        }

        std::set<std::string> missingDescriptorIds;
        const auto requireDescriptor = [&]<typename PointerFactory>(
            const FrameResourceRef& resource,
            PointerFactory&& pointer,
            std::optional<int> objectId,
            FramePlanIssueSeverity severity
        ) -> bool {
            if (resource.kind != FrameResourceKind::framebuffer ||
                descriptorIds.contains(resource.id)) {
                return true;
            }
            if (missingDescriptorIds.emplace(resource.id).second) {
                addIssue(
                    FramePlanIssueCode::framebufferDescriptorMissing,
                    objectId,
                    std::forward<PointerFactory>(pointer)(),
                    "Framebuffer resource '" + resource.id +
                        "' has no descriptor in the frame plan",
                    severity
                );
            }
            return false;
        };

        requireDescriptor(
            plan_.output,
            [] { return std::string("/framePlan/output"); },
            std::nullopt,
            FramePlanIssueSeverity::frameFatal
        );
        for (const FrameImageDescriptor& image : plan_.images) {
            std::optional<std::string> pointer;
            const auto imagePointer = [&]() -> const std::string& {
                if (!pointer) {
                    pointer = objectPointer(image.objectIndex, "image");
                }
                return *pointer;
            };
            requireDescriptor(
                image.source,
                [&] { return imagePointer() + "/source"; },
                image.objectId,
                FramePlanIssueSeverity::skipPass
            );
            requireDescriptor(
                image.compositeA,
                [&] { return imagePointer() + "/composite_a"; },
                image.objectId,
                FramePlanIssueSeverity::skipPass
            );
            requireDescriptor(
                image.compositeB,
                [&] { return imagePointer() + "/composite_b"; },
                image.objectId,
                FramePlanIssueSeverity::skipPass
            );
        }

        std::vector<FrameOperation> validOperations;
        validOperations.reserve(plan_.operations.size());
        for (std::size_t operationIndex = 0;
             operationIndex < plan_.operations.size(); ++operationIndex) {
            FrameOperation& operation = plan_.operations[operationIndex];
            const FramePassOrigin* origin = std::visit(
                [](const auto& value) -> const FramePassOrigin* {
                    using Operation = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Operation, FrameTextCommand> ||
                                  std::is_same_v<Operation, FrameParticleCommand>) {
                        return nullptr;
                    } else {
                        return &value.origin;
                    }
                }, operation);
            const auto* text = std::get_if<FrameTextCommand>(&operation);
            const auto* particle = std::get_if<FrameParticleCommand>(&operation);
            const std::optional<int> objectId = origin
                ? std::optional<int>(origin->objectId)
                : std::optional<int>(text ? text->objectId : particle->objectId);
            std::optional<std::string> basePointer;
            const auto operationBasePointer = [&]() -> const std::string& {
                if (!basePointer) {
                    std::string operationObjectPointer = origin
                        ? operationPointer(*origin)
                        : text
                            ? objectPointer(
                                plan_.texts.at(text->textIndex).objectIndex,
                                "text"
                            )
                            : objectPointer(
                                plan_.particles.at(particle->particleIndex).objectIndex,
                                "particle"
                            );
                    basePointer = std::move(operationObjectPointer) +
                        "/frameOperation/" + std::to_string(operationIndex);
                }
                return *basePointer;
            };
            std::set<std::string_view> operationReads;
            bool valid = true;
            const auto read = [&]<typename PointerFactory>(
                const FrameResourceRef& resource,
                PointerFactory&& pointer
            ) {
                if (resource.kind != FrameResourceKind::framebuffer) {
                    return;
                }
                if (!operationReads.emplace(resource.id).second) {
                    return;
                }
                const bool hasDescriptor = requireDescriptor(
                    resource,
                    std::forward<PointerFactory>(pointer),
                    objectId,
                    FramePlanIssueSeverity::skipPass
                );
                if (!hasDescriptor) {
                    valid = false;
                }
            };
            const auto write = [&]<typename PointerFactory>(
                const FrameResourceRef& resource,
                PointerFactory&& pointer
            ) {
                if (resource.kind != FrameResourceKind::framebuffer) {
                    return;
                }
                if (!requireDescriptor(
                        resource,
                        std::forward<PointerFactory>(pointer),
                        objectId,
                        FramePlanIssueSeverity::skipPass
                    )) {
                    valid = false;
                }
            };

            std::visit(
                [&](const auto& value) {
                    using Operation = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Operation, FrameRenderPass>) {
                        read(value.input, [&] {
                            return operationBasePointer() + "/input";
                        });
                        if (value.previousInput) {
                            read(*value.previousInput, [&] {
                                return operationBasePointer() + "/previousInput";
                            });
                        }
                        for (const auto& [slot, binding] : value.textures) {
                            for (std::size_t candidateIndex = 0;
                                 candidateIndex < binding.candidates.size();
                                 ++candidateIndex) {
                                read(
                                    binding.candidates[candidateIndex].resource,
                                    [&] {
                                        return operationBasePointer() + "/textures/" +
                                            std::to_string(slot) + "/candidates/" +
                                            std::to_string(candidateIndex);
                                    }
                                );
                            }
                        }
                        if (value.destination.kind == FrameResourceKind::framebuffer &&
                            operationReads.contains(value.destination.id)) {
                            addIssue(
                                FramePlanIssueCode::framebufferFeedbackLoop,
                                objectId,
                                operationBasePointer() + "/destination",
                                "Render operation samples framebuffer '" +
                                    value.destination.id +
                                    "' while writing to the same resource"
                            );
                            valid = false;
                        }
                        write(value.destination, [&] {
                            return operationBasePointer() + "/destination";
                        });
                    } else if constexpr (std::is_same_v<Operation, FrameCopyCommand>) {
                        read(value.source, [&] {
                            return operationBasePointer() + "/source";
                        });
                        if (value.source.kind == FrameResourceKind::framebuffer &&
                            value.destination.kind == FrameResourceKind::framebuffer &&
                            value.source.id == value.destination.id) {
                            addIssue(
                                FramePlanIssueCode::framebufferFeedbackLoop,
                                objectId,
                                operationBasePointer() + "/destination",
                                "Copy operation reads and writes framebuffer '" +
                                    value.destination.id + "'"
                            );
                            valid = false;
                        }
                        write(value.destination, [&] {
                            return operationBasePointer() + "/destination";
                        });
                    } else if constexpr (std::is_same_v<Operation, FrameSwapCommand>) {
                        read(value.source, [&] {
                            return operationBasePointer() + "/source";
                        });
                        read(value.destination, [&] {
                            return operationBasePointer() + "/destination";
                        });
                    } else if constexpr (std::is_same_v<Operation, FrameClearCommand>) {
                        write(value.destination, [&] {
                            return operationBasePointer() + "/destination";
                        });
                    } else if constexpr (std::is_same_v<Operation, FrameTextCommand>) {
                        write(value.destination, [&] {
                            return operationBasePointer() + "/destination";
                        });
                    } else if constexpr (std::is_same_v<Operation, FrameParticleCommand>) {
                        const FrameParticleDescriptor& descriptor =
                            plan_.particles.at(value.particleIndex);
                        const bool refract = [&] {
                            const auto combo = descriptor.combos.find("REFRACT");
                            return combo != descriptor.combos.end() &&
                                combo->second != 0;
                        }();
                        for (const auto& [slot, binding] : descriptor.textures) {
                            for (std::size_t candidateIndex = 0;
                                 candidateIndex < binding.candidates.size();
                                 ++candidateIndex) {
                                read(
                                    binding.candidates[candidateIndex].resource,
                                    [&] {
                                        return operationBasePointer() + "/textures/" +
                                            std::to_string(slot) +
                                            "/candidates/" +
                                            std::to_string(candidateIndex);
                                    }
                                );
                            }
                        }
                        if (value.destination.kind == FrameResourceKind::framebuffer &&
                            operationReads.contains(value.destination.id) &&
                            !refract) {
                            addIssue(
                                FramePlanIssueCode::framebufferFeedbackLoop,
                                objectId,
                                operationBasePointer() + "/destination",
                                "Particle render operation samples framebuffer '" +
                                    value.destination.id +
                                    "' while writing to the same resource"
                            );
                            valid = false;
                        }
                        write(value.destination, [&] {
                            return operationBasePointer() + "/destination";
                        });
                    }
                },
                operation
            );
            if (!valid) {
                continue;
            }
            validOperations.push_back(std::move(operation));
        }
        plan_.operations = std::move(validOperations);
    }

    std::shared_ptr<const SceneModel> model_;
    const SceneGraphSnapshot& graphSnapshot_;
    FrameProjectionSize projectionSize_;
    SceneFrameInputs inputs_;
    FrameRenderQuality renderQuality_ = FrameRenderQuality::high;
    SceneGraph::EvaluationFrame* evaluationFrame_ = nullptr;
    const std::map<std::string, EvaluatedValue>* scriptedValues_ = nullptr;
    const std::vector<int>* cursorInteractiveLayerIds_ = nullptr;
    FramePlan plan_;
    FramebufferMap sceneFramebuffers_;
    std::set<int> dependencyObjectIds_;
    std::set<int> skippedObjectIds_;
};

}  // namespace

FrameOperationKind operationKind(const FrameOperation& operation) noexcept {
    if (std::holds_alternative<FrameRenderPass>(operation)) {
        return FrameOperationKind::render;
    }
    if (std::holds_alternative<FrameCopyCommand>(operation)) {
        return FrameOperationKind::copy;
    }
    if (std::holds_alternative<FrameSwapCommand>(operation)) {
        return FrameOperationKind::swap;
    }
    if (std::holds_alternative<FrameClearCommand>(operation)) {
        return FrameOperationKind::clear;
    }
    if (std::holds_alternative<FrameTextCommand>(operation)) {
        return FrameOperationKind::text;
    }
    return FrameOperationKind::particle;
}

std::shared_ptr<SceneFrameGraph> SceneFrameGraph::create(
    std::shared_ptr<SceneGraph> graph
) {
    return std::shared_ptr<SceneFrameGraph>(
        new SceneFrameGraph(std::move(graph))
    );
}

SceneFrameGraph::SceneFrameGraph(std::shared_ptr<SceneGraph> graph)
    : graph_(std::move(graph)) {
    if (!graph_) {
        throw SceneModelError(
            SceneModelErrorCode::invalidValue,
            {},
            {},
            {},
            "Scene graph is required to create a frame graph"
        );
    }
    const Scene& scene = graph_->model()->project().scene;
    if (!scene.camera.projectionAuto) return;

    // Adapted from linux-wallpaperengine Render/Wallpapers/CScene.cpp at
    // b016d7d1fdcf4e5fd2f9c9fa420a8aaa07fee02d (GPL-3.0): automatic
    // projection is fixed from initial raw image origins and sizes, without
    // visibility, scale, or rotation. Only the no-extent fallback is host-sized.
    const SceneGraphSnapshot initial = graph_->snapshot();
    double maximumX = 0.0;
    double maximumY = 0.0;
    for (std::size_t objectIndex = 0; objectIndex < scene.objects.size(); ++objectIndex) {
        const auto* image = std::get_if<ImageObject>(&scene.objects[objectIndex].data);
        if (image == nullptr) continue;
        const std::string pointer = objectPointer(objectIndex, "size");
        const FrameVector2 size = vector2Value(
            *graph_->model(),
            evaluateDynamicValue(
                *graph_->model(), image->size, *initial.propertyValues, pointer
            ),
            pointer,
            "Image size"
        );
        if (size.x < 0.0 || size.y < 0.0) {
            frameError(
                *graph_->model(), SceneModelErrorCode::invalidValue, pointer,
                "Image size must not contain a negative component"
            );
        }
        const Vector3 origin = initial.nodes.at(objectIndex).localTransform.origin;
        maximumX = std::max(maximumX, std::abs(origin.x) + size.x * 0.5);
        maximumY = std::max(maximumY, std::abs(origin.y) + size.y * 0.5);
    }
    if (maximumX > 0.0 && maximumY > 0.0) {
        automaticProjectionSize_ = FrameProjectionSize{
            .width = checkedDimension(
                *graph_->model(), maximumX * 2.0,
                "/general/orthogonalprojection/auto", "Automatic projection width"
            ),
            .height = checkedDimension(
                *graph_->model(), maximumY * 2.0,
                "/general/orthogonalprojection/auto", "Automatic projection height"
            ),
        };
    }
}

SceneFrameGraph::~SceneFrameGraph() = default;

FrameProjectionSize SceneFrameGraph::projectionSize(
    std::optional<FrameProjectionSize> drawableFallback
) const {
    const SceneCamera& camera = graph_->model()->project().scene.camera;
    if (!camera.orthographic) {
        if (!drawableFallback || drawableFallback->width == 0 ||
            drawableFallback->height == 0) {
            frameError(
                *graph_->model(), SceneModelErrorCode::invalidValue,
                "/general/orthogonalprojection",
                "Perspective scene requires host drawable pixel dimensions"
            );
        }
        return *drawableFallback;
    }
    if (!camera.projectionAuto) {
        return {
            .width = checkedDimension(
                *graph_->model(), camera.projectionWidth,
                "/general/orthogonalprojection/width", "Projection width"
            ),
            .height = checkedDimension(
                *graph_->model(), camera.projectionHeight,
                "/general/orthogonalprojection/height", "Projection height"
            ),
        };
    }
    if (automaticProjectionSize_) return *automaticProjectionSize_;
    if (!drawableFallback || drawableFallback->width == 0 || drawableFallback->height == 0) {
        frameError(
            *graph_->model(), SceneModelErrorCode::invalidValue,
            "/general/orthogonalprojection/auto",
            "Automatic projection has no valid image extent and requires host drawable pixel dimensions"
        );
    }
    return *drawableFallback;
}

FrameEvaluationState::FrameEvaluationState(
    std::shared_ptr<const SceneGraph> graph,
    SceneGraphSnapshot graphSnapshot,
    SceneFrameInputs inputs,
    FrameRenderQuality renderQuality
) : graph_(std::move(graph)), graphSnapshot_(std::move(graphSnapshot)),
    inputs_(inputs), renderQuality_(renderQuality) {}

FramePlan SceneFrameGraph::snapshot(
    std::optional<FrameProjectionSize> drawableFallback,
    FrameRenderQuality renderQuality
) const {
    const SceneGraphSnapshot graphSnapshot = graph_->snapshot();
    const SceneFrameInputs inputs;
    return PlanBuilder(
        graph_->model(), graphSnapshot, projectionSize(drawableFallback), inputs,
        renderQuality
    ).build();
}

FramePlan SceneFrameGraph::snapshot(
    const SceneFrameInputs& inputs,
    std::optional<FrameProjectionSize> drawableFallback,
    FrameRenderQuality renderQuality
) const {
    EvaluatedFramePlan evaluated = evaluate(
        inputs, drawableFallback, renderQuality
    );
    return std::move(evaluated.plan);
}

EvaluatedFramePlan SceneFrameGraph::evaluate(
    const SceneFrameInputs& inputs,
    std::optional<FrameProjectionSize> drawableFallback,
    FrameRenderQuality renderQuality
) const {
    const auto traceStarted = FrameGraphTraceClock::now();
    auto traceStageStarted = traceStarted;
    const auto traceStage = [&](const char* stage) {
        const auto now = FrameGraphTraceClock::now();
        frameGraphTraceLog(
            "frameGraph.stage runtime=%.6f stage=%s ms=%.3f",
            inputs.runtimeSeconds,
            stage,
            frameGraphTraceMilliseconds(traceStageStarted, now)
        );
        traceStageStarted = now;
    };
    const FrameProjectionSize projection = projectionSize(drawableFallback);
    SceneFrameInputs resolvedInputs = inputs;
    resolvedInputs.canvasSize = std::array<double, 2>{
        static_cast<double>(projection.width),
        static_cast<double>(projection.height),
    };
    resolvedInputs.cursorWorldPosition = std::array<double, 3>{
        std::clamp(inputs.pointerX, 0.0, 1.0) *
            static_cast<double>(projection.width),
        (1.0 - std::clamp(inputs.pointerY, 0.0, 1.0)) *
            static_cast<double>(projection.height),
        0.0,
    };
    auto evaluation = graph_->evaluationFrame(resolvedInputs);
    traceStage("evaluationFrame");
    SceneGraphSnapshot initializationSnapshot;
    initializationSnapshot.modelRevision = evaluation->modelRevision();
    initializationSnapshot.propertyValues = evaluation->propertyValuesSnapshot();
    PlanBuilder(
        graph_->model(),
        initializationSnapshot,
        projection,
        resolvedInputs,
        renderQuality,
        evaluation.get()
    ).initializeMaterialScripts();
    traceStage("materialScripts");
    FrameEvaluationState state(
        graph_, graph_->snapshot(*evaluation), resolvedInputs, renderQuality
    );
    traceStage("graphSnapshot");
    FramePlan plan = PlanBuilder(
        graph_->model(), state.graphSnapshot_, projection,
        state.inputs_, renderQuality, evaluation.get()
    ).build();
    traceStage("planBuild");
    state.scriptedValues_ = evaluation->evaluatedScriptValues();
    state.cursorInteractiveLayerIds_ = evaluation->cursorInteractiveLayerIds();
    state.scriptEvaluations_ = evaluation->scriptEvaluationStats();
    plan.scriptEvaluations = state.scriptEvaluations_;
    frameGraphTraceLog(
        "frameGraph.end runtime=%.6f totalMs=%.3f objects=%zu operations=%zu "
        "scriptEvaluations=%zu",
        inputs.runtimeSeconds,
        frameGraphTraceMilliseconds(traceStarted),
        state.graphSnapshot_.nodes.size(),
        plan.operations.size(),
        state.scriptEvaluations_.size()
    );
    return EvaluatedFramePlan{
        .plan = std::move(plan),
        .evaluation = std::move(state),
    };
}

FramePlan SceneFrameGraph::reproject(
    const FrameEvaluationState& evaluation,
    std::optional<FrameProjectionSize> drawableFallback,
    std::optional<FrameRenderQuality> renderQuality
) const {
    if (evaluation.graph_.get() != graph_.get()) {
        frameError(
            *graph_->model(), SceneModelErrorCode::invalidValue, "/frameEvaluation",
            "Evaluated frame state belongs to a different scene graph"
        );
    }
    FramePlan plan = PlanBuilder(
        graph_->model(), evaluation.graphSnapshot_, projectionSize(drawableFallback),
        evaluation.inputs_, renderQuality.value_or(evaluation.renderQuality_),
        nullptr, &evaluation.scriptedValues_,
        &evaluation.cursorInteractiveLayerIds_
    ).build();
    plan.scriptEvaluations = evaluation.scriptEvaluations_;
    return plan;
}

bool SceneFrameGraph::requiresDrawableProjectionFallback() const noexcept {
    const SceneCamera& camera = graph_->model()->project().scene.camera;
    return !camera.orthographic ||
        (camera.projectionAuto && !automaticProjectionSize_.has_value());
}

std::shared_ptr<const SceneGraph> SceneFrameGraph::graph() const noexcept {
    return graph_;
}

}  // namespace we::scene
