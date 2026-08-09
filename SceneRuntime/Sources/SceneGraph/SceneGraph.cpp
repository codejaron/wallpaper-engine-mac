#include <SceneGraph/SceneGraph.hpp>

#include <SceneCore/AssetResolver.hpp>
#include <SceneCore/FormatError.hpp>
#include <SceneCore/Runtime.hpp>
#include <SceneCore/Texture.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <sstream>
#include <set>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace we::scene {
namespace {

[[nodiscard]] bool frameTraceEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("WE_SCENE_FRAME_TRACE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

double localDaytimeFraction() {
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) {
        throw SceneModelError(
            SceneModelErrorCode::invalidValue,
            "",
            "/frameInputs/timeOfDay",
            {},
            "Reading local wall-clock time failed"
        );
    }
    std::tm local{};
    if (::localtime_r(&now, &local) == nullptr) {
        throw SceneModelError(
            SceneModelErrorCode::invalidValue,
            "",
            "/frameInputs/timeOfDay",
            {},
            "Converting local wall-clock time failed"
        );
    }
    return static_cast<double>(local.tm_hour * 60 + local.tm_min) / (24.0 * 60.0);
}

enum class VisitState : std::uint8_t { unvisited, visiting, visited };

std::string objectPointer(std::size_t index, std::string_view field = {}) {
    std::string result = "/objects/" + std::to_string(index);
    if (!field.empty()) {
        result += '/';
        result += field;
    }
    return result;
}

[[noreturn]] void graphError(
    const SceneModel& model,
    SceneModelErrorCode code,
    std::string pointer,
    std::vector<std::string> chain,
    std::string message
) {
    throw SceneModelError(
        code,
        model.project().scene.assetPath,
        std::move(pointer),
        std::move(chain),
        std::move(message)
    );
}

std::vector<std::string> cycleChain(
    const std::vector<std::size_t>& stack,
    std::size_t repeated,
    const std::vector<SceneObject>& objects
) {
    const auto start = std::find(stack.begin(), stack.end(), repeated);
    std::vector<std::string> result;
    const auto first = start == stack.end() ? stack.begin() : start;
    result.reserve(static_cast<std::size_t>(stack.end() - first) + 1);
    for (auto iterator = first; iterator != stack.end(); ++iterator) {
        result.push_back("object " + std::to_string(objects[*iterator].base.id));
    }
    result.push_back("object " + std::to_string(objects[repeated].base.id));
    return result;
}

std::vector<std::size_t> buildOrder(
    const SceneModel& model,
    const std::map<int, std::size_t>& indices,
    bool includeParents
) {
    const auto& objects = model.project().scene.objects;
    std::vector<VisitState> states(objects.size(), VisitState::unvisited);
    std::vector<std::size_t> stack;
    std::vector<std::size_t> order;
    order.reserve(objects.size());

    struct Frame {
        std::size_t index = 0;
        std::size_t nextDependency = 0;
        bool parentProcessed = false;
    };
    std::vector<Frame> frames;
    frames.reserve(objects.size());

    const auto enter = [&](std::size_t index, std::string pointer) {
        if (states[index] == VisitState::visiting) {
            graphError(
                model,
                SceneModelErrorCode::referenceCycle,
                std::move(pointer),
                cycleChain(stack, index, objects),
                includeParents
                    ? "Scene object initialization cycle detected"
                    : "Scene object dependency cycle detected"
            );
        }
        if (states[index] == VisitState::unvisited) {
            states[index] = VisitState::visiting;
            stack.push_back(index);
            frames.push_back({.index = index});
        }
    };

    for (std::size_t index = 0; index < objects.size(); ++index) {
        if (states[index] != VisitState::unvisited) {
            continue;
        }
        enter(index, objectPointer(index));
        while (!frames.empty()) {
            Frame& frame = frames.back();
            const ObjectBase& object = objects[frame.index].base;
            bool descended = false;

            while (frame.nextDependency < object.dependencies.size()) {
                const std::size_t dependencyIndex = frame.nextDependency++;
                const int dependencyId = object.dependencies[dependencyIndex].id;
                // Wallpaper Engine scene files can contain a self dependency;
                // the upstream runtime explicitly treats it as a no-op.
                if (dependencyId == object.id) {
                    continue;
                }
                const std::string pointer =
                    objectPointer(frame.index, "dependencies") + '/' +
                    std::to_string(dependencyIndex);
                const auto dependency = indices.find(dependencyId);
                if (dependency == indices.end()) {
                    graphError(
                        model,
                        SceneModelErrorCode::danglingReference,
                        pointer,
                        {"object " + std::to_string(object.id)},
                        "Object dependency references unknown id " +
                            std::to_string(dependencyId)
                    );
                }
                if (states[dependency->second] == VisitState::visited) {
                    continue;
                }
                enter(dependency->second, pointer);
                descended = true;
                break;
            }
            if (descended) {
                continue;
            }

            if (includeParents && !frame.parentProcessed) {
                frame.parentProcessed = true;
                if (object.parent) {
                    const std::string pointer = objectPointer(
                        frame.index,
                        "parent"
                    );
                    const auto parent = indices.find(*object.parent);
                    if (parent == indices.end()) {
                        graphError(
                            model,
                            SceneModelErrorCode::danglingReference,
                            pointer,
                            {"object " + std::to_string(object.id)},
                            "Object parent references unknown id " +
                                std::to_string(*object.parent)
                        );
                    }
                    if (states[parent->second] != VisitState::visited) {
                        enter(parent->second, pointer);
                        continue;
                    }
                }
            }

            states[frame.index] = VisitState::visited;
            order.push_back(frame.index);
            frames.pop_back();
            stack.pop_back();
        }
    }
    return order;
}

struct LayerScriptOwner final {
    int id = 0;
    std::string property;
};

std::optional<LayerScriptOwner> layerScriptOwner(
    const SceneModel& model,
    std::string_view pointer
) {
    constexpr std::string_view prefix = "/objects/";
    if (!pointer.starts_with(prefix)) return std::nullopt;
    const std::size_t indexEnd = pointer.find('/', prefix.size());
    if (indexEnd == std::string_view::npos || indexEnd + 1 >= pointer.size()) {
        return std::nullopt;
    }
    std::size_t objectIndex = 0;
    const auto parsed = std::from_chars(
        pointer.data() + prefix.size(),
        pointer.data() + indexEnd,
        objectIndex
    );
    if (parsed.ec != std::errc{} || parsed.ptr != pointer.data() + indexEnd ||
        objectIndex >= model.project().scene.objects.size()) {
        return std::nullopt;
    }
    const std::size_t propertyStart = indexEnd + 1;
    const std::size_t propertyEnd = pointer.find('/', propertyStart);
    // Only the DynamicValue directly owned by the scene object may receive
    // the layer-registry overlay. Nested paths (for example
    // `/objects/0/origin/scriptproperties/amount`) describe a script's own
    // inputs; treating them as the owner's `origin` would replace the real
    // user-bound value with the layer vector and can turn arithmetic into NaN.
    if (propertyEnd != std::string_view::npos) return std::nullopt;
    std::string property(
        pointer.substr(
            propertyStart,
            std::string_view::npos
        )
    );
    if (property.empty()) return std::nullopt;
    // SceneScript exposes the names used by Wallpaper Engine's public layer
    // contract. Keep the graph's canonical DynamicValue keys as the single
    // storage location for common aliases.
    if (property == "opacity") property = "alpha";
    if (property == "pointsize") property = "pointSize";
    const SceneObject& object = model.project().scene.objects[objectIndex];
    return LayerScriptOwner{.id = object.base.id, .property = property};
}

std::optional<std::string> primaryTextureIdentity(const ImageObject& image) {
    if (!image.model || image.model->solidLayer ||
        !image.model->material || image.model->material->passes.empty()) {
        return std::nullopt;
    }
    const MaterialPass& pass = image.model->material->passes.front();
    const auto slotName = [](const TextureSlots& slots) -> std::optional<std::string> {
        if (slots.empty() || !slots.front().name || slots.front().name->empty()) {
            return std::nullopt;
        }
        return *slots.front().name;
    };
    // The renderable's primary image is detected only from ordinary material
    // textures. User textures are higher-priority shader providers, but they
    // must not replace the image source used for dimensions, animation, layer
    // identity, or TEX0FORMAT (matching CRenderable::detectTexture()).
    std::optional<std::string> name = slotName(pass.textures);
    if (!name) name = slotName(image.instanceTextures);
    if (!name) return std::nullopt;
    std::string path = "materials/" + *name;
    if (!path.ends_with(".tex")) path += ".tex";
    return path;
}

std::vector<script::ScriptLayerDescriptor> sceneLayerDescriptors(
    const SceneModel& model,
    const std::map<std::string, Value>& properties
) {
    const auto& objects = model.project().scene.objects;
    std::vector<script::ScriptLayerDescriptor> result;
    result.reserve(objects.size());
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const SceneObject& object = objects[index];
        script::ScriptLayerType type;
        if (std::holds_alternative<ImageObject>(object.data)) {
            type = script::ScriptLayerType::image;
        } else if (std::holds_alternative<TextObject>(object.data)) {
            type = script::ScriptLayerType::text;
        } else if (std::holds_alternative<ParticleObject>(object.data)) {
            type = script::ScriptLayerType::particle;
        } else if (std::holds_alternative<SoundObject>(object.data)) {
            type = script::ScriptLayerType::sound;
        } else if (std::holds_alternative<GroupObject>(object.data)) {
            type = script::ScriptLayerType::group;
        } else {
            continue;
        }
        script::ScriptLayerDescriptor descriptor{
            .id = object.base.id,
            .name = object.base.name,
            .type = type,
            .sourceObjectIndex = index,
            .parent = object.base.parent,
            .disablePropagation = object.base.disablePropagation,
            .initialConfig = object.initialConfig,
        };
        const auto add = [&](std::string_view name, const DynamicValue& value) {
            descriptor.properties.emplace(
                std::string(name),
                evaluateDynamicValue(
                    model, value,
                    properties,
                    objectPointer(index, name)
                ).value
            );
            if (value.animation) {
                descriptor.propertyAnimations.emplace(
                    std::string(name), *value.animation
                );
            }
        };
        add("origin", object.base.origin);
        add("scale", object.base.scale);
        add("angles", object.base.angles);
        add("visible", object.base.visible);
        if (const auto* image = std::get_if<ImageObject>(&object.data)) {
            descriptor.properties.emplace(
                "solid", RuntimeValue::boolean(object.base.solid)
            );
            descriptor.textureAssetIdentity = primaryTextureIdentity(*image);
            add("alpha", image->alpha);
            add("color", image->color);
            add("size", image->size);
            add("parallaxDepth", image->parallaxDepth);
            add("brightness", image->brightness);
            add("colorBlendMode", image->colorBlendMode);
        } else if (const auto* text = std::get_if<TextObject>(&object.data)) {
            add("text", text->text);
            add("pointSize", text->pointSize);
            add("size", text->size);
            add("color", text->color);
            add("alpha", text->alpha);
            add("padding", text->padding);
            add("spacing", text->spacing);
        } else if (const auto* particle = std::get_if<ParticleObject>(&object.data)) {
            add("parallaxDepth", particle->parallaxDepth);
            add("enabled", particle->instanceOverride.enabled);
            add("alpha", particle->instanceOverride.alpha);
            add("size", particle->instanceOverride.size);
            add("lifetime", particle->instanceOverride.lifetime);
            add("rate", particle->instanceOverride.rate);
            add("speed", particle->instanceOverride.speed);
            add("count", particle->instanceOverride.count);
            add("color", particle->instanceOverride.color);
            add("colorMultiplier", particle->instanceOverride.colorMultiplier);
        } else if (const auto* sound = std::get_if<SoundObject>(&object.data)) {
            descriptor.soundStartsAutomatically = !sound->startSilent;
            add("volume", sound->volume);
        }
        result.push_back(std::move(descriptor));
    }
    return result;
}

