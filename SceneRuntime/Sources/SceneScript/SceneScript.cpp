#include <SceneScript/SceneScript.hpp>
#include <quickjs.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace we::scene::script {
namespace {

constexpr std::string_view kScriptPropertiesBuiltins = R"JS(
globalThis.createScriptProperties = function createScriptProperties() {
    const values = globalThis.__sceneScriptProperties;
    const declared = globalThis.__sceneScriptDeclaredProperties;
    function add(options) {
        if (options === null || typeof options !== 'object' ||
            typeof options.name !== 'string' || options.name.length === 0) {
            throw new TypeError('script property declaration requires a non-empty name');
        }
        if (!Object.prototype.hasOwnProperty.call(values, options.name)) {
            throw new Error("script property '" + options.name + "' has no supplied value");
        }
        if (!declared.includes(options.name)) declared.push(options.name);
        return builder;
    }
    const builder = {
        addSlider: add,
        addCheckbox: add,
        addText: add,
        addCombo: add,
        addColor: add,
        finish: function finish() { return values; }
    };
    return builder;
};
)JS";

struct JSOwner {
    JSContext* ctx = nullptr;
    JSValue value = JS_UNDEFINED;
    JSOwner() = default;
    JSOwner(JSContext* c, JSValue v) : ctx(c), value(v) {}
    ~JSOwner() { if (ctx) JS_FreeValue(ctx, value); }
    JSOwner(const JSOwner&) = delete;
    JSOwner& operator=(const JSOwner&) = delete;
};

struct ScriptFailure {
    ScriptErrorCode code = ScriptErrorCode::exception;
    std::string message;
};

std::string stringValue(JSContext* ctx, JSValueConst value) {
    const char* text = JS_ToCString(ctx, value);
    if (!text) return {};
    std::string result(text);
    JS_FreeCString(ctx, text);
    return result;
}

ScriptFailure takeJSError(JSContext* ctx, ScriptErrorCode code, std::string phase) {
    JSOwner exception(ctx, JS_GetException(ctx));
    std::string message = stringValue(ctx, exception.value);
    JSOwner stack(ctx, JS_GetPropertyStr(ctx, exception.value, "stack"));
    std::string stackText = JS_IsUndefined(stack.value) ? std::string{} : stringValue(ctx, stack.value);
    if (message.empty()) message = "unknown JavaScript exception";
    phase += ": " + message;
    if (!stackText.empty() && stackText != message) phase += "\n" + stackText;
    return ScriptFailure{.code = code, .message = std::move(phase)};
}

[[noreturn]] void jsError(JSContext* ctx, ScriptErrorCode code, std::string phase) {
    ScriptFailure failure = takeJSError(ctx, code, std::move(phase));
    throw ScriptError(failure.code, std::move(failure.message));
}

void setProperty(JSContext* ctx, JSValueConst object, const char* name, JSValue value) {
    if (JS_SetPropertyStr(ctx, object, name, value) < 0) jsError(ctx, ScriptErrorCode::exception, "setting JavaScript property");
}

JSValue toJS(JSContext* ctx, const RuntimeValue& value) {
    switch (value.type()) {
        case RuntimeValueType::null:
            return JS_NULL;
        case RuntimeValueType::boolean:
            return JS_NewBool(ctx, value.boolean());
        case RuntimeValueType::integer:
            return JS_NewInt64(ctx, value.integer());
        case RuntimeValueType::floating:
            if (!std::isfinite(value.number())) {
                throw ScriptError(
                    ScriptErrorCode::nonFiniteResult,
                    "Scene value contains a non-finite number"
                );
            }
            return JS_NewFloat64(ctx, value.number());
        case RuntimeValueType::string:
            return JS_NewStringLen(
                ctx,
                value.string().data(),
                value.string().size()
            );
        case RuntimeValueType::vector2:
        case RuntimeValueType::vector3:
        case RuntimeValueType::vector4: {
            static constexpr const char* names[] = {"x", "y", "z", "w"};
            JSValue vector = JS_NewObject(ctx);
            for (std::size_t index = 0; index < value.componentCount(); ++index) {
                setProperty(
                    ctx,
                    vector,
                    names[index],
                    JS_NewFloat64(ctx, value.vector()[index])
                );
            }
            return vector;
        }
    }
    return JS_NULL;
}

