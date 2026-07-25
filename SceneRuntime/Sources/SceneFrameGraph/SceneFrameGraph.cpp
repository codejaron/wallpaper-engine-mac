#include <SceneFrameGraph/SceneFrameGraph.hpp>

#include <SceneCore/AssetResolver.hpp>
#include <SceneCore/FormatError.hpp>
#include <SceneCore/Package.hpp>
#include <SceneCore/Runtime.hpp>
#include <SceneCore/Texture.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace we::scene {

FrameResourceRef frameAssetTextureResource(std::string_view name) {
    std::string path = "materials/";
    path += name;
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

using FramebufferMap = std::map<std::string, FrameResourceRef>;
constexpr int wallpaperTextureSlotCount = 8;

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
};

struct PlanCheckpoint {
    std::size_t framebufferCount = 0;
    std::size_t imageCount = 0;
    std::size_t textCount = 0;
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
        SceneGraph::EvaluationFrame* evaluationFrame = nullptr,
        const std::map<std::string, EvaluatedValue>* scriptedValues = nullptr
    )
        : model_(std::move(model)), graphSnapshot_(graphSnapshot),
          projectionSize_(projectionSize), inputs_(inputs),
          evaluationFrame_(evaluationFrame), scriptedValues_(scriptedValues) {
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
        plan_.camera = snapshotCamera(scene.camera);
        plan_.parallax = snapshotParallax(scene);
        const auto generalColor = [&](std::string_view key) {
            const auto value = scene.generalValues.find(std::string(key));
            if (value == scene.generalValues.end()) {
                frameError(
                    *model_, SceneModelErrorCode::missingField,
                    "/general/" + std::string(key),
                    "Scene general color is required"
                );
            }
            return colorValue(
                *model_,
                evaluate(
                    value->second, "/general/" + std::string(key), std::nullopt
                ),
                "/general/" + std::string(key)
            );
        };
        plan_.ambientColor = generalColor("ambientcolor");
        plan_.skylightColor = generalColor("skylightcolor");
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
        registerImageCompositeResources();
        planRenderableObjects();
        planSoundObjects();
        validateFramePlan();
        return std::move(plan_);
    }

