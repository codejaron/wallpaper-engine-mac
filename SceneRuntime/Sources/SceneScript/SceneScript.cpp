#include <SceneScript/SceneScript.hpp>
#include <quickjs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace we::scene::script {
namespace {

constexpr double degreesToRadians =
    0.01745329251994329576923690768489;
constexpr double radiansToDegrees =
    57.295779513082320876798154814105;

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

// Wallpaper Engine's script classes are ordinary JavaScript vector objects,
// rather than opaque host values.  Keeping the implementation in the realm
// also means `instanceof VecN`, retained references, and prototype methods all
// behave like the Windows/WE contract.  The implementation intentionally uses
// the sane arithmetic semantics from the shipped WE classes; the pinned Linux
// adapter has several reversed operands and a lengthSqr typo that we must not
// reproduce.
constexpr std::string_view kVectorBuiltins = R"JS(
(() => {
    const EPSILON = 0.00001;
    const NAMES = ['x', 'y', 'z', 'w'];
    let Vec2;
    let Vec3;
    let Vec4;

    const isInstance = (value, ctor) => Boolean(ctor) && value instanceof ctor;
    const component = (value, index) =>
        typeof value === 'number' ? value : value[NAMES[index]];

    function makeVector(size, name) {
        class Vector {
            constructor(x, y, z, w) {
                if (typeof x === 'string') {
                    const parts = x.split(' ');
                    for (let i = 0; i < size; ++i) {
                        this[NAMES[i]] = parseFloat(parts[i]);
                    }
                    return;
                }

                if ((size === 2 && (isInstance(x, Vec2) || isInstance(x, Vec3))) ||
                    (size === 3 && (isInstance(x, Vec2) || isInstance(x, Vec3))) ||
                    (size === 4 && (isInstance(x, Vec2) || isInstance(x, Vec3) || isInstance(x, Vec4)))) {
                    this.x = x.x;
                    this.y = x.y;
                    if (size >= 3) this.z = isInstance(x, Vec2) ? 0 : x.z;
                    if (size >= 4) {
                        this.w = isInstance(x, Vec2) ? 0 :
                            (isInstance(x, Vec3) ? 0 : x.w);
                    }
                    return;
                }

                if (typeof x !== 'undefined') {
                    this.x = x;
                    this.y = typeof y === 'number' ? y : x;
                    if (size >= 3) {
                        this.z = typeof z === 'number' ? z :
                            (typeof y === 'number' ? 0 : x);
                    }
                    if (size >= 4) {
                        this.w = typeof w === 'number' ? w :
                            (typeof z === 'number' ? z :
                                (typeof y === 'number' ? 0 : x));
                    }
                    return;
                }

                for (let i = 0; i < size; ++i) this[NAMES[i]] = 0;
            }
        }

        Object.defineProperty(Vector, 'name', { value: name });

        const result = (...values) => new Vector(...values);
        const binary = (self, value, operation, preserveVec2Z) => {
            const values = [];
            for (let i = 0; i < size; ++i) {
                if (preserveVec2Z && i === 2 && isInstance(value, Vec2)) {
                    values.push(self.z);
                } else {
                    values.push(operation(self[NAMES[i]], component(value, i)));
                }
            }
            return result(...values);
        };
        const scalarOrVector = (value, index) => component(value, index);
        const clampComponent = (value, min, max, index) =>
            Math.max(scalarOrVector(min, index),
                Math.min(scalarOrVector(max, index), value));

        const methods = {
            length() {
                let sum = 0;
                for (let i = 0; i < size; ++i) sum += this[NAMES[i]] * this[NAMES[i]];
                return Math.sqrt(sum);
            },
            lengthSqr() {
                let sum = 0;
                for (let i = 0; i < size; ++i) sum += this[NAMES[i]] * this[NAMES[i]];
                return sum;
            },
            distance(value) {
                return this.subtract(value).length();
            },
            distanceSqr(value) {
                return this.subtract(value).lengthSqr();
            },
            normalize() {
                return this.divide(this.length());
            },
            copy() {
                return result(...NAMES.slice(0, size).map((key) => this[key]));
            },
            equals(value) {
                if (!(value instanceof Vector)) return false;
                for (let i = 0; i < size; ++i) {
                    if (Math.abs(this[NAMES[i]] - value[NAMES[i]]) >= EPSILON) return false;
                }
                return true;
            },
            isFinite() {
                return NAMES.slice(0, size).every((key) => Number.isFinite(this[key]));
            },
            negate() {
                return result(...NAMES.slice(0, size).map((key) => -this[key]));
            },
            add(value) {
                return binary(this, value, (a, b) => a + b, size === 3);
            },
            subtract(value) {
                return binary(this, value, (a, b) => a - b, size === 3);
            },
            multiply(value) {
                return binary(this, value, (a, b) => a * b, size === 3);
            },
            divide(value) {
                return binary(this, value, (a, b) => a / b, size === 3);
            },
            dot(value) {
                let sum = 0;
                for (let i = 0; i < size; ++i) sum += this[NAMES[i]] * value[NAMES[i]];
                return sum;
            },
            reflect(value) {
                return this.subtract(value.multiply(2 * this.dot(value)));
            },
            project(value) {
                const denominator = value.lengthSqr();
                if (denominator === 0) return result(...NAMES.slice(0, size).map(() => 0));
                return value.multiply(this.dot(value) / denominator);
            },
            mix(value, amount) {
                const values = [];
                for (let i = 0; i < size; ++i) {
                    const a = component(amount, i);
                    values.push(this[NAMES[i]] + (value[NAMES[i]] - this[NAMES[i]]) * a);
                }
                return result(...values);
            },
            min(value) {
                return binary(this, value, (a, b) => Math.min(a, b), false);
            },
            max(value) {
                return binary(this, value, (a, b) => Math.max(a, b), false);
            },
            clamp(minimum, maximum) {
                return result(...NAMES.slice(0, size).map((key, index) =>
                    clampComponent(this[key], minimum, maximum, index)));
            },
            abs() {
                return result(...NAMES.slice(0, size).map((key) => Math.abs(this[key])));
            },
            sign() {
                return result(...NAMES.slice(0, size).map((key) => Math.sign(this[key])));
            },
            round() {
                return result(...NAMES.slice(0, size).map((key) => Math.round(this[key])));
            },
            floor() {
                return result(...NAMES.slice(0, size).map((key) => Math.floor(this[key])));
            },
            ceil() {
                return result(...NAMES.slice(0, size).map((key) => Math.ceil(this[key])));
            },
            fract() {
                return result(...NAMES.slice(0, size).map((key) =>
                    this[key] - Math.floor(this[key])));
            },
            mod(value) {
                return binary(this, value,
                    (a, b) => a - b * Math.floor(a / b), false);
            },
            step(edge) {
                return result(...NAMES.slice(0, size).map((key, index) =>
                    this[key] < scalarOrVector(edge, index) ? 0 : 1));
            },
            smoothStep(minimum, maximum) {
                return result(...NAMES.slice(0, size).map((key, index) => {
                    let t = (this[key] - scalarOrVector(minimum, index)) /
                        (scalarOrVector(maximum, index) - scalarOrVector(minimum, index));
                    t = Math.max(0, Math.min(1, t));
                    return t * t * (3 - 2 * t);
                }));
            },
            toString() {
                return NAMES.slice(0, size).map((key) => this[key]).join(' ');
            },
            toConfigString() {
                return this.toString();
            },
        };

        if (size === 2) {
            methods.perpendicular = function perpendicular() {
                return new Vector(this.y, -this.x);
            };
            methods.angle = function angle() {
                return Math.atan2(this.y, this.x) * 180 / Math.PI;
            };
            methods.angleBetween = function angleBetween(value) {
                return Math.atan2(this.x * value.y - this.y * value.x,
                    this.x * value.x + this.y * value.y) * 180 / Math.PI;
            };
            methods.rotate = function rotate(angle) {
                const radians = angle * Math.PI / 180;
                const cosine = Math.cos(radians);
                const sine = Math.sin(radians);
                return new Vector(
                    cosine * this.x - sine * this.y,
                    sine * this.x + cosine * this.y
                );
            };
        }

        if (size === 3) {
            methods.cross = function cross(value) {
                return new Vector(
                    this.y * value.z - this.z * value.y,
                    this.z * value.x - this.x * value.z,
                    this.x * value.y - this.y * value.x
                );
            };
            methods.refract = function refract(normal, eta) {
                const nDotI = normal.dot(this);
                const k = 1 - eta * eta * (1 - nDotI * nDotI);
                if (k < 0) return new Vector(0, 0, 0);
                return this.multiply(eta).subtract(
                    normal.multiply(eta * nDotI + Math.sqrt(k)));
            };
            methods.angleBetween = function angleBetween(value) {
                const denominator = Math.sqrt(this.lengthSqr() * value.lengthSqr());
                if (denominator === 0) return 0;
                return Math.acos(Math.max(-1, Math.min(1,
                    this.dot(value) / denominator))) * 180 / Math.PI;
            };
            methods.toSpherical = function toSpherical() {
                const radius = this.length();
                if (radius === 0) return new Vector(0, 0, 0);
                return new Vector(
                    radius,
                    Math.acos(this.y / radius) * 180 / Math.PI,
                    Math.atan2(this.z, this.x) * 180 / Math.PI
                );
            };
        }

        for (const [methodName, method] of Object.entries(methods)) {
            Object.defineProperty(Vector.prototype, methodName, {
                value: method,
                configurable: true,
                writable: true,
                enumerable: false,
            });
        }
        return Vector;
    }

    Vec2 = makeVector(2, 'Vec2');
    Vec3 = makeVector(3, 'Vec3');
    Vec4 = makeVector(4, 'Vec4');
    Vec3.fromSpherical = function fromSpherical(radius, theta, phi) {
        const t = theta * Math.PI / 180;
        const p = phi * Math.PI / 180;
        const sine = Math.sin(t);
        return new Vec3(
            radius * sine * Math.cos(p),
            radius * Math.cos(t),
            radius * sine * Math.sin(p)
        );
    };

    Object.defineProperties(globalThis, {
        Vec2: { value: Vec2, configurable: true, writable: true, enumerable: true },
        Vec3: { value: Vec3, configurable: true, writable: true, enumerable: true },
        Vec4: { value: Vec4, configurable: true, writable: true, enumerable: true },
        // Host-side RuntimeValue conversion must not start calling a
        // constructor that a script replaced on globalThis.  Keep an
        // immutable realm-local reference for that boundary while preserving
        // the normal script-visible mutability of VecN.
        __sceneScriptHostVec2: {
            value: Vec2, configurable: false, writable: false, enumerable: false,
        },
        __sceneScriptHostVec3: {
            value: Vec3, configurable: false, writable: false, enumerable: false,
        },
        __sceneScriptHostVec4: {
            value: Vec4, configurable: false, writable: false, enumerable: false,
        },
        _Vec2: { value: Vec2.prototype, configurable: true, writable: true, enumerable: false },
        _Vec3: { value: Vec3.prototype, configurable: true, writable: true, enumerable: false },
        _Vec4: { value: Vec4.prototype, configurable: true, writable: true, enumerable: false },
    });
})();
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

void defineProperty(
    JSContext* ctx,
    JSValueConst object,
    const char* name,
    JSValue value,
    int flags
) {
    if (JS_DefinePropertyValueStr(ctx, object, name, value, flags) < 0) {
        jsError(ctx, ScriptErrorCode::exception, "defining JavaScript property");
    }
}

void defineAccessor(
    JSContext* ctx,
    JSValueConst object,
    const char* name,
    JSValue getter,
    JSValue setter
) {
    const JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, getter);
        JS_FreeValue(ctx, setter);
        throw ScriptError(
            ScriptErrorCode::resourceLimit,
            "allocating JavaScript property atom"
        );
    }
    const int result = JS_DefinePropertyGetSet(
        ctx,
        object,
        atom,
        getter,
        setter,
        JS_PROP_ENUMERABLE
    );
    JS_FreeAtom(ctx, atom);
    if (result < 0) {
        jsError(ctx, ScriptErrorCode::exception, "defining JavaScript accessor");
    }
}

JSValue newVector(
    JSContext* ctx,
    std::size_t componentCount,
    const std::array<double, 4>& components
) {
    const char* constructorName = componentCount == 2
        ? "__sceneScriptHostVec2"
        : (componentCount == 3
            ? "__sceneScriptHostVec3"
            : "__sceneScriptHostVec4");
    JSOwner global(ctx, JS_GetGlobalObject(ctx));
    JSOwner constructor(ctx, JS_GetPropertyStr(ctx, global.value, constructorName));
    if (JS_IsException(constructor.value)) {
        jsError(ctx, ScriptErrorCode::exception, "reading vector constructor");
    }
    if (!JS_IsFunction(ctx, constructor.value)) {
        throw ScriptError(
            ScriptErrorCode::exception,
            std::string("host vector constructor ") + constructorName +
                " is not callable"
        );
    }

    std::array<JSValue, 4> arguments{
        JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
    };
    for (std::size_t index = 0; index < componentCount; ++index) {
        arguments[index] = JS_NewFloat64(ctx, components[index]);
    }
    JSValue result = JS_CallConstructor(
        ctx,
        constructor.value,
        static_cast<int>(componentCount),
        arguments.data()
    );
    for (std::size_t index = 0; index < componentCount; ++index) {
        JS_FreeValue(ctx, arguments[index]);
    }
    if (JS_IsException(result)) {
        jsError(ctx, ScriptErrorCode::exception, "constructing JavaScript vector");
    }
    return result;
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
            return newVector(ctx, value.componentCount(), value.vector());
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
    return newVector(ctx, 3, std::array<double, 4>{red, green, blue, 0.0});
}

JSValue rgb2hsv(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "WEColor.rgb2hsv requires a vector object");
    }
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (!vectorComponent(ctx, argv[0], "x", red) ||
        !vectorComponent(ctx, argv[0], "y", green) ||
        !vectorComponent(ctx, argv[0], "z", blue)) {
        return JS_EXCEPTION;
    }

    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double range = maximum - minimum;
    double hue = 0.0;
    double saturation = 0.0;
    if (maximum != 0.0 && range != 0.0) {
        saturation = range / maximum;
        if (maximum == red) {
            hue = 60.0 * ((green - blue) / range);
        } else if (maximum == green) {
            hue = 60.0 * ((blue - red) / range) + 120.0;
        } else {
            hue = 60.0 * ((red - green) / range) + 240.0;
        }
        if (hue < 0.0) hue += 360.0;
    }
    return newVector(
        ctx,
        3,
        std::array<double, 4>{hue, saturation, maximum, 0.0}
    );
}

JSValue scaleColor(
    JSContext* ctx,
    JSValueConst value,
    double scale,
    const char* functionName
) {
    if (!JS_IsObject(value)) {
        return JS_ThrowTypeError(
            ctx, "WEColor.%s requires a vector object", functionName
        );
    }
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (!vectorComponent(ctx, value, "x", red) ||
        !vectorComponent(ctx, value, "y", green) ||
        !vectorComponent(ctx, value, "z", blue)) {
        return JS_EXCEPTION;
    }
    return newVector(
        ctx,
        3,
        std::array<double, 4>{
            red * scale,
            green * scale,
            blue * scale,
            0.0,
        }
    );
}

JSValue normalizeColor(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv
) {
    if (argc < 1) {
        return JS_ThrowTypeError(
            ctx, "WEColor.normalizeColor requires a vector object"
        );
    }
    return scaleColor(ctx, argv[0], 1.0 / 255.0, "normalizeColor");
}

JSValue expandColor(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv
) {
    if (argc < 1) {
        return JS_ThrowTypeError(
            ctx, "WEColor.expandColor requires a vector object"
        );
    }
    return scaleColor(ctx, argv[0], 255.0, "expandColor");
}

bool scalarArguments(
    JSContext* ctx,
    int argc,
    JSValueConst* argv,
    std::array<double, 3>& values,
    const char* functionName
) {
    if (argc != 3) {
        JS_ThrowTypeError(ctx, "WEMath.%s requires three numbers", functionName);
        return false;
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (!JS_IsNumber(argv[index]) ||
            JS_ToFloat64(ctx, &values[index], argv[index]) < 0 ||
            !std::isfinite(values[index])) {
            JS_ThrowTypeError(
                ctx,
                "WEMath.%s argument %zu must be a finite number",
                functionName,
                index + 1
            );
            return false;
        }
    }
    return true;
}

JSValue mathSmoothStep(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv
) {
    std::array<double, 3> values{};
    if (!scalarArguments(ctx, argc, argv, values, "smoothStep")) {
        return JS_EXCEPTION;
    }
    double amount = (values[2] - values[0]) / (values[1] - values[0]);
    amount = std::clamp(amount, 0.0, 1.0);
    return JS_NewFloat64(ctx, amount * amount * (3.0 - 2.0 * amount));
}

JSValue mathMix(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv
) {
    std::array<double, 3> values{};
    if (!scalarArguments(ctx, argc, argv, values, "mix")) {
        return JS_EXCEPTION;
    }
    return JS_NewFloat64(
        ctx,
        values[0] + values[2] * (values[1] - values[0])
    );
}