RuntimeValue fromJS(
    JSContext* ctx,
    JSValueConst value,
    const RuntimeValue* previous = nullptr
) {
    if (JS_IsException(value)) {
        jsError(
            ctx,
            ScriptErrorCode::exception,
            "converting JavaScript DynamicValue result"
        );
    }
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        return previous == nullptr
            ? RuntimeValue::null()
            : previous->updatedToNull();
    }
    if (JS_IsBool(value)) return RuntimeValue::boolean(JS_ToBool(ctx, value) != 0);
    if (JS_IsNumber(value)) {
        std::int64_t integer = 0;
        if (JS_VALUE_GET_TAG(value) == JS_TAG_INT && JS_ToInt64(ctx, &integer, value) == 0) {
            return RuntimeValue::integer(integer);
        }
        double number = 0;
        if (JS_ToFloat64(ctx, &number, value) < 0) jsError(ctx, ScriptErrorCode::invalidResultType, "converting JavaScript number");
        const float narrowed = static_cast<float>(number);
        if (!std::isfinite(narrowed)) throw ScriptError(ScriptErrorCode::nonFiniteResult, "JavaScript returned a non-finite number");
        return RuntimeValue::floating(static_cast<double>(narrowed));
    }
    if (JS_IsString(value)) return RuntimeValue::string(stringValue(ctx, value));
    if (!JS_IsObject(value)) throw ScriptError(ScriptErrorCode::invalidResultType, "JavaScript returned an unsupported result type");
    if (JS_IsArray(value)) {
        throw ScriptError(
            ScriptErrorCode::invalidResultType,
            "JavaScript arrays are not DynamicValue vectors"
        );
    }

    static constexpr const char* names[] = {"x", "y", "z", "w"};
    std::array<double, 4> components{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        JSOwner component(ctx, JS_GetPropertyStr(ctx, value, names[index]));
        if (JS_IsException(component.value)) {
            jsError(ctx, ScriptErrorCode::exception, "reading JavaScript vector result");
        }
        if (!JS_IsNumber(component.value)) {
            if (index < 2) {
                throw ScriptError(
                    ScriptErrorCode::invalidResultType,
                    "JavaScript vector result requires numeric x and y components"
                );
            }
            break;
        }
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, component.value) < 0) {
            jsError(ctx, ScriptErrorCode::invalidResultType, "converting JavaScript vector component");
        }
        const float narrowed = static_cast<float>(number);
        if (!std::isfinite(narrowed)) {
            throw ScriptError(
                ScriptErrorCode::nonFiniteResult,
                "JavaScript returned a non-finite vector component"
            );
        }
        components[index] = static_cast<double>(narrowed);
        count = index + 1;
    }
    return RuntimeValue::vector(components, count);
}

bool vectorComponent(JSContext* ctx, JSValueConst object, const char* name, double& output) {
    JSOwner value(ctx, JS_GetPropertyStr(ctx, object, name));
    if (JS_IsException(value.value) || JS_ToFloat64(ctx, &output, value.value) < 0 ||
        !std::isfinite(output)) {
        JS_ThrowTypeError(ctx, "WEColor vector component '%s' must be a finite number", name);
        return false;
    }
    return true;
}

JSValue hsv2rgb(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "WEColor.hsv2rgb requires a vector object");
    }
    double hue = 0;
    double saturation = 0;
    double brightness = 0;
    if (!vectorComponent(ctx, argv[0], "x", hue) ||
        !vectorComponent(ctx, argv[0], "y", saturation) ||
        !vectorComponent(ctx, argv[0], "z", brightness)) {
        return JS_EXCEPTION;
    }
    // Adapted from linux-wallpaperengine ColorModule.cpp at
    // b016d7d1fdcf4e5fd2f9c9fa420a8aaa07fee02d (GPL-3.0). Wallpaper
    // Engine expresses hue in degrees here, not as a normalized fraction.
    const int index = static_cast<int>(hue / 60.0) % 6;
    const double fraction = (hue / 60.0) - index;
    const double p = brightness * (1.0 - saturation);
    const double q = brightness * (1.0 - fraction * saturation);
    const double t = brightness * (1.0 - (1.0 - fraction) * saturation);
    double red = 0;
    double green = 0;
    double blue = 0;
    switch (index) {
        case 0: red = brightness; green = t; blue = p; break;
        case 1: red = q; green = brightness; blue = p; break;
        case 2: red = p; green = brightness; blue = t; break;
        case 3: red = p; green = q; blue = brightness; break;
        case 4: red = t; green = p; blue = brightness; break;
        default: red = brightness; green = p; blue = q; break;
    }
    JSValue result = JS_NewObject(ctx);
    setProperty(ctx, result, "x", JS_NewFloat64(ctx, red));
    setProperty(ctx, result, "y", JS_NewFloat64(ctx, green));
    setProperty(ctx, result, "z", JS_NewFloat64(ctx, blue));
    return result;
}

