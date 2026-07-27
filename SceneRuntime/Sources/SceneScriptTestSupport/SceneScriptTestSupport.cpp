#include <SceneScriptTestSupport/SceneScriptTestSupport.h>

#include <nlohmann/json.hpp>
#include <SceneScript/SceneScript.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using Json = nlohmann::json;
using we::scene::AudioSpectrumFrame;
using we::scene::RuntimeValue;
using we::scene::RuntimeValueType;
using we::scene::script::ScriptError;
using we::scene::script::ScriptErrorCode;
using we::scene::script::ScriptFrameInputs;
using we::scene::script::ScriptCursorEvent;
using we::scene::script::ScriptCursorEventType;
using we::scene::script::ScriptMediaPlaybackState;
using we::scene::script::ScriptMediaSnapshot;
using we::scene::script::ScriptUserPropertiesSnapshot;
using we::scene::script::ScriptInstance;
using we::scene::script::ScriptLayerDescriptor;
using we::scene::script::ScriptLayerRegistry;
using we::scene::script::ScriptLayerType;
using we::scene::script::ScriptLimits;
using we::scene::script::ScriptRuntime;
using we::scene::script::ScriptTextureAnimationMetadata;

struct WESceneScriptTestRuntimeStorage {
    explicit WESceneScriptTestRuntimeStorage(ScriptLimits limits = {}) : runtime(limits) {}

    ScriptRuntime runtime;
};

struct WESceneScriptTestRuntime {
    std::shared_ptr<WESceneScriptTestRuntimeStorage> storage;
};

struct WESceneScriptTestInstance {
    std::shared_ptr<WESceneScriptTestRuntimeStorage> storage;
    std::shared_ptr<ScriptLayerRegistry> layerRegistry;
    std::unique_ptr<ScriptInstance> instance;
    std::optional<bool> isScreensaver;
    std::map<std::string, RuntimeValue> userProperties;
};

struct WESceneScriptTestError {
    WESceneScriptTestErrorCode code = WE_SCENE_SCRIPT_TEST_ERROR_NONE;
    std::string message;
};