int initializeWEColor(JSContext* ctx, JSModuleDef* module) {
    if (JS_SetModuleExport(
            ctx, module, "rgb2hsv",
            JS_NewCFunction(ctx, rgb2hsv, "rgb2hsv", 1)
        ) < 0 ||
        JS_SetModuleExport(
            ctx, module, "hsv2rgb",
            JS_NewCFunction(ctx, hsv2rgb, "hsv2rgb", 1)
        ) < 0 ||
        JS_SetModuleExport(
            ctx, module, "normalizeColor",
            JS_NewCFunction(ctx, normalizeColor, "normalizeColor", 1)
        ) < 0 ||
        JS_SetModuleExport(
            ctx, module, "expandColor",
            JS_NewCFunction(ctx, expandColor, "expandColor", 1)
        ) < 0) {
        return -1;
    }
    return 0;
}

int initializeWEMath(JSContext* ctx, JSModuleDef* module) {
    if (JS_SetModuleExport(
            ctx, module, "smoothStep",
            JS_NewCFunction(ctx, mathSmoothStep, "smoothStep", 3)
        ) < 0 ||
        JS_SetModuleExport(
            ctx, module, "mix",
            JS_NewCFunction(ctx, mathMix, "mix", 3)
        ) < 0 ||
        JS_SetModuleExport(
            ctx, module, "deg2rad", JS_NewFloat64(ctx, degreesToRadians)
        ) < 0 ||
        JS_SetModuleExport(
            ctx, module, "rad2deg", JS_NewFloat64(ctx, radiansToDegrees)
        ) < 0) {
        return -1;
    }
    return 0;
}

JSModuleDef* loadBuiltinModule(JSContext* ctx, const char* name, void*) {
    const std::string_view moduleName(name);
    JSModuleDef* module = nullptr;
    if (moduleName == "WEColor") {
        module = JS_NewCModule(ctx, name, initializeWEColor);
        if (!module) return nullptr;
        constexpr std::array exports{
            "rgb2hsv", "hsv2rgb", "normalizeColor", "expandColor",
        };
        for (const char* exportName : exports) {
            if (JS_AddModuleExport(ctx, module, exportName) < 0) return nullptr;
        }
        return module;
    }
    if (moduleName == "WEMath") {
        module = JS_NewCModule(ctx, name, initializeWEMath);
        if (!module) return nullptr;
        constexpr std::array exports{
            "smoothStep", "mix", "deg2rad", "rad2deg",
        };
        for (const char* exportName : exports) {
            if (JS_AddModuleExport(ctx, module, exportName) < 0) return nullptr;
        }
        return module;
    }
    {
        JS_ThrowReferenceError(ctx, "Unsupported wallpaper script module '%s'", name);
        return nullptr;
    }
}

} // namespace

struct ScriptPropertyObjectRegistry::Impl final {
    struct Object final {
        ScriptPropertyObjectDescriptor base;
        std::map<std::string, RuntimeValue> overlay;
        std::map<std::string, RuntimeValue> pending;
    };

    mutable std::recursive_mutex mutex;
    std::map<std::string, Object> objects;

    static ScriptPropertyObjectDescriptor effective(const Object& object) {
        ScriptPropertyObjectDescriptor result = object.base;
        for (const auto& [property, value] : object.overlay) {
            result.properties[property] = value;
        }
        return result;
    }
};

ScriptPropertyObjectRegistry::ScriptPropertyObjectRegistry()
    : impl_(std::make_unique<Impl>()) {}

ScriptPropertyObjectRegistry::~ScriptPropertyObjectRegistry() = default;

void ScriptPropertyObjectRegistry::setBaseObject(
    ScriptPropertyObjectDescriptor descriptor
) {
    if (descriptor.id.empty()) {
        throw std::invalid_argument(
            "SceneScript property object id must not be empty"
        );
    }
    std::lock_guard lock(impl_->mutex);
    Impl::Object next;
    next.base = std::move(descriptor);
    if (const auto old = impl_->objects.find(next.base.id);
        old != impl_->objects.end()) {
        if (old->second.base.type != next.base.type) {
            throw std::invalid_argument(
                "SceneScript property object '" + next.base.id +
                "' changed type"
            );
        }
        for (const auto& [property, value] : old->second.overlay) {
            if (next.base.properties.contains(property)) {
                next.overlay.emplace(property, value);
            }
        }
    }
    impl_->objects.insert_or_assign(next.base.id, std::move(next));
}

std::optional<ScriptPropertyObjectDescriptor>
ScriptPropertyObjectRegistry::find(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->objects.find(std::string(id));
    if (found == impl_->objects.end()) return std::nullopt;
    return Impl::effective(found->second);
}

std::optional<RuntimeValue> ScriptPropertyObjectRegistry::read(
    std::string_view id,
    std::string_view property
) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->objects.find(std::string(id));
    if (found == impl_->objects.end()) return std::nullopt;
    const Impl::Object& object = found->second;
    if (const auto overlay = object.overlay.find(std::string(property));
        overlay != object.overlay.end()) {
        return overlay->second;
    }
    const auto base = object.base.properties.find(std::string(property));
    if (base == object.base.properties.end()) return std::nullopt;
    return base->second;
}

void ScriptPropertyObjectRegistry::write(
    std::string_view id,
    std::string property,
    RuntimeValue value
) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->objects.find(std::string(id));
    if (found == impl_->objects.end()) {
        throw std::invalid_argument(
            "SceneScript property object '" + std::string(id) +
            "' does not exist"
        );
    }
    Impl::Object& object = found->second;
    if (!object.base.properties.contains(property) &&
        !object.overlay.contains(property)) {
        throw std::invalid_argument(
            "SceneScript property object '" + object.base.id +
            "' has no writable property '" + property + "'"
        );
    }
    object.overlay[property] = value;
    object.pending[property] = std::move(value);
}

std::optional<RuntimeValue> ScriptPropertyObjectRegistry::takePendingWrite(
    std::string_view id,
    std::string_view property
) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->objects.find(std::string(id));
    if (found == impl_->objects.end()) return std::nullopt;
    auto& pending = found->second.pending;
    const auto value = pending.find(std::string(property));
    if (value == pending.end()) return std::nullopt;
    RuntimeValue result = value->second;
    pending.erase(value);
    return result;
}

void ScriptPropertyObjectRegistry::commit(
    std::string_view id,
    std::string_view property,
    RuntimeValue value
) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->objects.find(std::string(id));
    if (found == impl_->objects.end()) {
        throw std::invalid_argument(
            "SceneScript property object '" + std::string(id) +
            "' does not exist"
        );
    }
    Impl::Object& object = found->second;
    if (!object.base.properties.contains(std::string(property)) &&
        !object.overlay.contains(std::string(property))) {
        throw std::invalid_argument(
            "SceneScript property object '" + object.base.id +
            "' has no property '" + std::string(property) + "'"
        );
    }
    object.overlay[std::string(property)] = std::move(value);
}

struct ScriptLayerRegistry::Impl final {
    struct TextureAnimationController final {
        bool joined = true;
        bool playing = true;
        double rate = 1.0;
        double anchorRuntimeSeconds = 0.0;
        double anchorPositionSeconds = 0.0;
        std::optional<std::size_t> forcedFrame;
        double forcedFrameRuntimeSeconds = 0.0;
    };

    struct SoundController final {
        std::optional<ScriptSoundCommand> command;
        ScriptSoundRuntimeState runtimeState =
            ScriptSoundRuntimeState::stopped;
        double positionSeconds = 0.0;
        std::optional<ScriptSoundCommandAction> pendingAction;
    };

    struct Layer final {
        ScriptLayerDescriptor base;
        std::map<std::string, RuntimeValue> overlay;
        std::map<std::string, RuntimeValue> pending;
        bool animationResolved = false;
        std::optional<ScriptTextureAnimationMetadata> animationMetadata;
        TextureAnimationController animation;
        SoundController sound;
    };

    mutable std::recursive_mutex mutex;
    std::vector<int> order;
    std::map<int, Layer> layers;
    double runtimeSeconds = 0.0;
    std::function<std::optional<ScriptTextureAnimationMetadata>(
        std::string_view
    )> textureAnimationResolver;

    static ScriptLayerDescriptor effective(const Layer& layer) {
        ScriptLayerDescriptor result = layer.base;
        for (const auto& [property, value] : layer.overlay) {
            result.properties[property] = value;
        }
        return result;
    }

    static void setSoundCommand(
        Layer& layer,
        ScriptSoundCommandAction action
    ) {
        if (layer.sound.command &&
            layer.sound.command->generation ==
                std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "SceneScript sound command generation overflowed"
            );
        }
        const std::uint64_t generation = layer.sound.command
            ? layer.sound.command->generation + 1
            : 1;
        layer.sound.command = ScriptSoundCommand{
            .action = action,
            .generation = generation,
        };
        layer.sound.pendingAction = action;
    }

    static double animationDuration(
        const ScriptTextureAnimationMetadata& metadata
    ) {
        double result = 0.0;
        for (const double duration : metadata.frameDurations) {
            if (!std::isfinite(duration) || duration < 0.0) {
                throw std::invalid_argument(
                    "Texture animation frame durations must be finite and non-negative"
                );
            }
            result += duration;
        }
        if (metadata.assetIdentity.empty() || metadata.frameDurations.empty() ||
            !std::isfinite(result) || result <= 0.0) {
            throw std::invalid_argument(
                "Texture animation metadata requires an asset, frames, and positive duration"
            );
        }
        return result;
    }

    static double wrapAnimationTime(double value, double duration) {
        double result = std::fmod(value, duration);
        if (result < 0.0) result += duration;
        return result;
    }

    static std::size_t frameAt(
        const ScriptTextureAnimationMetadata& metadata,
        double timeSeconds
    ) {
        const double total = animationDuration(metadata);
        double remaining = wrapAnimationTime(timeSeconds, total);
        std::optional<std::size_t> lastPositive;
        for (std::size_t index = 0;
             index < metadata.frameDurations.size(); ++index) {
            const double duration = metadata.frameDurations[index];
            if (duration > 0.0) lastPositive = index;
            remaining -= duration;
            // Match the renderer's TEXS timeline: the exact right boundary
            // belongs to the preceding frame.
            if (remaining <= 0.0) return index;
        }
        return lastPositive.value_or(0);
    }

    static double frameStart(
        const ScriptTextureAnimationMetadata& metadata,
        std::size_t frame
    ) {
        if (frame >= metadata.frameDurations.size()) {
            throw std::out_of_range("Texture animation frame is out of range");
        }
        double result = 0.0;
        for (std::size_t index = 0; index < frame; ++index) {
            result += metadata.frameDurations[index];
        }
        return result;
    }

    std::optional<ScriptTextureAnimationMetadata> resolveAnimation(
        Layer& layer
    ) {
        if (layer.animationResolved) return layer.animationMetadata;
        layer.animationResolved = true;
        if (layer.base.textureAnimation) {
            layer.animationMetadata = layer.base.textureAnimation;
        } else if (layer.base.textureAssetIdentity && textureAnimationResolver) {
            layer.animationMetadata = textureAnimationResolver(
                *layer.base.textureAssetIdentity
            );
        }
        if (layer.animationMetadata) {
            animationDuration(*layer.animationMetadata);
            if (layer.base.textureAssetIdentity &&
                layer.animationMetadata->assetIdentity !=
                    *layer.base.textureAssetIdentity) {
                throw std::invalid_argument(
                    "Texture animation metadata asset does not match its layer source"
                );
            }
        }
        return layer.animationMetadata;
    }

    ScriptTextureAnimationSnapshot animationSnapshot(Layer& layer) {
        const auto metadata = resolveAnimation(layer);
        if (!metadata) {
            throw std::invalid_argument(
                "SceneScript layer '" + layer.base.name +
                "' has no texture animation"
            );
        }
        const double duration = animationDuration(*metadata);
        const TextureAnimationController& controller = layer.animation;
        double position = 0.0;
        if (controller.joined) {
            position = wrapAnimationTime(runtimeSeconds, duration);
        } else if (controller.playing) {
            position = wrapAnimationTime(
                controller.anchorPositionSeconds +
                    (runtimeSeconds - controller.anchorRuntimeSeconds) *
                        controller.rate,
                duration
            );
        } else {
            position = wrapAnimationTime(
                controller.anchorPositionSeconds, duration
            );
        }
        std::size_t frame = frameAt(*metadata, position);
        if (!controller.joined && controller.forcedFrame &&
            (!controller.playing ||
             controller.forcedFrameRuntimeSeconds == runtimeSeconds)) {
            frame = *controller.forcedFrame;
        }
        return {
            .layerId = layer.base.id,
            .assetIdentity = metadata->assetIdentity,
            .frame = frame,
            .timeSeconds = position,
            .rate = controller.joined ? 1.0 : controller.rate,
            .joined = controller.joined,
            .playing = controller.joined || controller.playing,
        };
    }

    void detachAnimation(Layer& layer) {
        const ScriptTextureAnimationSnapshot current = animationSnapshot(layer);
        TextureAnimationController& controller = layer.animation;
        controller.joined = false;
        controller.playing = current.playing;
        controller.rate = current.rate;
        controller.anchorRuntimeSeconds = runtimeSeconds;
        controller.anchorPositionSeconds = current.timeSeconds;
        controller.forcedFrame = current.frame;
        controller.forcedFrameRuntimeSeconds = runtimeSeconds;
    }
};

ScriptLayerRegistry::ScriptLayerRegistry()
    : impl_(std::make_unique<Impl>()) {}

ScriptLayerRegistry::~ScriptLayerRegistry() = default;

void ScriptLayerRegistry::setBaseLayers(
    std::vector<ScriptLayerDescriptor> descriptors
) {
    std::lock_guard lock(impl_->mutex);
    std::map<int, Impl::Layer> next;
    std::vector<int> nextOrder;
    nextOrder.reserve(descriptors.size());
    for (auto& descriptor : descriptors) {
        if (descriptor.id == 0) {
            throw std::invalid_argument("SceneScript layer id must be non-zero");
        }
        if (!next.emplace(descriptor.id, Impl::Layer{}).second) {
            throw std::invalid_argument(
                "SceneScript layer registry contains duplicate id " +
                std::to_string(descriptor.id)
            );
        }
        nextOrder.push_back(descriptor.id);
        Impl::Layer& target = next.at(descriptor.id);
        target.base = std::move(descriptor);
        if (const auto old = impl_->layers.find(target.base.id);
            old != impl_->layers.end()) {
            // Keep only overlays for properties that still exist on the new
            // descriptor. Host schema changes must not leave stale writes
            // reachable through a newly reused layer id.
            for (const auto& [property, value] : old->second.overlay) {
                if (target.base.properties.contains(property)) {
                    target.overlay.emplace(property, value);
                }
            }
            if (old->second.base.type == target.base.type) {
                target.sound = old->second.sound;
            }
            const auto oldAsset = old->second.base.textureAssetIdentity;
            const auto nextAsset = target.base.textureAssetIdentity;
            if (oldAsset == nextAsset &&
                old->second.base.type == ScriptLayerType::image &&
                target.base.type == ScriptLayerType::image) {
                target.animation = old->second.animation;
                target.animationResolved = old->second.animationResolved;
                target.animationMetadata = old->second.animationMetadata;
            }
        }
        if (target.base.textureAnimation) {
            Impl::animationDuration(*target.base.textureAnimation);
            target.animationResolved = true;
            target.animationMetadata = target.base.textureAnimation;
            if (!target.base.textureAssetIdentity) {
                target.base.textureAssetIdentity =
                    target.base.textureAnimation->assetIdentity;
            }
        }
    }
    impl_->order = std::move(nextOrder);
    impl_->layers = std::move(next);
}

void ScriptLayerRegistry::setRuntimeSeconds(double runtimeSeconds) {
    if (!std::isfinite(runtimeSeconds) || runtimeSeconds < 0.0) {
        throw std::invalid_argument(
            "SceneScript layer runtime must be finite and non-negative"
        );
    }
    std::lock_guard lock(impl_->mutex);
    impl_->runtimeSeconds = runtimeSeconds;
}

void ScriptLayerRegistry::setTextureAnimationResolver(
    std::function<std::optional<ScriptTextureAnimationMetadata>(
        std::string_view
    )> resolver
) {
    std::lock_guard lock(impl_->mutex);
    impl_->textureAnimationResolver = std::move(resolver);
}

std::vector<ScriptLayerDescriptor> ScriptLayerRegistry::enumerate() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<ScriptLayerDescriptor> result;
    result.reserve(impl_->order.size());
    for (const int id : impl_->order) {
        result.push_back(Impl::effective(impl_->layers.at(id)));
    }
    return result;
}

std::optional<ScriptLayerDescriptor> ScriptLayerRegistry::find(int id) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->layers.find(id);
    if (found == impl_->layers.end()) return std::nullopt;
    return Impl::effective(found->second);
}

std::optional<ScriptLayerDescriptor> ScriptLayerRegistry::find(
    std::string_view name
) const {
    std::lock_guard lock(impl_->mutex);
    for (const int id : impl_->order) {
        const Impl::Layer& layer = impl_->layers.at(id);
        if (layer.base.name == name) {
            return Impl::effective(layer);
        }
    }
    return std::nullopt;
}

std::optional<RuntimeValue> ScriptLayerRegistry::read(
    int id,
    std::string_view property
) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->layers.find(id);
    if (found == impl_->layers.end()) return std::nullopt;
    const Impl::Layer& layer = found->second;
    if (const auto overlay = layer.overlay.find(std::string(property));
        overlay != layer.overlay.end()) {
        return overlay->second;
    }
    const auto base = layer.base.properties.find(std::string(property));
    if (base == layer.base.properties.end()) return std::nullopt;
    return base->second;
}