int initializeWEColor(JSContext* ctx, JSModuleDef* module) {
    return JS_SetModuleExport(
        ctx, module, "hsv2rgb", JS_NewCFunction(ctx, hsv2rgb, "hsv2rgb", 1)
    );
}

JSModuleDef* loadBuiltinModule(JSContext* ctx, const char* name, void*) {
    if (std::string_view(name) != "WEColor") {
        JS_ThrowReferenceError(ctx, "Unsupported wallpaper script module '%s'", name);
        return nullptr;
    }
    JSModuleDef* module = JS_NewCModule(ctx, name, initializeWEColor);
    if (!module) return nullptr;
    if (JS_AddModuleExport(ctx, module, "hsv2rgb") < 0) return nullptr;
    return module;
}

} // namespace

struct ScriptRuntime::Impl {
    explicit Impl(ScriptLimits configured) : limits(configured) {
        if (!limits.memoryBytes || !limits.stackBytes || limits.executionTime.count() <= 0) {
            throw ScriptError(ScriptErrorCode::resourceLimit, "Script limits must be positive");
        }
        runtime = JS_NewRuntime();
        if (!runtime) throw ScriptError(ScriptErrorCode::resourceLimit, "Unable to allocate QuickJS runtime");
        JS_SetMemoryLimit(runtime, limits.memoryBytes);
        JS_SetMaxStackSize(runtime, limits.stackBytes);
        JS_SetModuleLoaderFunc(runtime, nullptr, loadBuiltinModule, nullptr);
        JS_SetInterruptHandler(runtime, [](JSRuntime*, void* opaque) -> int {
            auto* self = static_cast<Impl*>(opaque);
            if (self->active && std::chrono::steady_clock::now() >= self->deadline) {
                self->interrupted = true;
                return 1;
            }
            return 0;
        }, this);
    }
    ~Impl() {
        if (runtime) {
            JS_RunGC(runtime);
            JS_FreeRuntime(runtime);
        }
    }
    ScriptLimits limits;
    JSRuntime* runtime = nullptr;
    bool active = false;
    bool interrupted = false;
    std::chrono::steady_clock::time_point deadline;
    std::mutex mutex;
};

