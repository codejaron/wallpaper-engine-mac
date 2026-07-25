#include <SceneScriptTestSupport/SceneScriptTestSupport.h>

#include <nlohmann/json.hpp>
#include <SceneScript/SceneScript.hpp>

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

using Json = nlohmann::json;
using we::scene::RuntimeValue;
using we::scene::RuntimeValueType;
using we::scene::script::ScriptError;
using we::scene::script::ScriptErrorCode;
using we::scene::script::ScriptFrameInputs;
using we::scene::script::ScriptInstance;
using we::scene::script::ScriptLimits;
using we::scene::script::ScriptRuntime;

struct WESceneScriptTestRuntimeStorage {
    explicit WESceneScriptTestRuntimeStorage(ScriptLimits limits = {}) : runtime(limits) {}

    ScriptRuntime runtime;
};

struct WESceneScriptTestRuntime {
    std::shared_ptr<WESceneScriptTestRuntimeStorage> storage;
};

struct WESceneScriptTestInstance {
    std::shared_ptr<WESceneScriptTestRuntimeStorage> storage;
    std::unique_ptr<ScriptInstance> instance;
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

std::map<std::string, RuntimeValue> propertiesFromJson(const char* source) {
    const Json json = Json::parse(source);
    if (!json.is_object()) {
        throw std::invalid_argument("Script properties JSON must be an object");
    }
    std::map<std::string, RuntimeValue> properties;
    for (const auto& [key, value] : json.items()) {
        properties.emplace(key, runtimeValueFromJson(value));
    }
    return properties;
}

WESceneScriptTestInstanceRef createInstance(
    const char* moduleSource,
    const char* initialValueJson,
    const char* scriptPropertiesJson,
    std::shared_ptr<WESceneScriptTestRuntimeStorage> storage,
    WESceneScriptTestErrorRef* outError
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
        bridge->instance = bridge->storage->runtime.createInstance(
            moduleSource,
            runtimeValueFromJson(Json::parse(initialValueJson)),
            propertiesFromJson(scriptPropertiesJson)
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
            .pointerX = pointerX,
            .pointerY = pointerY,
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