struct LayerDynamicPropertyRef final {
    std::string_view name;
    const DynamicValue* value = nullptr;
};

template <typename Callback>
void forEachLayerDynamicProperty(
    const SceneObject& object,
    Callback&& callback
) {
    callback(LayerDynamicPropertyRef{"origin", &object.base.origin});
    callback(LayerDynamicPropertyRef{"scale", &object.base.scale});
    callback(LayerDynamicPropertyRef{"angles", &object.base.angles});
    callback(LayerDynamicPropertyRef{"visible", &object.base.visible});
    if (const auto* image = std::get_if<ImageObject>(&object.data)) {
        callback(LayerDynamicPropertyRef{"alpha", &image->alpha});
        callback(LayerDynamicPropertyRef{"color", &image->color});
        callback(LayerDynamicPropertyRef{"size", &image->size});
        callback(LayerDynamicPropertyRef{
            "parallaxDepth", &image->parallaxDepth
        });
        callback(LayerDynamicPropertyRef{"brightness", &image->brightness});
        callback(LayerDynamicPropertyRef{
            "colorBlendMode", &image->colorBlendMode
        });
    } else if (const auto* text = std::get_if<TextObject>(&object.data)) {
        callback(LayerDynamicPropertyRef{"text", &text->text});
        callback(LayerDynamicPropertyRef{"pointSize", &text->pointSize});
        callback(LayerDynamicPropertyRef{"size", &text->size});
        callback(LayerDynamicPropertyRef{"color", &text->color});
        callback(LayerDynamicPropertyRef{"alpha", &text->alpha});
        callback(LayerDynamicPropertyRef{"padding", &text->padding});
        callback(LayerDynamicPropertyRef{"spacing", &text->spacing});
    } else if (const auto* particle = std::get_if<ParticleObject>(&object.data)) {
        callback(LayerDynamicPropertyRef{
            "parallaxDepth", &particle->parallaxDepth
        });
        callback(LayerDynamicPropertyRef{
            "enabled", &particle->instanceOverride.enabled
        });
        callback(LayerDynamicPropertyRef{
            "alpha", &particle->instanceOverride.alpha
        });
        callback(LayerDynamicPropertyRef{
            "size", &particle->instanceOverride.size
        });
        callback(LayerDynamicPropertyRef{
            "lifetime", &particle->instanceOverride.lifetime
        });
        callback(LayerDynamicPropertyRef{
            "rate", &particle->instanceOverride.rate
        });
        callback(LayerDynamicPropertyRef{
            "speed", &particle->instanceOverride.speed
        });
        callback(LayerDynamicPropertyRef{
            "count", &particle->instanceOverride.count
        });
        callback(LayerDynamicPropertyRef{
            "color", &particle->instanceOverride.color
        });
        callback(LayerDynamicPropertyRef{
            "colorMultiplier", &particle->instanceOverride.colorMultiplier
        });
    } else if (const auto* sound = std::get_if<SoundObject>(&object.data)) {
        callback(LayerDynamicPropertyRef{"volume", &sound->volume});
    }
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
        graphError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            {},
            std::string(description) +
                " must project to three finite numbers"
        );
    }
    return result;
}