namespace {

void clearError(WESceneScriptTestErrorRef* outError) noexcept {
    if (outError != nullptr) *outError = nullptr;
}

WESceneScriptTestErrorCode bridgeCode(ScriptErrorCode code) noexcept {
    switch (code) {
        case ScriptErrorCode::module:
            return WE_SCENE_SCRIPT_TEST_ERROR_MODULE;
        case ScriptErrorCode::exception:
            return WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION;
        case ScriptErrorCode::invalidResultType:
            return WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE;
        case ScriptErrorCode::nonFiniteResult:
            return WE_SCENE_SCRIPT_TEST_ERROR_NONFINITE_RESULT;
        case ScriptErrorCode::audioInputUnavailable:
            return WE_SCENE_SCRIPT_TEST_ERROR_AUDIO_INPUT_UNAVAILABLE;
        case ScriptErrorCode::resourceLimit:
            return WE_SCENE_SCRIPT_TEST_ERROR_RESOURCE_LIMIT;
    }
    return WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION;
}

void setError(
    WESceneScriptTestErrorRef* outError,
    WESceneScriptTestErrorCode code,
    std::string message
) noexcept {
    if (outError == nullptr) return;
    try {
        *outError = new WESceneScriptTestError{
            .code = code,
            .message = std::move(message),
        };
    } catch (...) {
        *outError = nullptr;
    }
}

void setException(
    WESceneScriptTestErrorRef* outError,
    const std::exception& exception
) noexcept {
    if (const auto* scriptError = dynamic_cast<const ScriptError*>(&exception)) {
        setError(outError, bridgeCode(scriptError->code()), scriptError->what());
        return;
    }
    setError(outError, WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION, exception.what());
}

RuntimeValue runtimeValueFromJson(const Json& json) {
    if (json.is_null()) {
        return RuntimeValue::null();
    }
    if (json.is_boolean()) {
        return RuntimeValue::boolean(json.get<bool>());
    }
    if (json.is_number_unsigned()) {
        const auto value = json.get<std::uint64_t>();
        if (value <= static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return RuntimeValue::integer(static_cast<std::int64_t>(value));
        }
        throw std::out_of_range("Unsigned script test value exceeds int64");
    }
    if (json.is_number_integer()) {
        return RuntimeValue::integer(json.get<std::int64_t>());
    }
    if (json.is_number_float()) {
        const float value = json.get<float>();
        if (!std::isfinite(value)) {
            throw std::invalid_argument("Script test value must be finite");
        }
        return RuntimeValue::floating(static_cast<double>(value));
    }
    if (json.is_string()) {
        return RuntimeValue::string(json.get<std::string>());
    }
    if (!json.is_object()) {
        throw std::invalid_argument(
            "DynamicValue test JSON must be a scalar or x/y/z/w vector"
        );
    }

    static constexpr std::array<const char*, 4> names = {"x", "y", "z", "w"};
    std::array<double, 4> components{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto found = json.find(names[index]);
        if (found == json.end()) {
            if (index < 2) {
                throw std::invalid_argument(
                    "DynamicValue vector test JSON requires x and y"
                );
            }
            break;
        }
        if (!found->is_number()) {
            throw std::invalid_argument(
                "DynamicValue vector test components must be numeric"
            );
        }
        const float value = found->get<float>();
        if (!std::isfinite(value)) {
            throw std::invalid_argument(
                "DynamicValue vector test components must be finite"
            );
        }
        components[index] = static_cast<double>(value);
        count = index + 1;
    }
    return RuntimeValue::vector(components, count);
}

Json jsonFromRuntimeValue(const RuntimeValue& value) {
    switch (value.type()) {
        case RuntimeValueType::null:
            return nullptr;
        case RuntimeValueType::boolean:
            return value.boolean();
        case RuntimeValueType::integer:
            return value.integer();
        case RuntimeValueType::floating:
            return value.number();
        case RuntimeValueType::string:
            return value.string();
        case RuntimeValueType::vector2:
        case RuntimeValueType::vector3:
        case RuntimeValueType::vector4: {
            static constexpr std::array<const char*, 4> names = {
                "x", "y", "z", "w",
            };
            Json result = Json::object();
            for (std::size_t index = 0; index < value.componentCount(); ++index) {
                result[names[index]] = value.vector()[index];
            }
            return result;
        }
    }
    throw std::logic_error("Unknown RuntimeValue type");
}

std::map<std::string, RuntimeValue> propertiesFromJson(
    const char* source,
    const char* description = "Script properties"
) {
    const Json json = Json::parse(source);
    if (!json.is_object()) {
        throw std::invalid_argument(
            std::string(description) + " JSON must be an object"
        );
    }
    std::map<std::string, RuntimeValue> properties;
    for (const auto& [key, value] : json.items()) {
        properties.emplace(key, runtimeValueFromJson(value));
    }
    return properties;
}

ScriptTextureAnimationMetadata textureAnimationFromJson(const char* source) {
    const Json json = Json::parse(source);
    if (!json.is_object() || !json.contains("asset") ||
        !json.at("asset").is_string() || !json.contains("frames") ||
        !json.at("frames").is_array()) {
        throw std::invalid_argument(
            "Texture animation JSON requires string asset and frame array"
        );
    }
    ScriptTextureAnimationMetadata result{
        .assetIdentity = json.at("asset").get<std::string>(),
    };
    if (result.assetIdentity.empty() || json.at("frames").empty()) {
        throw std::invalid_argument(
            "Texture animation asset and frame array must not be empty"
        );
    }
    result.frameDurations.reserve(json.at("frames").size());
    for (const Json& frame : json.at("frames")) {
        if (!frame.is_number()) {
            throw std::invalid_argument(
                "Texture animation frame duration must be numeric"
            );
        }
        const double duration = frame.get<double>();
        if (!std::isfinite(duration) || duration < 0.0) {
            throw std::invalid_argument(
                "Texture animation frame duration must be finite and non-negative"
            );
        }
        result.frameDurations.push_back(duration);
    }
    return result;
}

std::vector<ScriptLayerDescriptor> layersFromInputs(
    const WESceneScriptTestLayer* layers,
    std::size_t count
) {
    if (count != 0 && layers == nullptr) {
        throw std::invalid_argument("Layer inputs are null");
    }
    std::vector<ScriptLayerDescriptor> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const WESceneScriptTestLayer& layer = layers[index];
        if (layer.name == nullptr || layer.properties_json == nullptr) {
            throw std::invalid_argument("Layer name and properties JSON are required");
        }
        ScriptLayerType type;
        switch (layer.type) {
            case WE_SCENE_SCRIPT_TEST_LAYER_IMAGE:
                type = ScriptLayerType::image;
                break;
            case WE_SCENE_SCRIPT_TEST_LAYER_TEXT:
                type = ScriptLayerType::text;
                break;
            case WE_SCENE_SCRIPT_TEST_LAYER_PARTICLE:
                type = ScriptLayerType::particle;
                break;
            case WE_SCENE_SCRIPT_TEST_LAYER_SOUND:
                type = ScriptLayerType::sound;
                break;
            case WE_SCENE_SCRIPT_TEST_LAYER_GROUP:
                type = ScriptLayerType::group;
                break;
            default:
                throw std::invalid_argument("Unknown test layer type");
        }
        ScriptLayerDescriptor descriptor{
            .id = layer.id,
            .name = layer.name,
            .type = type,
            .properties = propertiesFromJson(layer.properties_json),
        };
        if (layer.texture_animation_json != nullptr) {
            descriptor.textureAnimation = textureAnimationFromJson(
                layer.texture_animation_json
            );
            descriptor.textureAssetIdentity =
                descriptor.textureAnimation->assetIdentity;
        }
        result.push_back(std::move(descriptor));
    }
    return result;
}

