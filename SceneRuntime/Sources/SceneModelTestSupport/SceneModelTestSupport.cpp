#include <SceneModelTestSupport/SceneModelTestSupport.h>

#include <SceneCore/Runtime.hpp>
#include <SceneModel/SceneModel.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

struct WESceneModelTestHandle {
    std::shared_ptr<we::scene::SceneModel> model;
    WESceneModelTestStats stats{};
    std::map<std::size_t, std::array<std::string, 3>> textValueStrings;
    std::map<
        std::pair<std::size_t, std::size_t>,
        std::array<std::string, 2>
    > particleValueStrings;
};

namespace {

using we::scene::DynamicValue;
using we::scene::Effect;
using we::scene::EffectPass;
using we::scene::ImageEffect;
using we::scene::ImageObject;
using we::scene::Material;
using we::scene::MaterialPass;
using we::scene::Model;
using we::scene::SceneModel;
using we::scene::SceneModelError;
using we::scene::SceneObject;
using we::scene::SceneProject;
using we::scene::SoundObject;
using we::scene::ParticleAlphaFadeOperator;
using we::scene::ParticleAlphaRandomInitializer;
using we::scene::ParticleAngularMovementOperator;
using we::scene::ParticleAngularVelocityRandomInitializer;
using we::scene::ParticleColorRandomInitializer;
using we::scene::ParticleColorChangeOperator;
using we::scene::ParticleControlPointAttractOperator;
using we::scene::ParticleLifetimeRandomInitializer;
using we::scene::ParticleMapSequenceAroundControlPointInitializer;
using we::scene::ParticleMovementOperator;
using we::scene::ParticleObject;
using we::scene::ParticleOscillateAlphaOperator;
using we::scene::ParticleOscillatePositionOperator;
using we::scene::ParticleOscillateSizeOperator;
using we::scene::ParticleRotationRandomInitializer;
using we::scene::ParticleSizeRandomInitializer;
using we::scene::ParticleSizeChangeOperator;
using we::scene::ParticleAlphaChangeOperator;
using we::scene::ParticleTurbulenceOperator;
using we::scene::ParticleVortexOperator;
using we::scene::ParticleTurbulentVelocityRandomInitializer;
using we::scene::ParticleVelocityRandomInitializer;
using we::scene::TextureSlot;
using we::scene::TextureSlots;
using we::scene::TextObject;

constexpr std::string_view invalidHandleMessage =
    "Scene model test handle is null";

void writeError(char* buffer, size_t capacity, std::string_view message) noexcept {
    if (buffer == nullptr || capacity == 0) {
        return;
    }
    const size_t count = std::min(capacity - 1, message.size());
    if (count != 0) {
        std::memcpy(buffer, message.data(), count);
    }
    buffer[count] = '\0';
}

template <typename Fn>
int guarded(
    char* errorBuffer,
    size_t errorBufferSize,
    Fn&& function
) noexcept {
    try {
        return function();
    } catch (const SceneModelError& error) {
        writeError(errorBuffer, errorBufferSize, error.what());
    } catch (const std::exception& error) {
        writeError(errorBuffer, errorBufferSize, error.what());
    } catch (...) {
        writeError(
            errorBuffer,
            errorBufferSize,
            "Unknown exception while querying the scene model"
        );
    }
    return 0;
}

bool requireHandle(
    const WESceneModelTestHandle* handle,
    char* errorBuffer,
    size_t errorBufferSize
) noexcept {
    if (handle != nullptr) {
        return true;
    }
    writeError(errorBuffer, errorBufferSize, invalidHandleMessage);
    return false;
}

struct Counter {
    WESceneModelTestStats stats{};
    std::unordered_set<std::string> effectDefinitions;
    std::unordered_set<std::string> effectMaterials;
    std::unordered_set<std::string> objectModels;
    std::unordered_set<std::string> objectMaterials;
    std::unordered_set<std::string> visitedMaterials;
    std::unordered_set<std::string> visitedModels;
    std::unordered_set<std::string> visitedEffects;

    void dynamic(const DynamicValue& value) {
        if (value.script.has_value()) {
            ++stats.dynamic_script_count;
        }
        if (value.user.has_value()) {
            ++stats.dynamic_user_count;
            if (value.user->condition.has_value()) {
                ++stats.dynamic_condition_count;
            }
        }
        if (!value.scriptProperties.empty()) {
            ++stats.dynamic_script_property_count;
        }
        for (const auto& [name, property] : value.scriptProperties) {
            (void)name;
            dynamic(property);
        }
    }

    void material(
        const std::shared_ptr<const Material>& materialValue,
        bool effectRole
    ) {
        if (!materialValue) {
            return;
        }
        if (effectRole && !materialValue->assetPath.empty()) {
            effectMaterials.insert(materialValue->assetPath);
        }
        if (!visitedMaterials.insert(materialValue->assetPath).second) {
            return;
        }
        for (const MaterialPass& pass : materialValue->passes) {
            for (const auto& [name, value] : pass.constants) {
                (void)name;
                dynamic(value);
            }
        }
    }