struct ScriptInstance::Impl {
    Impl(
        std::shared_ptr<ScriptRuntime::Impl> shared,
        std::string source,
        RuntimeValue initial,
        std::map<std::string, RuntimeValue> properties,
        std::optional<std::string> valueCondition
    )
        : runtime(std::move(shared)), current(std::move(initial)),
          condition(std::move(valueCondition)) {
        std::lock_guard lock(runtime->mutex);
        ctx = JS_NewContext(runtime->runtime);
        if (!ctx) throw ScriptError(ScriptErrorCode::resourceLimit, "Unable to allocate QuickJS context");
        try {
            JS_SetContextOpaque(ctx, this);
            JSOwner global(ctx, JS_GetGlobalObject(ctx));
            JSValue engine = JS_NewObject(ctx);
            setProperty(ctx, engine, "runtime", JS_NewFloat64(ctx, 0));
            setProperty(ctx, engine, "frametime", JS_NewFloat64(ctx, 0));
            setProperty(ctx, engine, "registerAudioBuffers", JS_NewCFunction(ctx,
                [](JSContext* context, JSValueConst, int, JSValueConst*) -> JSValue {
                    static_cast<Impl*>(JS_GetContextOpaque(context))->audioUnavailable = true;
                    return JS_ThrowInternalError(context, "audioInputUnavailable: system audio capture is not available");
                }, "registerAudioBuffers", 1));
            setProperty(ctx, global.value, "engine", engine);
            JSValue input = JS_NewObject(ctx);
            cursor = JS_NewObject(ctx);
            setProperty(ctx, input, "cursorScreenPosition", JS_DupValue(ctx, cursor));
            setProperty(ctx, global.value, "input", input);
            scriptProperties = JS_NewObject(ctx);
            if (JS_IsException(scriptProperties)) jsError(ctx, ScriptErrorCode::resourceLimit, "creating scriptProperties");
            for (const auto& [key, value] : properties) {
                setProperty(ctx, scriptProperties, key.c_str(), toJS(ctx, value));
            }
            setProperty(ctx, global.value, "__sceneScriptProperties", JS_DupValue(ctx, scriptProperties));
            setProperty(ctx, global.value, "__sceneScriptDeclaredProperties", JS_NewArray(ctx));
            JSOwner builtins(ctx, JS_Eval(
                ctx,
                kScriptPropertiesBuiltins.data(),
                kScriptPropertiesBuiltins.size(),
                "<scene-script-properties>",
                JS_EVAL_TYPE_GLOBAL
            ));
            if (JS_IsException(builtins.value)) jsError(
                ctx,
                ScriptErrorCode::module,
                "installing script property builtins"
            );
            compiled = JS_Eval(ctx, source.data(), source.size(), "<wallpaper-script>", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(compiled)) jsError(
                ctx,
                ScriptErrorCode::module,
                "compiling module"
            );
            if (JS_ResolveModule(ctx, compiled) < 0) jsError(
                ctx,
                ScriptErrorCode::module,
                "resolving module imports"
            );
            module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        } catch (...) {
            JS_FreeValue(ctx, compiled);
            compiled = JS_UNDEFINED;
            JS_FreeValue(ctx, cursor);
            cursor = JS_UNDEFINED;
            JS_FreeValue(ctx, scriptProperties);
            scriptProperties = JS_UNDEFINED;
            JS_FreeContext(ctx);
            ctx = nullptr;
            throw;
        }
    }
    ~Impl() {
        if (!ctx) return;
        std::lock_guard lock(runtime->mutex);
        JS_FreeValue(ctx, init);
        JS_FreeValue(ctx, update);
        JS_FreeValue(ctx, cursor);
        JS_FreeValue(ctx, scriptProperties);
        JS_FreeValue(ctx, compiled);
        JS_FreeContext(ctx);
    }
    struct BudgetScope {
        explicit BudgetScope(ScriptRuntime::Impl& target) : runtime(target) {
            runtime.deadline = std::chrono::steady_clock::now() + runtime.limits.executionTime;
            runtime.interrupted = false;
            runtime.active = true;
        }
        ~BudgetScope() { runtime.active = false; }

        ScriptRuntime::Impl& runtime;
    };

    struct DrainResult {
        bool executedAny = false;
        std::optional<ScriptFailure> failure;
    };

    static int failurePriority(ScriptErrorCode code) noexcept {
        if (code == ScriptErrorCode::audioInputUnavailable) return 3;
        if (code == ScriptErrorCode::resourceLimit) return 2;
        return 1;
    }

    void preferFailure(
        std::optional<ScriptFailure>& currentFailure,
        ScriptFailure candidate
    ) const {
        if (!currentFailure ||
            failurePriority(candidate.code) > failurePriority(currentFailure->code)) {
            currentFailure = std::move(candidate);
        }
    }

    ScriptFailure classifiedFailure(ScriptFailure failure) const {
        if (audioUnavailable) failure.code = ScriptErrorCode::audioInputUnavailable;
        else if (runtime->interrupted) failure.code = ScriptErrorCode::resourceLimit;
        return failure;
    }

    DrainResult drainJobs(const char* phase) {
        DrainResult drained;
        while (JS_IsJobPending(runtime->runtime)) {
            JSContext* jobContext = nullptr;
            const int result = JS_ExecutePendingJob(runtime->runtime, &jobContext);
            drained.executedAny = true;
            if (result < 0) {
                preferFailure(
                    drained.failure,
                    classifiedFailure(takeJSError(
                        jobContext ? jobContext : ctx,
                        ScriptErrorCode::exception,
                        phase
                    ))
                );
            }
        }
        if (audioUnavailable) {
            preferFailure(drained.failure, ScriptFailure{
                .code = ScriptErrorCode::audioInputUnavailable,
                .message = std::string(phase) +
                    ": audioInputUnavailable: system audio capture is not available",
            });
        } else if (runtime->interrupted) {
            preferFailure(drained.failure, ScriptFailure{
                .code = ScriptErrorCode::resourceLimit,
                .message = std::string(phase) + ": JavaScript execution interrupted",
            });
        }
        return drained;
    }