void ScriptLayerRegistry::write(
    int id,
    std::string property,
    RuntimeValue value
) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->layers.find(id);
    if (found == impl_->layers.end()) {
        throw std::invalid_argument(
            "SceneScript layer " + std::to_string(id) + " does not exist"
        );
    }
    Impl::Layer& layer = found->second;
    if (!layer.base.properties.contains(property) &&
        !layer.overlay.contains(property)) {
        throw std::invalid_argument(
            "SceneScript layer '" + layer.base.name +
            "' has no writable property '" + property + "'"
        );
    }
    layer.overlay[property] = value;
    layer.pending[property] = std::move(value);
}

std::optional<RuntimeValue> ScriptLayerRegistry::takePendingWrite(
    int id,
    std::string_view property
) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->layers.find(id);
    if (found == impl_->layers.end()) return std::nullopt;
    auto& pending = found->second.pending;
    const auto value = pending.find(std::string(property));
    if (value == pending.end()) return std::nullopt;
    RuntimeValue result = value->second;
    pending.erase(value);
    return result;
}

void ScriptLayerRegistry::commit(
    int id,
    std::string_view property,
    RuntimeValue value
) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->layers.find(id);
    if (found == impl_->layers.end()) return;
    Impl::Layer& layer = found->second;
    if (!layer.base.properties.contains(std::string(property)) &&
        !layer.overlay.contains(std::string(property))) {
        return;
    }
    layer.overlay[std::string(property)] = std::move(value);
    layer.pending.erase(std::string(property));
}

std::optional<ScriptTextureAnimationMetadata>
ScriptLayerRegistry::textureAnimationMetadata(int id) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->layers.find(id);
    if (found == impl_->layers.end()) {
        throw std::invalid_argument(
            "SceneScript layer " + std::to_string(id) + " does not exist"
        );
    }
    if (found->second.base.type != ScriptLayerType::image) {
        return std::nullopt;
    }
    return impl_->resolveAnimation(found->second);
}

ScriptTextureAnimationSnapshot
ScriptLayerRegistry::textureAnimationSnapshot(int id) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->layers.find(id);
    if (found == impl_->layers.end()) {
        throw std::invalid_argument(
            "SceneScript layer " + std::to_string(id) + " does not exist"
        );
    }
    return impl_->animationSnapshot(found->second);
}

std::vector<ScriptTextureAnimationSnapshot>
ScriptLayerRegistry::textureAnimationSnapshots() {
    std::lock_guard lock(impl_->mutex);
    std::vector<ScriptTextureAnimationSnapshot> result;
    for (const int id : impl_->order) {
        Impl::Layer& layer = impl_->layers.at(id);
        if (layer.base.type != ScriptLayerType::image ||
            !layer.animationResolved || !layer.animationMetadata) {
            continue;
        }
        result.push_back(impl_->animationSnapshot(layer));
    }
    return result;
}

void ScriptLayerRegistry::setTextureAnimationRate(int id, double rate) {
    if (!std::isfinite(rate)) {
        throw std::invalid_argument(
            "Texture animation rate must be finite"
        );
    }
    std::lock_guard lock(impl_->mutex);
    Impl::Layer& layer = impl_->layers.at(id);
    impl_->detachAnimation(layer);
    layer.animation.rate = rate;
}

void ScriptLayerRegistry::playTextureAnimation(int id) {
    std::lock_guard lock(impl_->mutex);
    Impl::Layer& layer = impl_->layers.at(id);
    impl_->detachAnimation(layer);
    layer.animation.playing = true;
    layer.animation.anchorRuntimeSeconds = impl_->runtimeSeconds;
}

void ScriptLayerRegistry::pauseTextureAnimation(int id) {
    std::lock_guard lock(impl_->mutex);
    Impl::Layer& layer = impl_->layers.at(id);
    impl_->detachAnimation(layer);
    layer.animation.playing = false;
}

void ScriptLayerRegistry::stopTextureAnimation(int id) {
    std::lock_guard lock(impl_->mutex);
    Impl::Layer& layer = impl_->layers.at(id);
    impl_->detachAnimation(layer);
    layer.animation.playing = false;
    layer.animation.anchorRuntimeSeconds = impl_->runtimeSeconds;
    layer.animation.anchorPositionSeconds = 0.0;
    layer.animation.forcedFrame = 0;
    layer.animation.forcedFrameRuntimeSeconds = impl_->runtimeSeconds;
}

void ScriptLayerRegistry::setTextureAnimationFrame(
    int id,
    std::size_t frame
) {
    std::lock_guard lock(impl_->mutex);
    Impl::Layer& layer = impl_->layers.at(id);
    impl_->detachAnimation(layer);
    const auto metadata = impl_->resolveAnimation(layer);
    if (!metadata || frame >= metadata->frameDurations.size()) {
        throw std::out_of_range("Texture animation frame is out of range");
    }
    layer.animation.anchorRuntimeSeconds = impl_->runtimeSeconds;
    layer.animation.anchorPositionSeconds = Impl::frameStart(*metadata, frame);
    layer.animation.forcedFrame = frame;
    layer.animation.forcedFrameRuntimeSeconds = impl_->runtimeSeconds;
}

void ScriptLayerRegistry::joinTextureAnimation(int id) {
    std::lock_guard lock(impl_->mutex);
    Impl::Layer& layer = impl_->layers.at(id);
    if (!impl_->resolveAnimation(layer)) {
        throw std::invalid_argument(
            "SceneScript layer '" + layer.base.name +
            "' has no texture animation"
        );
    }
    layer.animation = Impl::TextureAnimationController{};
}

ScriptSoundSnapshot ScriptLayerRegistry::soundSnapshot(int id) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->layers.find(id);
    if (found == impl_->layers.end()) {
        throw std::invalid_argument(
            "SceneScript layer " + std::to_string(id) + " does not exist"
        );
    }
    const Impl::Layer& layer = found->second;
    if (layer.base.type != ScriptLayerType::sound) {
        throw std::invalid_argument(
            "SceneScript layer '" + layer.base.name + "' is not a sound layer"
        );
    }
    return {
        .layerId = layer.base.id,
        .command = layer.sound.command,
        .runtimeState = layer.sound.runtimeState,
        .positionSeconds = layer.sound.positionSeconds,
    };
}

std::vector<ScriptSoundSnapshot>
ScriptLayerRegistry::soundSnapshots() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<ScriptSoundSnapshot> result;
    for (const int id : impl_->order) {
        if (impl_->layers.at(id).base.type == ScriptLayerType::sound) {
            result.push_back(soundSnapshot(id));
        }
    }
    return result;
}

void ScriptLayerRegistry::setSoundRuntimeStates(
    const std::vector<ScriptSoundRuntimeSnapshot>& states
) {
    std::lock_guard lock(impl_->mutex);
    for (auto& [id, layer] : impl_->layers) {
        (void)id;
        if (layer.base.type != ScriptLayerType::sound) continue;
        layer.sound.runtimeState = ScriptSoundRuntimeState::stopped;
        layer.sound.positionSeconds = 0.0;
        layer.sound.pendingAction.reset();
    }

    std::set<int> seen;
    for (const ScriptSoundRuntimeSnapshot& state : states) {
        if (!std::isfinite(state.positionSeconds) ||
            state.positionSeconds < 0.0) {
            throw std::invalid_argument(
                "SceneScript sound runtime position must be finite and non-negative"
            );
        }
        switch (state.state) {
            case ScriptSoundRuntimeState::stopped:
            case ScriptSoundRuntimeState::playing:
            case ScriptSoundRuntimeState::paused:
            case ScriptSoundRuntimeState::ended:
                break;
        }
        if (!seen.emplace(state.layerId).second) {
            throw std::invalid_argument(
                "SceneScript sound runtime state contains duplicate layer " +
                std::to_string(state.layerId)
            );
        }
        const auto found = impl_->layers.find(state.layerId);
        if (found == impl_->layers.end()) {
            throw std::invalid_argument(
                "SceneScript sound runtime state references unknown layer " +
                std::to_string(state.layerId)
            );
        }
        if (found->second.base.type != ScriptLayerType::sound) {
            throw std::invalid_argument(
                "SceneScript sound runtime state references a non-sound layer"
            );
        }
        found->second.sound.runtimeState = state.state;
        found->second.sound.positionSeconds = state.positionSeconds;
    }
}

void ScriptLayerRegistry::playSound(int id) {
    std::lock_guard lock(impl_->mutex);
    Impl::Layer& layer = impl_->layers.at(id);
    if (layer.base.type != ScriptLayerType::sound) {
        throw std::invalid_argument("SceneScript layer is not a sound layer");
    }
    if (layer.sound.pendingAction == ScriptSoundCommandAction::play ||
        (!layer.sound.pendingAction &&
         layer.sound.runtimeState == ScriptSoundRuntimeState::playing)) {
        return;
    }
    Impl::setSoundCommand(layer, ScriptSoundCommandAction::play);
}

void ScriptLayerRegistry::pauseSound(int id) {
    std::lock_guard lock(impl_->mutex);
    Impl::Layer& layer = impl_->layers.at(id);
    if (layer.base.type != ScriptLayerType::sound) {
        throw std::invalid_argument("SceneScript layer is not a sound layer");
    }
    if (layer.sound.pendingAction == ScriptSoundCommandAction::pause ||
        (!layer.sound.pendingAction &&
         layer.sound.runtimeState != ScriptSoundRuntimeState::playing)) {
        return;
    }
    Impl::setSoundCommand(layer, ScriptSoundCommandAction::pause);
}

void ScriptLayerRegistry::stopSound(int id) {
    std::lock_guard lock(impl_->mutex);
    Impl::Layer& layer = impl_->layers.at(id);
    if (layer.base.type != ScriptLayerType::sound) {
        throw std::invalid_argument("SceneScript layer is not a sound layer");
    }
    if (layer.sound.pendingAction == ScriptSoundCommandAction::stop ||
        (!layer.sound.pendingAction &&
         layer.sound.runtimeState == ScriptSoundRuntimeState::stopped)) {
        return;
    }
    Impl::setSoundCommand(layer, ScriptSoundCommandAction::stop);
}

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

        // `shared` is deliberately owned by a context that lives for the
        // whole ScriptRuntime. JS values are runtime-scoped in QuickJS, so a
        // duplicated reference can be installed in every instance context
        // without cloning the object or weakening identity semantics.
        sharedContext = JS_NewContext(runtime);
        if (!sharedContext) {
            JS_FreeRuntime(runtime);
            runtime = nullptr;
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "Unable to allocate shared SceneScript context"
            );
        }
        shared = JS_NewObject(sharedContext);
        if (JS_IsException(shared)) {
            JS_FreeContext(sharedContext);
            sharedContext = nullptr;
            JS_FreeRuntime(runtime);
            runtime = nullptr;
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "Unable to allocate shared SceneScript object"
            );
        }
    }
    ~Impl() {
        if (runtime) {
            JS_RunGC(runtime);
            if (sharedContext) {
                JS_FreeValue(sharedContext, shared);
                shared = JS_UNDEFINED;
                JS_FreeContext(sharedContext);
                sharedContext = nullptr;
            }
            JS_FreeRuntime(runtime);
        }
    }
    ScriptLimits limits;
    JSRuntime* runtime = nullptr;
    JSContext* sharedContext = nullptr;
    JSValue shared = JS_UNDEFINED;
    bool active = false;
    bool interrupted = false;
    std::chrono::steady_clock::time_point deadline;
    std::mutex mutex;
};

struct ScriptInstance::Impl {
    // A cancellation function can outlive the instance that created it (for
    // example after being placed in the runtime-wide `shared` object).  The
    // closure therefore carries an owner token instead of discovering the
    // owner from the context used to invoke it.  QuickJS may invoke a retained
    // function with the consumer realm's JSContext, which otherwise makes an
    // id collide with a timer in the wrong instance.
    struct TimerOwnerState {
        std::atomic<void*> owner = nullptr;
        void (*cancel)(void*, std::uint64_t) = nullptr;
    };

    struct TimerClosureData {
        std::shared_ptr<TimerOwnerState> owner;
        std::uint64_t id = 0;
    };

    struct LayerPropertyClosureData {
        std::shared_ptr<TimerOwnerState> owner;
        int layerId = 0;
        std::string property;
    };

    struct PropertyObjectClosureData {
        std::shared_ptr<TimerOwnerState> owner;
        std::string objectId;
        std::string property;
    };

    enum TextureAnimationClosureMagic : int {
        textureAnimationForLayer = 1,
        textureAnimationFrameCount = 2,
        textureAnimationDuration = 3,
        textureAnimationRate = 4,
        textureAnimationSetRate = 5,
        textureAnimationPlay = 6,
        textureAnimationStop = 7,
        textureAnimationPause = 8,
        textureAnimationIsPlaying = 9,
        textureAnimationGetFrame = 10,
        textureAnimationSetFrame = 11,
        textureAnimationJoin = 12,
        textureAnimationReadOnly = 13,
    };

    enum SoundClosureMagic : int {
        soundPlay = 1,
        soundPause = 2,
        soundStop = 3,
        soundIsPlaying = 4,
    };

    enum PropertyObjectClosureMagic : int {
        effectGetMaterial = 1,
        effectGetMaterialCount = 2,
        effectSetMaterialProperty = 3,
        effectExecuteMaterialFunction = 4,
    };

    static Impl* fromContext(JSContext* context) {
        auto* self = static_cast<Impl*>(JS_GetContextOpaque(context));
        if (self == nullptr) {
            JS_ThrowInternalError(context, "SceneScript context has no instance");
        }
        return self;
    }

    static void destroyLayerPropertyClosure(void* opaque) {
        delete static_cast<LayerPropertyClosureData*>(opaque);
    }

    static void destroyPropertyObjectClosure(void* opaque) {
        delete static_cast<PropertyObjectClosureData*>(opaque);
    }

    static Impl* fromLayerPropertyClosure(
        JSContext* context,
        void* opaque,
        LayerPropertyClosureData*& data
    ) {
        data = static_cast<LayerPropertyClosureData*>(opaque);
        if (data == nullptr || !data->owner) {
            JS_ThrowInternalError(context, "invalid SceneScript layer handle");
            return nullptr;
        }
        auto* self = static_cast<Impl*>(
            data->owner->owner.load(std::memory_order_acquire)
        );
        if (self == nullptr || !self->layerRegistry) {
            JS_ThrowInternalError(context, "SceneScript layer registry is unavailable");
            return nullptr;
        }
        return self;
    }

    static Impl* fromPropertyObjectClosure(
        JSContext* context,
        void* opaque,
        PropertyObjectClosureData*& data
    ) {
        data = static_cast<PropertyObjectClosureData*>(opaque);
        if (data == nullptr || !data->owner) {
            JS_ThrowInternalError(
                context, "invalid SceneScript property object handle"
            );
            return nullptr;
        }
        auto* self = static_cast<Impl*>(
            data->owner->owner.load(std::memory_order_acquire)
        );
        if (self == nullptr || !self->propertyObjectRegistry) {
            JS_ThrowInternalError(
                context,
                "SceneScript property object registry is unavailable"
            );
            return nullptr;
        }
        return self;
    }

    static JSValue getLayerProperty(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*,
        int,
        void* opaque
    ) {
        LayerPropertyClosureData* data = nullptr;
        Impl* self = fromLayerPropertyClosure(context, opaque, data);
        if (self == nullptr) return JS_EXCEPTION;
        const auto value = self->layerRegistry->read(data->layerId, data->property);
        if (!value) return JS_UNDEFINED;
        self->layerContractUsed = true;
        try {
            return toJS(context, *value);
        } catch (const ScriptError& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        } catch (const std::exception& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        }
    }

    static JSValue setLayerProperty(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv,
        int,
        void* opaque
    ) {
        LayerPropertyClosureData* data = nullptr;
        Impl* self = fromLayerPropertyClosure(context, opaque, data);
        if (self == nullptr) return JS_EXCEPTION;
        if (argc != 1) {
            return JS_ThrowTypeError(
                context,
                "SceneScript layer property setter requires one value"
            );
        }
        try {
            RuntimeValue value = fromJS(context, argv[0]);
            self->layerRegistry->write(
                data->layerId, data->property, std::move(value)
            );
            self->layerContractUsed = true;
            self->layerMutationUsed = true;
            return JS_UNDEFINED;
        } catch (const ScriptError& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        } catch (const std::exception& error) {
            return JS_ThrowTypeError(context, "%s", error.what());
        }
    }