private:
    [[nodiscard]] EvaluatedValue evaluate(
        const DynamicValue& value,
        std::string pointer,
        std::optional<int> objectId
    ) {
        EvaluatedValue result;
        if (evaluationFrame_) {
            result = evaluationFrame_->evaluate(value, pointer);
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
                *model_, value, graphSnapshot_.propertyValues, pointer
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
                const auto node = std::find_if(
                    graphSnapshot_.nodes.begin(), graphSnapshot_.nodes.end(),
                    [&](const SceneGraphNodeSnapshot& candidate) {
                        return candidate.id == *objectId;
                    }
                );
                if (node != graphSnapshot_.nodes.end()) {
                    skippedObjectIndexes_.emplace(node->objectIndex);
                }
            }
        }
        return result;
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
                return issue.code == code && issue.jsonPointer == pointer;
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
        plan_.particles.resize(value.particleCount);
        plan_.sounds.resize(value.soundCount);
        plan_.operations.resize(value.operationCount);
    }

    void recordObjectPlanningFailure(
        std::size_t objectIndex,
        int objectId,
        const std::exception& error
    ) {
        skippedObjectIndexes_.emplace(objectIndex);
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
        if (skippedObjectIndexes_.contains(objectIndex)) {
            return;
        }
        const PlanCheckpoint before = checkpoint();
        try {
            callback();
            if (skippedObjectIndexes_.contains(objectIndex)) {
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
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const SceneObject& object = objects[index];
            bool perspective = false;
            if (const auto* image = std::get_if<ImageObject>(&object.data)) {
                perspective = image->perspective;
            } else if (const auto* text = std::get_if<TextObject>(&object.data)) {
                perspective = text->perspective;
            }
            if (perspective) {
                addIssue(
                    FramePlanIssueCode::perspectiveProjectionUnavailable,
                    object.base.id,
                    objectPointer(index, "perspective"),
                    "Perspective projection is not implemented for this layer; the Linux runtime ignores this flag"
                );
            }
            if (const auto* text = std::get_if<TextObject>(&object.data)) {
                if (text->limitRows || text->limitWidth || text->limitUseEllipsis) {
                    addIssue(
                        FramePlanIssueCode::textRenderingUnavailable,
                        object.base.id, objectPointer(index),
                        "Text row limiting, width limiting, and ellipsis layout are ignored to match the Linux runtime"
                    );
                }
            }
            if (const auto* image = std::get_if<ImageObject>(&object.data);
                image != nullptr && image->model && image->model->puppet) {
                addIssue(
                    FramePlanIssueCode::puppetUnavailable,
                    object.base.id,
                    objectPointer(index, "image"),
                    "Puppet mesh rendering is not implemented by FrameGraph"
                );
                skippedObjectIndexes_.emplace(index);
            }
        }
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
                        skippedObjectIndexes_.emplace(node.objectIndex);
                    }
                    break;
                }
            }
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
        return frameAssetTextureResource(name);
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
        const auto userTextures = namedSlots(pass.userTextures);
        std::optional<std::string> name;
        if (const auto user = userTextures.find(0); user != userTextures.end()) {
            name = user->second;
        } else if (const auto base = textures.find(0); base != textures.end()) {
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
        std::size_t objectIndex
    ) {
        const std::string pointer = objectPointer(objectIndex, "size");
        if (image.model && image.model->fullscreen) {
            return {
                .x = static_cast<double>(plan_.width),
                .y = static_cast<double>(plan_.height),
            };
        }
        FrameVector2 result = vector2Value(
            *model_, evaluate(image.size, pointer, std::nullopt), pointer,
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

    [[nodiscard]] std::optional<ImageContext> createImageContext(
        std::size_t objectIndex,
        std::optional<std::size_t>& retainedFramebufferCount
    ) {
        const auto& objects = model_->project().scene.objects;
        const auto* image = std::get_if<ImageObject>(&objects.at(objectIndex).data);
        if (image == nullptr) {
            return std::nullopt;
        }
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(objectIndex);
            const std::optional<FrameResourceRef> source = primarySource(
                *image, objectIndex, node.id
            );
            const bool solidLayer = image->model && image->model->solidLayer;
            const FrameVector2 size = imageSize(*image, source, objectIndex);
            const std::uint32_t width = checkedDimension(
                *model_, size.x, objectPointer(objectIndex, "size"), "Image width"
            );
            const std::uint32_t height = checkedDimension(
                *model_, size.y, objectPointer(objectIndex, "size"), "Image height"
            );
            const FrameResourceRef resourceA = imageCompositeResource(node.id, 'a');
            const FrameResourceRef resourceB = imageCompositeResource(node.id, 'b');
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
            plan_.images.push_back({
                .objectIndex = objectIndex,
                .objectId = node.id,
                .visible = node.isVisible,
                .passthrough = image->model && image->model->passthrough,
                .fullscreen = image->model && image->model->fullscreen,
                .size = size,
                .worldTransform = worldTransform,
                .source = resolvedSource,
                .compositeA = compositeA.resource,
                .compositeB = compositeB.resource,
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
        std::size_t objectIndex
    ) {
        const auto& objects = model_->project().scene.objects;
        const auto* text = std::get_if<TextObject>(&objects.at(objectIndex).data);
        if (text == nullptr || skippedObjectIndexes_.contains(objectIndex)) {
            return std::nullopt;
        }
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(objectIndex);
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
            if (spacing.x != 0.0 || spacing.y != 0.0) {
                addIssue(FramePlanIssueCode::textRenderingUnavailable, node.id,
                         base + "/spacing",
                         "Non-zero text character or line spacing is ignored to match the Linux runtime");
            }
            const std::size_t descriptorIndex = plan_.texts.size();
            plan_.texts.push_back({
                .objectIndex = objectIndex,
                .objectId = node.id,
                .visible = node.isVisible,
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
            });
            return descriptorIndex;
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
        const SceneGraphNodeSnapshot& node,
        std::size_t objectIndex,
        std::size_t controlPointIndex
    ) const {
        const std::string pointer = objectPointer(objectIndex, "particle") +
            "/controlpoint/" + std::to_string(controlPointIndex);
        const particle::Vector3 authoredOffset = concreteParticleVector(
            source.offset
        );
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
                authoredWorldParticlePoint(source.offset),
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
            }
        }, operation);
    }

    [[nodiscard]] FrameResourceRef particleTexture(
        const MaterialPass& pass,
        std::size_t objectIndex
    ) const {
        const std::string pointer = objectPointer(objectIndex, "particle") +
            "/material/passes/0";
        if (!pass.userTextures.empty()) {
            for (std::size_t index = 0; index < pass.userTextures.size(); ++index) {
                if (pass.userTextures[index].name) {
                    frameError(
                        *model_, SceneModelErrorCode::unsupportedObject,
                        pointer + "/usertextures/" + std::to_string(index),
                        "Particle user textures are not supported"
                    );
                }
            }
        }
        std::optional<std::string> texture;
        for (std::size_t index = 0; index < pass.textures.size(); ++index) {
            const TextureSlot& slot = pass.textures[index];
            if (!slot.name) continue;
            if (index != 0) {
                frameError(
                    *model_, SceneModelErrorCode::unsupportedObject,
                    pointer + "/textures/" + std::to_string(index),
                    "Particle materials support only static texture slot zero"
                );
            }
            texture = *slot.name;
        }
        if (!texture || texture->empty()) {
            frameError(
                *model_, SceneModelErrorCode::missingField,
                pointer + "/textures/0",
                "Particle material requires a static texture in slot zero"
            );
        }
        return resolveTexture(*texture, {}, pointer + "/textures/0");
    }

    [[nodiscard]] ComboMap resolvedParticleCombos(
        const ComboMap& combos,
        std::size_t objectIndex
    ) const {
        ComboMap result = {
            {"GS_ENABLED", 0},
            {"SPRITESHEET", 0},
            {"THICKFORMAT", 1},
            {"TRAILRENDERER", 0},
        };
        for (const auto& [name, value] : combos) {
            if (name == "SPRITESHEET" && (value == 0 || value == 1)) {
                result[name] = value;
                continue;
            }
            const auto expected = result.find(name);
            if (expected == result.end() || expected->second != value) {
                frameError(
                    *model_, SceneModelErrorCode::unsupportedObject,
                    objectPointer(objectIndex, "particle") +
                        "/material/passes/0/combos/" + name,
                    "Particle material combo '" + name + "=" +
                        std::to_string(value) + "' is not supported"
                );
            }
        }
        return result;
    }

    [[nodiscard]] std::optional<std::size_t> createParticleDescriptor(
        std::size_t objectIndex
    ) {
        const auto& objects = model_->project().scene.objects;
        const auto* object = std::get_if<ParticleObject>(&objects.at(objectIndex).data);
        if (object == nullptr || skippedObjectIndexes_.contains(objectIndex)) {
            return std::nullopt;
        }
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(objectIndex);
            const std::string pointer = objectPointer(objectIndex, "particle");
            if (!object->definition || !object->definition->material) {
                frameError(
                    *model_, SceneModelErrorCode::assetFailure, pointer,
                    "Particle object has no loaded definition and material"
                );
            }
            const ParticleDefinition& definition = *object->definition;
            const Material& material = *definition.material;
            if (material.passes.size() != 1) {
                frameError(
                    *model_, SceneModelErrorCode::unsupportedObject,
                    pointer + "/material/passes",
                    "Particle material must contain exactly one pass"
                );
            }
            const MaterialPass& pass = material.passes.front();
            if (pass.shader != "genericparticle") {
                frameError(
                    *model_, SceneModelErrorCode::unsupportedObject,
                    pointer + "/material/passes/0/shader",
                    "Particle phase one supports only the 'genericparticle' shader"
                );
            }
            if (!pass.constants.empty()) {
                frameError(
                    *model_, SceneModelErrorCode::unsupportedObject,
                    pointer + "/material/passes/0/constantshadervalues",
                    "Particle material shader constants are not supported"
                );
            }
            const ComboMap combos = resolvedParticleCombos(
                pass.combos, objectIndex
            );
            const FrameResourceRef texture0 = particleTexture(pass, objectIndex);
            if (texture0.kind != FrameResourceKind::assetTexture) {
                frameError(
                    *model_, SceneModelErrorCode::unsupportedObject,
                    pointer + "/material/passes/0/textures/0",
                    "Particle texture slot zero must reference a static asset texture"
                );
            }
            const ShaderAssetPaths paths = materialShaderPaths(
                *model_, material, pass.shader,
                pointer + "/material/passes/0"
            );
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
                        node,
                        objectIndex,
                        index
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
                .shader = pass.shader,
                .vertexShaderPath = paths.vertex,
                .fragmentShaderPath = paths.fragment,
                .blending = pass.blending,
                .culling = pass.culling,
                .depthTest = pass.depthTest,
                .depthWrite = pass.depthWrite,
                .texture0 = texture0,
                .combos = combos,
                .parallaxDepth = parallaxDepth,
                .perspective = (definition.flags & 4U) != 0U,
                .animationMode = definition.animationMode,
                .sequenceMultiplier = definition.sequenceMultiplier,
                .configuration = std::move(configuration),
            });
            return descriptorIndex;
    }

    void createSoundDescriptor(std::size_t objectIndex) {
        const auto& objects = model_->project().scene.objects;
        const auto* sound = std::get_if<SoundObject>(&objects.at(objectIndex).data);
        if (sound == nullptr || skippedObjectIndexes_.contains(objectIndex)) {
            return;
        }
            const SceneGraphNodeSnapshot& node = graphSnapshot_.nodes.at(objectIndex);
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
            FrameSoundPlaybackMode playbackMode = FrameSoundPlaybackMode::once;
            if (sound->playbackMode && !sound->playbackMode->empty() &&
                *sound->playbackMode != "once") {
                if (*sound->playbackMode == "loop") {
                    playbackMode = FrameSoundPlaybackMode::loop;
                } else {
                    addIssue(
                        FramePlanIssueCode::soundRuntimeUnavailable, node.id,
                        base + "/playbackmode",
                        "Sound playback mode '" + *sound->playbackMode +
                            "' is not implemented by the Scene runtime"
                    );
                }
            }
            if (!std::isfinite(sound->minimumTime) ||
                !std::isfinite(sound->maximumTime) ||
                sound->minimumTime < 0.0 || sound->maximumTime < 0.0) {
                frameError(
                    *model_, SceneModelErrorCode::invalidValue, base,
                    "Sound timing bounds must be finite and non-negative"
                );
            }
            if (sound->startSilent) {
                addIssue(
                    FramePlanIssueCode::soundRuntimeUnavailable, node.id,
                    base + "/startsilent",
                    "startSilent sound activation is not implemented by the Scene runtime"
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
        FramebufferMap result;
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
            result.emplace(definition.name, framebuffer.resource);
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
                checkedDimension(*model_, std::floor(image.size.x * scale), pointer + "/fit", "Framebuffer width"),
                checkedDimension(*model_, std::floor(image.size.y * scale), pointer + "/fit", "Framebuffer height"),
            };
        }
        return {
            checkedDimension(*model_, std::floor(image.size.x / definition.scale), pointer + "/scale", "Framebuffer width"),
            checkedDimension(*model_, std::floor(image.size.y / definition.scale), pointer + "/scale", "Framebuffer height"),
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
            MaterialPass material = effectiveBasePass(
                image.model->material->passes[passIndex], image, passIndex == 0
            );
            const ShaderAssetPaths paths = materialShaderPaths(
                *model_, *image.model->material, material.shader,
                objectPointer(imageContext.objectIndex, "image") +
                    "/material/passes/" + std::to_string(passIndex)
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
            });
        }

        for (std::size_t effectIndex = 0; effectIndex < image.effects.size(); ++effectIndex) {
            const ImageEffect& effectInstance = image.effects[effectIndex];
            const std::string effectPointer = objectPointer(imageContext.objectIndex, "effects") +
                '/' + std::to_string(effectIndex);
            if (!booleanValue(
                    *model_, evaluate(effectInstance.visible, effectPointer + "/visible", imageContext.node->id),
                    effectPointer + "/visible", "Effect visibility"
                )) {
                continue;
            }
            if (!effectInstance.effect) {
                frameError(
                    *model_, SceneModelErrorCode::assetFailure, effectPointer,
                    "Image effect definition is missing"
                );
            }
            const Effect& effect = *effectInstance.effect;
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
                    const ShaderAssetPaths paths = materialShaderPaths(
                        *model_, *effectPass.material, material.shader,
                        passPointer + "/material/passes/" +
                            std::to_string(materialPassIndex)
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
                        .overridePass = overridePass,
                        .binds = binds,
                        .localFramebuffers = localFramebuffers,
                        .target = target,
                    });
                }
            }
        }
        return result;
    }

    [[nodiscard]] std::map<int, FrameResourceRef> resolvePassTextures(
        const PendingRender& pending,
        const FrameResourceRef& input,
        const std::optional<FrameResourceRef>& previous,
        const FramebufferMap& localFramebuffers,
        std::string pointer
    ) const {
        std::map<int, std::string> names = namedSlots(pending.material.textures);
        const auto merge = [&names](const TextureSlots& slots) {
            for (const auto& [index, value] : namedSlots(slots)) {
                names.insert_or_assign(index, value);
            }
        };
        merge(pending.material.userTextures);
        if (pending.overridePass != nullptr) {
            merge(pending.overridePass->textures);
            merge(pending.overridePass->userTextures);
        }

        std::map<int, FrameResourceRef> result;
        for (const auto& [index, name] : names) {
            result.emplace(index, resolveTexture(name, localFramebuffers, pointer));
        }
        for (const auto& [index, name] : pending.binds) {
            if (name == "previous") {
                result.insert_or_assign(index, previous.value_or(input));
            } else {
                result.insert_or_assign(
                    index, resolveTexture(name, localFramebuffers, pointer)
                );
            }
        }
        if (!result.contains(0)) {
            result.emplace(0, input);
        }
        return result;
    }

    [[nodiscard]] ComboMap resolveCombos(const PendingRender& pending) const {
        ComboMap result = pending.material.combos;
        if (pending.overridePass != nullptr) {
            for (const auto& [name, value] : pending.overridePass->combos) {
                result.insert_or_assign(name, value);
            }
        }
        return result;
    }

    [[nodiscard]] std::map<std::string, EvaluatedValue> resolveConstants(
        const PendingRender& pending,
        std::string pointer,
        int objectId
    ) {
        ConstantMap constants = pending.material.constants;
        if (pending.overridePass != nullptr) {
            for (const auto& [name, value] : pending.overridePass->constants) {
                constants.insert_or_assign(name, value);
            }
        }
        std::map<std::string, EvaluatedValue> result;
        for (const auto& [name, value] : constants) {
            result.emplace(name, evaluate(value, pointer + '/' + name, objectId));
        }
        return result;
    }

    void planImageObject(std::size_t objectIndex, int objectId) {
        const PlanCheckpoint before = checkpoint();
        std::optional<std::size_t> retainedFramebufferCount;
        try {
            const std::optional<ImageContext> context = createImageContext(
                objectIndex, retainedFramebufferCount
            );
            if (context && !skippedObjectIndexes_.contains(objectIndex)) {
                scheduleImage(*context);
            }
            if (skippedObjectIndexes_.contains(objectIndex)) {
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
        for (const std::size_t objectIndex : graphSnapshot_.renderOrder) {
            const SceneObject& object = objects.at(objectIndex);
            if (std::holds_alternative<ImageObject>(object.data)) {
                planImageObject(objectIndex, object.base.id);
                continue;
            }
            isolateObjectPlanning(
                objectIndex,
                object.base.id,
                [&] {
                    if (std::holds_alternative<TextObject>(object.data)) {
                        const std::optional<std::size_t> descriptor =
                            createTextDescriptor(objectIndex);
                        if (descriptor &&
                            !skippedObjectIndexes_.contains(objectIndex) &&
                            plan_.texts.at(*descriptor).visible) {
                            plan_.operations.emplace_back(FrameTextCommand{
                                .textIndex = *descriptor,
                                .objectId = plan_.texts.at(*descriptor).objectId,
                                .destination = plan_.output,
                            });
                        }
                        return;
                    }
                    if (!std::holds_alternative<ParticleObject>(object.data)) {
                        return;
                    }
                    const std::optional<std::size_t> descriptor =
                        createParticleDescriptor(objectIndex);
                    if (!descriptor ||
                        skippedObjectIndexes_.contains(objectIndex) ||
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

    void planSoundObjects() {
        const auto& objects = model_->project().scene.objects;
        for (std::size_t objectIndex = 0;
             objectIndex < objects.size(); ++objectIndex) {
            const SceneObject& object = objects[objectIndex];
            if (!std::holds_alternative<SoundObject>(object.data)) {
                continue;
            }
            isolateObjectPlanning(
                objectIndex,
                object.base.id,
                [&] {
                    createSoundDescriptor(objectIndex);
                }
            );
        }
    }

    void scheduleImage(ImageContext context) {
        FrameImageDescriptor& image = plan_.images.at(context.planImageIndex);
        if (!image.visible) {
            return;
        }
        std::vector<PendingOperation> pending = pendingOperations(context);
        if (pending.empty()) {
            return;
        }
        const std::size_t basePassCount = context.image->model->material->passes.size();
        if (image.passthrough && pending.size() <= basePassCount) {
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
                const bool finalPass = !writesToTarget && isLastDraw(pendingIndex);
                const FrameResourceRef destination = finalPass ? plan_.output : drawTo;
                const std::optional<FrameResourceRef> previous =
                    inTargetEffectSequence ? effectInput : std::nullopt;
                FrameGeometryKind geometry = FrameGeometryKind::imageLocal;
                FrameTexCoordKind texcoords = FrameTexCoordKind::image;
                if (firstDraw && image.passthrough) {
                    geometry = image.fullscreen
                        ? FrameGeometryKind::fullscreenLocal
                        : FrameGeometryKind::passthroughCapture;
                    texcoords = FrameTexCoordKind::full;
                } else if (!firstDraw) {
                    geometry = FrameGeometryKind::fullscreenLocal;
                    texcoords = FrameTexCoordKind::full;
                }
                if (finalPass) {
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
                    .constants = resolveConstants(
                        *render, pointer + "/constants", context.node->id
                    ),
                    .writeAlpha = !finalPass,
                };
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
        const auto requireDescriptor = [&](
            const FrameResourceRef& resource,
            std::string pointer,
            std::optional<int> objectId,
            FramePlanIssueSeverity severity = FramePlanIssueSeverity::skipPass
        ) -> bool {
            if (resource.kind != FrameResourceKind::framebuffer ||
                descriptorIds.contains(resource.id)) {
                return true;
            }
            if (missingDescriptorIds.emplace(resource.id).second) {
                addIssue(
                    FramePlanIssueCode::framebufferDescriptorMissing,
                    objectId,
                    std::move(pointer),
                    "Framebuffer resource '" + resource.id +
                        "' has no descriptor in the frame plan",
                    severity
                );
            }
            return false;
        };

        requireDescriptor(
            plan_.output,
            "/framePlan/output",
            std::nullopt,
            FramePlanIssueSeverity::frameFatal
        );
        for (const FrameImageDescriptor& image : plan_.images) {
            const std::string pointer = objectPointer(image.objectIndex, "image");
            requireDescriptor(image.source, pointer + "/source", image.objectId);
            requireDescriptor(image.compositeA, pointer + "/composite_a", image.objectId);
            requireDescriptor(image.compositeB, pointer + "/composite_b", image.objectId);
        }

        std::vector<FrameOperation> validOperations;
        validOperations.reserve(plan_.operations.size());
        for (std::size_t operationIndex = 0;
             operationIndex < plan_.operations.size(); ++operationIndex) {
            const FrameOperation& operation = plan_.operations[operationIndex];
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
            const std::string operationObjectPointer = origin
                ? operationPointer(*origin)
                : text
                    ? objectPointer(plan_.texts.at(text->textIndex).objectIndex, "text")
                    : objectPointer(
                        plan_.particles.at(particle->particleIndex).objectIndex,
                        "particle"
                    );
            const std::string basePointer = operationObjectPointer +
                "/frameOperation/" + std::to_string(operationIndex);
            std::set<std::string> operationReads;
            bool valid = true;
            const auto read = [&](const FrameResourceRef& resource, std::string pointer) {
                if (resource.kind != FrameResourceKind::framebuffer) {
                    return;
                }
                if (!operationReads.emplace(resource.id).second) {
                    return;
                }
                const bool hasDescriptor = requireDescriptor(
                    resource, std::move(pointer), objectId
                );
                if (!hasDescriptor) {
                    valid = false;
                }
            };
            const auto write = [&](const FrameResourceRef& resource, std::string pointer) {
                if (resource.kind != FrameResourceKind::framebuffer) {
                    return;
                }
                if (!requireDescriptor(resource, std::move(pointer), objectId)) {
                    valid = false;
                }
            };

            std::visit(
                [&](const auto& value) {
                    using Operation = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Operation, FrameRenderPass>) {
                        read(value.input, basePointer + "/input");
                        if (value.previousInput) {
                            read(*value.previousInput, basePointer + "/previousInput");
                        }
                        for (const auto& [slot, resource] : value.textures) {
                            read(
                                resource,
                                basePointer + "/textures/" + std::to_string(slot)
                            );
                        }
                        if (value.destination.kind == FrameResourceKind::framebuffer &&
                            operationReads.contains(value.destination.id)) {
                            addIssue(
                                FramePlanIssueCode::framebufferFeedbackLoop,
                                objectId,
                                basePointer + "/destination",
                                "Render operation samples framebuffer '" +
                                    value.destination.id +
                                    "' while writing to the same resource"
                            );
                            valid = false;
                        }
                        write(value.destination, basePointer + "/destination");
                    } else if constexpr (std::is_same_v<Operation, FrameCopyCommand>) {
                        read(value.source, basePointer + "/source");
                        if (value.source.kind == FrameResourceKind::framebuffer &&
                            value.destination.kind == FrameResourceKind::framebuffer &&
                            value.source.id == value.destination.id) {
                            addIssue(
                                FramePlanIssueCode::framebufferFeedbackLoop,
                                objectId,
                                basePointer + "/destination",
                                "Copy operation reads and writes framebuffer '" +
                                    value.destination.id + "'"
                            );
                            valid = false;
                        }
                        write(value.destination, basePointer + "/destination");
                    } else if constexpr (std::is_same_v<Operation, FrameSwapCommand>) {
                        read(value.source, basePointer + "/source");
                        read(value.destination, basePointer + "/destination");
                    } else if constexpr (std::is_same_v<Operation, FrameClearCommand>) {
                        write(value.destination, basePointer + "/destination");
                    } else if constexpr (std::is_same_v<Operation, FrameTextCommand>) {
                        write(value.destination, basePointer + "/destination");
                    } else if constexpr (std::is_same_v<Operation, FrameParticleCommand>) {
                        write(value.destination, basePointer + "/destination");
                    }
                },
                operation
            );
            if (!valid) {
                continue;
            }
            validOperations.push_back(operation);
        }
        plan_.operations = std::move(validOperations);
    }

    std::shared_ptr<const SceneModel> model_;
    const SceneGraphSnapshot& graphSnapshot_;
    FrameProjectionSize projectionSize_;
    SceneFrameInputs inputs_;
    SceneGraph::EvaluationFrame* evaluationFrame_ = nullptr;
    const std::map<std::string, EvaluatedValue>* scriptedValues_ = nullptr;
    FramePlan plan_;
    FramebufferMap sceneFramebuffers_;
    std::set<std::size_t> skippedObjectIndexes_;
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
                *graph_->model(), image->size, initial.propertyValues, pointer
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
    SceneFrameInputs inputs
) : graph_(std::move(graph)), graphSnapshot_(std::move(graphSnapshot)),
    inputs_(inputs) {}

FramePlan SceneFrameGraph::snapshot(
    std::optional<FrameProjectionSize> drawableFallback
) const {
    const SceneGraphSnapshot graphSnapshot = graph_->snapshot();
    const SceneFrameInputs inputs;
    return PlanBuilder(
        graph_->model(), graphSnapshot, projectionSize(drawableFallback), inputs
    ).build();
}

FramePlan SceneFrameGraph::snapshot(
    const SceneFrameInputs& inputs,
    std::optional<FrameProjectionSize> drawableFallback
) const {
    EvaluatedFramePlan evaluated = evaluate(inputs, drawableFallback);
    return std::move(evaluated.plan);
}

EvaluatedFramePlan SceneFrameGraph::evaluate(
    const SceneFrameInputs& inputs,
    std::optional<FrameProjectionSize> drawableFallback
) const {
    auto evaluation = graph_->evaluationFrame(inputs);
    FrameEvaluationState state(graph_, graph_->snapshot(*evaluation), inputs);
    FramePlan plan = PlanBuilder(
        graph_->model(), state.graphSnapshot_, projectionSize(drawableFallback),
        state.inputs_, evaluation.get()
    ).build();
    state.scriptedValues_ = evaluation->evaluatedScriptValues();
    state.scriptEvaluations_ = evaluation->scriptEvaluationStats();
    plan.scriptEvaluations = state.scriptEvaluations_;
    return EvaluatedFramePlan{
        .plan = std::move(plan),
        .evaluation = std::move(state),
    };
}

FramePlan SceneFrameGraph::reproject(
    const FrameEvaluationState& evaluation,
    std::optional<FrameProjectionSize> drawableFallback
) const {
    if (evaluation.graph_.get() != graph_.get()) {
        frameError(
            *graph_->model(), SceneModelErrorCode::invalidValue, "/frameEvaluation",
            "Evaluated frame state belongs to a different scene graph"
        );
    }
    FramePlan plan = PlanBuilder(
        graph_->model(), evaluation.graphSnapshot_, projectionSize(drawableFallback),
        evaluation.inputs_, nullptr, &evaluation.scriptedValues_
    ).build();
    plan.scriptEvaluations = evaluation.scriptEvaluations_;
    return plan;
}

bool SceneFrameGraph::requiresDrawableProjectionFallback() const noexcept {
    return graph_->model()->project().scene.camera.projectionAuto &&
        !automaticProjectionSize_.has_value();
}

std::shared_ptr<const SceneGraph> SceneFrameGraph::graph() const noexcept {
    return graph_;
}

}  // namespace we::scene