bool booleanValue(
    const SceneModel& model,
    const EvaluatedValue& evaluated,
    std::string pointer
) {
    (void)model;
    (void)pointer;
    return evaluated.value.boolean();
}

script::ScriptSceneSnapshot sceneScriptSnapshot(
    const SceneModel& model,
    const std::map<std::string, Value>& properties
) {
    const Scene& scene = model.project().scene;
    script::ScriptSceneSnapshot result;

    const auto evaluate = [&](const DynamicValue& dynamic, std::string pointer) {
        return evaluateDynamicValue(
            model, dynamic, properties, std::move(pointer)
        ).value;
    };
    const auto number = [&](const DynamicValue& dynamic, std::string pointer) {
        const RuntimeValue value = evaluate(dynamic, pointer);
        const double result = value.number();
        if (!std::isfinite(result)) {
            graphError(
                model,
                SceneModelErrorCode::invalidValue,
                std::move(pointer),
                {},
                "SceneScript scene number is not finite"
            );
        }
        return result;
    };
    const auto boolean = [&](const DynamicValue& dynamic, std::string pointer) {
        return evaluate(dynamic, pointer).boolean();
    };
    const auto integer32 = [&](const DynamicValue& dynamic, std::string pointer) {
        const RuntimeValue value = evaluate(dynamic, pointer);
        const std::int64_t result = value.integer();
        if (result < std::numeric_limits<std::int32_t>::min() ||
            result > std::numeric_limits<std::int32_t>::max()) {
            graphError(
                model,
                SceneModelErrorCode::invalidValue,
                std::move(pointer),
                {},
                "SceneScript scene integer is outside Int32 range"
            );
        }
        return static_cast<std::int32_t>(result);
    };
    const auto color = [&](const DynamicValue& dynamic, std::string pointer) {
        const RuntimeValue value = evaluate(dynamic, pointer);
        if (!value.isVector() || value.componentCount() < 3) {
            graphError(
                model,
                SceneModelErrorCode::typeMismatch,
                std::move(pointer),
                {},
                "SceneScript scene color must project to three components"
            );
        }
        const auto& components = value.vector();
        std::array<double, 3> result{
            components[0], components[1], components[2]
        };
        if (!std::all_of(
                result.begin(), result.end(),
                [](double component) { return std::isfinite(component); })) {
            graphError(
                model,
                SceneModelErrorCode::invalidValue,
                std::move(pointer),
                {},
                "SceneScript scene color contains a non-finite component"
            );
        }
        return result;
    };

    const auto general = [&](std::string_view key) -> const DynamicValue& {
        const auto found = scene.generalValues.find(std::string(key));
        if (found == scene.generalValues.end()) {
            graphError(
                model,
                SceneModelErrorCode::missingField,
                "/general/" + std::string(key),
                {},
                "SceneScript scene value is missing"
            );
        }
        return found->second;
    };

    result.bloom = boolean(general("bloom"), "/general/bloom");
    // Linux's SceneObject adapter exposes Int32 projections for these values.
    result.bloomStrength = integer32(
        general("bloomstrength"), "/general/bloomstrength"
    );
    result.bloomThreshold = integer32(
        general("bloomthreshold"), "/general/bloomthreshold"
    );
    // Preserve the pinned Linux contract: clearenabled is wired to bloom,
    // rather than to a separate clear-enabled field.
    result.clearEnabled = result.bloom;
    result.clearColor = color(general("clearcolor"), "/general/clearcolor");
    result.ambientColor = color(
        general("ambientcolor"), "/general/ambientcolor"
    );
    // The upstream Linux adapter currently returns ambientcolor here. This is
    // intentional parity, including the historical accessor quirk.
    result.skylightColor = result.ambientColor;
    result.fieldOfView = number(scene.camera.fieldOfView, "/camera/fov");
    result.nearZ = number(scene.camera.nearPlane, "/camera/nearz");
    result.farZ = number(scene.camera.farPlane, "/camera/farz");
    result.cameraFade = boolean(general("camerafade"), "/general/camerafade");
    result.cameraShake = boolean(general("camerashake"), "/general/camerashake");
    result.cameraShakeSpeed = number(
        general("camerashakespeed"), "/general/camerashakespeed"
    );
    result.cameraShakeAmplitude = number(
        general("camerashakeamplitude"), "/general/camerashakeamplitude"
    );
    result.cameraShakeRoughness = number(
        general("camerashakeroughness"), "/general/camerashakeroughness"
    );
    result.cameraParallax = boolean(
        general("cameraparallax"), "/general/cameraparallax"
    );
    result.cameraParallaxAmount = number(
        general("cameraparallaxamount"), "/general/cameraparallaxamount"
    );
    result.cameraParallaxDelay = number(
        general("cameraparallaxdelay"), "/general/cameraparallaxdelay"
    );
    result.cameraParallaxMouseInfluence = number(
        general("cameraparallaxmouseinfluence"),
        "/general/cameraparallaxmouseinfluence"
    );
    return result;
}

