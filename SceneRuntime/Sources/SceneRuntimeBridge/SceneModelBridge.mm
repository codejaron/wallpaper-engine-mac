#include <SceneRuntimeBridge/SceneRuntimeBridge.h>

#include "SceneRuntimeBridgeInternal.hpp"

#include <SceneModel/SceneModel.hpp>

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace {

using we::scene::bridge::assignError;
using we::scene::bridge::assignExceptionError;
using we::scene::bridge::assignModelError;
using we::scene::bridge::clearError;
using we::scene::bridge::requireOutput;

bool requireModel(
    WESceneModelRef model,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (model != nullptr && model->model) {
        return true;
    }
    assignError(
        outError,
        WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Scene model is required"
    );
    return false;
}

const we::scene::SceneObject* objectAt(
    WESceneModelRef model,
    std::size_t index,
    WESceneRuntimeErrorRef* outError
) noexcept {
    const auto& objects = model->model->project().scene.objects;
    if (index >= objects.size()) {
        assignError(
            outError,
            WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
            "Scene object index is out of range"
        );
        return nullptr;
    }
    return &objects[index];
}

const we::scene::ProjectProperty* propertyAt(
    WESceneModelRef model,
    std::size_t index,
    const std::string** outKey,
    WESceneRuntimeErrorRef* outError
) noexcept {
    const auto& keys = model->model->propertyKeys();
    if (index >= keys.size()) {
        assignError(
            outError,
            WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
            "Scene property index is out of range"
        );
        return nullptr;
    }
    const std::string& key = keys[index];
    if (outKey != nullptr) {
        *outKey = &key;
    }
    return &model->model->project().properties.at(key);
}

WESceneObjectType objectType(const we::scene::SceneObjectData& data) noexcept {
    if (std::holds_alternative<we::scene::ImageObject>(data)) {
        return WE_SCENE_OBJECT_IMAGE;
    }
    if (std::holds_alternative<we::scene::TextObject>(data)) {
        return WE_SCENE_OBJECT_TEXT;
    }
    if (std::holds_alternative<we::scene::SoundObject>(data)) {
        return WE_SCENE_OBJECT_SOUND;
    }
    return WE_SCENE_OBJECT_GROUP;
}

WEScenePropertyType propertyType(we::scene::PropertyType type) noexcept {
    using Type = we::scene::PropertyType;
    switch (type) {
        case Type::boolean:
            return WE_SCENE_PROPERTY_BOOLEAN;
        case Type::slider:
            return WE_SCENE_PROPERTY_SLIDER;
        case Type::combo:
            return WE_SCENE_PROPERTY_COMBO;
        case Type::color:
            return WE_SCENE_PROPERTY_COLOR;
        case Type::text:
            return WE_SCENE_PROPERTY_TEXT;
        case Type::sceneTexture:
            return WE_SCENE_PROPERTY_SCENE_TEXTURE;
        case Type::file:
            return WE_SCENE_PROPERTY_FILE;
        case Type::directory:
            return WE_SCENE_PROPERTY_DIRECTORY;
        case Type::textInput:
            return WE_SCENE_PROPERTY_TEXT_INPUT;
        case Type::userShortcut:
            return WE_SCENE_PROPERTY_USER_SHORTCUT;
        case Type::group:
            return WE_SCENE_PROPERTY_GROUP;
    }
    return WE_SCENE_PROPERTY_GROUP;
}

bool propertyIsReadOnly(we::scene::PropertyType type) noexcept {
    return type == we::scene::PropertyType::text ||
           type == we::scene::PropertyType::group;
}

void fillValue(
    WESceneModelRef model,
    const std::optional<we::scene::Value>& value,
    WEScenePropertyValue& output
) {
    output = {};
    if (!value || value->isNull()) {
        output.type = WE_SCENE_VALUE_NULL;
        return;
    }
    if (const auto* boolean = std::get_if<bool>(&value->storage)) {
        output.type = WE_SCENE_VALUE_BOOLEAN;
        output.boolean_value = *boolean ? 1 : 0;
    } else if (const auto* integer = std::get_if<std::int64_t>(&value->storage)) {
        output.type = WE_SCENE_VALUE_INTEGER;
        output.integer_value = *integer;
    } else if (const auto* number = std::get_if<double>(&value->storage)) {
        output.type = WE_SCENE_VALUE_NUMBER;
        output.number_value = *number;
    } else if (const auto* string = std::get_if<std::string>(&value->storage)) {
        output.type = WE_SCENE_VALUE_STRING;
        const std::lock_guard lock(model->scratchMutex);
        model->valueScratch = *string;
        output.string_value = model->valueScratch.c_str();
    } else if (std::holds_alternative<we::scene::Value::Array>(value->storage)) {
        output.type = WE_SCENE_VALUE_ARRAY;
    } else {
        output.type = WE_SCENE_VALUE_OBJECT;
    }
}

std::optional<we::scene::Value> bridgeValue(
    const WEScenePropertyValue& input,
    WESceneRuntimeErrorRef* outError
) {
    we::scene::Value result;
    switch (input.type) {
        case WE_SCENE_VALUE_NULL:
            result.storage = nullptr;
            return result;
        case WE_SCENE_VALUE_BOOLEAN:
            if (input.boolean_value != 0 && input.boolean_value != 1) {
                assignError(
                    outError,
                    WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Boolean property input must be 0 or 1"
                );
                return std::nullopt;
            }
            result.storage = input.boolean_value != 0;
            return result;
        case WE_SCENE_VALUE_INTEGER:
            result.storage = input.integer_value;
            return result;
        case WE_SCENE_VALUE_NUMBER:
            result.storage = input.number_value;
            return result;
        case WE_SCENE_VALUE_STRING:
            if (input.string_value == nullptr) {
                assignError(
                    outError,
                    WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "String property input requires string_value"
                );
                return std::nullopt;
            }
            result.storage = std::string(input.string_value);
            return result;
        case WE_SCENE_VALUE_ARRAY:
        case WE_SCENE_VALUE_OBJECT:
            assignError(
                outError,
                WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                "Array and object property inputs are not supported by this ABI"
            );
            return std::nullopt;
    }
    assignError(
        outError,
        WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Property input has an unknown value type"
    );
    return std::nullopt;
}

}  // namespace