    void model(const std::shared_ptr<const Model>& modelValue) {
        if (!modelValue) {
            return;
        }
        if (!modelValue->assetPath.empty()) {
            objectModels.insert(modelValue->assetPath);
        }
        if (!visitedModels.insert(modelValue->assetPath).second) {
            return;
        }
        material(modelValue->material, false);
        if (modelValue->cropOffset.has_value()) {
            dynamic(*modelValue->cropOffset);
        }
    }

    void effect(const std::shared_ptr<const Effect>& effectValue) {
        if (!effectValue) {
            return;
        }
        if (!effectValue->assetPath.empty()) {
            effectDefinitions.insert(effectValue->assetPath);
        }
        if (!visitedEffects.insert(effectValue->assetPath).second) {
            return;
        }
        stats.total_effect_dependency_count +=
            static_cast<uint64_t>(effectValue->dependencies.size());
        for (const EffectPass& pass : effectValue->passes) {
            material(pass.material, true);
        }
    }
};

void countProject(const SceneProject& project, Counter& counter) {
    const auto& scene = project.scene;
    counter.dynamic(scene.camera.center);
    counter.dynamic(scene.camera.eye);
    counter.dynamic(scene.camera.up);
    counter.dynamic(scene.camera.nearPlane);
    counter.dynamic(scene.camera.farPlane);
    counter.dynamic(scene.camera.fieldOfView);
    for (const auto& [name, value] : scene.generalValues) {
        (void)name;
        counter.dynamic(value);
    }

    counter.stats.object_count = static_cast<uint64_t>(scene.objects.size());
    for (const SceneObject& object : scene.objects) {
        counter.dynamic(object.base.origin);
        counter.dynamic(object.base.scale);
        counter.dynamic(object.base.angles);
        counter.dynamic(object.base.visible);

        if (const auto* image = std::get_if<ImageObject>(&object.data)) {
            ++counter.stats.image_object_count;
            counter.model(image->model);
            counter.dynamic(image->alpha);
            counter.dynamic(image->color);
            counter.dynamic(image->size);
            counter.dynamic(image->parallaxDepth);
            counter.dynamic(image->brightness);
            counter.dynamic(image->colorBlendMode);
            for (const ImageEffect& imageEffect : image->effects) {
                ++counter.stats.effect_instance_count;
                counter.effect(imageEffect.effect);
                counter.dynamic(imageEffect.visible);
                counter.stats.effect_override_pass_count +=
                    static_cast<uint64_t>(imageEffect.passOverrides.size());
                for (const auto& overridePass : imageEffect.passOverrides) {
                    for (const auto& [name, value] : overridePass.constants) {
                        (void)name;
                        counter.dynamic(value);
                    }
                }
            }
        } else if (const auto* text = std::get_if<we::scene::TextObject>(
                       &object.data
                   )) {
            ++counter.stats.text_object_count;
            counter.dynamic(text->text);
            counter.dynamic(text->pointSize);
            counter.dynamic(text->size);
            counter.dynamic(text->color);
            counter.dynamic(text->alpha);
            counter.dynamic(text->padding);
            counter.dynamic(text->spacing);
        } else if (std::get_if<SoundObject>(&object.data) != nullptr) {
            ++counter.stats.sound_object_count;
            const auto& sound = std::get<SoundObject>(object.data);
            counter.dynamic(sound.volume);
        } else if (const auto* particle = std::get_if<ParticleObject>(
                       &object.data
                   )) {
            ++counter.stats.particle_object_count;
            counter.dynamic(particle->parallaxDepth);
            counter.dynamic(particle->instanceOverride.enabled);
            counter.dynamic(particle->instanceOverride.alpha);
            counter.dynamic(particle->instanceOverride.size);
            counter.dynamic(particle->instanceOverride.lifetime);
            counter.dynamic(particle->instanceOverride.rate);
            counter.dynamic(particle->instanceOverride.speed);
            counter.dynamic(particle->instanceOverride.count);
            counter.dynamic(particle->instanceOverride.color);
            counter.dynamic(particle->instanceOverride.colorMultiplier);
            if (particle->definition) {
                counter.material(particle->definition->material, false);
                for (const auto& initializer : particle->definition->initializers) {
                    std::visit([&](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<
                                          T,
                                          ParticleTurbulentVelocityRandomInitializer
                                      >) {
                            counter.dynamic(value.speedMinimum);
                            counter.dynamic(value.speedMaximum);
                            counter.dynamic(value.scale);
                            counter.dynamic(value.offset);
                            counter.dynamic(value.forward);
                            counter.dynamic(value.timeScale);
                            counter.dynamic(value.phaseMinimum);
                            counter.dynamic(value.phaseMaximum);
                            counter.dynamic(value.right);
                        } else if constexpr (std::is_same_v<
                                                 T,
                                                 ParticleMapSequenceAroundControlPointInitializer
                                             >) {
                            counter.dynamic(value.controlPoint);
                            counter.dynamic(value.count);
                            counter.dynamic(value.speedMinimum);
                            counter.dynamic(value.speedMaximum);
                        } else {
                            counter.dynamic(value.minimum);
                            counter.dynamic(value.maximum);
                        }
                        if constexpr (std::is_same_v<T, ParticleSizeRandomInitializer> ||
                                      std::is_same_v<
                                          T,
                                          ParticleAngularVelocityRandomInitializer
                                      >) {
                            counter.dynamic(value.exponent);
                        }
                    }, initializer);
                }
                for (const auto& particleOperator : particle->definition->operators) {
                    std::visit([&](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, ParticleMovementOperator>) {
                            counter.dynamic(value.drag);
                            counter.dynamic(value.gravity);
                        } else if constexpr (std::is_same_v<T, ParticleAlphaFadeOperator>) {
                            counter.dynamic(value.fadeInTime);
                            counter.dynamic(value.fadeOutTime);
                        } else if constexpr (std::is_same_v<
                                                 T,
                                                 ParticleAngularMovementOperator
                                             >) {
                            counter.dynamic(value.drag);
                            counter.dynamic(value.force);
                        } else if constexpr (std::is_same_v<
                                                 T,
                                                 ParticleOscillatePositionOperator
                                             >) {
                            counter.dynamic(value.frequencyMinimum);
                            counter.dynamic(value.frequencyMaximum);
                            counter.dynamic(value.scaleMinimum);
                            counter.dynamic(value.scaleMaximum);
                            counter.dynamic(value.phaseMinimum);
                            counter.dynamic(value.phaseMaximum);
                            counter.dynamic(value.mask);
                        } else if constexpr (std::is_same_v<
                                                 T,
                                                 ParticleOscillateAlphaOperator
                                             >) {
                            counter.dynamic(value.frequencyMinimum);
                            counter.dynamic(value.frequencyMaximum);
                            counter.dynamic(value.scaleMinimum);
                            counter.dynamic(value.scaleMaximum);
                            counter.dynamic(value.phaseMinimum);
                            counter.dynamic(value.phaseMaximum);
                        } else if constexpr (std::is_same_v<
                                                 T,
                                                 ParticleOscillateSizeOperator
                                             >) {
                            counter.dynamic(value.frequencyMinimum);
                            counter.dynamic(value.frequencyMaximum);
                            counter.dynamic(value.scaleMinimum);
                            counter.dynamic(value.scaleMaximum);
                            counter.dynamic(value.phaseMinimum);
                            counter.dynamic(value.phaseMaximum);
                        } else if constexpr (std::is_same_v<
                                                 T,
                                                 ParticleControlPointAttractOperator
                                             >) {
                            counter.dynamic(value.origin);
                            counter.dynamic(value.scale);
                            counter.dynamic(value.threshold);
                        } else if constexpr (
                            std::is_same_v<T, ParticleSizeChangeOperator> ||
                            std::is_same_v<T, ParticleAlphaChangeOperator>
                        ) {
                            counter.dynamic(value.startTime);
                            counter.dynamic(value.endTime);
                            counter.dynamic(value.startValue);
                            counter.dynamic(value.endValue);
                        } else if constexpr (std::is_same_v<
                                                 T,
                                                 ParticleColorChangeOperator
                                             >) {
                            counter.dynamic(value.startTime);
                            counter.dynamic(value.endTime);
                            counter.dynamic(value.startValue);
                            counter.dynamic(value.endValue);
                        } else if constexpr (std::is_same_v<
                                                 T,
                                                 ParticleTurbulenceOperator
                                             >) {
                            counter.dynamic(value.scale);
                            counter.dynamic(value.speedMinimum);
                            counter.dynamic(value.speedMaximum);
                            counter.dynamic(value.timeScale);
                            counter.dynamic(value.mask);
                            counter.dynamic(value.phaseMinimum);
                            counter.dynamic(value.phaseMaximum);
                            counter.dynamic(value.audioProcessingMode);
                            counter.dynamic(value.audioProcessingBounds);
                            counter.dynamic(value.audioProcessingExponent);
                            counter.dynamic(value.audioProcessingFrequencyStart);
                            counter.dynamic(value.audioProcessingFrequencyEnd);
                        } else if constexpr (std::is_same_v<
                                                 T,
                                                 ParticleVortexOperator
                                             >) {
                            counter.dynamic(value.axis);
                            counter.dynamic(value.offset);
                            counter.dynamic(value.distanceInner);
                            counter.dynamic(value.distanceOuter);
                            counter.dynamic(value.speedInner);
                            counter.dynamic(value.speedOuter);
                            counter.dynamic(value.centerForce);
                            counter.dynamic(value.ringRadius);
                            counter.dynamic(value.ringWidth);
                            counter.dynamic(value.ringPullDistance);
                            counter.dynamic(value.ringPullForce);
                            counter.dynamic(value.audioProcessingMode);
                            counter.dynamic(value.audioProcessingBounds);
                        }
                    }, particleOperator);
                }
            }
        } else {
            ++counter.stats.group_object_count;
        }
    }

    counter.stats.unique_effect_definition_count =
        static_cast<uint64_t>(counter.effectDefinitions.size());
    counter.stats.unique_effect_material_count =
        static_cast<uint64_t>(counter.effectMaterials.size());
    counter.stats.unique_object_model_count =
        static_cast<uint64_t>(counter.objectModels.size());
}