std::shared_ptr<const script::ScriptUserPropertiesSnapshot>
sceneScriptUserPropertiesSnapshot(
    const SceneModel& model,
    const std::map<std::string, Value>& properties
) {
    auto result = std::make_shared<script::ScriptUserPropertiesSnapshot>();
    for (const auto& [name, value] : properties) {
        const auto definition = model.project().properties.find(name);
        if (definition == model.project().properties.end()) {
            graphError(
                model,
                SceneModelErrorCode::danglingReference,
                "/properties/" + name,
                {name},
                "Project user-property snapshot contains an unknown key"
            );
        }
        try {
            if (definition->second.type == PropertyType::color) {
                const auto* source = std::get_if<std::string>(&value.storage);
                if (source == nullptr) {
                    graphError(
                        model,
                        SceneModelErrorCode::typeMismatch,
                        "/properties/" + name,
                        {name},
                        "Color user property must be stored as a color string"
                    );
                }
                const RuntimeValue color = RuntimeValue::colorString(*source);
                if (!color.isVector() || color.componentCount() < 3) {
                    graphError(
                        model,
                        SceneModelErrorCode::typeMismatch,
                        "/properties/" + name,
                        {name},
                        "Color user property must project to a Vec3"
                    );
                }
                result->values.emplace(
                    name, RuntimeValue::vector(color.vector(), 3)
                );
            } else {
                result->values.emplace(name, RuntimeValue::fromValue(value));
            }
        } catch (const SceneModelError&) {
            throw;
        } catch (const std::exception& error) {
            graphError(
                model,
                SceneModelErrorCode::invalidValue,
                "/properties/" + name,
                {name},
                "Project user property cannot be exposed to SceneScript: " +
                    std::string(error.what())
            );
        }
    }
    return result;
}

Vector3 multiply(const Vector3& lhs, const Vector3& rhs) {
    return {lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z};
}

Vector3 add(const Vector3& lhs, const Vector3& rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

ObjectTransform combine(
    const ObjectTransform& parent,
    const ObjectTransform& local
) {
    const Vector3 scaled = multiply(local.origin, parent.scale);
    const double cosine = std::cos(parent.angles.z);
    const double sine = std::sin(parent.angles.z);
    const Vector3 rotated{
        scaled.x * cosine - scaled.y * sine,
        scaled.x * sine + scaled.y * cosine,
        scaled.z,
    };
    return {
        .origin = add(parent.origin, rotated),
        .scale = multiply(local.scale, parent.scale),
        .angles = add(local.angles, parent.angles),
    };
}

}  // namespace

struct SceneGraph::ScriptState final {
    explicit ScriptState(
        std::shared_ptr<script::ScriptLocalStorage> localStorage
    ) : runtime({}, std::move(localStorage)) {}

    struct Instance final {
        std::unique_ptr<script::ScriptInstance> script;
        std::optional<RuntimeValue> connectedUserValue;
    };

    script::ScriptRuntime runtime;
    std::shared_ptr<script::ScriptLayerRegistry> layerRegistry =
        std::make_shared<script::ScriptLayerRegistry>();
    std::shared_ptr<script::ScriptPropertyObjectRegistry>
        propertyObjectRegistry =
            std::make_shared<script::ScriptPropertyObjectRegistry>();
    std::optional<std::uint64_t> propertySnapshotRevision;
    std::optional<script::ScriptSceneSnapshot> sceneSnapshot;
    std::shared_ptr<const script::ScriptUserPropertiesSnapshot> userProperties;
    std::optional<std::uint64_t> baseLayerRevision;
    struct TopologyCache final {
        std::uint64_t revision = 0;
        std::shared_ptr<const std::map<int, std::size_t>> nodeIndices;
        std::vector<std::size_t> initializationOrder;
        std::vector<std::size_t> renderOrder;
    };
    std::optional<TopologyCache> topologyCache;
    std::map<std::string, Instance> instances;
    std::recursive_mutex mutex;
};

struct SceneGraph::EvaluationFrame::Impl final {
    Impl(SceneGraph& owner, SceneFrameInputs frameInputs)
        : graph(owner), frameLock(owner.scriptState_->mutex), inputs(frameInputs),
          properties(owner.model_->propertyState()) {
        if (!std::isfinite(inputs.runtimeSeconds) || inputs.runtimeSeconds < 0 ||
            !std::isfinite(inputs.frameTimeSeconds) || inputs.frameTimeSeconds < 0 ||
            (inputs.timeOfDay &&
                (!std::isfinite(*inputs.timeOfDay) || *inputs.timeOfDay < 0 || *inputs.timeOfDay > 1)) ||
            (inputs.audioSpectrum &&
                !audioSpectrumIsValid(*inputs.audioSpectrum)) ||
            (inputs.canvasSize &&
                (!std::isfinite((*inputs.canvasSize)[0]) ||
                 !std::isfinite((*inputs.canvasSize)[1]) ||
                 (*inputs.canvasSize)[0] <= 0.0 ||
                 (*inputs.canvasSize)[1] <= 0.0)) ||
            !std::isfinite(inputs.pointerX) || !std::isfinite(inputs.pointerY)) {
            graphError(
                *graph.model_, SceneModelErrorCode::invalidValue, "/frameInputs", {},
                "Scene frame inputs must be finite and time values must be non-negative"
            );
        }
        if (!inputs.timeOfDay) {
            inputs.timeOfDay = localDaytimeFraction();
        }
        if (graph.scriptState_->propertySnapshotRevision != properties.revision) {
            graph.scriptState_->sceneSnapshot = sceneScriptSnapshot(
                *graph.model_, *properties.values
            );
            graph.scriptState_->userProperties =
                sceneScriptUserPropertiesSnapshot(
                    *graph.model_, *properties.values
                );
            graph.scriptState_->propertySnapshotRevision = properties.revision;
        }
        if (!inputs.sceneSnapshot) {
            inputs.sceneSnapshot = graph.scriptState_->sceneSnapshot;
        }
        userProperties = graph.scriptState_->userProperties;
        graph.scriptState_->layerRegistry->setRuntimeSeconds(
            inputs.runtimeSeconds
        );
        graph.scriptState_->propertyObjectRegistry->setRuntimeSeconds(
            inputs.runtimeSeconds
        );
        graph.scriptState_->layerRegistry->beginFrame();
        if (graph.scriptState_->baseLayerRevision != properties.revision) {
            graph.scriptState_->layerRegistry->setBaseLayers(
                sceneLayerDescriptors(*graph.model_, *properties.values)
            );
            graph.scriptState_->baseLayerRevision = properties.revision;
        }
        graph.scriptState_->layerRegistry->setSoundRuntimeStates(
            inputs.soundRuntimeStates
        );
    }

    [[nodiscard]] script::ScriptFrameInputs scriptInputs() const {
        return {
            .runtimeSeconds = inputs.runtimeSeconds,
            .frameTimeSeconds = inputs.frameTimeSeconds,
            .timeOfDay = inputs.timeOfDay,
            .isScreensaver = inputs.isScreensaver,
            .audioSpectrum = inputs.audioSpectrum,
            .canvasSize = inputs.canvasSize,
            .sceneSnapshot = inputs.sceneSnapshot,
            .userProperties = userProperties,
            .pointerX = inputs.pointerX,
            .pointerY = inputs.pointerY,
            .cursorWorldPosition = inputs.cursorWorldPosition,
            .pointerLeftDown = inputs.pointerLeftDown,
            .cursorEvents = inputs.cursorEvents,
            .mediaSnapshot = inputs.mediaSnapshot,
        };
    }