    void defineRegistryProperty(
        JSValue object,
        int layerId,
        const std::string& property,
        std::string registryProperty = {}
    ) {
        if (registryProperty.empty()) registryProperty = property;
        auto* getterData = new (std::nothrow) LayerPropertyClosureData{
            .owner = timerOwner,
            .layerId = layerId,
            .property = registryProperty,
        };
        if (getterData == nullptr) {
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "allocating SceneScript layer getter"
            );
        }
        JSValue getter = JS_NewCClosure(
            ctx,
            getLayerProperty,
            nullptr,
            destroyLayerPropertyClosure,
            0,
            0,
            getterData
        );
        if (JS_IsException(getter)) {
            delete getterData;
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "creating SceneScript layer getter"
            );
        }
        auto* setterData = new (std::nothrow) LayerPropertyClosureData{
            .owner = timerOwner,
            .layerId = layerId,
            .property = std::move(registryProperty),
        };
        if (setterData == nullptr) {
            JS_FreeValue(ctx, getter);
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "allocating SceneScript layer setter"
            );
        }
        JSValue setter = JS_NewCClosure(
            ctx,
            setLayerProperty,
            nullptr,
            destroyLayerPropertyClosure,
            1,
            0,
            setterData
        );
        if (JS_IsException(setter)) {
            delete setterData;
            JS_FreeValue(ctx, getter);
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "creating SceneScript layer setter"
            );
        }
        defineAccessor(ctx, object, property.c_str(), getter, setter);
    }

    static JSValue getPropertyObjectProperty(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*,
        int,
        void* opaque
    ) {
        PropertyObjectClosureData* data = nullptr;
        Impl* self = fromPropertyObjectClosure(context, opaque, data);
        if (self == nullptr) return JS_EXCEPTION;
        const auto value = self->propertyObjectRegistry->read(
            data->objectId, data->property
        );
        if (!value) return JS_UNDEFINED;
        try {
            return toJS(context, *value);
        } catch (const ScriptError& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        } catch (const std::exception& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        }
    }

    static JSValue setPropertyObjectProperty(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv,
        int,
        void* opaque
    ) {
        PropertyObjectClosureData* data = nullptr;
        Impl* self = fromPropertyObjectClosure(context, opaque, data);
        if (self == nullptr) return JS_EXCEPTION;
        if (argc != 1) {
            return JS_ThrowTypeError(
                context,
                "SceneScript property object setter requires one value"
            );
        }
        try {
            self->propertyObjectRegistry->write(
                data->objectId, data->property, fromJS(context, argv[0])
            );
            self->layerMutationUsed = true;
            return JS_UNDEFINED;
        } catch (const ScriptError& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        } catch (const std::exception& error) {
            return JS_ThrowTypeError(context, "%s", error.what());
        }
    }

    void definePropertyObjectProperty(
        JSValue object,
        const std::string& objectId,
        const std::string& property
    ) {
        auto* getterData = new (std::nothrow) PropertyObjectClosureData{
            .owner = timerOwner,
            .objectId = objectId,
            .property = property,
        };
        if (getterData == nullptr) {
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "allocating SceneScript property object getter"
            );
        }
        JSValue getter = JS_NewCClosure(
            ctx,
            getPropertyObjectProperty,
            nullptr,
            destroyPropertyObjectClosure,
            0,
            0,
            getterData
        );
        if (JS_IsException(getter)) {
            delete getterData;
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "creating SceneScript property object getter"
            );
        }
        auto* setterData = new (std::nothrow) PropertyObjectClosureData{
            .owner = timerOwner,
            .objectId = objectId,
            .property = property,
        };
        if (setterData == nullptr) {
            JS_FreeValue(ctx, getter);
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "allocating SceneScript property object setter"
            );
        }
        JSValue setter = JS_NewCClosure(
            ctx,
            setPropertyObjectProperty,
            nullptr,
            destroyPropertyObjectClosure,
            1,
            0,
            setterData
        );
        if (JS_IsException(setter)) {
            delete setterData;
            JS_FreeValue(ctx, getter);
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "creating SceneScript property object setter"
            );
        }
        defineAccessor(ctx, object, property.c_str(), getter, setter);
    }

    JSValue makePropertyObjectClosure(
        const std::string& objectId,
        const char* name,
        int length,
        int magic
    ) {
        auto* data = new (std::nothrow) PropertyObjectClosureData{
            .owner = timerOwner,
            .objectId = objectId,
        };
        if (data == nullptr) {
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "allocating SceneScript property object method"
            );
        }
        JSValue function = JS_NewCClosure(
            ctx,
            propertyObjectMethod,
            name,
            destroyPropertyObjectClosure,
            length,
            magic,
            data
        );
        if (JS_IsException(function)) {
            delete data;
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "creating SceneScript property object method"
            );
        }
        return function;
    }

    JSValue makeLayerClosure(
        int layerId,
        const char* name,
        int length,
        int magic,
        JSCClosure* callback
    ) {
        auto* data = new (std::nothrow) LayerPropertyClosureData{
            .owner = timerOwner,
            .layerId = layerId,
        };
        if (data == nullptr) {
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "allocating SceneScript layer method"
            );
        }
        JSValue function = JS_NewCClosure(
            ctx,
            callback,
            name,
            destroyLayerPropertyClosure,
            length,
            magic,
            data
        );
        if (JS_IsException(function)) {
            delete data;
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "creating SceneScript layer method"
            );
        }
        return function;
    }

    static JSValue textureAnimationClosure(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv,
        int magic,
        void* opaque
    ) {
        LayerPropertyClosureData* data = nullptr;
        Impl* self = fromLayerPropertyClosure(context, opaque, data);
        if (self == nullptr) return JS_EXCEPTION;
        try {
            switch (magic) {
                case textureAnimationForLayer:
                    return self->makeTextureAnimationObject(data->layerId);
                case textureAnimationFrameCount: {
                    const auto metadata =
                        self->layerRegistry->textureAnimationMetadata(
                            data->layerId
                        );
                    if (!metadata) return JS_UNDEFINED;
                    return JS_NewFloat64(
                        context,
                        static_cast<double>(metadata->frameDurations.size())
                    );
                }
                case textureAnimationDuration: {
                    const auto metadata =
                        self->layerRegistry->textureAnimationMetadata(
                            data->layerId
                        );
                    if (!metadata) return JS_UNDEFINED;
                    double duration = 0.0;
                    for (const double frame : metadata->frameDurations) {
                        duration += frame;
                    }
                    return JS_NewFloat64(context, duration);
                }
                case textureAnimationRate:
                    return JS_NewFloat64(
                        context,
                        self->layerRegistry->textureAnimationSnapshot(
                            data->layerId
                        ).rate
                    );
                case textureAnimationSetRate: {
                    if (argc != 1) {
                        return JS_ThrowTypeError(
                            context,
                            "ITextureAnimation.rate setter requires one value"
                        );
                    }
                    double rate = 0.0;
                    if (JS_ToFloat64(context, &rate, argv[0]) < 0) {
                        return JS_EXCEPTION;
                    }
                    self->layerRegistry->setTextureAnimationRate(
                        data->layerId, rate
                    );
                    return JS_UNDEFINED;
                }
                case textureAnimationPlay:
                    self->layerRegistry->playTextureAnimation(data->layerId);
                    return JS_UNDEFINED;
                case textureAnimationStop:
                    self->layerRegistry->stopTextureAnimation(data->layerId);
                    return JS_UNDEFINED;
                case textureAnimationPause:
                    self->layerRegistry->pauseTextureAnimation(data->layerId);
                    return JS_UNDEFINED;
                case textureAnimationIsPlaying:
                    return JS_NewBool(
                        context,
                        self->layerRegistry->textureAnimationSnapshot(
                            data->layerId
                        ).playing
                    );
                case textureAnimationGetFrame:
                    return JS_NewFloat64(
                        context,
                        static_cast<double>(
                            self->layerRegistry->textureAnimationSnapshot(
                                data->layerId
                            ).frame
                        )
                    );
                case textureAnimationSetFrame: {
                    if (argc != 1) {
                        return JS_ThrowTypeError(
                            context,
                            "ITextureAnimation.setFrame requires one frame"
                        );
                    }
                    double frame = 0.0;
                    if (JS_ToFloat64(context, &frame, argv[0]) < 0) {
                        return JS_EXCEPTION;
                    }
                    if (!std::isfinite(frame) || frame < 0.0 ||
                        std::trunc(frame) != frame ||
                        frame > static_cast<double>(
                            std::numeric_limits<std::size_t>::max()
                        )) {
                        return JS_ThrowRangeError(
                            context,
                            "Texture animation frame must be a non-negative integer"
                        );
                    }
                    self->layerRegistry->setTextureAnimationFrame(
                        data->layerId, static_cast<std::size_t>(frame)
                    );
                    return JS_UNDEFINED;
                }
                case textureAnimationJoin:
                    self->layerRegistry->joinTextureAnimation(data->layerId);
                    return JS_UNDEFINED;
                case textureAnimationReadOnly:
                    return JS_ThrowTypeError(
                        context,
                        "ITextureAnimation property is read-only"
                    );
            }
            return JS_ThrowInternalError(
                context, "Unknown texture animation operation"
            );
        } catch (const std::out_of_range& error) {
            return JS_ThrowRangeError(context, "%s", error.what());
        } catch (const std::invalid_argument& error) {
            return JS_ThrowTypeError(context, "%s", error.what());
        } catch (const ScriptError& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        } catch (const std::exception& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        }
    }

    JSValue makeTextureAnimationObject(int layerId) {
        if (const auto found = textureAnimationObjects.find(layerId);
            found != textureAnimationObjects.end()) {
            return JS_DupValue(ctx, found->second);
        }
        const auto metadata = layerRegistry->textureAnimationMetadata(layerId);
        if (!metadata) return JS_UNDEFINED;

        JSValue object = JS_NewObject(ctx);
        if (JS_IsException(object)) {
            jsError(
                ctx,
                ScriptErrorCode::resourceLimit,
                "creating ITextureAnimation object"
            );
        }
        try {
            defineAccessor(
                ctx,
                object,
                "frameCount",
                makeLayerClosure(
                    layerId, "get frameCount", 0,
                    textureAnimationFrameCount, textureAnimationClosure
                ),
                makeLayerClosure(
                    layerId, "set frameCount", 1,
                    textureAnimationReadOnly, textureAnimationClosure
                )
            );
            defineAccessor(
                ctx,
                object,
                "duration",
                makeLayerClosure(
                    layerId, "get duration", 0,
                    textureAnimationDuration, textureAnimationClosure
                ),
                makeLayerClosure(
                    layerId, "set duration", 1,
                    textureAnimationReadOnly, textureAnimationClosure
                )
            );
            defineAccessor(
                ctx,
                object,
                "rate",
                makeLayerClosure(
                    layerId, "get rate", 0,
                    textureAnimationRate, textureAnimationClosure
                ),
                makeLayerClosure(
                    layerId, "set rate", 1,
                    textureAnimationSetRate, textureAnimationClosure
                )
            );
            for (const auto& method : std::array{
                     std::tuple{"play", 0, textureAnimationPlay},
                     std::tuple{"stop", 0, textureAnimationStop},
                     std::tuple{"pause", 0, textureAnimationPause},
                     std::tuple{"isPlaying", 0, textureAnimationIsPlaying},
                     std::tuple{"getFrame", 0, textureAnimationGetFrame},
                     std::tuple{"setFrame", 1, textureAnimationSetFrame},
                     std::tuple{"join", 0, textureAnimationJoin},
                 }) {
                const auto [name, length, operation] = method;
                setProperty(
                    ctx,
                    object,
                    name,
                    makeLayerClosure(
                        layerId, name, length, operation,
                        textureAnimationClosure
                    )
                );
            }
            textureAnimationObjects.emplace(
                layerId, JS_DupValue(ctx, object)
            );
            return object;
        } catch (...) {
            JS_FreeValue(ctx, object);
            throw;
        }
    }

    static JSValue soundClosure(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*,
        int magic,
        void* opaque
    ) {
        LayerPropertyClosureData* data = nullptr;
        Impl* self = fromLayerPropertyClosure(context, opaque, data);
        if (self == nullptr) return JS_EXCEPTION;
        try {
            switch (magic) {
                case soundPlay:
                    self->layerRegistry->playSound(data->layerId);
                    return JS_UNDEFINED;
                case soundPause:
                    self->layerRegistry->pauseSound(data->layerId);
                    return JS_UNDEFINED;
                case soundStop:
                    self->layerRegistry->stopSound(data->layerId);
                    return JS_UNDEFINED;
                case soundIsPlaying:
                    return JS_NewBool(
                        context,
                        self->layerRegistry->soundSnapshot(data->layerId)
                                .runtimeState ==
                            ScriptSoundRuntimeState::playing
                    );
            }
            return JS_ThrowInternalError(context, "Unknown sound operation");
        } catch (const std::invalid_argument& error) {
            return JS_ThrowTypeError(context, "%s", error.what());
        } catch (const std::exception& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        }
    }

    JSValue makeLayerObject(int layerId) {
        if (!layerRegistry) return JS_UNDEFINED;
        if (const auto found = layerObjects.find(layerId);
            found != layerObjects.end()) {
            return JS_DupValue(ctx, found->second);
        }
        const auto descriptor = layerRegistry->find(layerId);
        if (!descriptor) return JS_UNDEFINED;

        JSValue object = JS_NewObject(ctx);
        if (JS_IsException(object)) {
            jsError(ctx, ScriptErrorCode::resourceLimit, "creating SceneScript layer object");
        }
        defineProperty(
            ctx, object, "id", JS_NewInt32(ctx, descriptor->id),
            JS_PROP_ENUMERABLE
        );
        defineProperty(
            ctx, object, "name", JS_NewString(ctx, descriptor->name.c_str()),
            JS_PROP_ENUMERABLE
        );
        const char* type = "image";
        if (descriptor->type == ScriptLayerType::text) type = "text";
        if (descriptor->type == ScriptLayerType::particle) type = "particle";
        if (descriptor->type == ScriptLayerType::sound) type = "sound";
        defineProperty(
            ctx, object, "type", JS_NewString(ctx, type), JS_PROP_ENUMERABLE
        );
        for (const auto& [property, value] : descriptor->properties) {
            (void)value;
            defineRegistryProperty(object, descriptor->id, property);
        }
        if (descriptor->properties.contains("alpha") &&
            !descriptor->properties.contains("opacity")) {
            defineRegistryProperty(
                object, descriptor->id, "opacity", "alpha"
            );
        }
        if (descriptor->properties.contains("pointSize") &&
            !descriptor->properties.contains("pointsize")) {
            defineRegistryProperty(
                object, descriptor->id, "pointsize", "pointSize"
            );
        }
        if (descriptor->type == ScriptLayerType::image) {
            setProperty(
                ctx,
                object,
                "getTextureAnimation",
                makeLayerClosure(
                    descriptor->id,
                    "getTextureAnimation",
                    0,
                    textureAnimationForLayer,
                    textureAnimationClosure
                )
            );
        }
        if (descriptor->type == ScriptLayerType::sound) {
            for (const auto& method : std::array{
                     std::pair{"play", soundPlay},
                     std::pair{"pause", soundPause},
                     std::pair{"stop", soundStop},
                     std::pair{"isPlaying", soundIsPlaying},
                 }) {
                setProperty(
                    ctx,
                    object,
                    method.first,
                    makeLayerClosure(
                        descriptor->id,
                        method.first,
                        0,
                        method.second,
                        soundClosure
                    )
                );
            }
        }
        layerObjects.emplace(descriptor->id, JS_DupValue(ctx, object));
        return object;
    }

    static JSValue propertyObjectMethod(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv,
        int magic,
        void* opaque
    ) {
        PropertyObjectClosureData* data = nullptr;
        Impl* self = fromPropertyObjectClosure(context, opaque, data);
        if (self == nullptr) return JS_EXCEPTION;
        try {
            const auto descriptor = self->propertyObjectRegistry->find(
                data->objectId
            );
            if (!descriptor) {
                return JS_ThrowInternalError(
                    context,
                    "SceneScript property object '%s' no longer exists",
                    data->objectId.c_str()
                );
            }
            if (descriptor->type != ScriptPropertyObjectType::effect) {
                return JS_ThrowTypeError(
                    context, "SceneScript material has no effect method"
                );
            }
            switch (magic) {
                case effectGetMaterial: {
                    if (argc != 1) {
                        return JS_ThrowTypeError(
                            context, "IEffect.getMaterial requires one index"
                        );
                    }
                    std::int32_t index = 0;
                    if (JS_ToInt32(context, &index, argv[0]) < 0) {
                        return JS_EXCEPTION;
                    }
                    if (index < 0 ||
                        static_cast<std::size_t>(index) >=
                            descriptor->materialIds.size()) {
                        return JS_ThrowRangeError(
                            context, "IEffect material index is out of range"
                        );
                    }
                    return self->makePropertyObject(
                        descriptor->materialIds[static_cast<std::size_t>(index)]
                    );
                }
                case effectGetMaterialCount:
                    if (argc != 0) {
                        return JS_ThrowTypeError(
                            context,
                            "IEffect.getMaterialCount does not accept arguments"
                        );
                    }
                    return JS_NewInt64(
                        context,
                        static_cast<std::int64_t>(descriptor->materialIds.size())
                    );
                case effectSetMaterialProperty: {
                    if (argc != 2) {
                        return JS_ThrowTypeError(
                            context,
                            "IEffect.setMaterialProperty requires a property name and value"
                        );
                    }
                    const std::string property = stringValue(context, argv[0]);
                    const RuntimeValue value = fromJS(context, argv[1]);
                    for (const std::string& materialId : descriptor->materialIds) {
                        const auto material = self->propertyObjectRegistry->find(
                            materialId
                        );
                        if (material && material->properties.contains(property)) {
                            self->propertyObjectRegistry->write(
                                materialId, property, value
                            );
                        }
                    }
                    self->layerMutationUsed = true;
                    return JS_UNDEFINED;
                }
                case effectExecuteMaterialFunction:
                    return JS_ThrowInternalError(
                        context,
                        "IEffect.executeMaterialFunction is unsupported because the renderer has no custom material-function dispatch"
                    );
            }
            return JS_ThrowInternalError(
                context, "Unknown SceneScript property object operation"
            );
        } catch (const ScriptError& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        } catch (const std::invalid_argument& error) {
            return JS_ThrowTypeError(context, "%s", error.what());
        } catch (const std::exception& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        }
    }

    JSValue makePropertyObject(const std::string& objectId) {
        if (!propertyObjectRegistry) return JS_UNDEFINED;
        if (const auto found = propertyObjects.find(objectId);
            found != propertyObjects.end()) {
            return JS_DupValue(ctx, found->second);
        }
        const auto descriptor = propertyObjectRegistry->find(objectId);
        if (!descriptor) return JS_UNDEFINED;

        JSValue object = JS_NewObject(ctx);
        if (JS_IsException(object)) {
            jsError(
                ctx,
                ScriptErrorCode::resourceLimit,
                "creating SceneScript property object"
            );
        }
        try {
            if (!descriptor->name.empty()) {
                defineProperty(
                    ctx,
                    object,
                    "name",
                    JS_NewString(ctx, descriptor->name.c_str()),
                    JS_PROP_ENUMERABLE
                );
            }
            for (const auto& [property, value] : descriptor->properties) {
                (void)value;
                if (property == "name") continue;
                definePropertyObjectProperty(object, descriptor->id, property);
            }
            if (descriptor->type == ScriptPropertyObjectType::effect) {
                setProperty(
                    ctx,
                    object,
                    "getMaterial",
                    makePropertyObjectClosure(
                        descriptor->id,
                        "getMaterial",
                        1,
                        effectGetMaterial
                    )
                );
                setProperty(
                    ctx,
                    object,
                    "getMaterialCount",
                    makePropertyObjectClosure(
                        descriptor->id,
                        "getMaterialCount",
                        0,
                        effectGetMaterialCount
                    )
                );
                setProperty(
                    ctx,
                    object,
                    "setMaterialProperty",
                    makePropertyObjectClosure(
                        descriptor->id,
                        "setMaterialProperty",
                        2,
                        effectSetMaterialProperty
                    )
                );
                setProperty(
                    ctx,
                    object,
                    "executeMaterialFunction",
                    makePropertyObjectClosure(
                        descriptor->id,
                        "executeMaterialFunction",
                        1,
                        effectExecuteMaterialFunction
                    )
                );
            }
            propertyObjects.emplace(objectId, JS_DupValue(ctx, object));
            return object;
        } catch (...) {
            JS_FreeValue(ctx, object);
            throw;
        }
    }

    void clearLayerObjects() {
        for (auto& [id, object] : textureAnimationObjects) {
            (void)id;
            JS_FreeValue(ctx, object);
        }
        textureAnimationObjects.clear();
        for (auto& [id, object] : layerObjects) {
            (void)id;
            JS_FreeValue(ctx, object);
        }
        layerObjects.clear();
        for (auto& [id, object] : propertyObjects) {
            (void)id;
            JS_FreeValue(ctx, object);
        }
        propertyObjects.clear();
    }

    static JSValue getSceneLayer(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        if (self->layerRegistry == nullptr) {
            return JS_ThrowInternalError(
                context, "SceneScript layer registry is unavailable"
            );
        }
        if (argc != 1) {
            return JS_ThrowTypeError(context, "thisScene.getLayer requires one id or name");
        }
        if (JS_IsNumber(argv[0])) {
            int id = 0;
            if (JS_ToInt32(context, &id, argv[0]) < 0) return JS_EXCEPTION;
            return self->makeLayerObject(id);
        }
        if (JS_IsString(argv[0])) {
            const std::string name = stringValue(context, argv[0]);
            const auto descriptor = self->layerRegistry->find(name);
            return descriptor
                ? self->makeLayerObject(descriptor->id)
                : JS_UNDEFINED;
        }
        return JS_ThrowTypeError(context, "thisScene.getLayer requires an id or name");
    }

    static JSValue enumerateSceneLayers(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        if (self->layerRegistry == nullptr) {
            return JS_ThrowInternalError(
                context, "SceneScript layer registry is unavailable"
            );
        }
        JSValue result = JS_NewArray(context);
        if (JS_IsException(result)) return result;
        try {
            std::uint32_t index = 0;
            for (const auto& descriptor : self->layerRegistry->enumerate()) {
                JSValue layer = self->makeLayerObject(descriptor.id);
                if (JS_IsException(layer) ||
                    JS_SetPropertyUint32(context, result, index++, layer) < 0) {
                    if (!JS_IsException(layer)) JS_FreeValue(context, layer);
                    JS_FreeValue(context, result);
                    return JS_EXCEPTION;
                }
            }
            return result;
        } catch (const std::exception& error) {
            JS_FreeValue(context, result);
            return JS_ThrowInternalError(context, "%s", error.what());
        }
    }

    static JSValue getLayerText(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        self->layerContractUsed = true;
        return JS_DupValue(context, self->layerText);
    }

    static JSValue setLayerText(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        if (argc != 1) {
            return JS_ThrowInternalError(
                context,
                "thisLayer.text setter requires one value"
            );
        }
        JSValue next = JS_DupValue(context, argv[0]);
        JS_FreeValue(context, self->layerText);
        self->layerText = next;
        self->layerContractUsed = true;
        self->layerTextDirty = true;
        return JS_UNDEFINED;
    }

    static JSValue getSceneRuntime(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        return JS_NewFloat64(context, self->frameInputs.runtimeSeconds);
    }

    static JSValue getSceneFrameTime(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        return JS_NewFloat64(context, self->frameInputs.frameTimeSeconds);
    }

    static JSValue getSceneFPS(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        // This is the fallback used by the pinned Linux layer adapter when a
        // frame has no positive delta yet.
        const double fps = self->frameInputs.frameTimeSeconds > 0.0
            ? 1.0 / self->frameInputs.frameTimeSeconds
            : 60.0;
        return JS_NewFloat64(context, fps);
    }

    static JSValue getSceneTimeOfDay(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        if (!self->frameInputs.timeOfDay) {
            return JS_ThrowInternalError(
                context,
                "engine.timeOfDay is unavailable until the host supplies local-day time"
            );
        }
        return JS_NewFloat64(context, *self->frameInputs.timeOfDay);
    }

    static JSValue getEngineScreensaver(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        if (!self->frameInputs.isScreensaver) {
            return JS_ThrowInternalError(
                context,
                "engine.isScreensaver() is unavailable until the host supplies wallpaper mode"
            );
        }
        return JS_NewBool(context, *self->frameInputs.isScreensaver);
    }

    static JSValue engineIsScreensaver(
        JSContext* context,
        JSValueConst thisValue,
        int argc,
        JSValueConst* argv
    ) {
        if (argc != 0) {
            return JS_ThrowTypeError(
                context, "engine.isScreensaver() does not accept arguments"
            );
        }
        return getEngineScreensaver(context, thisValue, argc, argv);
    }

    static JSValue engineIsWallpaper(
        JSContext* context,
        JSValueConst thisValue,
        int argc,
        JSValueConst* argv
    ) {
        if (argc != 0) {
            return JS_ThrowTypeError(
                context, "engine.isWallpaper() does not accept arguments"
            );
        }
        JSValue value = getEngineScreensaver(context, thisValue, argc, argv);
        if (JS_IsException(value)) return value;
        const int screensaver = JS_ToBool(context, value);
        JS_FreeValue(context, value);
        return JS_NewBool(context, screensaver == 0);
    }

    static JSValue getEngineUserProperties(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        if (JS_IsUndefined(self->userProperties)) {
            return JS_ThrowInternalError(
                context,
                "engine.userProperties is unavailable until the host supplies project user properties"
            );
        }
        return JS_DupValue(context, self->userProperties);
    }

    static JSValue getCursorWorldPosition(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        if (!self->frameInputs.cursorWorldPosition) {
            return JS_ThrowInternalError(
                context,
                "input.cursorWorldPosition is unavailable until the frame projection is resolved"
            );
        }
        const auto& position = *self->frameInputs.cursorWorldPosition;
        return self->vec3(position[0], position[1], position[2]);
    }

    static JSValue getCursorLeftDown(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*
    ) {
        Impl* self = fromContext(context);
        return self == nullptr
            ? JS_EXCEPTION
            : JS_NewBool(context, self->frameInputs.pointerLeftDown);
    }

    static const ScriptSceneSnapshot* requireSceneSnapshot(
        JSContext* context,
        Impl* self
    ) {
        if (self == nullptr) return nullptr;
        if (!self->frameInputs.sceneSnapshot) {
            JS_ThrowInternalError(
                context,
                "thisScene values are unavailable until the host supplies a scene snapshot"
            );
            return nullptr;
        }
        return &*self->frameInputs.sceneSnapshot;
    }

    static JSValue getSceneBloom(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewBool(context, snapshot->bloom);
    }

    static JSValue getSceneBloomStrength(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewInt32(context, snapshot->bloomStrength);
    }

    static JSValue getSceneBloomThreshold(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewInt32(context, snapshot->bloomThreshold);
    }

    static JSValue getSceneClearEnabled(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            // Keep the pinned Linux adapter's historical wiring: its
            // `clearenabled` accessor reads the bloom-enabled flag rather
            // than a separate clear-enabled scene property.
            : JS_NewBool(context, snapshot->bloom);
    }

    static JSValue getSceneClearColor(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        if (snapshot == nullptr) return JS_EXCEPTION;
        return self->vec3(
            snapshot->clearColor[0],
            snapshot->clearColor[1],
            snapshot->clearColor[2]
        );
    }

    static JSValue getSceneAmbientColor(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        if (snapshot == nullptr) return JS_EXCEPTION;
        return self->vec3(
            snapshot->ambientColor[0],
            snapshot->ambientColor[1],
            snapshot->ambientColor[2]
        );
    }

    static JSValue getSceneSkylightColor(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        if (snapshot == nullptr) return JS_EXCEPTION;
        // The pinned Linux accessor aliases skylightcolor to ambientcolor.
        // Keep that behavior in the public adapter even when a host snapshot
        // carries the scene's distinct skylight value.
        return self->vec3(
            snapshot->ambientColor[0],
            snapshot->ambientColor[1],
            snapshot->ambientColor[2]
        );
    }

    static JSValue getSceneFOV(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewFloat64(context, snapshot->fieldOfView);
    }

    static JSValue getSceneNearZ(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewFloat64(context, snapshot->nearZ);
    }

    static JSValue getSceneFarZ(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewFloat64(context, snapshot->farZ);
    }

    static JSValue getSceneCameraFade(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewBool(context, snapshot->cameraFade);
    }

    static JSValue getSceneCameraShake(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewBool(context, snapshot->cameraShake);
    }

    static JSValue getSceneCameraShakeSpeed(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewFloat64(context, snapshot->cameraShakeSpeed);
    }

    static JSValue getSceneCameraShakeAmplitude(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewFloat64(context, snapshot->cameraShakeAmplitude);
    }

    static JSValue getSceneCameraShakeRoughness(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewFloat64(context, snapshot->cameraShakeRoughness);
    }

    static JSValue getSceneCameraParallax(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewBool(context, snapshot->cameraParallax);
    }

    static JSValue getSceneCameraParallaxAmount(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewFloat64(context, snapshot->cameraParallaxAmount);
    }

    static JSValue getSceneCameraParallaxDelay(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewFloat64(context, snapshot->cameraParallaxDelay);
    }

    static JSValue getSceneCameraParallaxMouseInfluence(
        JSContext* context, JSValueConst, int, JSValueConst*
    ) {
        Impl* self = fromContext(context);
        const auto* snapshot = requireSceneSnapshot(context, self);
        return snapshot == nullptr
            ? JS_EXCEPTION
            : JS_NewFloat64(context, snapshot->cameraParallaxMouseInfluence);
    }

    static JSValue rejectReadOnlyWrite(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst*
    ) {
        (void)argc;
        return JS_ThrowTypeError(context, "SceneScript engine property is read-only");
    }

    static JSValue consoleWrite(
        JSContext* context,
        int argc,
        JSValueConst* argv,
        bool error
    ) {
        if (argc < 1) return JS_UNDEFINED;
        std::string message;
        for (int index = 0; index < argc; ++index) {
            const char* text = JS_ToCString(context, argv[index]);
            if (text == nullptr) return JS_EXCEPTION;
            message += text;
            JS_FreeCString(context, text);
        }
        FILE* stream = error ? stderr : stdout;
        if (!message.empty()) {
            std::fwrite(message.data(), 1, message.size(), stream);
        }
        std::fwrite("\n", 1, 1, stream);
        std::fflush(stream);
        return JS_UNDEFINED;
    }

    static JSValue consoleLog(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv
    ) {
        return consoleWrite(context, argc, argv, false);
    }

    static JSValue consoleError(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv
    ) {
        return consoleWrite(context, argc, argv, true);
    }

    struct AudioBuffers {
        std::size_t resolution = 0;
        JSValue object = JS_UNDEFINED;
        JSValue left = JS_UNDEFINED;
        JSValue right = JS_UNDEFINED;
        JSValue average = JS_UNDEFINED;
    };

    static JSValue registerAudioBuffersFunction(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        if (argc < 1) {
            return JS_ThrowTypeError(
                context,
                "engine.registerAudioBuffers requires a resolution"
            );
        }
        double requested = 0.0;
        if (JS_ToFloat64(context, &requested, argv[0]) < 0) {
            return JS_EXCEPTION;
        }
        if (!std::isfinite(requested) ||
            (requested != 16.0 && requested != 32.0 && requested != 64.0)) {
            return JS_ThrowRangeError(
                context,
                "audio resolution must be 16, 32, or 64"
            );
        }
        if (!self->frameInputs.audioSpectrum) {
            self->audioUnavailable = true;
            return JS_ThrowInternalError(
                context,
                "audioInputUnavailable: system audio spectrum input is unavailable"
            );
        }
        try {
            return self->registerAudioBuffers(
                static_cast<std::size_t>(requested)
            );
        } catch (const ScriptError& error) {
            if (error.code() == ScriptErrorCode::resourceLimit) {
                return JS_ThrowOutOfMemory(context);
            }
            return JS_ThrowInternalError(context, "%s", error.what());
        } catch (const std::exception& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        } catch (...) {
            return JS_ThrowInternalError(
                context,
                "unknown failure while registering audio buffers"
            );
        }
    }

    template <std::size_t Count>
    void writeAudioBuffers(
        AudioBuffers& buffers,
        const std::array<float, Count>& left,
        const std::array<float, Count>& right
    ) {
        if (buffers.resolution != Count) {
            throw ScriptError(
                ScriptErrorCode::invalidResultType,
                "SceneScript audio buffer resolution does not match its host frame"
            );
        }
        for (std::uint32_t index = 0; index < Count; ++index) {
            const double leftValue = left[index];
            const double rightValue = right[index];
            if (JS_SetPropertyUint32(
                    ctx, buffers.left, index,
                    JS_NewFloat64(ctx, leftValue)
                ) < 0 ||
                JS_SetPropertyUint32(
                    ctx, buffers.right, index,
                    JS_NewFloat64(ctx, rightValue)
                ) < 0 ||
                JS_SetPropertyUint32(
                    ctx, buffers.average, index,
                    JS_NewFloat64(ctx, (leftValue + rightValue) * 0.5)
                ) < 0) {
                jsError(
                    ctx,
                    ScriptErrorCode::exception,
                    "updating SceneScript audio buffers"
                );
            }
        }
    }

    void writeAudioBuffers(
        AudioBuffers& buffers,
        const AudioSpectrumFrame& frame
    ) {
        switch (buffers.resolution) {
            case 16:
                writeAudioBuffers(
                    buffers, frame.spectrum16Left, frame.spectrum16Right
                );
                return;
            case 32:
                writeAudioBuffers(
                    buffers, frame.spectrum32Left, frame.spectrum32Right
                );
                return;
            case 64:
                writeAudioBuffers(
                    buffers, frame.spectrum64Left, frame.spectrum64Right
                );
                return;
        }
        throw ScriptError(
            ScriptErrorCode::invalidResultType,
            "SceneScript audio buffer has an unsupported resolution"
        );
    }

    JSValue registerAudioBuffers(std::size_t resolution) {
        if (const auto found = audioBuffers.find(resolution);
            found != audioBuffers.end()) {
            return JS_DupValue(ctx, found->second.object);
        }
        if (!frameInputs.audioSpectrum) {
            audioUnavailable = true;
            throw ScriptError(
                ScriptErrorCode::audioInputUnavailable,
                "audioInputUnavailable: system audio spectrum input is unavailable"
            );
        }

        AudioBuffers buffers{.resolution = resolution};
        try {
            buffers.object = JS_NewObject(ctx);
            buffers.left = JS_NewArray(ctx);
            buffers.right = JS_NewArray(ctx);
            buffers.average = JS_NewArray(ctx);
            if (JS_IsException(buffers.object) ||
                JS_IsException(buffers.left) ||
                JS_IsException(buffers.right) ||
                JS_IsException(buffers.average)) {
                jsError(
                    ctx,
                    ScriptErrorCode::resourceLimit,
                    "creating SceneScript audio buffers"
                );
            }
            defineProperty(
                ctx, buffers.object, "left",
                JS_DupValue(ctx, buffers.left), JS_PROP_ENUMERABLE
            );
            defineProperty(
                ctx, buffers.object, "right",
                JS_DupValue(ctx, buffers.right), JS_PROP_ENUMERABLE
            );
            defineProperty(
                ctx, buffers.object, "average",
                JS_DupValue(ctx, buffers.average), JS_PROP_ENUMERABLE
            );
            writeAudioBuffers(buffers, *frameInputs.audioSpectrum);
            auto [stored, inserted] = audioBuffers.emplace(
                resolution, buffers
            );
            if (!inserted) {
                throw ScriptError(
                    ScriptErrorCode::exception,
                    "SceneScript audio buffer registration identity collision"
                );
            }
            buffers = AudioBuffers{};
            return JS_DupValue(ctx, stored->second.object);
        } catch (...) {
            JS_FreeValue(ctx, buffers.average);
            JS_FreeValue(ctx, buffers.right);
            JS_FreeValue(ctx, buffers.left);
            JS_FreeValue(ctx, buffers.object);
            throw;
        }
    }

    void updateAudioBuffers() {
        if (audioBuffers.empty()) return;
        if (!frameInputs.audioSpectrum) {
            audioUnavailable = true;
            throw ScriptError(
                ScriptErrorCode::audioInputUnavailable,
                "audioInputUnavailable: system audio spectrum input is unavailable"
            );
        }
        for (auto& [resolution, buffers] : audioBuffers) {
            (void)resolution;
            writeAudioBuffers(buffers, *frameInputs.audioSpectrum);
        }
    }

    void clearAudioBuffers() {
        for (auto& [resolution, buffers] : audioBuffers) {
            (void)resolution;
            JS_FreeValue(ctx, buffers.average);
            JS_FreeValue(ctx, buffers.right);
            JS_FreeValue(ctx, buffers.left);
            JS_FreeValue(ctx, buffers.object);
        }
        audioBuffers.clear();
    }

    static JSValue cancelTimerClosure(
        JSContext* context,
        JSValueConst,
        int,
        JSValueConst*,
        int,
        void* opaque
    ) {
        auto* data = static_cast<TimerClosureData*>(opaque);
        if (data == nullptr || !data->owner) {
            return JS_ThrowInternalError(context, "invalid SceneScript timer handle");
        }
        // Normal script execution already holds the runtime mutex.  The
        // atomic owner token closes the lifetime race with instance teardown
        // without recursively locking that mutex from a callback.
        void* owner = data->owner->owner.load(std::memory_order_acquire);
        if (owner != nullptr && data->owner->cancel != nullptr) {
            data->owner->cancel(owner, data->id);
        }
        return JS_UNDEFINED;
    }

    static void destroyTimerClosure(void* opaque) {
        delete static_cast<TimerClosureData*>(opaque);
    }

    static JSValue scheduleTimerFunction(
        JSContext* context,
        JSValueConst,
        int argc,
        JSValueConst* argv,
        int magic
    ) {
        Impl* self = fromContext(context);
        if (self == nullptr) return JS_EXCEPTION;
        if (argc < 1 || !JS_IsFunction(context, argv[0])) {
            return JS_ThrowTypeError(
                context,
                "engine.%s requires a callback function",
                magic == 0 ? "setTimeout" : "setInterval"
            );
        }
        double delayMilliseconds = 0.0;
        if (argc >= 2 && JS_ToFloat64(context, &delayMilliseconds, argv[1]) < 0) {
            return JS_EXCEPTION;
        }
        if (!std::isfinite(delayMilliseconds)) {
            return JS_ThrowRangeError(context, "timer delay must be finite");
        }
        delayMilliseconds = std::max(0.0, delayMilliseconds);
        return self->scheduleTimer(context, argv[0], delayMilliseconds, magic != 0);
    }

    struct Timer {
        JSValue callback = JS_UNDEFINED;
        double dueRuntimeSeconds = 0.0;
        double intervalSeconds = 0.0;
        bool interval = false;
    };

    JSValue scheduleTimer(
        JSContext* context,
        JSValueConst callback,
        double delayMilliseconds,
        bool interval
    ) {
        const std::uint64_t id = ++nextTimerId;
        const double delaySeconds = delayMilliseconds / 1000.0;
        timers.emplace(
            id,
            Timer{
                .callback = JS_DupValue(context, callback),
                .dueRuntimeSeconds = frameInputs.runtimeSeconds + delaySeconds,
                .intervalSeconds = delaySeconds,
                .interval = interval,
            }
        );

        auto* closureData = new (std::nothrow) TimerClosureData{
            .owner = timerOwner,
            .id = id,
        };
        if (closureData == nullptr) {
            cancelTimer(id);
            throw ScriptError(
                ScriptErrorCode::resourceLimit,
                "allocating SceneScript timer cancellation handle"
            );
        }
        // Passing a null name avoids a second allocation after QuickJS has
        // taken ownership of `closureData`; all failure paths before the
        // opaque payload is attached are therefore safe to clean up here.
        JSValue cancel = JS_NewCClosure(
            context,
            cancelTimerClosure,
            nullptr,
            destroyTimerClosure,
            0,
            0,
            closureData
        );
        if (JS_IsException(cancel)) {
            delete closureData;
            cancelTimer(id);
            return JS_EXCEPTION;
        }
        return cancel;
    }

    void cancelTimer(std::uint64_t id) {
        const auto found = timers.find(id);
        if (found == timers.end()) return;
        JS_FreeValue(ctx, found->second.callback);
        timers.erase(found);
    }

    void clearTimers() {
        for (auto& [id, timer] : timers) {
            (void)id;
            JS_FreeValue(ctx, timer.callback);
        }
        timers.clear();
    }

    void processTimers() {
        std::vector<std::uint64_t> due;
        due.reserve(timers.size());
        for (const auto& [id, timer] : timers) {
            if (timer.interval && timer.dueRuntimeSeconds <= frameInputs.runtimeSeconds) {
                due.push_back(id);
            }
        }
        // The pinned Linux engine dispatches intervals before one-shot
        // timeouts on each tick.
        for (const auto& [id, timer] : timers) {
            if (!timer.interval && timer.dueRuntimeSeconds <= frameInputs.runtimeSeconds) {
                due.push_back(id);
            }
        }

        for (const std::uint64_t id : due) {
            const auto found = timers.find(id);
            if (found == timers.end()) continue; // cancelled by an earlier callback

            Timer& timer = found->second;
            const bool isInterval = timer.interval;
            JSOwner callback(ctx, JS_DupValue(ctx, timer.callback));
            if (isInterval) {
                // Advance before invoking user code. This prevents a zero-
                // duration interval from recursively firing in this tick and
                // matches the upstream one-dispatch-per-tick behavior.
                timer.dueRuntimeSeconds = frameInputs.runtimeSeconds + timer.intervalSeconds;
            } else {
                JS_FreeValue(ctx, timer.callback);
                timers.erase(found);
            }

            JSOwner result(ctx, JS_Call(ctx, callback.value, JS_NULL, 0, nullptr));
            settleSynchronousCall(result.value, isInterval ? "timer interval" : "timer timeout");
        }
    }

    void destroyRealm(bool final) noexcept {
        timerOwner->owner.store(nullptr, std::memory_order_release);
        if (ctx != nullptr) {
            clearTimers();
            clearAudioBuffers();
            clearLayerObjects();
            JS_FreeValue(ctx, init);
            JS_FreeValue(ctx, update);
            JS_FreeValue(ctx, thisScene);
            JS_FreeValue(ctx, thisLayer);
            JS_FreeValue(ctx, thisObject);
            JS_FreeValue(ctx, layerText);
            JS_FreeValue(ctx, cursor);
            JS_FreeValue(ctx, scriptProperties);
            JS_FreeValue(ctx, userProperties);
            JS_FreeValue(ctx, compiled);
            JS_FreeContext(ctx);
        } else {
            timers.clear();
            audioBuffers.clear();
            layerObjects.clear();
            textureAnimationObjects.clear();
            propertyObjects.clear();
        }

        ctx = nullptr;
        compiled = JS_UNDEFINED;
        module = nullptr;
        init = JS_UNDEFINED;
        update = JS_UNDEFINED;
        thisLayer = JS_UNDEFINED;
        thisObject = JS_UNDEFINED;
        thisScene = JS_UNDEFINED;
        layerText = JS_UNDEFINED;
        cursor = JS_UNDEFINED;
        scriptProperties = JS_UNDEFINED;
        userProperties = JS_UNDEFINED;
        nextTimerId = 0;
        started = false;
        hasUpdate = false;
        layerContractUsed = false;
        layerTextDirty = false;
        layerMutationUsed = false;
        lastInvocationReturnedUndefined = false;
        audioUnavailable = false;
        lastMediaRevision.reset();
        lastMediaAvailable.reset();
        pendingUserProperties = userPropertyValues;
        userPropertiesInitialized = false;
        if (!final) {
            timerOwner->owner.store(this, std::memory_order_release);
        }
    }

    void initializeRealm() {
        timerOwner->owner.store(this, std::memory_order_release);
        ctx = JS_NewContext(runtime->runtime);
        if (!ctx) throw ScriptError(ScriptErrorCode::resourceLimit, "Unable to allocate QuickJS context");
        try {
            JS_SetContextOpaque(ctx, this);
            JSOwner global(ctx, JS_GetGlobalObject(ctx));

            JSOwner vectorBuiltins(ctx, JS_Eval(
                ctx,
                kVectorBuiltins.data(),
                kVectorBuiltins.size(),
                "<scene-script-vectors>",
                JS_EVAL_TYPE_GLOBAL
            ));
            if (JS_IsException(vectorBuiltins.value)) {
                jsError(ctx, ScriptErrorCode::module, "installing vector builtins");
            }

            JSValue mediaPlaybackEvent = JS_NewObject(ctx);
            if (JS_IsException(mediaPlaybackEvent)) {
                jsError(
                    ctx,
                    ScriptErrorCode::resourceLimit,
                    "creating MediaPlaybackEvent constants"
                );
            }
            defineProperty(
                ctx,
                mediaPlaybackEvent,
                "PLAYBACK_STOPPED",
                JS_NewInt32(
                    ctx,
                    static_cast<std::int32_t>(ScriptMediaPlaybackState::stopped)
                ),
                JS_PROP_ENUMERABLE
            );
            defineProperty(
                ctx,
                mediaPlaybackEvent,
                "PLAYBACK_PLAYING",
                JS_NewInt32(
                    ctx,
                    static_cast<std::int32_t>(ScriptMediaPlaybackState::playing)
                ),
                JS_PROP_ENUMERABLE
            );
            defineProperty(
                ctx,
                mediaPlaybackEvent,
                "PLAYBACK_PAUSED",
                JS_NewInt32(
                    ctx,
                    static_cast<std::int32_t>(ScriptMediaPlaybackState::paused)
                ),
                JS_PROP_ENUMERABLE
            );
            defineProperty(
                ctx,
                global.value,
                "MediaPlaybackEvent",
                mediaPlaybackEvent,
                JS_PROP_ENUMERABLE
            );

            JSValue engine = JS_NewObject(ctx);
            defineAccessor(
                ctx,
                engine,
                "runtime",
                JS_NewCFunction(ctx, getSceneRuntime, "get runtime", 0),
                JS_NewCFunction(ctx, rejectReadOnlyWrite, "set runtime", 1)
            );
            defineAccessor(
                ctx,
                engine,
                "frametime",
                JS_NewCFunction(ctx, getSceneFrameTime, "get frametime", 0),
                JS_NewCFunction(ctx, rejectReadOnlyWrite, "set frametime", 1)
            );
            defineAccessor(
                ctx,
                engine,
                "time",
                JS_NewCFunction(ctx, getSceneRuntime, "get time", 0),
                JS_NewCFunction(ctx, rejectReadOnlyWrite, "set time", 1)
            );
            defineAccessor(
                ctx,
                engine,
                "timeOfDay",
                JS_NewCFunction(ctx, getSceneTimeOfDay, "get timeOfDay", 0),
                JS_NewCFunction(ctx, rejectReadOnlyWrite, "set timeOfDay", 1)
            );
            defineAccessor(
                ctx,
                engine,
                "userProperties",
                JS_NewCFunction(
                    ctx, getEngineUserProperties, "get userProperties", 0
                ),
                JS_NewCFunction(
                    ctx, rejectReadOnlyWrite, "set userProperties", 1
                )
            );
            setProperty(
                ctx,
                engine,
                "isScreensaver",
                JS_NewCFunction(ctx, engineIsScreensaver, "isScreensaver", 0)
            );
            setProperty(
                ctx,
                engine,
                "isWallpaper",
                JS_NewCFunction(ctx, engineIsWallpaper, "isWallpaper", 0)
            );
            defineProperty(ctx, engine, "AUDIO_RESOLUTION_16", JS_NewInt32(ctx, 16), JS_PROP_ENUMERABLE);
            defineProperty(ctx, engine, "AUDIO_RESOLUTION_32", JS_NewInt32(ctx, 32), JS_PROP_ENUMERABLE);
            defineProperty(ctx, engine, "AUDIO_RESOLUTION_64", JS_NewInt32(ctx, 64), JS_PROP_ENUMERABLE);
            defineProperty(
                ctx,
                engine,
                "setTimeout",
                JS_NewCFunctionMagic(ctx, scheduleTimerFunction, "setTimeout", 2, JS_CFUNC_generic_magic, 0),
                JS_PROP_ENUMERABLE
            );
            defineProperty(
                ctx,
                engine,
                "setInterval",
                JS_NewCFunctionMagic(ctx, scheduleTimerFunction, "setInterval", 2, JS_CFUNC_generic_magic, 1),
                JS_PROP_ENUMERABLE
            );
            setProperty(
                ctx,
                engine,
                "openUserShortcut",
                JS_NewCFunction(ctx,
                    [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
                        return JS_UNDEFINED;
                    },
                    "openUserShortcut",
                    0)
            );
            setProperty(
                ctx,
                engine,
                "registerAudioBuffers",
                JS_NewCFunction(
                    ctx,
                    registerAudioBuffersFunction,
                    "registerAudioBuffers",
                    1
                )
            );
            setProperty(ctx, global.value, "engine", engine);

            JSValue sharedValue = JS_DupValueRT(runtime->runtime, runtime->shared);
            defineProperty(ctx, global.value, "shared", sharedValue, JS_PROP_ENUMERABLE);

            JSValue console = JS_NewObject(ctx);
            setProperty(ctx, console, "log", JS_NewCFunction(ctx, consoleLog, "log", 1));
            setProperty(ctx, console, "error", JS_NewCFunction(ctx, consoleError, "error", 1));
            setProperty(ctx, global.value, "console", console);

            // Keep one object identity for the lifetime of the module. When
            // the host supplied a real layer owner, this object is a view over
            // the shared registry; standalone DynamicValue scripts retain the
            // historical local text view.
            if (layerRegistry && owner.layerId) {
                thisLayer = makeLayerObject(*owner.layerId);
                if (JS_IsUndefined(thisLayer)) {
                    throw ScriptError(
                        ScriptErrorCode::exception,
                        "SceneScript owner layer is not present in the registry"
                    );
                }
            } else {
                thisLayer = JS_NewObject(ctx);
                if (JS_IsException(thisLayer)) {
                    jsError(ctx, ScriptErrorCode::resourceLimit, "creating thisLayer");
                }
                defineAccessor(
                    ctx,
                    thisLayer,
                    "text",
                    JS_NewCFunction(ctx, getLayerText, "get text", 0),
                    JS_NewCFunction(ctx, setLayerText, "set text", 1)
                );
            }
            defineProperty(
                ctx,
                global.value,
                "thisLayer",
                JS_DupValue(ctx, thisLayer),
                JS_PROP_ENUMERABLE
            );
            // For a layer-owned property, Wallpaper Engine exposes the same
            // live object through both thisLayer and thisObject.  Keeping one
            // registry-backed identity is important: writes through either
            // name must reach the same frame overlay.  Non-layer dynamic
            // values intentionally expose undefined until a typed object
            // adapter is available; a fabricated layer would corrupt writes.
            if (owner.type == ScriptPropertyOwnerType::layer) {
                if (!layerRegistry || !owner.layerId) {
                    throw ScriptError(
                        ScriptErrorCode::exception,
                        "SceneScript layer owner is missing its layer registry identity"
                    );
                }
                thisObject = JS_DupValue(ctx, thisLayer);
            } else if (
                owner.type == ScriptPropertyOwnerType::effect ||
                owner.type == ScriptPropertyOwnerType::material
            ) {
                thisObject = makePropertyObject(owner.objectId);
                if (JS_IsUndefined(thisObject)) {
                    throw ScriptError(
                        ScriptErrorCode::exception,
                        "SceneScript property owner is not present in the registry"
                    );
                }
                const auto descriptor = propertyObjectRegistry->find(
                    owner.objectId
                );
                const ScriptPropertyObjectType expected =
                    owner.type == ScriptPropertyOwnerType::effect
                    ? ScriptPropertyObjectType::effect
                    : ScriptPropertyObjectType::material;
                if (!descriptor || descriptor->type != expected) {
                    throw ScriptError(
                        ScriptErrorCode::exception,
                        "SceneScript property owner type does not match its registry descriptor"
                    );
                }
            }
            defineProperty(
                ctx,
                global.value,
                "thisObject",
                JS_DupValue(ctx, thisObject),
                JS_PROP_ENUMERABLE
            );

            thisScene = JS_NewObject(ctx);
            if (JS_IsException(thisScene)) {
                jsError(ctx, ScriptErrorCode::resourceLimit, "creating thisScene");
            }
            const auto defineSceneInput = [this](
                const char* name,
                JSCFunction* getter
            ) {
                defineAccessor(
                    ctx,
                    thisScene,
                    name,
                    JS_NewCFunction(ctx, getter, name, 0),
                    JS_UNDEFINED
                );
            };
            const auto defineSceneAccessor = [this](
                const char* name,
                JSCFunction* getter
            ) {
                defineAccessor(
                    ctx,
                    thisScene,
                    name,
                    JS_NewCFunction(ctx, getter, name, 0),
                    JS_NewCFunction(ctx, rejectReadOnlyWrite, name, 1)
                );
            };
            // Linux's text-layer adapter exposes time/currentTime and dt.
            // runtime/frametime are included as the direct names used by the
            // existing SceneScript frame-input contract.
            defineSceneInput("runtime", getSceneRuntime);
            defineSceneInput("time", getSceneRuntime);
            defineSceneInput("currentTime", getSceneRuntime);
            defineSceneInput("frametime", getSceneFrameTime);
            defineSceneInput("dt", getSceneFrameTime);
            defineSceneInput("fps", getSceneFPS);
            defineSceneAccessor("bloom", getSceneBloom);
            defineSceneAccessor("bloomstrength", getSceneBloomStrength);
            defineSceneAccessor("bloomthreshold", getSceneBloomThreshold);
            defineSceneAccessor("clearenabled", getSceneClearEnabled);
            defineSceneAccessor("clearcolor", getSceneClearColor);
            defineSceneAccessor("ambientcolor", getSceneAmbientColor);
            defineSceneAccessor("skylightcolor", getSceneSkylightColor);
            defineSceneAccessor("fov", getSceneFOV);
            defineSceneAccessor("nearz", getSceneNearZ);
            defineSceneAccessor("farz", getSceneFarZ);
            defineSceneAccessor("camerafade", getSceneCameraFade);
            defineSceneAccessor("camerashake", getSceneCameraShake);
            defineSceneAccessor("camerashakespeed", getSceneCameraShakeSpeed);
            defineSceneAccessor(
                "camerashakeamplitude", getSceneCameraShakeAmplitude
            );
            defineSceneAccessor(
                "camerashakeroughness", getSceneCameraShakeRoughness
            );
            defineSceneAccessor("cameraparallax", getSceneCameraParallax);
            defineSceneAccessor(
                "cameraparallaxamount", getSceneCameraParallaxAmount
            );
            defineSceneAccessor(
                "cameraparallaxdelay", getSceneCameraParallaxDelay
            );
            defineSceneAccessor(
                "cameraparallaxmouseinfluence",
                getSceneCameraParallaxMouseInfluence
            );
            setProperty(
                ctx,
                thisScene,
                "getLayer",
                JS_NewCFunction(ctx, getSceneLayer, "getLayer", 1)
            );
            setProperty(
                ctx,
                thisScene,
                "enumerateLayers",
                JS_NewCFunction(ctx, enumerateSceneLayers, "enumerateLayers", 0)
            );
            defineProperty(
                ctx,
                global.value,
                "thisScene",
                JS_DupValue(ctx, thisScene),
                JS_PROP_ENUMERABLE
            );

            JSValue input = JS_NewObject(ctx);
            cursor = JS_NewObject(ctx);
            setProperty(ctx, input, "cursorScreenPosition", JS_DupValue(ctx, cursor));
            defineAccessor(
                ctx,
                input,
                "cursorWorldPosition",
                JS_NewCFunction(
                    ctx,
                    getCursorWorldPosition,
                    "get cursorWorldPosition",
                    0
                ),
                JS_NewCFunction(
                    ctx,
                    rejectReadOnlyWrite,
                    "set cursorWorldPosition",
                    1
                )
            );
            defineAccessor(
                ctx,
                input,
                "cursorLeftDown",
                JS_NewCFunction(
                    ctx,
                    getCursorLeftDown,
                    "get cursorLeftDown",
                    0
                ),
                JS_NewCFunction(
                    ctx,
                    rejectReadOnlyWrite,
                    "set cursorLeftDown",
                    1
                )
            );
            setProperty(ctx, global.value, "input", input);
            scriptProperties = JS_NewObject(ctx);
            if (JS_IsException(scriptProperties)) jsError(ctx, ScriptErrorCode::resourceLimit, "creating scriptProperties");
            for (const auto& [key, value] : scriptPropertyValues) {
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
            compiled = JS_Eval(
                ctx,
                moduleSource.data(),
                moduleSource.size(),
                "<wallpaper-script>",
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
            );
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
            destroyRealm(false);
            throw;
        }
    }

    Impl(
        std::shared_ptr<ScriptRuntime::Impl> shared,
        std::string source,
        RuntimeValue initial,
        std::map<std::string, RuntimeValue> properties,
        std::optional<std::string> valueCondition,
        std::shared_ptr<ScriptLayerRegistry> registry,
        std::shared_ptr<ScriptPropertyObjectRegistry> objectRegistry,
        ScriptPropertyOwner propertyOwner
    )
        : runtime(std::move(shared)),
          timerOwner(std::make_shared<TimerOwnerState>()),
          moduleSource(std::move(source)),
          scriptPropertyValues(std::move(properties)),
          current(std::move(initial)),
          condition(std::move(valueCondition)),
          layerRegistry(std::move(registry)),
          propertyObjectRegistry(std::move(objectRegistry)),
          owner(std::move(propertyOwner)) {
        timerOwner->cancel = [](void* owner, std::uint64_t id) {
            static_cast<Impl*>(owner)->cancelTimer(id);
        };
        std::lock_guard lock(runtime->mutex);
        try {
            initializeRealm();
        } catch (...) {
            timerOwner->owner.store(nullptr, std::memory_order_release);
            throw;
        }
    }

    void rebuildRealm() {
        destroyRealm(false);
        try {
            initializeRealm();
            realmNeedsRebuild = false;
            transientAudioFailureMessage.clear();
        } catch (...) {
            realmNeedsRebuild = true;
            throw;
        }
    }

    ~Impl() {
        std::lock_guard lock(runtime->mutex);
        destroyRealm(true);
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

    static const char* cursorEventName(ScriptCursorEventType type) {
        switch (type) {
            case ScriptCursorEventType::enter: return "cursorEnter";
            case ScriptCursorEventType::leave: return "cursorLeave";
            case ScriptCursorEventType::move: return "cursorMove";
            case ScriptCursorEventType::down: return "cursorDown";
            case ScriptCursorEventType::up: return "cursorUp";
            case ScriptCursorEventType::click: return "cursorClick";
        }
        throw ScriptError(
            ScriptErrorCode::invalidResultType,
            "SceneScript cursor event has an unknown type"
        );
    }

    static bool finiteCursorEvent(const ScriptCursorEvent& event) noexcept {
        return std::isfinite(event.worldX) && std::isfinite(event.worldY) &&
            std::isfinite(event.worldZ) && std::isfinite(event.localX) &&
            std::isfinite(event.localY) && std::isfinite(event.localZ);
    }

    static bool validCursorEvent(const ScriptCursorEvent& event) noexcept {
        if (!finiteCursorEvent(event)) return false;
        switch (event.type) {
            case ScriptCursorEventType::enter:
            case ScriptCursorEventType::leave:
            case ScriptCursorEventType::move:
            case ScriptCursorEventType::down:
            case ScriptCursorEventType::up:
            case ScriptCursorEventType::click:
                return true;
        }
        return false;
    }

    static bool finiteColor(const std::array<double, 3>& color) noexcept {
        return std::all_of(color.begin(), color.end(), [](double component) {
            return std::isfinite(component) && component >= 0.0 && component <= 1.0;
        });
    }

    static bool finiteSceneSnapshot(
        const ScriptSceneSnapshot& snapshot
    ) noexcept {
        const auto finite = [](double value) { return std::isfinite(value); };
        return std::all_of(
                   snapshot.clearColor.begin(), snapshot.clearColor.end(), finite
               ) &&
            std::all_of(
                snapshot.ambientColor.begin(), snapshot.ambientColor.end(), finite
            ) &&
            std::all_of(
                snapshot.skylightColor.begin(),
                snapshot.skylightColor.end(),
                finite
            ) &&
            finite(snapshot.fieldOfView) && finite(snapshot.nearZ) &&
            finite(snapshot.farZ) && finite(snapshot.cameraShakeSpeed) &&
            finite(snapshot.cameraShakeAmplitude) &&
            finite(snapshot.cameraShakeRoughness) &&
            finite(snapshot.cameraParallaxAmount) &&
            finite(snapshot.cameraParallaxDelay) &&
            finite(snapshot.cameraParallaxMouseInfluence);
    }

    static bool validMediaSnapshot(const ScriptMediaSnapshot& snapshot) noexcept {
        if (!snapshot.available) return true;
        if (!std::isfinite(snapshot.position) || snapshot.position < 0.0 ||
            !std::isfinite(snapshot.duration) || snapshot.duration < 0.0 ||
            !finiteColor(snapshot.primaryColor) ||
            !finiteColor(snapshot.secondaryColor) ||
            !finiteColor(snapshot.tertiaryColor) ||
            !finiteColor(snapshot.textColor) ||
            !finiteColor(snapshot.highContrastColor)) {
            return false;
        }
        switch (snapshot.playbackState) {
            case ScriptMediaPlaybackState::stopped:
            case ScriptMediaPlaybackState::playing:
            case ScriptMediaPlaybackState::paused:
                return true;
        }
        return false;
    }

    JSValue vec3(double x, double y, double z) {
        return newVector(ctx, 3, std::array<double, 4>{x, y, z, 0.0});
    }

    JSValue makeCursorEvent(const ScriptCursorEvent& event) {
        JSOwner object(ctx, JS_NewObject(ctx));
        if (JS_IsException(object.value)) {
            jsError(ctx, ScriptErrorCode::resourceLimit, "creating cursor event");
        }
        setProperty(
            ctx,
            object.value,
            "worldPosition",
            vec3(event.worldX, event.worldY, event.worldZ)
        );
        setProperty(
            ctx,
            object.value,
            "localPosition",
            vec3(event.localX, event.localY, event.localZ)
        );
        if (event.hitBox) {
            setProperty(
                ctx,
                object.value,
                "hitBox",
                JS_NewStringLen(ctx, event.hitBox->data(), event.hitBox->size())
            );
        }
        JSValue result = object.value;
        object.value = JS_UNDEFINED;
        return result;
    }

    JSValue makeMediaPropertiesEvent(const ScriptMediaSnapshot& snapshot) {
        JSOwner object(ctx, JS_NewObject(ctx));
        if (JS_IsException(object.value)) {
            jsError(ctx, ScriptErrorCode::resourceLimit, "creating media properties event");
        }
        setProperty(ctx, object.value, "title", JS_NewString(ctx, snapshot.title.c_str()));
        setProperty(ctx, object.value, "artist", JS_NewString(ctx, snapshot.artist.c_str()));
        setProperty(ctx, object.value, "contentType", JS_NewString(ctx, snapshot.contentType.c_str()));
        setProperty(ctx, object.value, "albumTitle", JS_NewString(ctx, snapshot.albumTitle.c_str()));
        setProperty(ctx, object.value, "subTitle", JS_NewString(ctx, snapshot.subTitle.c_str()));
        setProperty(ctx, object.value, "albumArtist", JS_NewString(ctx, snapshot.albumArtist.c_str()));
        setProperty(ctx, object.value, "genres", JS_NewString(ctx, snapshot.genres.c_str()));
        JSValue result = object.value;
        object.value = JS_UNDEFINED;
        return result;
    }

    JSValue makeMediaPlaybackEvent(const ScriptMediaSnapshot& snapshot) {
        JSOwner object(ctx, JS_NewObject(ctx));
        if (JS_IsException(object.value)) {
            jsError(ctx, ScriptErrorCode::resourceLimit, "creating media playback event");
        }
        setProperty(
            ctx,
            object.value,
            "state",
            JS_NewInt32(ctx, static_cast<std::int32_t>(snapshot.playbackState))
        );
        JSValue result = object.value;
        object.value = JS_UNDEFINED;
        return result;
    }

    JSValue makeMediaTimelineEvent(const ScriptMediaSnapshot& snapshot) {
        JSOwner object(ctx, JS_NewObject(ctx));
        if (JS_IsException(object.value)) {
            jsError(ctx, ScriptErrorCode::resourceLimit, "creating media timeline event");
        }
        setProperty(ctx, object.value, "position", JS_NewFloat64(ctx, snapshot.position));
        setProperty(ctx, object.value, "duration", JS_NewFloat64(ctx, snapshot.duration));
        JSValue result = object.value;
        object.value = JS_UNDEFINED;
        return result;
    }

    JSValue makeMediaThumbnailEvent(const ScriptMediaSnapshot& snapshot) {
        JSOwner object(ctx, JS_NewObject(ctx));
        if (JS_IsException(object.value)) {
            jsError(ctx, ScriptErrorCode::resourceLimit, "creating media thumbnail event");
        }
        setProperty(ctx, object.value, "hasThumbnail", JS_NewBool(ctx, snapshot.hasThumbnail));
        setProperty(ctx, object.value, "primaryColor", vec3(
            snapshot.primaryColor[0], snapshot.primaryColor[1], snapshot.primaryColor[2]
        ));
        setProperty(ctx, object.value, "secondaryColor", vec3(
            snapshot.secondaryColor[0], snapshot.secondaryColor[1], snapshot.secondaryColor[2]
        ));
        setProperty(ctx, object.value, "tertiaryColor", vec3(
            snapshot.tertiaryColor[0], snapshot.tertiaryColor[1], snapshot.tertiaryColor[2]
        ));
        setProperty(ctx, object.value, "textColor", vec3(
            snapshot.textColor[0], snapshot.textColor[1], snapshot.textColor[2]
        ));
        setProperty(ctx, object.value, "highContrastColor", vec3(
            snapshot.highContrastColor[0], snapshot.highContrastColor[1], snapshot.highContrastColor[2]
        ));
        JSValue result = object.value;
        object.value = JS_UNDEFINED;
        return result;
    }

    JSValue makeMediaStatusEvent(const ScriptMediaSnapshot& snapshot) {
        JSOwner object(ctx, JS_NewObject(ctx));
        if (JS_IsException(object.value)) {
            jsError(ctx, ScriptErrorCode::resourceLimit, "creating media status event");
        }
        setProperty(ctx, object.value, "enabled", JS_NewBool(ctx, snapshot.available));
        JSValue result = object.value;
        object.value = JS_UNDEFINED;
        return result;
    }

    void callOptionalEvent(
        JSValueConst nameSpace,
        const char* exportName,
        JSValueConst event
    ) {
        JSOwner function(ctx, JS_GetPropertyStr(ctx, nameSpace, exportName));
        if (JS_IsException(function.value)) {
            jsError(ctx, ScriptErrorCode::exception, std::string("reading ") + exportName);
        }
        if (JS_IsUndefined(function.value) || JS_IsNull(function.value)) return;
        if (!JS_IsFunction(ctx, function.value)) {
            throw ScriptError(
                ScriptErrorCode::invalidResultType,
                std::string("Module export ") + exportName + " must be a function"
            );
        }
        audioUnavailable = false;
        JSValue argument = event;
        JSOwner result(ctx, JS_Call(ctx, function.value, JS_UNDEFINED, 1, &argument));
        settleSynchronousCall(result.value, exportName);
    }

    void updateUserPropertiesSnapshot(
        const std::shared_ptr<const ScriptUserPropertiesSnapshot>& snapshot
    ) {
        if (!snapshot) {
            JS_FreeValue(ctx, userProperties);
            userProperties = JS_UNDEFINED;
            userPropertyValues.clear();
            pendingUserProperties.clear();
            userPropertiesInitialized = false;
            return;
        }

        JSOwner staged(ctx, JS_NewObject(ctx));
        if (JS_IsException(staged.value)) {
            jsError(
                ctx,
                ScriptErrorCode::resourceLimit,
                "staging project user properties"
            );
        }
        for (const auto& [key, value] : snapshot->values) {
            setProperty(ctx, staged.value, key.c_str(), toJS(ctx, value));
        }
        requireSynchronous(
            "userProperties",
            false,
            "validating project user properties"
        );

        std::map<std::string, RuntimeValue> changed;
        if (!userPropertiesInitialized) {
            changed = snapshot->values;
        } else {
            for (const auto& [key, value] : snapshot->values) {
                const auto previous = userPropertyValues.find(key);
                if (previous == userPropertyValues.end() ||
                    previous->second != value) {
                    changed.emplace(key, value);
                }
            }
            for (const auto& [key, value] : userPropertyValues) {
                (void)value;
                if (!snapshot->values.contains(key)) {
                    throw ScriptError(
                        ScriptErrorCode::invalidResultType,
                        "Project user-property snapshot removed key '" + key +
                            "' from an immutable project"
                    );
                }
            }
        }

        JS_FreeValue(ctx, userProperties);
        userProperties = staged.value;
        staged.value = JS_UNDEFINED;
        userPropertyValues = snapshot->values;
        pendingUserProperties = std::move(changed);
    }

    // Wallpaper Engine sends a sparse project-property object. The first
    // dispatch contains the complete project snapshot; later dispatches only
    // contain host values that changed. DynamicValue-local scriptProperties
    // are a separate data path and never trigger this callback.
    void dispatchUserProperties() {
        if (module == nullptr) return;
        if (userPropertiesInitialized && pendingUserProperties.empty()) return;

        JSOwner nameSpace(ctx, JS_GetModuleNamespace(ctx, module));
        if (JS_IsException(nameSpace.value)) {
            jsError(ctx, ScriptErrorCode::exception, "reading SceneScript module namespace");
        }
        JSOwner function(ctx, JS_GetPropertyStr(
            ctx, nameSpace.value, "applyUserProperties"
        ));
        if (JS_IsException(function.value)) {
            jsError(ctx, ScriptErrorCode::exception, "reading applyUserProperties");
        }
        if (JS_IsUndefined(function.value) || JS_IsNull(function.value)) {
            pendingUserProperties.clear();
            userPropertiesInitialized = true;
            return;
        }
        if (!JS_IsFunction(ctx, function.value)) {
            throw ScriptError(
                ScriptErrorCode::invalidResultType,
                "Module export applyUserProperties must be a function"
            );
        }
        if (JS_IsUndefined(userProperties)) {
            throw ScriptError(
                ScriptErrorCode::invalidResultType,
                "applyUserProperties is unavailable until the host supplies project user properties"
            );
        }

        JSOwner event(ctx, JS_NewObject(ctx));
        if (JS_IsException(event.value)) {
            jsError(ctx, ScriptErrorCode::resourceLimit, "creating applyUserProperties event");
        }
        for (const auto& [key, value] : pendingUserProperties) {
            setProperty(ctx, event.value, key.c_str(), toJS(ctx, value));
        }

        JSOwner result(ctx, JS_Call(ctx, function.value, JS_UNDEFINED, 1, &event.value));
        settleSynchronousCall(result.value, "applyUserProperties");
        pendingUserProperties.clear();
        userPropertiesInitialized = true;
    }

    void dispatchCursorEvents(const std::vector<ScriptCursorEvent>& events) {
        if (events.empty() || module == nullptr || !owner.layerId) return;
        JSOwner nameSpace(ctx, JS_GetModuleNamespace(ctx, module));
        if (JS_IsException(nameSpace.value)) {
            jsError(ctx, ScriptErrorCode::exception, "reading SceneScript module namespace");
        }
        for (const auto& event : events) {
            if (event.layerId != *owner.layerId) continue;
            if (!finiteCursorEvent(event)) {
                throw ScriptError(
                    ScriptErrorCode::invalidResultType,
                    "SceneScript cursor event positions must be finite"
                );
            }
            JSOwner value(ctx, makeCursorEvent(event));
            callOptionalEvent(nameSpace.value, cursorEventName(event.type), value.value);
        }
    }

    void dispatchMediaSnapshot(const std::optional<ScriptMediaSnapshot>& snapshot) {
        if (!snapshot || module == nullptr) return;
        const bool changed = !lastMediaRevision ||
            *lastMediaRevision != snapshot->revision ||
            (lastMediaAvailable && *lastMediaAvailable != snapshot->available);
        if (!changed) return;
        if (!validMediaSnapshot(*snapshot)) {
            throw ScriptError(
                ScriptErrorCode::invalidResultType,
                "SceneScript media snapshot contains invalid values"
            );
        }

        JSOwner nameSpace(ctx, JS_GetModuleNamespace(ctx, module));
        if (JS_IsException(nameSpace.value)) {
            jsError(ctx, ScriptErrorCode::exception, "reading SceneScript module namespace");
        }

        if (!lastMediaAvailable || *lastMediaAvailable != snapshot->available) {
            JSOwner status(ctx, makeMediaStatusEvent(*snapshot));
            callOptionalEvent(nameSpace.value, "mediaStatusChanged", status.value);
        }
        if (snapshot->available) {
            JSOwner properties(ctx, makeMediaPropertiesEvent(*snapshot));
            callOptionalEvent(nameSpace.value, "mediaPropertiesChanged", properties.value);
            JSOwner playback(ctx, makeMediaPlaybackEvent(*snapshot));
            callOptionalEvent(nameSpace.value, "mediaPlaybackChanged", playback.value);
            JSOwner timeline(ctx, makeMediaTimelineEvent(*snapshot));
            callOptionalEvent(nameSpace.value, "mediaTimelineChanged", timeline.value);
            JSOwner thumbnail(ctx, makeMediaThumbnailEvent(*snapshot));
            callOptionalEvent(nameSpace.value, "mediaThumbnailChanged", thumbnail.value);
        }
        lastMediaRevision = snapshot->revision;
        lastMediaAvailable = snapshot->available;
    }

    [[nodiscard]] bool hasBoundOwnerProperty() const noexcept {
        if (owner.property.empty()) return false;
        if (owner.type == ScriptPropertyOwnerType::layer) {
            return layerRegistry != nullptr && owner.layerId.has_value();
        }
        return (
            owner.type == ScriptPropertyOwnerType::effect ||
            owner.type == ScriptPropertyOwnerType::material
        ) && propertyObjectRegistry != nullptr && !owner.objectId.empty();
    }

    [[nodiscard]] std::optional<RuntimeValue> readOwnerProperty() const {
        if (!hasBoundOwnerProperty()) return std::nullopt;
        if (owner.type == ScriptPropertyOwnerType::layer) {
            return layerRegistry->read(*owner.layerId, owner.property);
        }
        return propertyObjectRegistry->read(owner.objectId, owner.property);
    }

    [[nodiscard]] std::optional<RuntimeValue> takePendingOwnerWrite() {
        if (!hasBoundOwnerProperty()) return std::nullopt;
        if (owner.type == ScriptPropertyOwnerType::layer) {
            return layerRegistry->takePendingWrite(
                *owner.layerId, owner.property
            );
        }
        return propertyObjectRegistry->takePendingWrite(
            owner.objectId, owner.property
        );
    }

    void commitOwnerProperty(RuntimeValue value) {
        if (!hasBoundOwnerProperty()) return;
        if (owner.type == ScriptPropertyOwnerType::layer) {
            layerRegistry->commit(
                *owner.layerId, owner.property, std::move(value)
            );
            return;
        }
        propertyObjectRegistry->commit(
            owner.objectId, owner.property, std::move(value)
        );
    }

    void refreshCurrentFromEventMutations() {
        if (const auto value = readOwnerProperty()) {
            current = *value;
        } else if (layerTextDirty) {
            current = currentLayerText();
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
        // Missing host audio is a recoverable integration state.  A frame may
        // legitimately arrive before the capture lease is established; keep
        // the QuickJS instance and its module state alive so the next frame
        // can retry with a real spectrum.  All other failures retain the
        // historical poison-once diagnostic semantics.
        if (finalFailure->code == ScriptErrorCode::audioInputUnavailable) {
            if (!started) {
                // Module evaluation and init both run before the instance is
                // marked started.  A failed audio registration in either
                // phase may have consumed the compiled module and left
                // timers/audio registrations in a partially initialized
                // realm.  Keep the diagnostic transient, but rebuild that
                // realm before the next frame that supplies real spectrum.
                realmNeedsRebuild = true;
                transientAudioFailureMessage = finalFailure->message;
            }
            audioUnavailable = false;
            throw ScriptError(finalFailure->code, finalFailure->message);
        }
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
        lastInvocationReturnedUndefined = JS_IsUndefined(result.value);

        RuntimeValue converted;
        if (lastInvocationReturnedUndefined) {
            // JavaScript `undefined` means the lifecycle callback did not
            // replace the DynamicValue. This is distinct from an explicit
            // `null`, which remains a real value and is committed below.
            converted = !layerRegistry && (layerTextDirty || layerContractUsed)
                ? currentLayerText()
                : argument;
        } else if (!layerRegistry && JS_IsString(result.value)) {
            // linux-wallpaperengine lets a returned string win over an
            // assignment to thisLayer.text in the same update.
            if (layerContractUsed) {
                JSValue returnedText = JS_DupValue(ctx, result.value);
                JS_FreeValue(ctx, layerText);
                layerText = returnedText;
                layerTextDirty = false;
            }
            converted = applyCondition(fromJS(ctx, result.value, &argument));
        } else if (!layerRegistry && (layerTextDirty || layerContractUsed)) {
            converted = currentLayerText();
        } else {
            converted = applyCondition(fromJS(ctx, result.value, &argument));
        }
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
        lastInvocationReturnedUndefined = JS_IsUndefined(result.value);

        RuntimeValue mutated = applyCondition(fromJS(ctx, jsArgument.value));
        requireSynchronous("init", false, "converting init argument");
        return layerTextDirty ? currentLayerText() : mutated;
    }

    void seedLayerText(const RuntimeValue& value) {
        JSValue next = JS_UNDEFINED;
        if (value.type() == RuntimeValueType::string) {
            next = toJS(ctx, value);
        } else if (layerContractUsed && JS_IsString(layerText)) {
            // A conditioned string has a non-string runtime tag but still
            // represents the same live layer text. Preserve that backing value
            // instead of manufacturing a replacement from numeric projections.
            next = JS_DupValue(ctx, layerText);
        }
        JS_FreeValue(ctx, layerText);
        layerText = next;
        layerTextDirty = false;
    }

    RuntimeValue currentLayerText() const {
        const char* text = JS_ToCString(ctx, layerText);
        if (text == nullptr) {
            jsError(
                ctx,
                ScriptErrorCode::exception,
                "converting thisLayer.text"
            );
        }
        std::string value(text);
        JS_FreeCString(ctx, text);
        return applyCondition(RuntimeValue::string(std::move(value)));
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
            (inputs.timeOfDay &&
                (!std::isfinite(*inputs.timeOfDay) || *inputs.timeOfDay < 0.0 || *inputs.timeOfDay > 1.0)) ||
            (inputs.audioSpectrum &&
                !audioSpectrumIsFinite(*inputs.audioSpectrum)) ||
            (inputs.sceneSnapshot &&
                !finiteSceneSnapshot(*inputs.sceneSnapshot)) ||
            !std::isfinite(inputs.pointerX) || !std::isfinite(inputs.pointerY) ||
            (inputs.cursorWorldPosition &&
                !std::all_of(
                    inputs.cursorWorldPosition->begin(),
                    inputs.cursorWorldPosition->end(),
                    [](double component) { return std::isfinite(component); }
                )) ||
            std::any_of(
                inputs.cursorEvents.begin(),
                inputs.cursorEvents.end(),
                [](const ScriptCursorEvent& event) {
                    return !validCursorEvent(event);
                }
            ) ||
            (inputs.mediaSnapshot && !validMediaSnapshot(*inputs.mediaSnapshot))) {
            throw ScriptError(
                ScriptErrorCode::invalidResultType,
                "Script frame inputs must be finite, non-negative, and event/media values must be valid"
            );
        }
        if (realmNeedsRebuild && !inputs.audioSpectrum) {
            const std::string& message = transientAudioFailureMessage.empty()
                ? std::string("audioInputUnavailable: system audio spectrum input is unavailable")
                : transientAudioFailureMessage;
            // Do not enter the old realm merely to rediscover the same
            // transient failure. In particular, this avoids dispatching
            // timers or pending jobs left behind by the failed module/init.
            throw ScriptError(ScriptErrorCode::audioInputUnavailable, message);
        }
        BudgetScope budget(*runtime);
        try {
            audioUnavailable = false;
            frameInputs = inputs;
            if (layerRegistry) {
                layerRegistry->setRuntimeSeconds(inputs.runtimeSeconds);
            }
            if (realmNeedsRebuild) {
                // Rebuild before updating audio buffers or processing timers:
                // both structures belong to the failed realm and must not be
                // observable on the recovery frame.
                rebuildRealm();
            }
            updateUserPropertiesSnapshot(inputs.userProperties);
            if (const auto value = readOwnerProperty()) {
                current = *value;
            }
            seedLayerText(current);
            JSOwner global(ctx, JS_GetGlobalObject(ctx));
            JSOwner engine(ctx, JS_GetPropertyStr(ctx, global.value, "engine"));
            if (JS_IsException(engine.value)) {
                jsError(ctx, ScriptErrorCode::exception, "reading engine frame input");
            }
            JSOwner input(ctx, JS_GetPropertyStr(ctx, global.value, "input"));
            if (JS_IsException(input.value)) {
                jsError(ctx, ScriptErrorCode::exception, "reading pointer frame input");
            }
            setProperty(ctx, cursor, "x", JS_NewFloat64(ctx, inputs.pointerX));
            setProperty(ctx, cursor, "y", JS_NewFloat64(ctx, inputs.pointerY));
            // Existing registrations are updated in place before any timer,
            // module, init, or update callback can observe the frame. A
            // registration first created during this frame is populated by
            // registerAudioBuffers() from the same host snapshot.
            updateAudioBuffers();
            requireSynchronous(
                "update",
                false,
                "updating audio frame inputs"
            );
            processTimers();
            requireSynchronous("update", false, "updating frame inputs");
            if (!started) {
                evaluateModule();
                JSOwner nameSpace(ctx, JS_GetModuleNamespace(ctx, module));
                init = JS_GetPropertyStr(ctx, nameSpace.value, "init");
                update = JS_GetPropertyStr(ctx, nameSpace.value, "update");
                // The initial user-property event is delivered after module
                // evaluation (so declarations exist) and before init/update,
                // matching Wallpaper Engine's lifecycle contract.
                dispatchUserProperties();
                if (!JS_IsUndefined(init)) {
                    if (!JS_IsFunction(ctx, init)) throw ScriptError(ScriptErrorCode::invalidResultType, "Module export init must be a function");
                    current = invokeInit(init, current);
                    if (hasBoundOwnerProperty()) {
                        const auto mutation = takePendingOwnerWrite();
                        if (lastInvocationReturnedUndefined && mutation) {
                            current = applyCondition(*mutation);
                        } else {
                            commitOwnerProperty(current);
                        }
                    }
                } else if (layerTextDirty) {
                    current = currentLayerText();
                }
                hasUpdate = !JS_IsUndefined(update) && JS_IsFunction(ctx, update);
                started = true;
            } else {
                // Property changes are staged by updateProperties() before
                // this frame enters QuickJS. Deliver them before cursor/media
                // callbacks and before update() observes the frame.
                dispatchUserProperties();
            }
            dispatchCursorEvents(inputs.cursorEvents);
            dispatchMediaSnapshot(inputs.mediaSnapshot);
            refreshCurrentFromEventMutations();
            if (!hasUpdate) {
                if (hasBoundOwnerProperty()) {
                    if (const auto mutation = takePendingOwnerWrite()) {
                        current = applyCondition(*mutation);
                    }
                }
                if (layerContractUsed) {
                    current = currentLayerText();
                    return current;
                }
                if (hasBoundOwnerProperty()) {
                    return current;
                }
                current = current.updatedToNull();
                return current;
            }
            if (!layerTextDirty) seedLayerText(current);
            current = invoke(update, current, "update");
            if (hasBoundOwnerProperty()) {
                const auto mutation = takePendingOwnerWrite();
                if (lastInvocationReturnedUndefined && mutation) {
                    current = applyCondition(*mutation);
                } else {
                    commitOwnerProperty(current);
                }
            }
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
        // A connected host value replaces the DynamicValue before the next
        // script update. Keep the typed owner view on that same canonical
        // value so evaluate() cannot restore the previous script result from
        // the registry and hide the host change.
        commitOwnerProperty(current);
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
            // Keep the host-side source of truth in sync.  A realm waiting
            // for audio recovery may be discarded before these values can be
            // copied into its JS object, so the next realm must be initialized
            // from the latest successful property map.
            scriptPropertyValues = std::move(properties);
        } catch (const ScriptError& error) {
            poisonAndThrow(error, "cleaning failed property jobs");
        }
    }
    std::shared_ptr<ScriptRuntime::Impl> runtime;
    std::shared_ptr<TimerOwnerState> timerOwner;
    std::string moduleSource;
    std::map<std::string, RuntimeValue> scriptPropertyValues;
    JSContext* ctx = nullptr;
    JSValue compiled = JS_UNDEFINED;
    JSModuleDef* module = nullptr;
    JSValue init = JS_UNDEFINED;
    JSValue update = JS_UNDEFINED;
    JSValue thisLayer = JS_UNDEFINED;
    JSValue thisObject = JS_UNDEFINED;
    JSValue thisScene = JS_UNDEFINED;
    JSValue layerText = JS_UNDEFINED;
    JSValue cursor = JS_UNDEFINED;
    JSValue scriptProperties = JS_UNDEFINED;
    JSValue userProperties = JS_UNDEFINED;
    std::map<std::string, RuntimeValue> userPropertyValues;
    std::map<std::string, RuntimeValue> pendingUserProperties;
    bool userPropertiesInitialized = false;
    std::shared_ptr<ScriptLayerRegistry> layerRegistry;
    std::shared_ptr<ScriptPropertyObjectRegistry> propertyObjectRegistry;
    ScriptPropertyOwner owner;
    std::map<int, JSValue> layerObjects;
    std::map<int, JSValue> textureAnimationObjects;
    std::map<std::string, JSValue> propertyObjects;
    std::map<std::size_t, AudioBuffers> audioBuffers;
    std::map<std::uint64_t, Timer> timers;
    std::uint64_t nextTimerId = 0;
    RuntimeValue current;
    ScriptFrameInputs frameInputs;
    std::optional<std::string> condition;
    bool started = false;
    bool hasUpdate = false;
    bool layerContractUsed = false;
    bool layerTextDirty = false;
    bool layerMutationUsed = false;
    bool lastInvocationReturnedUndefined = false;
    bool audioUnavailable = false;
    std::optional<std::uint64_t> lastMediaRevision;
    std::optional<bool> lastMediaAvailable;
    bool realmNeedsRebuild = false;
    std::string transientAudioFailureMessage;
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
    std::optional<std::string> condition,
    std::shared_ptr<ScriptLayerRegistry> layerRegistry,
    std::shared_ptr<ScriptPropertyObjectRegistry> propertyObjectRegistry,
    ScriptPropertyOwner owner
) {
    return std::unique_ptr<ScriptInstance>(new ScriptInstance(std::make_unique<ScriptInstance::Impl>(
        impl_, std::move(source), std::move(initial), std::move(properties),
        std::move(condition), std::move(layerRegistry),
        std::move(propertyObjectRegistry), std::move(owner))));
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
