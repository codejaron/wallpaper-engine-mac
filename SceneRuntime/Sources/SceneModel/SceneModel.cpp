#include <SceneModel/SceneModel.hpp>

#include <SceneCore/Runtime.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <utility>

namespace we::scene {
namespace {

std::string formatError(
    std::string_view assetPath,
    std::string_view jsonPointer,
    const std::vector<std::string>& referenceChain,
    std::string_view message
) {
    std::ostringstream result;
    result << message;
    if (!assetPath.empty()) {
        result << " [asset: " << assetPath << ']';
    }
    if (!jsonPointer.empty()) {
        result << " [pointer: " << jsonPointer << ']';
    }
    if (!referenceChain.empty()) {
        result << " [reference chain: ";
        for (std::size_t index = 0; index < referenceChain.size(); ++index) {
            if (index != 0) {
                result << " -> ";
            }
            result << referenceChain[index];
        }
        result << ']';
    }
    return result.str();
}

std::string escapePointerToken(std::string_view token) {
    std::string escaped;
    escaped.reserve(token.size());
    for (const char character : token) {
        if (character == '~') {
            escaped += "~0";
        } else if (character == '/') {
            escaped += "~1";
        } else {
            escaped += character;
        }
    }
    return escaped;
}

[[noreturn]] void propertyError(
    const SceneProject& project,
    std::string_view property,
    std::string message
) {
    throw SceneModelError(
        SceneModelErrorCode::invalidValue,
        project.assetPath,
        "/general/properties/" + escapePointerToken(property) + "/value",
        {project.assetPath},
        std::move(message)
    );
}

bool isString(const Value& value) {
    return std::holds_alternative<std::string>(value.storage);
}

bool isValidColor(std::string_view source) {
    try {
        (void)RuntimeValue::colorString(std::string(source));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

double normalizedSliderValue(
    const Value& value,
    const SceneProject& project,
    std::string_view property
) {
    double result = 0.0;
    if (const auto* number = std::get_if<double>(&value.storage)) {
        result = *number;
    } else if (const auto* integer = std::get_if<std::int64_t>(&value.storage)) {
        result = static_cast<double>(*integer);
    } else {
        propertyError(project, property, "Slider property requires a number");
    }
    if (!std::isfinite(result)) {
        propertyError(project, property, "Slider property requires a finite number");
    }
    return result;
}

Value validatePropertyValue(
    const SceneProject& project,
    const ProjectProperty& property,
    Value value
) {
    switch (property.type) {
        case PropertyType::boolean:
            if (!std::holds_alternative<bool>(value.storage)) {
                propertyError(project, property.name, "Boolean property requires a boolean");
            }
            return value;

        case PropertyType::slider: {
            const double number = normalizedSliderValue(
                value,
                project,
                property.name
            );
            if (property.minimum && number < *property.minimum) {
                propertyError(project, property.name, "Slider value is below its minimum");
            }
            if (property.maximum && number > *property.maximum) {
                propertyError(project, property.name, "Slider value is above its maximum");
            }
            if (property.step && *property.step > 0.0) {
                const double base = property.minimum.value_or(0.0);
                const double steps = (number - base) / *property.step;
                if (std::abs(steps - std::round(steps)) > 1e-7) {
                    propertyError(
                        project,
                        property.name,
                        "Slider value does not align with its declared step"
                    );
                }
            }
            if (property.fraction && !*property.fraction &&
                std::abs(number - std::round(number)) > 1e-7) {
                propertyError(
                    project,
                    property.name,
                    "Slider property requires an integer value"
                );
            }
            value.storage = number;
            return value;
        }

        case PropertyType::combo: {
            const auto* selected = std::get_if<std::string>(&value.storage);
            if (selected == nullptr) {
                propertyError(project, property.name, "Combo property requires a string option value");
            }
            const bool found = std::ranges::any_of(
                property.options,
                [&](const PropertyOption& option) {
                    return option.value == *selected;
                }
            );
            if (!found) {
                propertyError(project, property.name, "Combo property value is not a declared option");
            }
            return value;
        }

        case PropertyType::color:
            if (!isString(value) ||
                !isValidColor(std::get<std::string>(value.storage))) {
                propertyError(
                    project,
                    property.name,
                    "Color property requires a Wallpaper Engine color value"
                );
            }
            return value;

        case PropertyType::sceneTexture:
        case PropertyType::file:
        case PropertyType::directory:
        case PropertyType::textInput:
        case PropertyType::userShortcut:
            if (!isString(value)) {
                propertyError(project, property.name, "Property requires a string value");
            }
            return value;

        case PropertyType::text:
        case PropertyType::group:
            propertyError(project, property.name, "Property is display-only and cannot be changed");
    }

    propertyError(project, property.name, "Unsupported property type");
}

}  // namespace

SceneModelError::SceneModelError(
    SceneModelErrorCode code,
    std::string assetPath,
    std::string jsonPointer,
    std::vector<std::string> referenceChain,
    std::string message
)
    : std::runtime_error(formatError(
          assetPath,
          jsonPointer,
          referenceChain,
          message
      )),
      code_(code),
      assetPath_(std::move(assetPath)),
      jsonPointer_(std::move(jsonPointer)),
      referenceChain_(std::move(referenceChain)) {}

SceneModelErrorCode SceneModelError::code() const noexcept {
    return code_;
}

const std::string& SceneModelError::assetPath() const noexcept {
    return assetPath_;
}

const std::string& SceneModelError::jsonPointer() const noexcept {
    return jsonPointer_;
}

const std::vector<std::string>& SceneModelError::referenceChain() const noexcept {
    return referenceChain_;
}

bool Value::isNull() const noexcept {
    return std::holds_alternative<std::nullptr_t>(storage);
}

namespace {

std::int64_t integerProjection(double value) noexcept {
    if (value >= static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (value <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

std::optional<double> numericComponent(const Value& value) noexcept {
    if (const auto* integer = std::get_if<std::int64_t>(&value.storage)) {
        return static_cast<double>(*integer);
    }
    if (const auto* number = std::get_if<double>(&value.storage)) {
        return *number;
    }
    return std::nullopt;
}

}  // namespace

RuntimeValue RuntimeValue::null() {
    return {};
}

RuntimeValue RuntimeValue::floating(double value) {
    const float narrowed = static_cast<float>(value);
    // Upstream stores float32. Rejecting non-finite values avoids its undefined
    // float-to-int projection while keeping every accepted value equivalent.
    if (!std::isfinite(narrowed)) {
        throw std::invalid_argument("Runtime float must be finite");
    }
    value = static_cast<double>(narrowed);
    RuntimeValue result;
    result.type_ = RuntimeValueType::floating;
    result.vector_.fill(value);
    result.number_ = value;
    result.integer_ = integerProjection(value);
    // Linux DynamicValue converts float to int before deriving its bool.
    result.boolean_ = result.integer_ != 0;
    return result;
}

RuntimeValue RuntimeValue::integer(std::int64_t value) {
    const double projected = static_cast<double>(static_cast<float>(value));
    RuntimeValue result;
    result.type_ = RuntimeValueType::integer;
    result.vector_.fill(projected);
    result.number_ = projected;
    result.integer_ = value;
    result.boolean_ = value != 0;
    return result;
}

RuntimeValue RuntimeValue::boolean(bool value) {
    RuntimeValue result;
    result.type_ = RuntimeValueType::boolean;
    result.vector_.fill(value ? 1.0 : 0.0);
    result.number_ = value ? 1.0 : 0.0;
    result.integer_ = value ? 1 : 0;
    result.boolean_ = value;
    return result;
}

RuntimeValue RuntimeValue::condition(
    std::string source,
    std::string_view expected
) {
    RuntimeValue result = boolean(source == expected);
    result.string_ = std::move(source);
    return result;
}

RuntimeValue RuntimeValue::string(std::string value) {
    RuntimeValue result;
    result.type_ = RuntimeValueType::string;
    result.string_ = std::move(value);
    return result;
}

RuntimeValue RuntimeValue::initialString(std::string value) {
    std::istringstream input(value);
    std::array<std::string, 5> tokens;
    std::size_t count = 0;
    while (count < tokens.size() && input >> tokens[count]) {
        ++count;
    }
    input >> std::ws;
    if (!input.eof() || count == 0 || count > 4) {
        return string(std::move(value));
    }

    const auto parseComponent = [](const std::string& token)
        -> std::optional<double> {
        char* end = nullptr;
        errno = 0;
        const float parsed = std::strtof(token.c_str(), &end);
        if (end == token.c_str() ||
            end != token.c_str() + token.size()) {
            return std::nullopt;
        }
        if (errno == ERANGE || !std::isfinite(parsed)) {
            throw std::invalid_argument(
                "Dynamic number is outside the finite float32 range"
            );
        }
        return static_cast<double>(parsed);
    };

    if (count == 1) {
        const std::optional<double> number = parseComponent(tokens[0]);
        return number ? floating(*number) : string(std::move(value));
    }

    std::array<double, 4> components{};
    for (std::size_t index = 0; index < count; ++index) {
        const std::optional<double> component = parseComponent(tokens[index]);
        if (!component) {
            return string(std::move(value));
        }
        components[index] = *component;
    }
    return vector(components, count);
}

RuntimeValue RuntimeValue::colorString(std::string value) {
    const auto hexDigit = [](char digit) noexcept {
        if (digit >= '0' && digit <= '9') return digit - '0';
        if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
        if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
        return -1;
    };

    if (!value.empty() && value.front() == '#') {
        // Preserve CSS channel order here. The pinned Linux ColorBuilder has a
        // six-digit bit-shift bug that is not part of the intended contract.
        std::string digits = value.substr(1);
        if (digits.size() == 3 || digits.size() == 4) {
            std::string expanded;
            expanded.reserve(digits.size() * 2);
            for (const char digit : digits) {
                expanded.push_back(digit);
                expanded.push_back(digit);
            }
            digits = std::move(expanded);
        }
        if (digits.size() != 6 && digits.size() != 8) {
            throw std::invalid_argument("Invalid CSS color length");
        }
        std::array<double, 4> components{0.0, 0.0, 0.0, 1.0};
        for (std::size_t index = 0; index < digits.size() / 2; ++index) {
            const int high = hexDigit(digits[index * 2]);
            const int low = hexDigit(digits[index * 2 + 1]);
            if (high < 0 || low < 0) {
                throw std::invalid_argument("Invalid CSS color digit");
            }
            components[index] = static_cast<double>((high << 4) | low) / 255.0;
        }
        return color(components);
    }

    std::ranges::replace(value, ',', ' ');
    std::istringstream input(value);
    std::array<std::string, 4> tokens;
    std::size_t count = 0;
    while (count < tokens.size() && input >> tokens[count]) ++count;
    input >> std::ws;
    if (!input.eof() || (count != 3 && count != 4)) {
        throw std::invalid_argument("Color must contain three or four components");
    }

    std::array<double, 4> components{0.0, 0.0, 0.0, 1.0};
    for (std::size_t index = 0; index < count; ++index) {
        const float component = std::stof(tokens[index]);
        if (!std::isfinite(component)) {
            throw std::invalid_argument("Invalid floating color component");
        }
        components[index] = static_cast<double>(component);
    }
    const bool byteColor = std::any_of(
        components.begin(), components.begin() + count,
        [](double component) { return component > 1.0; }
    );
    if (byteColor) {
        for (std::size_t index = 0; index < count; ++index) {
            components[index] /= 255.0;
        }
    }
    return color(components);
}

RuntimeValue RuntimeValue::vector(
    const std::array<double, 4>& components,
    std::size_t componentCount
) {
    if (componentCount < 2 || componentCount > 4) {
        throw std::invalid_argument("Runtime vector must have two to four components");
    }
    std::array<double, 4> narrowedComponents{};
    for (std::size_t index = 0; index < componentCount; ++index) {
        const float narrowed = static_cast<float>(components[index]);
        if (!std::isfinite(narrowed)) {
            throw std::invalid_argument("Runtime vector components must be finite");
        }
        narrowedComponents[index] = static_cast<double>(narrowed);
    }
    RuntimeValue result;
    result.type_ = componentCount == 2
        ? RuntimeValueType::vector2
        : componentCount == 3
            ? RuntimeValueType::vector3
            : RuntimeValueType::vector4;
    result.vector_ = narrowedComponents;
    for (std::size_t index = componentCount; index < result.vector_.size(); ++index) {
        result.vector_[index] = 0.0;
    }
    result.number_ = result.vector_[0];
    result.integer_ = integerProjection(result.number_);
    result.boolean_ = result.number_ != 0.0;
    return result;
}

RuntimeValue RuntimeValue::color(const std::array<double, 4>& components) {
    RuntimeValue result = vector(components, 4);
    result.integer_ = integerProjection(result.vector_[0] * 255.0);
    result.boolean_ = result.vector_[3] != 0.0;
    return result;
}

RuntimeValue RuntimeValue::fromValue(const Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value.storage)) {
        return null();
    }
    if (const auto* booleanValue = std::get_if<bool>(&value.storage)) {
        return boolean(*booleanValue);
    }
    if (const auto* integerValue = std::get_if<std::int64_t>(&value.storage)) {
        return integer(*integerValue);
    }
    if (const auto* numberValue = std::get_if<double>(&value.storage)) {
        return floating(*numberValue);
    }
    if (const auto* stringValue = std::get_if<std::string>(&value.storage)) {
        return string(*stringValue);
    }

    std::array<double, 4> components{};
    const auto* object = std::get_if<Value::Object>(&value.storage);
    if (object == nullptr) {
        throw std::invalid_argument(
            "RuntimeValue source must be a scalar or x/y/z/w object"
        );
    }
    static constexpr std::array<std::string_view, 4> names = {"x", "y", "z", "w"};
    std::size_t count = 0;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto found = object->find(std::string(names[index]));
        if (found == object->end()) {
            if (index < 2) {
                throw std::invalid_argument(
                    "Runtime vector source requires x and y components"
                );
            }
            break;
        }
        const std::optional<double> component = numericComponent(found->second);
        if (!component) {
            throw std::invalid_argument(
                "Runtime vector source components must be numeric"
            );
        }
        components[index] = *component;
        count = index + 1;
    }
    return vector(components, count);
}

RuntimeValue RuntimeValue::updatedToNull() const {
    RuntimeValue result;
    // Linux resets the numeric/vector projections for a Null update but leaves
    // the last string projection intact.
    result.string_ = string_;
    return result;
}

RuntimeValueType RuntimeValue::type() const noexcept { return type_; }
bool RuntimeValue::isVector() const noexcept {
    return type_ == RuntimeValueType::vector2 ||
           type_ == RuntimeValueType::vector3 ||
           type_ == RuntimeValueType::vector4;
}
std::size_t RuntimeValue::componentCount() const noexcept {
    switch (type_) {
        case RuntimeValueType::vector2: return 2;
        case RuntimeValueType::vector3: return 3;
        case RuntimeValueType::vector4: return 4;
        case RuntimeValueType::floating:
        case RuntimeValueType::integer:
        case RuntimeValueType::boolean:
            return 1;
        case RuntimeValueType::null:
        case RuntimeValueType::string:
            return 0;
    }
    return 0;
}
const std::array<double, 4>& RuntimeValue::vector() const noexcept { return vector_; }
double RuntimeValue::number() const noexcept { return number_; }
std::int64_t RuntimeValue::integer() const noexcept { return integer_; }
bool RuntimeValue::boolean() const noexcept { return boolean_; }
const std::string& RuntimeValue::string() const noexcept { return string_; }

std::string RuntimeValue::toString() const {
    switch (type_) {
        case RuntimeValueType::floating:
            return std::to_string(number_);
        case RuntimeValueType::integer:
            return std::to_string(integer_);
        case RuntimeValueType::boolean:
            return std::to_string(boolean_);
        case RuntimeValueType::vector2:
            return std::to_string(vector_[0]) + ", " +
                std::to_string(vector_[1]);
        case RuntimeValueType::vector3:
            return std::to_string(vector_[0]) + ", " +
                std::to_string(vector_[1]) + ", " +
                std::to_string(vector_[2]);
        case RuntimeValueType::vector4:
            return std::to_string(vector_[0]) + ", " +
                std::to_string(vector_[1]) + ", " +
                std::to_string(vector_[2]) + ", " +
                std::to_string(vector_[3]);
        case RuntimeValueType::string:
            return string_;
        case RuntimeValueType::null:
            return "Unknown conversion for dynamic value of type: " +
                std::to_string(static_cast<int>(type_));
    }
    return {};
}

struct SceneModel::State {
    std::shared_ptr<const Runtime> runtime;
    SceneProject project;
    std::vector<std::string> propertyKeys;
    std::map<std::string, Value> propertyValues;
    mutable std::shared_mutex propertyMutex;
    std::atomic<std::uint64_t> revision = 0;
};

std::shared_ptr<SceneModel> SceneModel::load(
    std::shared_ptr<const Runtime> runtime,
    std::string_view projectPath
) {
    if (!runtime) {
        throw SceneModelError(
            SceneModelErrorCode::invalidValue,
            {},
            {},
            {},
            "Scene runtime is required"
        );
    }

    auto state = std::make_unique<State>();
    state->runtime = std::move(runtime);
    state->project = SceneModelLoader::load(
        state->runtime->assetResolver(),
        projectPath
    );
    state->propertyKeys.reserve(state->project.properties.size());
    for (const auto& [key, property] : state->project.properties) {
        state->propertyKeys.push_back(key);
        if (property.value) {
            state->propertyValues.emplace(key, *property.value);
        }
    }

    constexpr int missingSortValue = std::numeric_limits<int>::max();
    std::ranges::sort(
        state->propertyKeys,
        [&](const std::string& lhs, const std::string& rhs) {
            const auto& left = state->project.properties.at(lhs);
            const auto& right = state->project.properties.at(rhs);
            return std::tuple(
                       left.order.value_or(missingSortValue),
                       left.index.value_or(missingSortValue),
                       lhs
                   ) <
                   std::tuple(
                       right.order.value_or(missingSortValue),
                       right.index.value_or(missingSortValue),
                       rhs
                   );
        }
    );

    return std::shared_ptr<SceneModel>(new SceneModel(std::move(state)));
}

SceneModel::SceneModel(std::unique_ptr<State> state)
    : state_(std::move(state)) {}

SceneModel::~SceneModel() = default;

const SceneProject& SceneModel::project() const noexcept {
    return state_->project;
}

std::shared_ptr<const Runtime> SceneModel::runtime() const noexcept {
    return state_->runtime;
}

const std::vector<std::string>& SceneModel::propertyKeys() const noexcept {
    return state_->propertyKeys;
}

std::optional<Value> SceneModel::propertyValue(std::string_view property) const {
    const std::shared_lock lock(state_->propertyMutex);
    const auto found = state_->propertyValues.find(std::string(property));
    if (found == state_->propertyValues.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::map<std::string, Value> SceneModel::propertyValues() const {
    return propertyState().values;
}

PropertyStateSnapshot SceneModel::propertyState() const {
    const std::shared_lock lock(state_->propertyMutex);
    return {
        .revision = state_->revision.load(std::memory_order_relaxed),
        .values = state_->propertyValues,
    };
}

void SceneModel::setPropertyValue(std::string_view propertyName, Value value) {
    setPropertyValues({{std::string(propertyName), std::move(value)}});
}

void SceneModel::setPropertyValues(
    std::vector<std::pair<std::string, Value>> values
) {
    std::map<std::string, Value> validatedValues;
    for (auto& [propertyName, value] : values) {
        if (validatedValues.contains(propertyName)) {
            propertyError(
                state_->project,
                propertyName,
                "Property update batch contains a duplicate key"
            );
        }
        const auto found = state_->project.properties.find(propertyName);
        if (found == state_->project.properties.end()) {
            propertyError(
                state_->project,
                propertyName,
                "User property does not exist"
            );
        }

        Value validated = validatePropertyValue(
            state_->project,
            found->second,
            std::move(value)
        );
        validatedValues.emplace(std::move(propertyName), std::move(validated));
    }

    const std::unique_lock lock(state_->propertyMutex);
    bool changed = false;
    for (const auto& [propertyName, value] : validatedValues) {
        const auto current = state_->propertyValues.find(propertyName);
        if (current == state_->propertyValues.end() || current->second != value) {
            changed = true;
            break;
        }
    }
    if (!changed) return;
    for (auto& [propertyName, value] : validatedValues) {
        state_->propertyValues[propertyName] = std::move(value);
    }
    state_->revision.fetch_add(1, std::memory_order_release);
}

std::uint64_t SceneModel::revision() const noexcept {
    return state_->revision.load(std::memory_order_acquire);
}

}  // namespace we::scene