/* The model loader currently exposes one shared material through each model.
 * Keep object-material accounting separate from effect-material accounting so
 * the stats describe each ownership role without counting an object model's
 * material as an effect material. */
void countObjectMaterialRole(const SceneProject& project, Counter& counter) {
    for (const SceneObject& object : project.scene.objects) {
        const auto* image = std::get_if<ImageObject>(&object.data);
        const auto* particle = std::get_if<ParticleObject>(&object.data);
        const std::shared_ptr<const Material> material = image != nullptr &&
                image->model
            ? image->model->material
            : particle != nullptr && particle->definition
                ? particle->definition->material
                : nullptr;
        if (!material) continue;
        const std::string& path = material->assetPath;
        if (!path.empty()) {
            counter.objectMaterials.insert(path);
        }
    }
}

const SceneObject* objectAt(
    const WESceneModelTestHandle* handle,
    size_t index,
    std::string& error
) {
    const auto& objects = handle->model->project().scene.objects;
    if (index >= objects.size()) {
        error = "Object index is out of range";
        return nullptr;
    }
    return &objects[index];
}

const ImageObject* imageAt(
    const WESceneModelTestHandle* handle,
    size_t index,
    std::string& error
) {
    const SceneObject* object = objectAt(handle, index, error);
    if (object == nullptr) {
        return nullptr;
    }
    const auto* image = std::get_if<ImageObject>(&object->data);
    if (image == nullptr) {
        error = "Object is not an image object";
        return nullptr;
    }
    return image;
}