extern "C" WESceneModelRef we_scene_runtime_model_create(
    WESceneRuntimeRef runtime,
    const char* project_path,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (runtime == nullptr || !runtime->runtime || project_path == nullptr ||
        project_path[0] == '\0') {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Runtime and an explicit project path are required"
        );
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneModel>();
        handle->model = we::scene::SceneModel::load(
            runtime->runtime,
            project_path
        );
        return handle.release();
    } catch (const we::scene::SceneModelError& error) {
        assignModelError(out_error, error);
        return nullptr;
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "loading the scene model", error.what());
        return nullptr;
    } catch (...) {
        assignExceptionError(out_error, "loading the scene model", nullptr);
        return nullptr;
    }
}

extern "C" void we_scene_model_destroy(WESceneModelRef model) {
    delete model;
}

extern "C" int we_scene_model_project_info(
    WESceneModelRef model,
    WESceneProjectInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_info, out_error, "scene project information")) {
        return 0;
    }
    const auto& project = model->model->project();
    *out_info = {};
    out_info->asset_path = project.assetPath.c_str();
    out_info->title = project.title.c_str();
    out_info->workshop_id = project.workshopId
        ? project.workshopId->c_str()
        : nullptr;
    out_info->scene_asset_path = project.scene.assetPath.c_str();
    out_info->supports_audio_processing = project.supportsAudioProcessing ? 1 : 0;
    out_info->audio_input_status = project.supportsAudioProcessing
        ? WE_SCENE_AUDIO_INPUT_UNAVAILABLE
        : WE_SCENE_AUDIO_INPUT_NOT_REQUESTED;
    out_info->projection_auto = project.scene.camera.projectionAuto ? 1 : 0;
    out_info->projection_width = project.scene.camera.projectionWidth;
    out_info->projection_height = project.scene.camera.projectionHeight;
    return 1;
}