    void requireSynchronous(
        const char* lifecycle,
        bool returnedPromise,
        const char* jobPhase
    ) {
        DrainResult drained = drainJobs(jobPhase);
        if (drained.failure) {
            throw ScriptError(drained.failure->code, std::move(drained.failure->message));
        }
        if (returnedPromise || drained.executedAny) {
            throw ScriptError(
                ScriptErrorCode::invalidResultType,
                std::string("async ") + lifecycle + " is not supported"
            );
        }
    }

    void settleSynchronousCall(JSValueConst result, const char* lifecycle) {
        std::optional<ScriptFailure> callFailure;
        const bool returnedPromise = !JS_IsException(result) && JS_IsPromise(result);
        if (JS_IsException(result)) {
            callFailure = classifiedFailure(takeJSError(
                ctx,
                ScriptErrorCode::exception,
                std::string("calling ") + lifecycle
            ));
        }
        const std::string jobPhase = std::string("executing ") + lifecycle + " job";
        DrainResult callJobs = drainJobs(jobPhase.c_str());
        if (callJobs.failure) preferFailure(callFailure, std::move(*callJobs.failure));
        if (callFailure) {
            throw ScriptError(callFailure->code, std::move(callFailure->message));
        }
        if (returnedPromise || callJobs.executedAny) {
            throw ScriptError(
                ScriptErrorCode::invalidResultType,
                std::string("async ") + lifecycle + " is not supported"
            );
        }
    }

    [[noreturn]] void poisonAndThrow(
        const ScriptError& original,
        const char* cleanupPhase
    ) {
        std::optional<ScriptFailure> finalFailure = classifiedFailure(ScriptFailure{
            .code = original.code(),
            .message = original.what(),
        });
        DrainResult cleanup = drainJobs(cleanupPhase);
        if (cleanup.failure) preferFailure(finalFailure, std::move(*cleanup.failure));
        if (!failed) {
            failed = true;
            failureCode = finalFailure->code;
            failureMessage = std::move(finalFailure->message);
        }
        throw ScriptError(failureCode, failureMessage);
    }

    RuntimeValue invoke(
        JSValueConst function,
        const RuntimeValue& argument,
        const char* lifecycle
    ) {
        audioUnavailable = false;
        JSOwner jsArgument(ctx, toJS(ctx, argument));
        JSOwner result(ctx, JS_Call(ctx, function, JS_UNDEFINED, 1, &jsArgument.value));
        settleSynchronousCall(result.value, lifecycle);

        RuntimeValue converted = applyCondition(fromJS(
            ctx,
            result.value,
            &argument
        ));
        const std::string conversionPhase =
            std::string("converting ") + lifecycle + " result";
        requireSynchronous(
            lifecycle,
            false,
            conversionPhase.c_str()
        );
        return converted;
    }

    RuntimeValue invokeInit(JSValueConst function, const RuntimeValue& argument) {
        audioUnavailable = false;
        JSOwner jsArgument(ctx, toJS(ctx, argument));
        JSOwner result(ctx, JS_Call(ctx, function, JS_UNDEFINED, 1, &jsArgument.value));
        settleSynchronousCall(result.value, "init");

        RuntimeValue mutated = applyCondition(fromJS(ctx, jsArgument.value));
        requireSynchronous("init", false, "converting init argument");
        return mutated;
    }

    void evaluateModule() {
        audioUnavailable = false;
        JSOwner evaluation(ctx, JS_EvalFunction(ctx, std::exchange(compiled, JS_UNDEFINED)));
        std::optional<ScriptFailure> moduleFailure;
        if (JS_IsException(evaluation.value)) {
            moduleFailure = classifiedFailure(takeJSError(
                ctx,
                ScriptErrorCode::exception,
                "evaluating module"
            ));
        }
        DrainResult moduleJobs = drainJobs("executing module job");
        if (moduleJobs.failure) preferFailure(moduleFailure, std::move(*moduleJobs.failure));
        if (moduleFailure) {
            throw ScriptError(moduleFailure->code, std::move(moduleFailure->message));
        }
        if (JS_IsPromise(evaluation.value)) {
            const JSPromiseStateEnum state = JS_PromiseState(ctx, evaluation.value);
            if (state == JS_PROMISE_PENDING) {
                throw ScriptError(
                    ScriptErrorCode::exception,
                    "evaluating module: module promise remained pending"
                );
            }
            if (state == JS_PROMISE_REJECTED) {
                JS_Throw(ctx, JS_PromiseResult(ctx, evaluation.value));
                ScriptFailure failure = classifiedFailure(takeJSError(
                    ctx,
                    ScriptErrorCode::exception,
                    "evaluating module"
                ));
                throw ScriptError(failure.code, std::move(failure.message));
            }
        }
    }