AudioSpectrumFrame audioSpectrumFromInputs(
    const WESceneScriptTestAudioSpectrumInputs* inputs
) {
    if (inputs == nullptr ||
        inputs->spectrum_16_left == nullptr ||
        inputs->spectrum_16_right == nullptr ||
        inputs->spectrum_32_left == nullptr ||
        inputs->spectrum_32_right == nullptr ||
        inputs->spectrum_64_left == nullptr ||
        inputs->spectrum_64_right == nullptr) {
        throw std::invalid_argument(
            "Audio spectrum input requires all six fixed-size arrays"
        );
    }
    AudioSpectrumFrame result;
    std::copy_n(
        inputs->spectrum_16_left, result.spectrum16Left.size(),
        result.spectrum16Left.begin()
    );
    std::copy_n(
        inputs->spectrum_16_right, result.spectrum16Right.size(),
        result.spectrum16Right.begin()
    );
    std::copy_n(
        inputs->spectrum_32_left, result.spectrum32Left.size(),
        result.spectrum32Left.begin()
    );
    std::copy_n(
        inputs->spectrum_32_right, result.spectrum32Right.size(),
        result.spectrum32Right.begin()
    );
    std::copy_n(
        inputs->spectrum_64_left, result.spectrum64Left.size(),
        result.spectrum64Left.begin()
    );
    std::copy_n(
        inputs->spectrum_64_right, result.spectrum64Right.size(),
        result.spectrum64Right.begin()
    );
    return result;
}

double finiteNumber(const Json& object, const char* key, double fallback = 0.0) {
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (!found->is_number()) {
        throw std::invalid_argument(
            std::string("SceneScript event field '") + key + "' must be numeric"
        );
    }
    const double value = found->get<double>();
    if (!std::isfinite(value)) {
        throw std::invalid_argument(
            std::string("SceneScript event field '") + key + "' must be finite"
        );
    }
    return value;
}

std::array<double, 3> vec3FromJson(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) return {0.0, 0.0, 0.0};
    if (!found->is_object()) {
        throw std::invalid_argument(
            std::string("SceneScript media color '") + key + "' must be an object"
        );
    }
    return {
        finiteNumber(*found, "x"),
        finiteNumber(*found, "y"),
        finiteNumber(*found, "z"),
    };
}

ScriptCursorEventType cursorEventTypeFromJson(const Json& value) {
    if (value.is_string()) {
        const std::string name = value.get<std::string>();
        if (name == "enter") return ScriptCursorEventType::enter;
        if (name == "leave") return ScriptCursorEventType::leave;
        if (name == "move") return ScriptCursorEventType::move;
        if (name == "down") return ScriptCursorEventType::down;
        if (name == "up") return ScriptCursorEventType::up;
        if (name == "click") return ScriptCursorEventType::click;
    }
    if (value.is_number_integer()) {
        const auto raw = value.get<std::int32_t>();
        if (raw >= static_cast<std::int32_t>(ScriptCursorEventType::enter) &&
            raw <= static_cast<std::int32_t>(ScriptCursorEventType::click)) {
            return static_cast<ScriptCursorEventType>(raw);
        }
    }
    throw std::invalid_argument("Unknown SceneScript cursor event type");
}

std::vector<ScriptCursorEvent> cursorEventsFromJson(const char* source) {
    if (source == nullptr) return {};
    const Json json = Json::parse(source);
    if (json.is_null()) return {};
    if (!json.is_array()) {
        throw std::invalid_argument("SceneScript cursor events JSON must be an array");
    }
    std::vector<ScriptCursorEvent> result;
    result.reserve(json.size());
    for (const auto& item : json) {
        if (!item.is_object()) {
            throw std::invalid_argument("SceneScript cursor event must be an object");
        }
        const auto type = item.find("type");
        if (type == item.end()) {
            throw std::invalid_argument("SceneScript cursor event type is required");
        }
        const auto layer = item.find("layerId");
        if (layer == item.end() || !layer->is_number_integer()) {
            throw std::invalid_argument("SceneScript cursor event layerId is required");
        }
        ScriptCursorEvent event{
            .type = cursorEventTypeFromJson(*type),
            .layerId = layer->get<int>(),
            .worldX = finiteNumber(item, "worldX"),
            .worldY = finiteNumber(item, "worldY"),
            .worldZ = finiteNumber(item, "worldZ"),
            .localX = finiteNumber(item, "localX"),
            .localY = finiteNumber(item, "localY"),
            .localZ = finiteNumber(item, "localZ"),
        };
        const auto hitBox = item.find("hitBox");
        if (hitBox != item.end() && !hitBox->is_null()) {
            if (!hitBox->is_string()) {
                throw std::invalid_argument("SceneScript cursor event hitBox must be a string");
            }
            event.hitBox = hitBox->get<std::string>();
        }
        result.push_back(std::move(event));
    }
    return result;
}