const TextObject* textAt(
    const WESceneModelTestHandle* handle,
    size_t index,
    std::string& error
) {
    const SceneObject* object = objectAt(handle, index, error);
    if (object == nullptr) {
        return nullptr;
    }
    const auto* text = std::get_if<TextObject>(&object->data);
    if (text == nullptr) {
        error = "Object is not a text object";
        return nullptr;
    }
    return text;
}

const ParticleObject* particleAt(
    const WESceneModelTestHandle* handle,
    size_t index,
    std::string& error
) {
    const SceneObject* object = objectAt(handle, index, error);
    if (object == nullptr) return nullptr;
    const auto* particle = std::get_if<ParticleObject>(&object->data);
    if (particle == nullptr || !particle->definition) {
        error = "Object is not a particle object";
        return nullptr;
    }
    return particle;
}

const std::string* stringValue(const DynamicValue& value) {
    return value.value.type() == we::scene::RuntimeValueType::string
        ? &value.value.string()
        : nullptr;
}

std::optional<double> numberValue(const DynamicValue& value) {
    switch (value.value.type()) {
        case we::scene::RuntimeValueType::floating:
        case we::scene::RuntimeValueType::integer:
            return value.value.number();
        default:
            return std::nullopt;
    }
}

const std::string* vectorString(
    const DynamicValue& value,
    std::size_t expectedComponents,
    std::string& storage
) {
    if (!value.value.isVector() ||
        value.value.componentCount() != expectedComponents) {
        return nullptr;
    }
    storage = value.value.toString();
    return &storage;
}

const TextureSlots* textureSet(
    const TextureSlots& textures,
    const TextureSlots& userTextures,
    WESceneModelTestTextureSet set,
    std::string& error
) {
    switch (set) {
        case WE_SCENE_MODEL_TEST_TEXTURES:
            return &textures;
        case WE_SCENE_MODEL_TEST_USER_TEXTURES:
            return &userTextures;
    }
    error = "Unknown texture set";
    return nullptr;
}

int fillTextureSlot(
    const TextureSlots& textures,
    const TextureSlots& userTextures,
    WESceneModelTestTextureSet set,
    size_t index,
    WESceneModelTestTextureSlot* out,
    std::string& error
) {
    const TextureSlots* selected = textureSet(textures, userTextures, set, error);
    if (selected == nullptr) {
        return 0;
    }
    if (index >= selected->size()) {
        error = "Texture slot index is out of range";
        return 0;
    }
    const TextureSlot& slot = (*selected)[index];
    if (slot.name.has_value()) {
        out->kind = WE_SCENE_MODEL_TEST_TEXTURE_NAME;
        out->name = slot.name->c_str();
    } else {
        out->kind = WE_SCENE_MODEL_TEST_TEXTURE_NULL;
        out->name = nullptr;
    }
    return 1;
}

int failQuery(
    char* errorBuffer,
    size_t errorBufferSize,
    std::string_view message
) noexcept {
    writeError(errorBuffer, errorBufferSize, message);
    return 0;
}

}  // namespace