    RuntimeValue evaluate(const ScriptFrameInputs& inputs) {
        std::lock_guard lock(runtime->mutex);
        if (failed) throw ScriptError(failureCode, failureMessage);
        if (!std::isfinite(inputs.runtimeSeconds) || inputs.runtimeSeconds < 0 ||
            !std::isfinite(inputs.frameTimeSeconds) || inputs.frameTimeSeconds < 0 ||
            !std::isfinite(inputs.pointerX) || !std::isfinite(inputs.pointerY)) {
            throw ScriptError(ScriptErrorCode::invalidResultType, "Script frame inputs must be finite and non-negative");
        }
        BudgetScope budget(*runtime);
        try {
            audioUnavailable = false;
            JSOwner global(ctx, JS_GetGlobalObject(ctx));
            JSOwner engine(ctx, JS_GetPropertyStr(ctx, global.value, "engine"));
            if (JS_IsException(engine.value)) {
                jsError(ctx, ScriptErrorCode::exception, "reading engine frame input");
            }
            setProperty(ctx, engine.value, "runtime", JS_NewFloat64(ctx, inputs.runtimeSeconds));
            setProperty(ctx, engine.value, "frametime", JS_NewFloat64(ctx, inputs.frameTimeSeconds));
            JSOwner input(ctx, JS_GetPropertyStr(ctx, global.value, "input"));
            if (JS_IsException(input.value)) {
                jsError(ctx, ScriptErrorCode::exception, "reading pointer frame input");
            }
            setProperty(ctx, cursor, "x", JS_NewFloat64(ctx, inputs.pointerX));
            setProperty(ctx, cursor, "y", JS_NewFloat64(ctx, inputs.pointerY));
            requireSynchronous("update", false, "updating frame inputs");
            if (!started) {
                evaluateModule();
                JSOwner nameSpace(ctx, JS_GetModuleNamespace(ctx, module));
                init = JS_GetPropertyStr(ctx, nameSpace.value, "init");
                update = JS_GetPropertyStr(ctx, nameSpace.value, "update");
                if (!JS_IsUndefined(init)) {
                    if (!JS_IsFunction(ctx, init)) throw ScriptError(ScriptErrorCode::invalidResultType, "Module export init must be a function");
                    current = invokeInit(init, current);
                }
                hasUpdate = !JS_IsUndefined(update) && JS_IsFunction(ctx, update);
                started = true;
            }
            if (!hasUpdate) {
                current = current.updatedToNull();
                return current;
            }
            current = invoke(update, current, "update");
            return current;
        } catch (const ScriptError& error) {
            poisonAndThrow(error, "cleaning failed evaluation jobs");
        }
    }
    RuntimeValue applyCondition(RuntimeValue value) const {
        if (condition && value.type() == RuntimeValueType::string) {
            return RuntimeValue::condition(value.string(), *condition);
        }
        return value;
    }
    RuntimeValue currentValue() const {
        std::lock_guard lock(runtime->mutex);
        return current;
    }
    void updateCurrent(RuntimeValue value) {
        std::lock_guard lock(runtime->mutex);
        if (failed && failureCode != ScriptErrorCode::audioInputUnavailable) {
            throw ScriptError(failureCode, failureMessage);
        }
        current = applyCondition(std::move(value));
    }
    void updateProperties(std::map<std::string, RuntimeValue> properties) {
        std::lock_guard lock(runtime->mutex);
        if (failed) throw ScriptError(failureCode, failureMessage);
        BudgetScope budget(*runtime);
        try {
            audioUnavailable = false;
            JSOwner global(ctx, JS_GetGlobalObject(ctx));
            JSOwner declared(ctx, JS_GetPropertyStr(
                ctx, global.value, "__sceneScriptDeclaredProperties"
            ));
            if (JS_IsException(declared.value)) {
                jsError(ctx, ScriptErrorCode::exception, "reading declared script properties");
            }
            JSOwner length(ctx, JS_GetPropertyStr(ctx, declared.value, "length"));
            std::uint32_t declaredCount = 0;
            if (JS_ToUint32(ctx, &declaredCount, length.value) < 0) {
                jsError(ctx, ScriptErrorCode::exception, "reading declared script properties");
            }
            for (std::uint32_t index = 0; index < declaredCount; ++index) {
                JSOwner name(ctx, JS_GetPropertyUint32(ctx, declared.value, index));
                if (JS_IsException(name.value)) {
                    jsError(ctx, ScriptErrorCode::exception, "reading declared script property");
                }
                const std::string key = stringValue(ctx, name.value);
                if (!properties.contains(key)) {
                    throw ScriptError(
                        ScriptErrorCode::invalidResultType,
                        "Script property '" + key + "' has no supplied value"
                    );
                }
            }

            // Convert the complete update before touching the live object. A
            // conversion failure therefore cannot leave a partially updated map.
            JSOwner staged(ctx, JS_NewObject(ctx));
            if (JS_IsException(staged.value)) {
                jsError(ctx, ScriptErrorCode::resourceLimit, "staging script properties");
            }
            for (const auto& [key, value] : properties) {
                setProperty(ctx, staged.value, key.c_str(), toJS(ctx, value));
            }
            requireSynchronous(
                "updateProperties",
                false,
                "validating script properties"
            );
            for (const auto& [key, value] : properties) {
                JSOwner converted(ctx, JS_GetPropertyStr(ctx, staged.value, key.c_str()));
                if (JS_IsException(converted.value)) {
                    jsError(ctx, ScriptErrorCode::exception, "reading staged script property");
                }
                setProperty(
                    ctx,
                    scriptProperties,
                    key.c_str(),
                    JS_DupValue(ctx, converted.value)
                );
            }
            requireSynchronous(
                "updateProperties",
                false,
                "applying script properties"
            );
        } catch (const ScriptError& error) {
            poisonAndThrow(error, "cleaning failed property jobs");
        }
    }
    std::shared_ptr<ScriptRuntime::Impl> runtime;
    JSContext* ctx = nullptr;
    JSValue compiled = JS_UNDEFINED;
    JSModuleDef* module = nullptr;
    JSValue init = JS_UNDEFINED;
    JSValue update = JS_UNDEFINED;
    JSValue cursor = JS_UNDEFINED;
    JSValue scriptProperties = JS_UNDEFINED;
    RuntimeValue current;
    std::optional<std::string> condition;
    bool started = false;
    bool hasUpdate = false;
    bool audioUnavailable = false;
    bool failed = false;
    ScriptErrorCode failureCode = ScriptErrorCode::exception;
    std::string failureMessage;
};