std::optional<ScriptMediaSnapshot> mediaSnapshotFromJson(const char* source) {
    if (source == nullptr) return std::nullopt;
    const Json json = Json::parse(source);
    if (json.is_null()) return std::nullopt;
    if (!json.is_object()) {
        throw std::invalid_argument("SceneScript media snapshot JSON must be an object");
    }
    ScriptMediaSnapshot snapshot;
    const auto readRevision = [&json](const char* key) {
        const auto found = json.find(key);
        if (found == json.end()) return std::uint64_t{0};
        if (!found->is_number_unsigned() && !found->is_number_integer()) {
            throw std::invalid_argument(
                std::string("SceneScript media ") + key + " must be an integer"
            );
        }
        const auto revision = found->get<std::int64_t>();
        if (revision < 0) {
            throw std::invalid_argument(
                std::string("SceneScript media ") + key +
                    " must be non-negative"
            );
        }
        return static_cast<std::uint64_t>(revision);
    };
    snapshot.statusRevision = readRevision("statusRevision");
    snapshot.metadataRevision = readRevision("metadataRevision");
    snapshot.playbackRevision = readRevision("playbackRevision");
    snapshot.timelineRevision = readRevision("timelineRevision");
    snapshot.thumbnailRevision = readRevision("thumbnailRevision");
    if (const auto found = json.find("available"); found != json.end()) {
        if (!found->is_boolean()) throw std::invalid_argument("SceneScript media available must be boolean");
        snapshot.available = found->get<bool>();
    }
    if (const auto found = json.find("playbackState"); found != json.end()) {
        if (!found->is_number_integer()) throw std::invalid_argument("SceneScript media playbackState must be an integer");
        const auto state = found->get<std::int32_t>();
        if (state < 0 || state > 2) throw std::invalid_argument("SceneScript media playbackState is out of range");
        snapshot.playbackState = static_cast<ScriptMediaPlaybackState>(state);
    }
    const auto readString = [&json](const char* key) {
        const auto found = json.find(key);
        if (found == json.end()) return std::string{};
        if (!found->is_string()) {
            throw std::invalid_argument(
                std::string("SceneScript media field '") + key + "' must be a string"
            );
        }
        return found->get<std::string>();
    };
    snapshot.title = readString("title");
    snapshot.artist = readString("artist");
    snapshot.contentType = readString("contentType");
    snapshot.albumTitle = readString("albumTitle");
    snapshot.subTitle = readString("subTitle");
    snapshot.albumArtist = readString("albumArtist");
    snapshot.genres = readString("genres");
    snapshot.position = finiteNumber(json, "position");
    snapshot.duration = finiteNumber(json, "duration");
    if (const auto found = json.find("hasThumbnail"); found != json.end()) {
        if (!found->is_boolean()) throw std::invalid_argument("SceneScript media hasThumbnail must be boolean");
        snapshot.hasThumbnail = found->get<bool>();
    }
    snapshot.primaryColor = vec3FromJson(json, "primaryColor");
    snapshot.secondaryColor = vec3FromJson(json, "secondaryColor");
    snapshot.tertiaryColor = vec3FromJson(json, "tertiaryColor");
    snapshot.textColor = vec3FromJson(json, "textColor");
    snapshot.highContrastColor = vec3FromJson(json, "highContrastColor");
    return snapshot;
}

std::optional<we::scene::script::ScriptSceneSnapshot> sceneSnapshotFromJson(
    const char* source
) {
    if (source == nullptr) return std::nullopt;
    const Json json = Json::parse(source);
    if (json.is_null()) return std::nullopt;
    if (!json.is_object()) {
        throw std::invalid_argument(
            "SceneScript scene snapshot JSON must be an object"
        );
    }
    using Snapshot = we::scene::script::ScriptSceneSnapshot;
    Snapshot snapshot;
    const auto required = [&json](const char* key) -> const Json& {
        const auto found = json.find(key);
        if (found == json.end()) {
            throw std::invalid_argument(
                std::string("SceneScript scene snapshot field '") + key +
                "' is required"
            );
        }
        return *found;
    };
    const auto requiredBool = [&required](const char* key) {
        const Json& value = required(key);
        if (!value.is_boolean()) {
            throw std::invalid_argument(
                std::string("SceneScript scene snapshot field '") + key +
                "' must be boolean"
            );
        }
        return value.get<bool>();
    };
    const auto requiredInt = [&required](const char* key) {
        const Json& value = required(key);
        if (!value.is_number_integer()) {
            throw std::invalid_argument(
                std::string("SceneScript scene snapshot field '") + key +
                "' must be an integer"
            );
        }
        return value.get<std::int32_t>();
    };
    const auto requiredNumber = [&required](const char* key) {
        const Json& value = required(key);
        if (!value.is_number()) {
            throw std::invalid_argument(
                std::string("SceneScript scene snapshot field '") + key +
                "' must be numeric"
            );
        }
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            throw std::invalid_argument(
                std::string("SceneScript scene snapshot field '") + key +
                "' must be finite"
            );
        }
        return number;
    };
    const auto requiredColor = [&required](const char* key) {
        const Json& value = required(key);
        if (!value.is_array() || value.size() != 3) {
            throw std::invalid_argument(
                std::string("SceneScript scene snapshot color '") + key +
                "' must be an array of three numbers"
            );
        }
        std::array<double, 3> result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            if (!value[index].is_number()) {
                throw std::invalid_argument(
                    std::string("SceneScript scene snapshot color '") + key +
                    "' must contain numbers"
                );
            }
            result[index] = value[index].get<double>();
            if (!std::isfinite(result[index])) {
                throw std::invalid_argument(
                    std::string("SceneScript scene snapshot color '") + key +
                    "' must contain finite numbers"
                );
            }
        }
        return result;
    };

    snapshot.bloom = requiredBool("bloom");
    snapshot.bloomStrength = requiredInt("bloomStrength");
    snapshot.bloomThreshold = requiredInt("bloomThreshold");
    snapshot.clearEnabled = requiredBool("clearEnabled");
    snapshot.clearColor = requiredColor("clearColor");
    snapshot.ambientColor = requiredColor("ambientColor");
    snapshot.skylightColor = requiredColor("skylightColor");
    snapshot.fieldOfView = requiredNumber("fieldOfView");
    snapshot.nearZ = requiredNumber("nearZ");
    snapshot.farZ = requiredNumber("farZ");
    snapshot.cameraFade = requiredBool("cameraFade");
    snapshot.cameraShake = requiredBool("cameraShake");
    snapshot.cameraShakeSpeed = requiredNumber("cameraShakeSpeed");
    snapshot.cameraShakeAmplitude = requiredNumber("cameraShakeAmplitude");
    snapshot.cameraShakeRoughness = requiredNumber("cameraShakeRoughness");
    snapshot.cameraParallax = requiredBool("cameraParallax");
    snapshot.cameraParallaxAmount = requiredNumber("cameraParallaxAmount");
    snapshot.cameraParallaxDelay = requiredNumber("cameraParallaxDelay");
    snapshot.cameraParallaxMouseInfluence = requiredNumber(
        "cameraParallaxMouseInfluence"
    );
    return snapshot;
}