extern "C" WESceneModelTestHandleRef we_scene_model_test_load(
    const char* assetsDirectory,
    const char* scenePackagePath,
    const char* projectPath,
    WESceneModelTestStats* outStats,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outStats != nullptr) {
        *outStats = {};
    }
    return [=]() -> WESceneModelTestHandleRef {
        if (assetsDirectory == nullptr || scenePackagePath == nullptr ||
            projectPath == nullptr || assetsDirectory[0] == '\0' ||
            scenePackagePath[0] == '\0' || projectPath[0] == '\0') {
            writeError(
                errorBuffer,
                errorBufferSize,
                "assets_directory, scene_package_path and project_path are required"
            );
            return nullptr;
        }

        try {
            we::scene::RuntimeError runtimeError;
            auto runtime = we::scene::Runtime::create(
                we::scene::RuntimeConfiguration{
                    .assetsDirectory = assetsDirectory,
                    .scenePackagePath = scenePackagePath,
                },
                runtimeError
            );
            if (!runtime) {
                writeError(errorBuffer, errorBufferSize, runtimeError.message);
                return nullptr;
            }
            std::shared_ptr<const we::scene::Runtime> sharedRuntime(
                std::move(runtime)
            );
            auto model = SceneModel::load(sharedRuntime, projectPath);
            auto* handle = new WESceneModelTestHandle();
            handle->model = std::move(model);
            Counter counter;
            countProject(handle->model->project(), counter);
            countObjectMaterialRole(handle->model->project(), counter);
            counter.stats.unique_object_material_count =
                static_cast<uint64_t>(counter.objectMaterials.size());
            handle->stats = counter.stats;
            if (outStats != nullptr) {
                *outStats = handle->stats;
            }
            return handle;
        } catch (const SceneModelError& error) {
            writeError(errorBuffer, errorBufferSize, error.what());
        } catch (const std::exception& error) {
            writeError(errorBuffer, errorBufferSize, error.what());
        } catch (...) {
            writeError(
                errorBuffer,
                errorBufferSize,
                "Unknown exception while loading the scene model"
            );
        }
        return nullptr;
    }();
}

extern "C" void we_scene_model_test_destroy(
    WESceneModelTestHandleRef handle
) {
    delete handle;
}

extern "C" int we_scene_model_test_stats(
    WESceneModelTestHandleRef handle,
    WESceneModelTestStats* outStats,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outStats == nullptr) {
        return failQuery(errorBuffer, errorBufferSize, "Output stats are required");
    }
    if (!requireHandle(handle, errorBuffer, errorBufferSize)) {
        return 0;
    }
    *outStats = handle->stats;
    return 1;
}

extern "C" int we_scene_model_test_object_texture_slot(
    WESceneModelTestHandleRef handle,
    size_t objectIndex,
    WESceneModelTestTextureSet textureSetValue,
    size_t slotIndex,
    WESceneModelTestTextureSlot* outSlot,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outSlot == nullptr) {
        return failQuery(errorBuffer, errorBufferSize, "Output texture slot is required");
    }
    if (!requireHandle(handle, errorBuffer, errorBufferSize)) {
        return 0;
    }
    return guarded(errorBuffer, errorBufferSize, [&]() {
        std::string error;
        const ImageObject* image = imageAt(handle, objectIndex, error);
        if (image == nullptr) {
            return failQuery(errorBuffer, errorBufferSize, error);
        }
        return fillTextureSlot(
            image->instanceTextures,
            image->instanceUserTextures,
            textureSetValue,
            slotIndex,
            outSlot,
            error
        ) == 1
            ? 1
            : failQuery(errorBuffer, errorBufferSize, error);
    });
}

extern "C" int we_scene_model_test_effect_texture_slot(
    WESceneModelTestHandleRef handle,
    size_t objectIndex,
    size_t effectIndex,
    size_t passIndex,
    WESceneModelTestTextureSet textureSetValue,
    size_t slotIndex,
    WESceneModelTestTextureSlot* outSlot,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outSlot == nullptr) {
        return failQuery(errorBuffer, errorBufferSize, "Output texture slot is required");
    }
    if (!requireHandle(handle, errorBuffer, errorBufferSize)) {
        return 0;
    }
    return guarded(errorBuffer, errorBufferSize, [&]() {
        std::string error;
        const ImageObject* image = imageAt(handle, objectIndex, error);
        if (image == nullptr) {
            return failQuery(errorBuffer, errorBufferSize, error);
        }
        if (effectIndex >= image->effects.size()) {
            return failQuery(errorBuffer, errorBufferSize, "Effect index is out of range");
        }
        const auto& effect = image->effects[effectIndex].effect;
        if (!effect) {
            return failQuery(errorBuffer, errorBufferSize, "Effect definition is missing");
        }
        if (passIndex >= effect->passes.size()) {
            return failQuery(errorBuffer, errorBufferSize, "Effect pass index is out of range");
        }
        const auto& pass = effect->passes[passIndex];
        if (!pass.material) {
            return failQuery(errorBuffer, errorBufferSize, "Effect pass material is missing");
        }
        if (pass.material->passes.empty()) {
            return failQuery(errorBuffer, errorBufferSize, "Effect material has no passes");
        }
        const MaterialPass& materialPass = pass.material->passes[0];
        return fillTextureSlot(
            materialPass.textures,
            materialPass.userTextures,
            textureSetValue,
            slotIndex,
            outSlot,
            error
        ) == 1
            ? 1
            : failQuery(errorBuffer, errorBufferSize, error);
    });
}