extern "C" int we_scene_model_object_count(
    WESceneModelRef model,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_count, out_error, "scene object count")) {
        return 0;
    }
    *out_count = model->model->project().scene.objects.size();
    return 1;
}

extern "C" int we_scene_model_object_info(
    WESceneModelRef model,
    size_t index,
    WESceneObjectInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_info, out_error, "scene object information")) {
        return 0;
    }
    const we::scene::SceneObject* object = objectAt(model, index, out_error);
    if (object == nullptr) {
        return 0;
    }
    *out_info = {};
    out_info->id = object->base.id;
    out_info->name = object->base.name.c_str();
    out_info->type = objectType(object->data);
    out_info->has_parent = object->base.parent.has_value() ? 1 : 0;
    out_info->parent_id = object->base.parent.value_or(0);
    out_info->dependency_count = object->base.dependencies.size();
    if (const auto* image = std::get_if<we::scene::ImageObject>(&object->data)) {
        out_info->referenced_asset_path = image->model->assetPath.c_str();
    }
    return 1;
}

extern "C" int we_scene_model_object_dependency(
    WESceneModelRef model,
    size_t object_index,
    size_t dependency_index,
    int32_t* out_dependency_id,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_dependency_id, out_error, "object dependency id")) {
        return 0;
    }
    const we::scene::SceneObject* object = objectAt(
        model,
        object_index,
        out_error
    );
    if (object == nullptr) {
        return 0;
    }
    if (dependency_index >= object->base.dependencies.size()) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
            "Object dependency index is out of range"
        );
        return 0;
    }
    *out_dependency_id = object->base.dependencies[dependency_index].id;
    return 1;
}

extern "C" int we_scene_model_object_dependency_info(
    WESceneModelRef model,
    size_t object_index,
    size_t dependency_index,
    WESceneObjectDependencyInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_info, out_error, "object dependency information")) {
        return 0;
    }
    const we::scene::SceneObject* object = objectAt(
        model,
        object_index,
        out_error
    );
    if (object == nullptr) {
        return 0;
    }
    if (dependency_index >= object->base.dependencies.size()) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
            "Object dependency index is out of range"
        );
        return 0;
    }
    const auto& dependency = object->base.dependencies[dependency_index];
    *out_info = {};
    out_info->id = dependency.id;
    out_info->has_index = dependency.index.has_value() ? 1 : 0;
    out_info->index = dependency.index.value_or(0);
    out_info->type = dependency.type ? dependency.type->c_str() : nullptr;
    return 1;
}

extern "C" int we_scene_model_property_count(
    WESceneModelRef model,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_count, out_error, "scene property count")) {
        return 0;
    }
    *out_count = model->model->propertyKeys().size();
    return 1;
}

extern "C" int we_scene_model_property_info(
    WESceneModelRef model,
    size_t index,
    WEScenePropertyInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_info, out_error, "scene property information")) {
        return 0;
    }
    const std::string* key = nullptr;
    const we::scene::ProjectProperty* property = propertyAt(
        model,
        index,
        &key,
        out_error
    );
    if (property == nullptr) {
        return 0;
    }
    *out_info = {};
    out_info->key = key->c_str();
    out_info->text = property->text.c_str();
    out_info->type = propertyType(property->type);
    out_info->has_index = property->index.has_value() ? 1 : 0;
    out_info->index = property->index.value_or(0);
    out_info->has_order = property->order.has_value() ? 1 : 0;
    out_info->order = property->order.value_or(0);
    out_info->has_minimum = property->minimum.has_value() ? 1 : 0;
    out_info->minimum = property->minimum.value_or(0.0);
    out_info->has_maximum = property->maximum.has_value() ? 1 : 0;
    out_info->maximum = property->maximum.value_or(0.0);
    out_info->has_step = property->step.has_value() ? 1 : 0;
    out_info->step = property->step.value_or(0.0);
    out_info->has_precision = property->precision.has_value() ? 1 : 0;
    out_info->precision = property->precision.value_or(0);
    out_info->has_fraction = property->fraction.has_value() ? 1 : 0;
    out_info->fraction = property->fraction.value_or(false) ? 1 : 0;
    out_info->is_read_only = propertyIsReadOnly(property->type) ? 1 : 0;
    return 1;
}