WESceneScriptTestInstanceRef createInstance(
    const char* moduleSource,
    const char* initialValueJson,
    const char* scriptPropertiesJson,
    std::shared_ptr<WESceneScriptTestRuntimeStorage> storage,
    WESceneScriptTestErrorRef* outError,
    std::shared_ptr<ScriptLayerRegistry> layerRegistry = nullptr,
    std::optional<int> ownerLayerId = std::nullopt,
    std::optional<std::string> ownerProperty = std::nullopt
) {
    clearError(outError);
    if (moduleSource == nullptr || initialValueJson == nullptr ||
        scriptPropertiesJson == nullptr) {
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_MODULE,
            "Module source and JSON inputs must not be null"
        );
        return nullptr;
    }
    try {
        auto bridge = std::make_unique<WESceneScriptTestInstance>();
        bridge->storage = std::move(storage);
        bridge->layerRegistry = std::move(layerRegistry);
        we::scene::script::ScriptPropertyOwner owner;
        if (ownerLayerId) {
            owner.layerId = ownerLayerId;
            owner.type = we::scene::script::ScriptPropertyOwnerType::layer;
            owner.property = ownerProperty.value_or(std::string{});
        }
        bridge->instance = bridge->storage->runtime.createInstance(
            moduleSource,
            runtimeValueFromJson(Json::parse(initialValueJson)),
            propertiesFromJson(scriptPropertiesJson),
            std::nullopt,
            bridge->layerRegistry,
            nullptr,
            std::move(owner)
        );
        return bridge.release();
    } catch (const std::exception& exception) {
        setException(outError, exception);
        return nullptr;
    }
}

}  // namespace

extern "C" WESceneScriptTestInstanceRef we_scene_script_test_create(
    const char* moduleSource,
    const char* initialValueJson,
    const char* scriptPropertiesJson,
    WESceneScriptTestErrorRef* outError
) {
    try {
        return createInstance(
            moduleSource,
            initialValueJson,
            scriptPropertiesJson,
            std::make_shared<WESceneScriptTestRuntimeStorage>(),
            outError
        );
    } catch (const std::exception& exception) {
        clearError(outError);
        setException(outError, exception);
        return nullptr;
    }
}

extern "C" WESceneScriptTestInstanceRef
we_scene_script_test_create_with_execution_budget_nanoseconds(
    const char* moduleSource,
    const char* initialValueJson,
    const char* scriptPropertiesJson,
    uint64_t executionBudgetNanoseconds,
    WESceneScriptTestErrorRef* outError
) {
    if (executionBudgetNanoseconds >
        static_cast<uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        clearError(outError);
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_RESOURCE_LIMIT,
            "Execution budget exceeds the supported nanosecond range"
        );
        return nullptr;
    }
    ScriptLimits limits;
    limits.executionTime = std::chrono::nanoseconds(executionBudgetNanoseconds);
    try {
        return createInstance(
            moduleSource,
            initialValueJson,
            scriptPropertiesJson,
            std::make_shared<WESceneScriptTestRuntimeStorage>(limits),
            outError
        );
    } catch (const std::exception& exception) {
        clearError(outError);
        setException(outError, exception);
        return nullptr;
    }
}