    void resolveOwner(
        const std::string& pointer,
        script::ScriptPropertyOwner& owner
    ) const {
        if (owner.type != script::ScriptPropertyOwnerType::none) return;
        if (const auto layerOwner = layerScriptOwner(*graph.model_, pointer)) {
            owner.layerId = layerOwner->id;
            owner.type = script::ScriptPropertyOwnerType::layer;
            owner.property = layerOwner->property;
        }
    }

    [[nodiscard]] ScriptState::Instance& prepareInstance(
        const DynamicValue& dynamic,
        const std::string& pointer,
        const EvaluatedValue& connected,
        std::map<std::string, RuntimeValue> scriptProperties,
        const script::ScriptPropertyOwner& owner
    ) {
        auto& instance = graph.scriptState_->instances[pointer];
        if (!instance.script) {
            instance.script = graph.scriptState_->runtime.createInstance(
                *dynamic.script,
                connected.value,
                std::move(scriptProperties),
                dynamic.user ? dynamic.user->condition : std::nullopt,
                graph.scriptState_->layerRegistry,
                graph.scriptState_->propertyObjectRegistry,
                owner
            );
            if (dynamic.user) {
                instance.connectedUserValue = connected.value;
            }
        } else {
            if (dynamic.user &&
                (!instance.connectedUserValue ||
                 *instance.connectedUserValue != connected.value)) {
                instance.script->updateCurrent(connected.value);
                instance.connectedUserValue = connected.value;
            }
            instance.script->updateProperties(std::move(scriptProperties));
        }
        return instance;
    }

    void initialize(
        const DynamicValue& dynamic,
        const std::string& pointer,
        script::ScriptPropertyOwner owner
    ) {
        if (!dynamic.script || values.contains(pointer)) return;
        resolveOwner(pointer, owner);
        if (!evaluating.emplace(pointer).second) {
            graphError(
                *graph.model_, SceneModelErrorCode::referenceCycle, pointer, {},
                "Dynamic script properties contain a recursive initialization cycle"
            );
        }
        struct EraseGuard final {
            std::set<std::string>& set;
            const std::string& key;
            ~EraseGuard() { set.erase(key); }
        } guard{evaluating, pointer};

        const EvaluatedValue connected = evaluateDynamicValue(
            *graph.model_, dynamic, *properties.values, pointer
        );
        std::map<std::string, RuntimeValue> scriptProperties;
        for (const auto& [name, child] : dynamic.scriptProperties) {
            script::ScriptPropertyOwner childOwner;
            childOwner.layerId = owner.layerId;
            scriptProperties.emplace(
                name,
                evaluate(
                    child,
                    pointer + "/scriptproperties/" + name,
                    childOwner
                ).value
            );
        }
        auto& instance = prepareInstance(
            dynamic,
            pointer,
            connected,
            std::move(scriptProperties),
            owner
        );
        try {
            instance.script->initialize(scriptInputs());
            if (owner.layerId && instance.script->hasCursorCallbacks()) {
                cursorInteractiveLayerIds.insert(*owner.layerId);
            }
        } catch (const script::ScriptError& error) {
            if (error.code() == script::ScriptErrorCode::audioInputUnavailable) {
                return;
            }
            graphError(
                *graph.model_, SceneModelErrorCode::invalidValue, pointer, {},
                std::string("Dynamic script failed: ") + error.what()
            );
        }
    }

    EvaluatedValue evaluate(
        const DynamicValue& dynamic,
        const std::string& pointer,
        script::ScriptPropertyOwner owner = {}
    ) {
        const EvaluatedValue connected = evaluateDynamicValue(
            *graph.model_, dynamic, *properties.values, pointer
        );
        resolveOwner(pointer, owner);
        EvaluatedValue connectedWithOverlay = connected;
        const auto readOwner = [&]() -> std::optional<RuntimeValue> {
            if (owner.type == script::ScriptPropertyOwnerType::layer &&
                owner.layerId && !owner.property.empty()) {
                return graph.scriptState_->layerRegistry->read(
                    *owner.layerId, owner.property
                );
            }
            if ((owner.type == script::ScriptPropertyOwnerType::effect ||
                 owner.type == script::ScriptPropertyOwnerType::material) &&
                !owner.objectId.empty() && !owner.property.empty()) {
                return graph.scriptState_->propertyObjectRegistry->read(
                    owner.objectId, owner.property
                );
            }
            return std::nullopt;
        };
        if (!dynamic.script) {
            if (const auto ownerValue = readOwner()) {
                connectedWithOverlay.value = *ownerValue;
                connectedWithOverlay.source = dynamic.user
                    ? DynamicValueSource::user
                    : DynamicValueSource::literal;
            }
        }
        if (!dynamic.script) {
            return connectedWithOverlay;
        }
        if (const auto found = values.find(pointer); found != values.end()) {
            ++scriptStats.at(pointer).cacheHitCount;
            return found->second;
        }
        if (!evaluating.emplace(pointer).second) {
            graphError(
                *graph.model_, SceneModelErrorCode::referenceCycle, pointer, {},
                "Dynamic script properties contain a recursive evaluation cycle"
            );
        }
        struct EraseGuard final {
            std::set<std::string>& set;
            const std::string& key;
            ~EraseGuard() { set.erase(key); }
        } guard{evaluating, pointer};

        std::map<std::string, RuntimeValue> scriptProperties;
        for (const auto& [name, child] : dynamic.scriptProperties) {
            script::ScriptPropertyOwner childOwner;
            childOwner.layerId = owner.layerId;
            scriptProperties.emplace(
                name,
                evaluate(
                    child,
                    pointer + "/scriptproperties/" + name,
                    childOwner
                ).value
            );
        }

        auto& instance = prepareInstance(
            dynamic,
            pointer,
            connected,
            std::move(scriptProperties),
            owner
        );
        auto& stats = scriptStats[pointer];
        stats.jsonPointer = pointer;
        ++stats.executionCount;
        try {
            RuntimeValue evaluated = instance.script->evaluate(scriptInputs());
            if (owner.layerId && instance.script->hasCursorCallbacks()) {
                cursorInteractiveLayerIds.insert(*owner.layerId);
            }
            EvaluatedValue result{
                .value = std::move(evaluated),
                .source = DynamicValueSource::script,
            };
            values.emplace(pointer, result);
            return result;
        } catch (const script::ScriptError& error) {
            if (error.code() == script::ScriptErrorCode::audioInputUnavailable) {
                if (owner.layerId && instance.script &&
                    instance.script->hasCursorCallbacks()) {
                    cursorInteractiveLayerIds.insert(*owner.layerId);
                }
                EvaluatedValue unavailable{
                    .value = instance.script->currentValue(),
                    .source = DynamicValueSource::scriptUnavailable,
                };
                stats.status = EvaluationFrame::ScriptEvaluationStatus::unavailable;
                values.emplace(pointer, unavailable);
                return unavailable;
            }
            graphError(
                *graph.model_, SceneModelErrorCode::invalidValue, pointer, {},
                std::string("Dynamic script failed: ") + error.what()
            );
        }
    }