ScriptError::ScriptError(ScriptErrorCode code, std::string message) : std::runtime_error(std::move(message)), code_(code) {}
ScriptErrorCode ScriptError::code() const noexcept { return code_; }
ScriptRuntime::ScriptRuntime(ScriptLimits limits) : impl_(std::make_shared<Impl>(limits)) {}
ScriptRuntime::~ScriptRuntime() = default;
std::unique_ptr<ScriptInstance> ScriptRuntime::createInstance(
    std::string source,
    RuntimeValue initial,
    std::map<std::string, RuntimeValue> properties,
    std::optional<std::string> condition
) {
    return std::unique_ptr<ScriptInstance>(new ScriptInstance(std::make_unique<ScriptInstance::Impl>(
        impl_, std::move(source), std::move(initial), std::move(properties),
        std::move(condition))));
}
ScriptInstance::ScriptInstance(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ScriptInstance::~ScriptInstance() = default;
RuntimeValue ScriptInstance::evaluate(const ScriptFrameInputs& inputs) {
    return impl_->evaluate(inputs);
}
RuntimeValue ScriptInstance::currentValue() const {
    return impl_->currentValue();
}
void ScriptInstance::updateCurrent(RuntimeValue value) {
    impl_->updateCurrent(std::move(value));
}
void ScriptInstance::updateProperties(std::map<std::string, RuntimeValue> properties) {
    impl_->updateProperties(std::move(properties));
}
[[noreturn]] void ScriptInstance::registerAudioBuffers() {
    throw ScriptError(ScriptErrorCode::audioInputUnavailable, "audioInputUnavailable: system audio capture is not available");
}

} // namespace we::scene::script