extern "C" WESceneScriptTestRuntimeRef we_scene_script_test_runtime_create(
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    try {
        auto runtime = std::make_unique<WESceneScriptTestRuntime>();
        runtime->storage = std::make_shared<WESceneScriptTestRuntimeStorage>();
        return runtime.release();
    } catch (const std::exception& exception) {
        setException(outError, exception);
        return nullptr;
    }
}

extern "C" WESceneScriptTestInstanceRef we_scene_script_test_create_with_layers(
    const char* moduleSource,
    const char* initialValueJson,
    const char* scriptPropertiesJson,
    const WESceneScriptTestLayer* layers,
    size_t layerCount,
    int32_t ownerLayerId,
    const char* ownerProperty,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    if (ownerProperty == nullptr) {
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_MODULE,
            "Owner layer property must not be null"
        );
        return nullptr;
    }
    try {
        auto registry = std::make_shared<ScriptLayerRegistry>();
        registry->setBaseLayers(layersFromInputs(layers, layerCount));
        return createInstance(
            moduleSource,
            initialValueJson,
            scriptPropertiesJson,
            std::make_shared<WESceneScriptTestRuntimeStorage>(),
            outError,
            std::move(registry),
            ownerLayerId,
            std::string(ownerProperty)
        );
    } catch (const std::exception& exception) {
        setException(outError, exception);
        return nullptr;
    }
}

extern "C" void we_scene_script_test_runtime_destroy(
    WESceneScriptTestRuntimeRef runtime
) {
    delete runtime;
}

extern "C" WESceneScriptTestInstanceRef
we_scene_script_test_runtime_create_instance(
    WESceneScriptTestRuntimeRef runtime,
    const char* moduleSource,
    const char* initialValueJson,
    const char* scriptPropertiesJson,
    WESceneScriptTestErrorRef* outError
) {
    if (runtime == nullptr || runtime->storage == nullptr) {
        clearError(outError);
        setError(outError, WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION, "Script runtime is null");
        return nullptr;
    }
    return createInstance(
        moduleSource,
        initialValueJson,
        scriptPropertiesJson,
        runtime->storage,
        outError
    );
}

extern "C" void we_scene_script_test_destroy(
    WESceneScriptTestInstanceRef instance
) {
    delete instance;
}

namespace {

int evaluateInstanceWithSnapshot(
    WESceneScriptTestInstanceRef instance,
    double runtimeSeconds,
    double frameTimeSeconds,
    double pointerX,
    double pointerY,
    std::optional<double> timeOfDay,
    std::optional<AudioSpectrumFrame> audioSpectrum,
    std::vector<ScriptCursorEvent> cursorEvents,
    std::optional<ScriptMediaSnapshot> mediaSnapshot,
    std::optional<we::scene::script::ScriptSceneSnapshot> sceneSnapshot,
    char* outputJson,
    size_t outputJsonSize,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    if (outputJson != nullptr && outputJsonSize != 0) outputJson[0] = '\0';
    if (instance == nullptr || instance->instance == nullptr) {
        setError(outError, WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION, "Script instance is null");
        return 0;
    }
    if (outputJson == nullptr || outputJsonSize == 0) {
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION,
            "Output JSON buffer must have non-zero capacity"
        );
        return 0;
    }
    try {
        const RuntimeValue value = instance->instance->evaluate({
            .runtimeSeconds = runtimeSeconds,
            .frameTimeSeconds = frameTimeSeconds,
            .timeOfDay = timeOfDay,
            .isScreensaver = instance->isScreensaver,
            .audioSpectrum = std::move(audioSpectrum),
            .sceneSnapshot = std::move(sceneSnapshot),
            .userProperties = std::make_shared<ScriptUserPropertiesSnapshot>(
                ScriptUserPropertiesSnapshot{
                    .values = instance->userProperties,
                }
            ),
            .pointerX = pointerX,
            .pointerY = pointerY,
            .cursorEvents = std::move(cursorEvents),
            .mediaSnapshot = std::move(mediaSnapshot),
        });
        const std::string encoded = jsonFromRuntimeValue(value).dump();
        const std::size_t required = encoded.size() + 1;
        if (required > outputJsonSize) {
            throw std::length_error(
                "Output JSON buffer is too small: requires " +
                std::to_string(required) + " bytes including the terminator, got " +
                std::to_string(outputJsonSize)
            );
        }
        std::memcpy(outputJson, encoded.c_str(), required);
        return 1;
    } catch (const std::exception& exception) {
        outputJson[0] = '\0';
        setException(outError, exception);
        return 0;
    }
}