extern "C" int we_scene_model_test_effect_override_texture_slot(
    WESceneModelTestHandleRef handle,
    size_t objectIndex,
    size_t effectIndex,
    size_t overridePassIndex,
    WESceneModelTestTextureSet textureSetValue,
    size_t slotIndex,
    WESceneModelTestTextureSlot* outSlot,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outSlot == nullptr) {
        return failQuery(errorBuffer, errorBufferSize, "Output texture slot is required");
    }
    if (!requireHandle(handle, errorBuffer, errorBufferSize)) {
        return 0;
    }
    return guarded(errorBuffer, errorBufferSize, [&]() {
        std::string error;
        const ImageObject* image = imageAt(handle, objectIndex, error);
        if (image == nullptr) {
            return failQuery(errorBuffer, errorBufferSize, error);
        }
        if (effectIndex >= image->effects.size()) {
            return failQuery(errorBuffer, errorBufferSize, "Effect index is out of range");
        }
        const auto& overrides = image->effects[effectIndex].passOverrides;
        if (overridePassIndex >= overrides.size()) {
            return failQuery(errorBuffer, errorBufferSize, "Effect override pass index is out of range");
        }
        const auto& pass = overrides[overridePassIndex];
        return fillTextureSlot(
            pass.textures,
            pass.userTextures,
            textureSetValue,
            slotIndex,
            outSlot,
            error
        ) == 1
            ? 1
            : failQuery(errorBuffer, errorBufferSize, error);
    });
}

extern "C" int we_scene_model_test_sound_source(
    WESceneModelTestHandleRef handle,
    size_t objectIndex,
    size_t sourceIndex,
    const char** outSource,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outSource == nullptr) {
        return failQuery(errorBuffer, errorBufferSize, "Output sound source is required");
    }
    *outSource = nullptr;
    if (!requireHandle(handle, errorBuffer, errorBufferSize)) {
        return 0;
    }
    return guarded(errorBuffer, errorBufferSize, [&]() {
        std::string error;
        const SceneObject* object = objectAt(handle, objectIndex, error);
        if (object == nullptr) {
            return failQuery(errorBuffer, errorBufferSize, error);
        }
        const auto* sound = std::get_if<SoundObject>(&object->data);
        if (sound == nullptr) {
            return failQuery(errorBuffer, errorBufferSize, "Object is not a sound object");
        }
        if (sourceIndex >= sound->sounds.size()) {
            return failQuery(errorBuffer, errorBufferSize, "Sound source index is out of range");
        }
        *outSource = sound->sounds[sourceIndex].c_str();
        return 1;
    });
}

extern "C" int we_scene_model_test_text_info(
    WESceneModelTestHandleRef handle,
    size_t objectIndex,
    WESceneModelTestTextInfo* outInfo,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outInfo == nullptr) {
        return failQuery(errorBuffer, errorBufferSize, "Output text info is required");
    }
    *outInfo = {};
    if (!requireHandle(handle, errorBuffer, errorBufferSize)) {
        return 0;
    }
    return guarded(errorBuffer, errorBufferSize, [&]() {
        std::string error;
        const SceneObject* object = objectAt(handle, objectIndex, error);
        const TextObject* text = textAt(handle, objectIndex, error);
        if (object == nullptr || text == nullptr) {
            return failQuery(errorBuffer, errorBufferSize, error);
        }
        const std::string* initialText = stringValue(text->text);
        const std::optional<double> pointSize = numberValue(text->pointSize);
        if (initialText == nullptr || !pointSize.has_value()) {
            return failQuery(
                errorBuffer,
                errorBufferSize,
                "Text object retained an unexpected literal value type"
            );
        }
        auto [strings, inserted] = handle->textValueStrings.try_emplace(
            objectIndex
        );
        if (inserted) {
            const std::string* size = vectorString(
                text->size, 2, strings->second[0]
            );
            const std::string* padding = vectorString(
                text->padding, 2, strings->second[1]
            );
            const std::string* spacing = vectorString(
                text->spacing, 2, strings->second[2]
            );
            if (size == nullptr || padding == nullptr || spacing == nullptr) {
                handle->textValueStrings.erase(strings);
                return failQuery(
                    errorBuffer,
                    errorBufferSize,
                    "Text object retained an unexpected vector value type"
                );
            }
        }
        outInfo->object_id = object->base.id;
        outInfo->name = object->base.name.c_str();
        outInfo->font = text->font.c_str();
        outInfo->initial_text = initialText->c_str();
        outInfo->horizontal_alignment = text->horizontalAlignment.c_str();
        outInfo->vertical_alignment = text->verticalAlignment.c_str();
        outInfo->point_size = *pointSize;
        outInfo->size = strings->second[0].c_str();
        outInfo->padding = strings->second[1].c_str();
        outInfo->spacing = strings->second[2].c_str();
        outInfo->text_is_dynamic = text->text.isDynamic();
        outInfo->origin_is_dynamic = object->base.origin.isDynamic();
        outInfo->color_is_dynamic = text->color.isDynamic();
        outInfo->visible_is_dynamic = object->base.visible.isDynamic();
        outInfo->limit_rows = text->limitRows;
        outInfo->limit_use_ellipsis = text->limitUseEllipsis;
        outInfo->limit_width = text->limitWidth;
        outInfo->max_rows = text->maxRows;
        outInfo->max_width = text->maxWidth;
        return 1;
    });
}