    SceneGraph& graph;
    // A QuickJS instance is stateful. Keep every scripted value in one frame
    // serialized as a unit so concurrent snapshots cannot interleave updates.
    std::unique_lock<std::recursive_mutex> frameLock;
    SceneFrameInputs inputs;
    PropertyStateSnapshot properties;
    std::shared_ptr<const script::ScriptUserPropertiesSnapshot> userProperties;
    std::map<std::string, EvaluatedValue> values;
    std::map<std::string, EvaluationFrame::ScriptEvaluationStats> scriptStats;
    std::set<int> cursorInteractiveLayerIds;
    std::set<std::string> evaluating;
};

EvaluatedValue evaluateDynamicValue(
    const SceneModel& model,
    const DynamicValue& dynamic,
    const std::map<std::string, Value>& properties,
    std::string pointer
) {
    if (!dynamic.user) {
        return {
            .value = dynamic.value,
            .source = dynamic.script
                ? DynamicValueSource::scriptInitial
                : DynamicValueSource::literal,
        };
    }

    const auto property = properties.find(dynamic.user->property);
    if (property == properties.end()) {
        graphError(
            model,
            SceneModelErrorCode::danglingReference,
            std::move(pointer),
            {dynamic.user->property},
            "User binding has no current value for property '" +
                dynamic.user->property + "'"
        );
    }

    RuntimeValue value;
    try {
        const auto definition = model.project().properties.find(
            dynamic.user->property
        );
        const bool color = definition != model.project().properties.end() &&
            definition->second.type == PropertyType::color;
        if (color) {
            const auto* source = std::get_if<std::string>(&property->second.storage);
            value = source == nullptr
                ? RuntimeValue::fromValue(property->second)
                : RuntimeValue::colorString(*source);
        } else {
            value = RuntimeValue::fromValue(property->second);
        }
    } catch (const std::exception& error) {
        graphError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            {dynamic.user->property},
            "User property '" + dynamic.user->property +
                "' cannot update its DynamicValue: " + error.what()
        );
    }
    if (dynamic.user->condition) {
        // Upstream applies a condition only when the connected property is a
        // string. Other property types propagate their value unchanged.
        if (value.type() == RuntimeValueType::string) {
            value = RuntimeValue::condition(
                value.string(),
                *dynamic.user->condition
            );
        }
    }
    return {
        .value = std::move(value),
        // A connected property replaces the script's current input; it does
        // not turn a scripted DynamicValue into a pure user value. Static
        // snapshots must still report that QuickJS has not run yet.
        .source = dynamic.script
            ? DynamicValueSource::scriptInitial
            : DynamicValueSource::user,
    };
}

const SceneGraphNodeSnapshot* SceneGraphSnapshot::node(int id) const noexcept {
    if (!nodeIndices) return nullptr;
    const auto found = nodeIndices->find(id);
    return found == nodeIndices->end() ? nullptr : &nodes[found->second];
}

std::shared_ptr<SceneGraph> SceneGraph::create(
    std::shared_ptr<SceneModel> model,
    std::shared_ptr<script::ScriptLocalStorage> localStorage
) {
    return std::shared_ptr<SceneGraph>(new SceneGraph(
        std::move(model),
        std::move(localStorage)
    ));
}

SceneGraph::SceneGraph(
    std::shared_ptr<SceneModel> model,
    std::shared_ptr<script::ScriptLocalStorage> localStorage
) : model_(std::move(model)),
    scriptState_(std::make_unique<ScriptState>(std::move(localStorage))) {
    if (!model_) {
        throw SceneModelError(
            SceneModelErrorCode::invalidValue,
            {},
            {},
            {},
            "Scene model is required to create a scene graph"
        );
    }
    const std::weak_ptr<SceneModel> weakModel = model_;
    scriptState_->layerRegistry->setTextureAnimationResolver(
        [weakModel](std::string_view assetIdentity)
            -> std::optional<script::ScriptTextureAnimationMetadata> {
            const auto model = weakModel.lock();
            if (!model) {
                throw std::runtime_error(
                    "Scene model is unavailable while resolving texture animation"
                );
            }
            try {
                const Texture texture = model->runtime()->assetResolver().parseTexture(
                    assetIdentity
                );
                if (!texture.isAnimated() || texture.frames.empty()) {
                    return std::nullopt;
                }
                script::ScriptTextureAnimationMetadata result{
                    .assetIdentity = std::string(assetIdentity),
                };
                result.frameDurations.reserve(texture.frames.size());
                for (const TextureFrame& frame : texture.frames) {
                    result.frameDurations.push_back(frame.frameTime);
                }
                return result;
            } catch (const FormatError& error) {
                throw SceneModelError(
                    SceneModelErrorCode::assetFailure,
                    model->project().scene.assetPath,
                    "/textureAnimations/" + std::string(assetIdentity),
                    {std::string(assetIdentity)},
                    error.what()
                );
            }
        }
    );
    const auto& objects = model_->project().scene.objects;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        objectIndices_.emplace(objects[index].base.id, index);
    }
    initializationOrder_ = buildOrder(*model_, objectIndices_, true);
    renderOrder_ = buildOrder(*model_, objectIndices_, false);
}

SceneGraph::~SceneGraph() = default;

SceneGraphSnapshot SceneGraph::snapshot() const {
    PropertyStateSnapshot propertyState = model_->propertyState();
    const auto& objects = model_->project().scene.objects;

    SceneGraphSnapshot result;
    result.modelRevision = propertyState.revision;
    result.propertyValues = std::move(propertyState.values);
    result.initializationOrder = initializationOrder_;
    result.renderOrder = renderOrder_;
    result.nodeIndices = std::make_shared<const std::map<int, std::size_t>>(
        objectIndices_
    );
    {
        std::lock_guard lock(scriptState_->mutex);
        result.textureAnimations = scriptState_->layerRegistry->textureAnimationSnapshots();
        result.sounds = scriptState_->layerRegistry->soundSnapshots();
    }
    result.nodes.reserve(objects.size());

    const auto& snapshotProperties = *result.propertyValues;

    for (std::size_t index = 0; index < objects.size(); ++index) {
        const ObjectBase& object = objects[index].base;
        SceneGraphNodeSnapshot node;
        node.objectIndex = index;
        node.id = object.id;
        node.parent = object.parent;
        node.disablePropagation = object.disablePropagation;
        node.origin = evaluateDynamicValue(
            *model_, object.origin, snapshotProperties,
            objectPointer(index, "origin")
        );
        node.scale = evaluateDynamicValue(
            *model_, object.scale, snapshotProperties,
            objectPointer(index, "scale")
        );
        node.angles = evaluateDynamicValue(
            *model_, object.angles, snapshotProperties,
            objectPointer(index, "angles")
        );
        node.visible = evaluateDynamicValue(
            *model_, object.visible, snapshotProperties,
            objectPointer(index, "visible")
        );
        node.localTransform = {
            .origin = vector3Value(
                *model_, node.origin, objectPointer(index, "origin"), "Object origin"
            ),
            .scale = vector3Value(
                *model_, node.scale, objectPointer(index, "scale"), "Object scale"
            ),
            .angles = vector3Value(
                *model_, node.angles, objectPointer(index, "angles"), "Object angles"
            ),
        };
        node.worldTransform = node.localTransform;
        node.isVisible = booleanValue(
            *model_, node.visible, objectPointer(index, "visible")
        );
        result.nodes.push_back(std::move(node));
    }

    // Initialization order guarantees every parent has already been resolved,
    // even when the child appeared first in scene.json.
    for (const std::size_t index : initializationOrder_) {
        SceneGraphNodeSnapshot& node = result.nodes[index];
        if (!node.parent) {
            continue;
        }
        const std::size_t parentIndex = objectIndices_.at(*node.parent);
        node.worldTransform = combine(
            result.nodes[parentIndex].worldTransform,
            node.localTransform
        );
    }
    return result;
}