int evaluateInstance(
    WESceneScriptTestInstanceRef instance,
    double runtimeSeconds,
    double frameTimeSeconds,
    double pointerX,
    double pointerY,
    std::optional<double> timeOfDay,
    std::optional<AudioSpectrumFrame> audioSpectrum,
    std::vector<ScriptCursorEvent> cursorEvents,
    std::optional<ScriptMediaSnapshot> mediaSnapshot,
    char* outputJson,
    size_t outputJsonSize,
    WESceneScriptTestErrorRef* outError
) {
    return evaluateInstanceWithSnapshot(
        instance,
        runtimeSeconds,
        frameTimeSeconds,
        pointerX,
        pointerY,
        std::move(timeOfDay),
        std::move(audioSpectrum),
        std::move(cursorEvents),
        std::move(mediaSnapshot),
        std::nullopt,
        outputJson,
        outputJsonSize,
        outError
    );
}

}  // namespace

extern "C" int we_scene_script_test_evaluate(
    WESceneScriptTestInstanceRef instance,
    double runtimeSeconds,
    double frameTimeSeconds,
    double pointerX,
    double pointerY,
    char* outputJson,
    size_t outputJsonSize,
    WESceneScriptTestErrorRef* outError
) {
    return evaluateInstance(
        instance,
        runtimeSeconds,
        frameTimeSeconds,
        pointerX,
        pointerY,
        std::nullopt,
        std::nullopt,
        {},
        std::nullopt,
        outputJson,
        outputJsonSize,
        outError
    );
}

extern "C" int we_scene_script_test_evaluate_with_time_of_day(
    WESceneScriptTestInstanceRef instance,
    double runtimeSeconds,
    double frameTimeSeconds,
    double timeOfDay,
    double pointerX,
    double pointerY,
    char* outputJson,
    size_t outputJsonSize,
    WESceneScriptTestErrorRef* outError
) {
    return evaluateInstance(
        instance,
        runtimeSeconds,
        frameTimeSeconds,
        pointerX,
        pointerY,
        timeOfDay,
        std::nullopt,
        {},
        std::nullopt,
        outputJson,
        outputJsonSize,
        outError
    );
}

extern "C" int we_scene_script_test_evaluate_with_audio_spectrum(
    WESceneScriptTestInstanceRef instance,
    double runtimeSeconds,
    double frameTimeSeconds,
    const WESceneScriptTestAudioSpectrumInputs* audioSpectrum,
    double pointerX,
    double pointerY,
    char* outputJson,
    size_t outputJsonSize,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    try {
        return evaluateInstance(
            instance,
            runtimeSeconds,
            frameTimeSeconds,
            pointerX,
            pointerY,
            std::nullopt,
            audioSpectrumFromInputs(audioSpectrum),
            {},
            std::nullopt,
            outputJson,
            outputJsonSize,
            outError
        );
    } catch (const std::exception& exception) {
        if (outputJson != nullptr && outputJsonSize != 0) outputJson[0] = '\0';
        setException(outError, exception);
        return 0;
    }
}

extern "C" int we_scene_script_test_evaluate_with_events_json(
    WESceneScriptTestInstanceRef instance,
    double runtimeSeconds,
    double frameTimeSeconds,
    const char* cursorEventsJSON,
    const char* mediaSnapshotJSON,
    double pointerX,
    double pointerY,
    char* outputJSON,
    size_t outputJSONSize,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    try {
        return evaluateInstance(
            instance,
            runtimeSeconds,
            frameTimeSeconds,
            pointerX,
            pointerY,
            std::nullopt,
            std::nullopt,
            cursorEventsFromJson(cursorEventsJSON),
            mediaSnapshotFromJson(mediaSnapshotJSON),
            outputJSON,
            outputJSONSize,
            outError
        );
    } catch (const std::exception& exception) {
        if (outputJSON != nullptr && outputJSONSize != 0) outputJSON[0] = '\0';
        setException(outError, exception);
        return 0;
    }
}

extern "C" int we_scene_script_test_evaluate_with_scene_snapshot_json(
    WESceneScriptTestInstanceRef instance,
    double runtimeSeconds,
    double frameTimeSeconds,
    const char* sceneSnapshotJSON,
    double pointerX,
    double pointerY,
    char* outputJSON,
    size_t outputJSONSize,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    try {
        return evaluateInstanceWithSnapshot(
            instance,
            runtimeSeconds,
            frameTimeSeconds,
            pointerX,
            pointerY,
            std::nullopt,
            std::nullopt,
            {},
            std::nullopt,
            sceneSnapshotFromJson(sceneSnapshotJSON),
            outputJSON,
            outputJSONSize,
            outError
        );
    } catch (const std::exception& exception) {
        if (outputJSON != nullptr && outputJSONSize != 0) outputJSON[0] = '\0';
        setException(outError, exception);
        return 0;
    }
}

extern "C" int we_scene_script_test_register_audio_buffers(
    WESceneScriptTestInstanceRef instance,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    if (instance == nullptr || instance->instance == nullptr) {
        setError(outError, WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION, "Script instance is null");
        return 0;
    }
    try {
        instance->instance->registerAudioBuffers();
    } catch (const std::exception& exception) {
        setException(outError, exception);
        return 0;
    }
}