extern "C" int we_scene_model_test_particle_info(
    WESceneModelTestHandleRef handle,
    size_t objectIndex,
    WESceneModelTestParticleInfo* outInfo,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outInfo == nullptr) {
        return failQuery(errorBuffer, errorBufferSize, "Output particle info is required");
    }
    *outInfo = {};
    if (!requireHandle(handle, errorBuffer, errorBufferSize)) return 0;
    return guarded(errorBuffer, errorBufferSize, [&]() {
        std::string error;
        const SceneObject* object = objectAt(handle, objectIndex, error);
        const ParticleObject* particle = particleAt(handle, objectIndex, error);
        if (object == nullptr || particle == nullptr) {
            return failQuery(errorBuffer, errorBufferSize, error);
        }
        const auto& definition = *particle->definition;
        outInfo->object_id = object->base.id;
        outInfo->asset_path = definition.assetPath.c_str();
        outInfo->material_asset_path = definition.material
            ? definition.material->assetPath.c_str()
            : nullptr;
        outInfo->max_count = definition.maxCount;
        outInfo->flags = definition.flags;
        outInfo->emitter_count = definition.emitters.size();
        outInfo->initializer_count = definition.initializers.size();
        outInfo->operator_count = definition.operators.size();
        outInfo->child_count = definition.children.size();
        outInfo->renderer_name = definition.renderer.name.c_str();
        outInfo->renderer_orientation = definition.renderer.orientation.c_str();
        outInfo->renderer_length = definition.renderer.length;
        outInfo->renderer_max_length = definition.renderer.maxLength;
        outInfo->renderer_min_length = definition.renderer.minLength;
        outInfo->renderer_subdivision = definition.renderer.subdivision;
        outInfo->renderer_segments = definition.renderer.segments;
        outInfo->renderer_uv_scale = definition.renderer.uvScale;
        outInfo->renderer_uv_scrolling = definition.renderer.uvScrolling ? 1 : 0;
        outInfo->renderer_uv_smoothing = definition.renderer.uvSmoothing ? 1 : 0;
        outInfo->renderer_fade_alpha = definition.renderer.fadeAlpha ? 1 : 0;
        outInfo->renderer_fade_size = definition.renderer.fadeSize ? 1 : 0;
        return 1;
    });
}