SceneGraph::EvaluationFrame::EvaluationFrame(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
SceneGraph::EvaluationFrame::~EvaluationFrame() = default;
EvaluatedValue SceneGraph::EvaluationFrame::evaluate(
    const DynamicValue& dynamic,
    std::string pointer,
    script::ScriptPropertyOwner owner
) {
    return impl_->evaluate(dynamic, pointer, std::move(owner));
}
void SceneGraph::EvaluationFrame::initialize(
    const DynamicValue& dynamic,
    std::string pointer,
    script::ScriptPropertyOwner owner
) {
    impl_->initialize(dynamic, pointer, std::move(owner));
}
void SceneGraph::EvaluationFrame::registerScriptPropertyObject(
    script::ScriptPropertyObjectDescriptor descriptor
) {
    impl_->graph.scriptState_->propertyObjectRegistry->setBaseObject(
        std::move(descriptor)
    );
}
bool SceneGraph::EvaluationFrame::scriptPropertyObjectsCurrent() const {
    return impl_->graph.scriptState_->propertyObjectRegistry
        ->baseObjectsCurrent(impl_->properties.revision);
}
void SceneGraph::EvaluationFrame::commitScriptPropertyObjects() {
    impl_->graph.scriptState_->propertyObjectRegistry
        ->commitBaseObjectsRevision(impl_->properties.revision);
}
std::uint64_t SceneGraph::EvaluationFrame::modelRevision() const noexcept {
    return impl_->properties.revision;
}
const std::map<std::string, Value>&
SceneGraph::EvaluationFrame::propertyValues() const noexcept {
    return *impl_->properties.values;
}
std::shared_ptr<const PropertyValueMap>
SceneGraph::EvaluationFrame::propertyValuesSnapshot() const noexcept {
    return impl_->properties.values;
}
const std::map<std::string, EvaluatedValue>&
SceneGraph::EvaluationFrame::evaluatedScriptValues() const noexcept {
    return impl_->values;
}
std::vector<SceneGraph::EvaluationFrame::ScriptEvaluationStats>
SceneGraph::EvaluationFrame::scriptEvaluationStats() const {
    std::vector<ScriptEvaluationStats> result;
    result.reserve(impl_->scriptStats.size());
    for (const auto& [pointer, stats] : impl_->scriptStats) result.push_back(stats);
    return result;
}

std::vector<script::ScriptTextureAnimationSnapshot>
SceneGraph::EvaluationFrame::textureAnimationSnapshots() const {
    return impl_->graph.scriptState_->layerRegistry->textureAnimationSnapshots();
}

std::vector<script::ScriptSoundSnapshot>
SceneGraph::EvaluationFrame::soundSnapshots() const {
    return impl_->graph.scriptState_->layerRegistry->soundSnapshots();
}

std::vector<int> SceneGraph::EvaluationFrame::cursorInteractiveLayerIds() const {
    return {
        impl_->cursorInteractiveLayerIds.begin(),
        impl_->cursorInteractiveLayerIds.end(),
    };
}

std::optional<RuntimeValue> SceneGraph::EvaluationFrame::layerProperty(
    int layerId,
    std::string_view property
) const {
    return impl_->graph.scriptState_->layerRegistry->read(layerId, property);
}

std::unique_ptr<SceneGraph::EvaluationFrame> SceneGraph::evaluationFrame(
    const SceneFrameInputs& inputs
) {
    return std::unique_ptr<EvaluationFrame>(new EvaluationFrame(
        std::make_unique<EvaluationFrame::Impl>(*this, inputs)
    ));
}

SceneGraphSnapshot SceneGraph::snapshot(EvaluationFrame& frame) const {
    const auto& objects = model_->project().scene.objects;
    SceneGraphSnapshot result;
    result.modelRevision = frame.modelRevision();
    result.propertyValues = frame.propertyValuesSnapshot();
    std::map<int, std::map<std::string, EvaluatedValue>> evaluatedProperties;
    const auto layersBeforeEvaluation = scriptState_->layerRegistry->references();
    for (const script::ScriptLayerReference& layer : layersBeforeEvaluation) {
        if (layer.dynamic) continue;
        if (layer.sourceObjectIndex >= objects.size()) {
            graphError(
                *model_, SceneModelErrorCode::danglingReference,
                "/objects", {std::to_string(layer.id)},
                "SceneScript layer references an unavailable source object"
            );
        }
        const SceneObject& object = objects[layer.sourceObjectIndex];
        forEachLayerDynamicProperty(object, [&](
            const LayerDynamicPropertyRef& property
        ) {
            if (frameTraceEnabled() && property.value->script) {
                std::fprintf(
                    stderr,
                    "[SceneFrameTrace] graph.script.evaluate "
                    "layer=%d source=%zu property=%.*s\n",
                    layer.id,
                    layer.sourceObjectIndex,
                    static_cast<int>(property.name.size()),
                    property.name.data()
                );
            }
            evaluatedProperties[layer.id].insert_or_assign(
                std::string(property.name),
                frame.evaluate(
                    *property.value,
                    objectPointer(layer.sourceObjectIndex, property.name),
                    script::ScriptPropertyOwner{
                        .layerId = layer.id,
                        .type = script::ScriptPropertyOwnerType::layer,
                        .property = std::string(property.name),
                    }
                )
            );
        });
    }

    script::ScriptLayerRegistrySnapshot layerSnapshot =
        scriptState_->layerRegistry->snapshot();
    auto& layers = layerSnapshot.layers;
    result.nodes.reserve(layers.size());
    const bool topologyCurrent = scriptState_->topologyCache &&
        scriptState_->topologyCache->revision == layerSnapshot.topologyRevision;
    std::shared_ptr<std::map<int, std::size_t>> pendingNodeIndices;
    if (topologyCurrent) {
        result.nodeIndices = scriptState_->topologyCache->nodeIndices;
    } else {
        pendingNodeIndices = std::make_shared<std::map<int, std::size_t>>();
    }
    for (script::ScriptLayerSnapshot& layer : layers) {
        if (layer.sourceObjectIndex >= objects.size()) {
            graphError(
                *model_, SceneModelErrorCode::danglingReference,
                "/objects", {std::to_string(layer.id)},
                "SceneScript layer references an unavailable source object"
            );
        }
        SceneGraphNodeSnapshot node;
        node.objectIndex = layer.sourceObjectIndex;
        node.id = layer.id;
        node.dynamic = layer.dynamic;
        node.parent = layer.parent;
        node.disablePropagation = layer.disablePropagation;
        const auto value = [&](std::string_view property) -> EvaluatedValue {
            if (const auto layerValues = evaluatedProperties.find(layer.id);
                layerValues != evaluatedProperties.end()) {
                if (const auto found = layerValues->second.find(
                        std::string(property)
                    ); found != layerValues->second.end()) {
                    return found->second;
                }
            }
            const auto found = layer.properties.find(std::string(property));
            if (found == layer.properties.end()) {
                graphError(
                    *model_, SceneModelErrorCode::missingField,
                    objectPointer(layer.sourceObjectIndex, property), {},
                    "SceneScript layer snapshot is missing property '" +
                        std::string(property) + "'"
                );
            }
            return {
                .value = found->second,
                .source = DynamicValueSource::literal,
            };
        };
        node.origin = value("origin");
        node.scale = value("scale");
        node.angles = value("angles");
        node.visible = value("visible");
        node.layerProperties = std::move(layer.properties);
        node.localTransform = {
            .origin = vector3Value(
                *model_, node.origin,
                objectPointer(layer.sourceObjectIndex, "origin"), "Object origin"
            ),
            .scale = vector3Value(
                *model_, node.scale,
                objectPointer(layer.sourceObjectIndex, "scale"), "Object scale"
            ),
            .angles = vector3Value(
                *model_, node.angles,
                objectPointer(layer.sourceObjectIndex, "angles"), "Object angles"
            ),
        };
        node.worldTransform = node.localTransform;
        node.isVisible = booleanValue(
            *model_, node.visible,
            objectPointer(layer.sourceObjectIndex, "visible")
        );
        const std::size_t nodeIndex = result.nodes.size();
        if (!topologyCurrent &&
            !pendingNodeIndices->emplace(node.id, nodeIndex).second) {
            graphError(
                *model_, SceneModelErrorCode::duplicateId,
                objectPointer(layer.sourceObjectIndex, "id"), {},
                "SceneGraph snapshot contains duplicate runtime layer id"
            );
        }
        result.nodes.push_back(std::move(node));
    }
    if (!topologyCurrent) {
        result.nodeIndices = pendingNodeIndices;
        result.renderOrder.reserve(result.nodes.size());
        std::vector<VisitState> renderStates(
            result.nodes.size(), VisitState::unvisited
        );
        const auto appendRenderNode = [&](auto&& self, std::size_t index) -> void {
            if (renderStates[index] == VisitState::visited) return;
            const SceneGraphNodeSnapshot& node = result.nodes[index];
            if (renderStates[index] == VisitState::visiting) {
                graphError(
                    *model_, SceneModelErrorCode::referenceCycle,
                    objectPointer(node.objectIndex, "dependencies"),
                    {std::to_string(node.id)},
                    "SceneScript runtime layer dependency cycle detected"
                );
            }
            renderStates[index] = VisitState::visiting;
            const SceneObject& sourceObject = objects.at(node.objectIndex);
            for (std::size_t dependencyIndex = 0;
                 dependencyIndex < sourceObject.base.dependencies.size();
                 ++dependencyIndex) {
                const int dependencyId =
                    sourceObject.base.dependencies[dependencyIndex].id;
                if (dependencyId == sourceObject.base.id) continue;
                const auto dependency = result.nodeIndices->find(dependencyId);
                if (dependency == result.nodeIndices->end()) {
                    graphError(
                        *model_, SceneModelErrorCode::danglingReference,
                        objectPointer(node.objectIndex, "dependencies") + '/' +
                            std::to_string(dependencyIndex),
                        {std::to_string(node.id)},
                        "SceneScript runtime layer references an unavailable dependency"
                    );
                }
                self(self, dependency->second);
            }
            renderStates[index] = VisitState::visited;
            result.renderOrder.push_back(index);
        };
        for (std::size_t index = 0; index < result.nodes.size(); ++index) {
            appendRenderNode(appendRenderNode, index);
        }

        result.initializationOrder.reserve(result.nodes.size());
        std::vector<VisitState> states(
            result.nodes.size(), VisitState::unvisited
        );
        const auto appendInitializationNode = [&](
            auto&& self,
            std::size_t index
        ) -> void {
            if (states[index] == VisitState::visited) return;
            if (states[index] == VisitState::visiting) {
                graphError(
                    *model_, SceneModelErrorCode::referenceCycle,
                    objectPointer(result.nodes[index].objectIndex, "parent"),
                    {std::to_string(result.nodes[index].id)},
                    "SceneScript runtime layer parent cycle detected"
                );
            }
            states[index] = VisitState::visiting;
            const SceneGraphNodeSnapshot& node = result.nodes[index];
            if (node.parent) {
                const auto parent = result.nodeIndices->find(*node.parent);
                if (parent == result.nodeIndices->end()) {
                    graphError(
                        *model_, SceneModelErrorCode::danglingReference,
                        objectPointer(node.objectIndex, "parent"),
                        {std::to_string(*node.parent)},
                        "SceneScript runtime layer references an unavailable parent"
                    );
                }
                self(self, parent->second);
            }
            states[index] = VisitState::visited;
            result.initializationOrder.push_back(index);
        };
        for (std::size_t index = 0; index < result.nodes.size(); ++index) {
            appendInitializationNode(appendInitializationNode, index);
        }
        scriptState_->topologyCache = ScriptState::TopologyCache{
            .revision = layerSnapshot.topologyRevision,
            .nodeIndices = result.nodeIndices,
            .initializationOrder = result.initializationOrder,
            .renderOrder = result.renderOrder,
        };
    } else {
        result.initializationOrder =
            scriptState_->topologyCache->initializationOrder;
        result.renderOrder = scriptState_->topologyCache->renderOrder;
    }

    for (const std::size_t index : result.initializationOrder) {
        SceneGraphNodeSnapshot& node = result.nodes[index];
        if (!node.parent) continue;
        const std::size_t parentIndex = result.nodeIndices->at(*node.parent);
        node.worldTransform = combine(
            result.nodes[parentIndex].worldTransform,
            node.localTransform
        );
    }
    // Dynamic object fields can execute SceneScript callbacks that mutate
    // texture-animation and sound state through the shared layer registry.
    // Capture those behavior snapshots only after every dynamic field has
    // been evaluated, otherwise this immutable graph snapshot can contain
    // state from the beginning of the frame while its node values represent
    // the end of the frame.
    result.textureAnimations = frame.textureAnimationSnapshots();
    result.sounds = frame.soundSnapshots();
    return result;
}

std::shared_ptr<const SceneModel> SceneGraph::model() const noexcept {
    return model_;
}

}  // namespace we::scene