extern "C" int we_scene_script_test_update_properties(
    WESceneScriptTestInstanceRef instance,
    const char* scriptPropertiesJson,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    if (instance == nullptr || instance->instance == nullptr) {
        setError(outError, WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION, "Script instance is null");
        return 0;
    }
    if (scriptPropertiesJson == nullptr) {
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE,
            "Script properties JSON must not be null"
        );
        return 0;
    }
    try {
        instance->instance->updateProperties(
            propertiesFromJson(scriptPropertiesJson)
        );
        return 1;
    } catch (const std::exception& exception) {
        setException(outError, exception);
        return 0;
    }
}

extern "C" int we_scene_script_test_set_user_properties(
    WESceneScriptTestInstanceRef instance,
    const char* userPropertiesJson,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    if (instance == nullptr || instance->instance == nullptr) {
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION,
            "Script instance is null"
        );
        return 0;
    }
    if (userPropertiesJson == nullptr) {
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_INVALID_RESULT_TYPE,
            "User properties JSON must not be null"
        );
        return 0;
    }
    try {
        instance->userProperties = propertiesFromJson(
            userPropertiesJson,
            "User properties"
        );
        return 1;
    } catch (const std::exception& exception) {
        setException(outError, exception);
        return 0;
    }
}

extern "C" int we_scene_script_test_set_screensaver_state(
    WESceneScriptTestInstanceRef instance,
    int isScreensaver,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    if (instance == nullptr || instance->instance == nullptr) {
        setError(outError, WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION, "Script instance is null");
        return 0;
    }
    if (isScreensaver != 0 && isScreensaver != 1) {
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION,
            "Screensaver state must be zero or one"
        );
        return 0;
    }
    instance->isScreensaver = isScreensaver == 1;
    return 1;
}

extern "C" int we_scene_script_test_set_sound_runtime_state(
    WESceneScriptTestInstanceRef instance,
    int32_t layerId,
    WESceneScriptTestSoundRuntimeState state,
    double positionSeconds,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    if (instance == nullptr || instance->layerRegistry == nullptr) {
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION,
            "Layer registry is null"
        );
        return 0;
    }
    try {
        we::scene::script::ScriptSoundRuntimeState runtimeState;
        switch (state) {
            case WE_SCENE_SCRIPT_TEST_SOUND_STOPPED:
                runtimeState =
                    we::scene::script::ScriptSoundRuntimeState::stopped;
                break;
            case WE_SCENE_SCRIPT_TEST_SOUND_PLAYING:
                runtimeState =
                    we::scene::script::ScriptSoundRuntimeState::playing;
                break;
            case WE_SCENE_SCRIPT_TEST_SOUND_PAUSED:
                runtimeState =
                    we::scene::script::ScriptSoundRuntimeState::paused;
                break;
            case WE_SCENE_SCRIPT_TEST_SOUND_ENDED:
                runtimeState =
                    we::scene::script::ScriptSoundRuntimeState::ended;
                break;
            default:
                throw std::invalid_argument("Unknown sound runtime state");
        }
        instance->layerRegistry->setSoundRuntimeStates({{
            .layerId = layerId,
            .state = runtimeState,
            .positionSeconds = positionSeconds,
        }});
        return 1;
    } catch (const std::exception& exception) {
        setException(outError, exception);
        return 0;
    }
}

extern "C" int we_scene_script_test_layer_property(
    WESceneScriptTestInstanceRef instance,
    int32_t layerId,
    const char* property,
    char* outputJson,
    size_t outputJsonSize,
    WESceneScriptTestErrorRef* outError
) {
    clearError(outError);
    if (outputJson != nullptr && outputJsonSize != 0) outputJson[0] = '\0';
    if (instance == nullptr || instance->layerRegistry == nullptr) {
        setError(outError, WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION, "Layer registry is null");
        return 0;
    }
    if (property == nullptr || outputJson == nullptr || outputJsonSize == 0) {
        setError(
            outError,
            WE_SCENE_SCRIPT_TEST_ERROR_EXCEPTION,
            "Layer property and output buffer are required"
        );
        return 0;
    }
    try {
        const auto value = instance->layerRegistry->read(layerId, property);
        if (!value) {
            throw std::invalid_argument("Layer property is unavailable");
        }
        const std::string encoded = jsonFromRuntimeValue(*value).dump();
        if (encoded.size() + 1 > outputJsonSize) {
            throw std::length_error("Layer property JSON output buffer is too small");
        }
        std::memcpy(outputJson, encoded.c_str(), encoded.size() + 1);
        return 1;
    } catch (const std::exception& exception) {
        setException(outError, exception);
        return 0;
    }
}

extern "C" WESceneScriptTestErrorCode we_scene_script_test_error_code(
    WESceneScriptTestErrorRef error
) {
    return error == nullptr ? WE_SCENE_SCRIPT_TEST_ERROR_NONE : error->code;
}

extern "C" const char* we_scene_script_test_error_message(
    WESceneScriptTestErrorRef error
) {
    return error == nullptr ? "" : error->message.c_str();
}

extern "C" void we_scene_script_test_error_destroy(
    WESceneScriptTestErrorRef error
) {
    delete error;
}