extern "C" int we_scene_model_test_particle_initializer_info(
    WESceneModelTestHandleRef handle,
    size_t objectIndex,
    size_t initializerIndex,
    WESceneModelTestParticleInitializerInfo* outInfo,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outInfo == nullptr) {
        return failQuery(
            errorBuffer,
            errorBufferSize,
            "Output particle initializer info is required"
        );
    }
    *outInfo = {};
    if (!requireHandle(handle, errorBuffer, errorBufferSize)) return 0;
    return guarded(errorBuffer, errorBufferSize, [&]() {
        std::string error;
        const ParticleObject* particle = particleAt(handle, objectIndex, error);
        if (particle == nullptr) {
            return failQuery(errorBuffer, errorBufferSize, error);
        }
        const auto& initializers = particle->definition->initializers;
        if (initializerIndex >= initializers.size()) {
            return failQuery(
                errorBuffer,
                errorBufferSize,
                "Particle initializer index is out of range"
            );
        }
        return std::visit([&](const auto& initializer) {
            using Initializer = std::decay_t<decltype(initializer)>;
            if constexpr (std::is_same_v<Initializer, ParticleLifetimeRandomInitializer>) {
                outInfo->kind = WE_SCENE_MODEL_TEST_PARTICLE_LIFETIME_RANDOM;
            } else if constexpr (std::is_same_v<Initializer, ParticleSizeRandomInitializer>) {
                outInfo->kind = WE_SCENE_MODEL_TEST_PARTICLE_SIZE_RANDOM;
            } else if constexpr (std::is_same_v<Initializer, ParticleColorRandomInitializer>) {
                outInfo->kind = WE_SCENE_MODEL_TEST_PARTICLE_COLOR_RANDOM;
            } else if constexpr (std::is_same_v<Initializer, ParticleAlphaRandomInitializer>) {
                outInfo->kind = WE_SCENE_MODEL_TEST_PARTICLE_ALPHA_RANDOM;
            } else if constexpr (std::is_same_v<Initializer, ParticleVelocityRandomInitializer>) {
                outInfo->kind = WE_SCENE_MODEL_TEST_PARTICLE_VELOCITY_RANDOM;
            } else if constexpr (std::is_same_v<Initializer, ParticleRotationRandomInitializer>) {
                outInfo->kind = WE_SCENE_MODEL_TEST_PARTICLE_ROTATION_RANDOM;
            } else if constexpr (std::is_same_v<
                                     Initializer,
                                     ParticleAngularVelocityRandomInitializer
                                 >) {
                outInfo->kind =
                    WE_SCENE_MODEL_TEST_PARTICLE_ANGULAR_VELOCITY_RANDOM;
            } else if constexpr (std::is_same_v<
                                     Initializer,
                                     ParticleMapSequenceAroundControlPointInitializer
                                 >) {
                outInfo->kind =
                    WE_SCENE_MODEL_TEST_PARTICLE_MAP_SEQUENCE_AROUND_CONTROL_POINT;
            } else {
                outInfo->kind =
                    WE_SCENE_MODEL_TEST_PARTICLE_TURBULENT_VELOCITY_RANDOM;
            }
            if (initializer.id.has_value()) {
                outInfo->has_id = 1;
                outInfo->id = *initializer.id;
            }
            const DynamicValue& minimum = [&]() -> const DynamicValue& {
                if constexpr (std::is_same_v<
                                  Initializer,
                                  ParticleTurbulentVelocityRandomInitializer
                              >) {
                    return initializer.speedMinimum;
                } else if constexpr (std::is_same_v<
                                         Initializer,
                                         ParticleMapSequenceAroundControlPointInitializer
                                     >) {
                    return initializer.speedMinimum;
                } else {
                    return initializer.minimum;
                }
            }();
            const DynamicValue& maximum = [&]() -> const DynamicValue& {
                if constexpr (std::is_same_v<
                                  Initializer,
                                  ParticleTurbulentVelocityRandomInitializer
                              >) {
                    return initializer.speedMaximum;
                } else if constexpr (std::is_same_v<
                                         Initializer,
                                         ParticleMapSequenceAroundControlPointInitializer
                                     >) {
                    return initializer.speedMaximum;
                } else {
                    return initializer.maximum;
                }
            }();
            constexpr bool expectsNumber =
                std::is_same_v<Initializer, ParticleLifetimeRandomInitializer> ||
                std::is_same_v<Initializer, ParticleSizeRandomInitializer> ||
                std::is_same_v<Initializer, ParticleAlphaRandomInitializer> ||
                std::is_same_v<
                    Initializer,
                    ParticleTurbulentVelocityRandomInitializer
                >;
            constexpr std::size_t expectedComponents =
                std::is_same_v<Initializer, ParticleColorRandomInitializer>
                    ? 4
                    : 3;
            if constexpr (expectsNumber) {
                const std::optional<double> minimumNumber = numberValue(minimum);
                const std::optional<double> maximumNumber = numberValue(maximum);
                if (!minimumNumber.has_value() || !maximumNumber.has_value()) {
                    return failQuery(
                        errorBuffer,
                        errorBufferSize,
                        "Particle initializer range has an unexpected type"
                    );
                }
                outInfo->minimum_is_number = 1;
                outInfo->minimum_number = *minimumNumber;
                outInfo->maximum_is_number = 1;
                outInfo->maximum_number = *maximumNumber;
            } else {
                auto [strings, inserted] =
                    handle->particleValueStrings.try_emplace(std::make_pair(
                        objectIndex,
                        initializerIndex
                    ));
                if (inserted &&
                    (vectorString(
                         minimum,
                         expectedComponents,
                         strings->second[0]
                     ) == nullptr ||
                     vectorString(
                         maximum,
                         expectedComponents,
                         strings->second[1]
                     ) == nullptr)) {
                    handle->particleValueStrings.erase(strings);
                    return failQuery(
                        errorBuffer,
                        errorBufferSize,
                        "Particle initializer vector range has an unexpected type"
                    );
                }
                outInfo->minimum_text = strings->second[0].c_str();
                outInfo->maximum_text = strings->second[1].c_str();
            }
            if constexpr (std::is_same_v<Initializer, ParticleSizeRandomInitializer> ||
                          std::is_same_v<
                              Initializer,
                              ParticleAngularVelocityRandomInitializer
                          >) {
                const auto exponent = numberValue(initializer.exponent);
                if (!exponent.has_value()) {
                    return failQuery(
                        errorBuffer,
                        errorBufferSize,
                        "Particle size exponent has an unexpected type"
                    );
                }
                outInfo->has_exponent = 1;
                outInfo->exponent = *exponent;
            }
            return 1;
        }, initializers[initializerIndex]);
    });
}

extern "C" int we_scene_model_test_effect_dependency(
    WESceneModelTestHandleRef handle,
    size_t objectIndex,
    size_t effectIndex,
    size_t dependencyIndex,
    const char** outDependency,
    char* errorBuffer,
    size_t errorBufferSize
) {
    if (outDependency == nullptr) {
        return failQuery(errorBuffer, errorBufferSize, "Output effect dependency is required");
    }
    *outDependency = nullptr;
    if (!requireHandle(handle, errorBuffer, errorBufferSize)) {
        return 0;
    }
    return guarded(errorBuffer, errorBufferSize, [&]() {
        std::string error;
        const ImageObject* image = imageAt(handle, objectIndex, error);
        if (image == nullptr) {
            return failQuery(errorBuffer, errorBufferSize, error);
        }
        if (effectIndex >= image->effects.size()) {
            return failQuery(errorBuffer, errorBufferSize, "Effect index is out of range");
        }
        const auto& effect = image->effects[effectIndex].effect;
        if (!effect) {
            return failQuery(errorBuffer, errorBufferSize, "Effect definition is missing");
        }
        if (dependencyIndex >= effect->dependencies.size()) {
            return failQuery(errorBuffer, errorBufferSize, "Effect dependency index is out of range");
        }
        *outDependency = effect->dependencies[dependencyIndex].c_str();
        return 1;
    });
}