extern "C" int we_scene_model_property_option_count(
    WESceneModelRef model,
    size_t property_index,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_count, out_error, "property option count")) {
        return 0;
    }
    const we::scene::ProjectProperty* property = propertyAt(
        model,
        property_index,
        nullptr,
        out_error
    );
    if (property == nullptr) {
        return 0;
    }
    *out_count = property->options.size();
    return 1;
}

extern "C" int we_scene_model_property_option_info(
    WESceneModelRef model,
    size_t property_index,
    size_t option_index,
    WEScenePropertyOptionInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_info, out_error, "property option information")) {
        return 0;
    }
    const we::scene::ProjectProperty* property = propertyAt(
        model,
        property_index,
        nullptr,
        out_error
    );
    if (property == nullptr) {
        return 0;
    }
    if (option_index >= property->options.size()) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
            "Property option index is out of range"
        );
        return 0;
    }
    const auto& option = property->options[option_index];
    out_info->value = option.value.c_str();
    out_info->label = option.label.c_str();
    return 1;
}

extern "C" int we_scene_model_property_value(
    WESceneModelRef model,
    size_t property_index,
    WEScenePropertyValue* out_value,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_value, out_error, "property value")) {
        return 0;
    }
    const std::string* key = nullptr;
    if (propertyAt(model, property_index, &key, out_error) == nullptr) {
        return 0;
    }
    try {
        fillValue(model, model->model->propertyValue(*key), *out_value);
        return 1;
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "reading a scene property", error.what());
        return 0;
    } catch (...) {
        assignExceptionError(out_error, "reading a scene property", nullptr);
        return 0;
    }
}

extern "C" int we_scene_model_set_property_value(
    WESceneModelRef model,
    const char* property_key,
    const WEScenePropertyValue* value,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) || property_key == nullptr ||
        property_key[0] == '\0' || value == nullptr) {
        if (model != nullptr && model->model) {
            assignError(
                out_error,
                WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                "Property key and value are required"
            );
        }
        return 0;
    }
    try {
        std::optional<we::scene::Value> parsed = bridgeValue(*value, out_error);
        if (!parsed) {
            return 0;
        }
        model->model->setPropertyValue(property_key, std::move(*parsed));
        const std::lock_guard lock(model->scratchMutex);
        model->valueScratch.clear();
        return 1;
    } catch (const we::scene::SceneModelError& error) {
        assignModelError(out_error, error);
        return 0;
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "setting a scene property", error.what());
        return 0;
    } catch (...) {
        assignExceptionError(out_error, "setting a scene property", nullptr);
        return 0;
    }
}

extern "C" int we_scene_model_set_property_values(
    WESceneModelRef model,
    const WEScenePropertyUpdate* updates,
    size_t update_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error)) return 0;
    if (update_count > 0 && updates == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Property updates are required when update_count is non-zero"
        );
        return 0;
    }
    try {
        std::vector<std::pair<std::string, we::scene::Value>> parsed;
        parsed.reserve(update_count);
        for (size_t index = 0; index < update_count; ++index) {
            if (updates[index].key == nullptr || updates[index].key[0] == '\0') {
                assignError(
                    out_error,
                    WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Every property update requires a non-empty key"
                );
                return 0;
            }
            std::optional<we::scene::Value> value = bridgeValue(
                updates[index].value,
                out_error
            );
            if (!value) return 0;
            parsed.emplace_back(updates[index].key, std::move(*value));
        }
        model->model->setPropertyValues(std::move(parsed));
        const std::lock_guard lock(model->scratchMutex);
        model->valueScratch.clear();
        return 1;
    } catch (const we::scene::SceneModelError& error) {
        assignModelError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "setting scene properties", error.what());
    } catch (...) {
        assignExceptionError(out_error, "setting scene properties", nullptr);
    }
    return 0;
}

extern "C" int we_scene_model_revision(
    WESceneModelRef model,
    uint64_t* out_revision,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error) ||
        !requireOutput(out_revision, out_error, "scene model revision")) {
        return 0;
    }
    *out_revision = model->model->revision();
    return 1;
}
