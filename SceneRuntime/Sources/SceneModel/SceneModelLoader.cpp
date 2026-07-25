#include <SceneModel/SceneModel.hpp>

#include <SceneCore/AssetResolver.hpp>
#include <SceneCore/FormatError.hpp>
#include <SceneCore/PuppetMesh.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace we::scene {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximumValueDepth = 128;
constexpr std::size_t maximumCollectionSize = 1'000'000;
constexpr int particleControlPointSlotCount = 8;
constexpr std::uint32_t linuxDefaultParticlePoolSize = 1'000;

struct Document {
    std::string path;
    Json root;
    std::vector<std::string> referenceChain;
};

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

std::string childPointer(std::string_view parent, std::string_view token) {
    std::string result(parent);
    result += '/';
    result += escapePointerToken(token);
    return result;
}

std::string childPointer(std::string_view parent, std::size_t index) {
    return childPointer(parent, std::to_string(index));
}

[[noreturn]] void fail(
    const Document& document,
    std::string pointer,
    SceneModelErrorCode code,
    std::string message
) {
    throw SceneModelError(
        code,
        document.path,
        std::move(pointer),
        document.referenceChain,
        std::move(message)
    );
}

void requireObject(
    const Document& document,
    const Json& value,
    std::string_view pointer,
    std::string_view description
) {
    if (!value.is_object()) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::typeMismatch,
            std::string(description) + " must be an object"
        );
    }
}

void requireArray(
    const Document& document,
    const Json& value,
    std::string_view pointer,
    std::string_view description
) {
    if (!value.is_array()) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::typeMismatch,
            std::string(description) + " must be an array"
        );
    }
    if (value.size() > maximumCollectionSize) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::invalidValue,
            std::string(description) + " exceeds the collection size limit"
        );
    }
}

const Json& requiredField(
    const Document& document,
    const Json& object,
    std::string_view key,
    std::string_view parentPointer
) {
    const auto found = object.find(std::string(key));
    if (found == object.end()) {
        fail(
            document,
            childPointer(parentPointer, key),
            SceneModelErrorCode::missingField,
            "Required field '" + std::string(key) + "' is missing"
        );
    }
    return *found;
}

const Json* optionalField(const Json& object, std::string_view key) {
    const auto found = object.find(std::string(key));
    return found == object.end() || found->is_null() ? nullptr : &*found;
}

std::string stringValue(
    const Document& document,
    const Json& value,
    std::string_view pointer,
    std::string_view description,
    bool allowEmpty = true
) {
    if (!value.is_string()) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::typeMismatch,
            std::string(description) + " must be a string"
        );
    }
    std::string result = value.get<std::string>();
    if (!allowEmpty && result.empty()) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::invalidValue,
            std::string(description) + " must not be empty"
        );
    }
    return result;
}

std::string requiredString(
    const Document& document,
    const Json& object,
    std::string_view key,
    std::string_view parentPointer,
    bool allowEmpty = true
) {
    const std::string pointer = childPointer(parentPointer, key);
    return stringValue(
        document,
        requiredField(document, object, key, parentPointer),
        pointer,
        std::string("Field '") + std::string(key) + "'",
        allowEmpty
    );
}

std::optional<std::string> optionalString(
    const Document& document,
    const Json& object,
    std::string_view key,
    std::string_view parentPointer,
    bool allowEmpty = true
) {
    const Json* value = optionalField(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return stringValue(
        document,
        *value,
        childPointer(parentPointer, key),
        std::string("Field '") + std::string(key) + "'",
        allowEmpty
    );
}

std::int64_t integerValue(
    const Document& document,
    const Json& value,
    std::string_view pointer,
    std::string_view description
) {
    if (value.is_number_unsigned()) {
        const auto result = value.get<std::uint64_t>();
        if (result <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()
                      )) {
            return static_cast<std::int64_t>(result);
        }
    } else if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    fail(
        document,
        std::string(pointer),
        SceneModelErrorCode::typeMismatch,
        std::string(description) + " must be an integer"
    );
}

int intValue(
    const Document& document,
    const Json& value,
    std::string_view pointer,
    std::string_view description
) {
    const std::int64_t parsed = integerValue(
        document,
        value,
        pointer,
        description
    );
    if (parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::invalidValue,
            std::string(description) + " is outside the supported integer range"
        );
    }
    return static_cast<int>(parsed);
}

int requiredInt(
    const Document& document,
    const Json& object,
    std::string_view key,
    std::string_view parentPointer
) {
    const std::string pointer = childPointer(parentPointer, key);
    return intValue(
        document,
        requiredField(document, object, key, parentPointer),
        pointer,
        std::string("Field '") + std::string(key) + "'"
    );
}

std::optional<int> optionalInt(
    const Document& document,
    const Json& object,
    std::string_view key,
    std::string_view parentPointer
) {
    const Json* value = optionalField(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return intValue(
        document,
        *value,
        childPointer(parentPointer, key),
        std::string("Field '") + std::string(key) + "'"
    );
}

std::uint32_t uint32Value(
    const Document& document,
    const Json& value,
    std::string_view pointer,
    std::string_view description
) {
    const std::int64_t parsed = integerValue(
        document,
        value,
        pointer,
        description
    );
    if (parsed < 0 ||
        static_cast<std::uint64_t>(parsed) >
            std::numeric_limits<std::uint32_t>::max()) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::invalidValue,
            std::string(description) + " is outside the supported 32-bit range"
        );
    }
    return static_cast<std::uint32_t>(parsed);
}

std::optional<std::uint32_t> optionalUInt32(
    const Document& document,
    const Json& object,
    std::string_view key,
    std::string_view parentPointer
) {
    const Json* value = optionalField(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return uint32Value(
        document,
        *value,
        childPointer(parentPointer, key),
        std::string("Field '") + std::string(key) + "'"
    );
}

double numberValue(
    const Document& document,
    const Json& value,
    std::string_view pointer,
    std::string_view description
) {
    if (!value.is_number()) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::typeMismatch,
            std::string(description) + " must be a number"
        );
    }
    const double result = value.get<double>();
    if (!std::isfinite(result)) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::invalidValue,
            std::string(description) + " must be finite"
        );
    }
    return result;
}

std::optional<double> optionalNumber(
    const Document& document,
    const Json& object,
    std::string_view key,
    std::string_view parentPointer
) {
    const Json* value = optionalField(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return numberValue(
        document,
        *value,
        childPointer(parentPointer, key),
        std::string("Field '") + std::string(key) + "'"
    );
}

bool boolValue(
    const Document& document,
    const Json& value,
    std::string_view pointer,
    std::string_view description
) {
    if (!value.is_boolean()) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::typeMismatch,
            std::string(description) + " must be a boolean"
        );
    }
    return value.get<bool>();
}

bool optionalBool(
    const Document& document,
    const Json& object,
    std::string_view key,
    std::string_view parentPointer,
    bool defaultValue
) {
    const Json* value = optionalField(object, key);
    if (value == nullptr) {
        return defaultValue;
    }
    return boolValue(
        document,
        *value,
        childPointer(parentPointer, key),
        std::string("Field '") + std::string(key) + "'"
    );
}

Value parseValue(
    const Document& document,
    const Json& source,
    std::string_view pointer,
    std::size_t depth = 0
) {
    if (depth > maximumValueDepth) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::invalidValue,
            "Value nesting exceeds the supported depth"
        );
    }

    Value result;
    if (source.is_null()) {
        result.storage = nullptr;
    } else if (source.is_boolean()) {
        result.storage = source.get<bool>();
    } else if (source.is_number_unsigned()) {
        result.storage = integerValue(document, source, pointer, "Value");
    } else if (source.is_number_integer()) {
        result.storage = source.get<std::int64_t>();
    } else if (source.is_number_float()) {
        result.storage = numberValue(document, source, pointer, "Value");
    } else if (source.is_string()) {
        result.storage = source.get<std::string>();
    } else if (source.is_array()) {
        requireArray(document, source, pointer, "Value");
        Value::Array values;
        values.reserve(source.size());
        for (std::size_t index = 0; index < source.size(); ++index) {
            values.push_back(parseValue(
                document,
                source[index],
                childPointer(pointer, index),
                depth + 1
            ));
        }
        result.storage = std::move(values);
    } else if (source.is_object()) {
        if (source.size() > maximumCollectionSize) {
            fail(
                document,
                std::string(pointer),
                SceneModelErrorCode::invalidValue,
                "Value object exceeds the collection size limit"
            );
        }
        Value::Object values;
        for (const auto& [key, value] : source.items()) {
            values.emplace(
                key,
                parseValue(
                    document,
                    value,
                    childPointer(pointer, key),
                    depth + 1
                )
            );
        }
        result.storage = std::move(values);
    } else {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::typeMismatch,
            "Unsupported JSON value type"
        );
    }
    return result;
}

enum class DynamicValueParseMode { standard, color };

RuntimeValue runtimeValue(
    const Document& document,
    const Json& source,
    std::string_view pointer,
    DynamicValueParseMode mode
) {
    if (source.is_null()) return RuntimeValue::null();
    if (source.is_boolean()) return RuntimeValue::boolean(source.get<bool>());
    if (source.is_number_unsigned()) {
        return RuntimeValue::integer(integerValue(document, source, pointer, "Value"));
    }
    if (source.is_number_integer()) {
        return RuntimeValue::integer(source.get<std::int64_t>());
    }
    if (source.is_number_float()) {
        const double parsed = numberValue(document, source, pointer, "Value");
        const float narrowed = static_cast<float>(parsed);
        if (!std::isfinite(narrowed)) {
            fail(
                document,
                std::string(pointer),
                SceneModelErrorCode::invalidValue,
                "Dynamic float is outside the finite 32-bit range"
            );
        }
        return RuntimeValue::floating(static_cast<double>(narrowed));
    }
    if (!source.is_string()) {
        // Upstream leaves unsupported DynamicValue JSON types at Null.
        return RuntimeValue::null();
    }
    try {
        std::string value = source.get<std::string>();
        return mode == DynamicValueParseMode::color
            ? RuntimeValue::colorString(std::move(value))
            : RuntimeValue::initialString(std::move(value));
    } catch (const std::exception& error) {
        fail(
            document,
            std::string(pointer),
            SceneModelErrorCode::invalidValue,
            std::string("Invalid dynamic value: ") + error.what()
        );
    }
}

DynamicValue literal(Value::Storage storage) {
    DynamicValue result;
    if (auto* text = std::get_if<std::string>(&storage)) {
        result.value = RuntimeValue::initialString(std::move(*text));
    } else {
        result.value = RuntimeValue::fromValue(Value{.storage = std::move(storage)});
    }
    return result;
}

DynamicValue colorLiteral(const std::array<double, 4>& components) {
    DynamicValue result;
    result.value = RuntimeValue::color(components);
    return result;
}

class Parser final {
public:
    explicit Parser(const AssetResolver& resolver) : resolver_(resolver) {}

    SceneProject parse(std::string_view projectPath) {
        if (projectPath.empty()) {
            throw SceneModelError(
                SceneModelErrorCode::invalidValue,
                {},
                {},
                {},
                "Project path must not be empty"
            );
        }

        const std::string path(projectPath);
        Document document = loadDocument(path, {path});
        requireObject(document, document.root, "", "Project root");

        std::string type = requiredString(document, document.root, "type", "", false);
        std::ranges::transform(type, type.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (type != "scene") {
            fail(
                document,
                "/type",
                SceneModelErrorCode::unsupportedProject,
                "Only Scene projects are supported by the Scene runtime"
            );
        }

        SceneProject project;
        project.assetPath = path;
        project.title = requiredString(document, document.root, "title", "");
        if (const Json* workshopId = optionalField(document.root, "workshopid")) {
            if (workshopId->is_string()) {
                project.workshopId = workshopId->get<std::string>();
            } else if (workshopId->is_number_unsigned()) {
                project.workshopId = std::to_string(workshopId->get<std::uint64_t>());
            } else if (workshopId->is_number_integer()) {
                project.workshopId = std::to_string(workshopId->get<std::int64_t>());
            } else {
                fail(
                    document,
                    "/workshopid",
                    SceneModelErrorCode::typeMismatch,
                    "Workshop id must be a string or integer"
                );
            }
        }

        if (const Json* general = optionalField(document.root, "general")) {
            requireObject(document, *general, "/general", "Project general section");
            project.supportsAudioProcessing = optionalBool(
                document,
                *general,
                "supportsaudioprocessing",
                "/general",
                false
            );
            if (const Json* properties = optionalField(*general, "properties")) {
                project.properties = parseProperties(
                    document,
                    *properties,
                    "/general/properties"
                );
            }
        }

        properties_ = &project.properties;
        const std::string scenePath = requiredString(
            document,
            document.root,
            "file",
            "",
            false
        );
        project.scene = parseScene(
            scenePath,
            referenceChain(document, "/file", scenePath)
        );
        properties_ = nullptr;
        return project;
    }

private:
    Document loadDocument(
        const std::string& path,
        std::vector<std::string> chain
    ) const {
        std::string contents;
        try {
            contents = resolver_.readString(path);
        } catch (const FormatError& error) {
            const SceneModelErrorCode code =
                error.code() == FormatErrorCode::assetNotFound
                    ? SceneModelErrorCode::danglingReference
                    : SceneModelErrorCode::assetFailure;
            throw SceneModelError(
                code,
                path,
                "",
                std::move(chain),
                error.what()
            );
        }

        try {
            return Document{
                .path = path,
                .root = Json::parse(contents),
                .referenceChain = std::move(chain),
            };
        } catch (const Json::parse_error& error) {
            throw SceneModelError(
                SceneModelErrorCode::invalidJson,
                path,
                "",
                std::move(chain),
                "Invalid JSON at byte " + std::to_string(error.byte) +
                    ": " + error.what()
            );
        }
    }

    std::vector<std::string> referenceChain(
        const Document& document,
        std::string_view pointer,
        std::string_view target
    ) const {
        auto result = document.referenceChain;
        result.push_back(
            document.path + "#" + std::string(pointer) + " -> " +
            std::string(target)
        );
        return result;
    }

    Value parsePropertyValue(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        return parseValue(document, source, pointer);
    }

    std::map<std::string, ProjectProperty> parseProperties(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Project properties");
        std::map<std::string, ProjectProperty> result;
        for (const auto& [key, value] : source.items()) {
            const std::string propertyPointer = childPointer(pointer, key);
            requireObject(document, value, propertyPointer, "Project property");

            // Upstream treats missing types as groups and ignores unknown
            // property types before inspecting any type-specific metadata.
            const Json* typeValue = optionalField(value, "type");
            if (typeValue == nullptr || !typeValue->is_string()) {
                continue;
            }

            ProjectProperty property;
            property.name = key;
            const std::string type = typeValue->get<std::string>();
            if (type == "bool") {
                property.type = PropertyType::boolean;
            } else if (type == "slider") {
                property.type = PropertyType::slider;
            } else if (type == "combo") {
                property.type = PropertyType::combo;
            } else if (type == "color") {
                property.type = PropertyType::color;
            } else if (type == "text") {
                property.type = PropertyType::text;
            } else if (type == "scenetexture") {
                property.type = PropertyType::sceneTexture;
            } else if (type == "file") {
                property.type = PropertyType::file;
            } else if (type == "directory") {
                property.type = PropertyType::directory;
            } else if (type == "textinput") {
                property.type = PropertyType::textInput;
            } else if (type == "usershortcut") {
                property.type = PropertyType::userShortcut;
            } else {
                continue;
            }

            property.text = optionalString(
                                document,
                                value,
                                "text",
                                propertyPointer
                            )
                                .value_or("");
            property.index = optionalInt(
                document,
                value,
                "index",
                propertyPointer
            );
            property.order = optionalInt(
                document,
                value,
                "order",
                propertyPointer
            );
            property.minimum = optionalNumber(
                document,
                value,
                "min",
                propertyPointer
            );
            property.maximum = optionalNumber(
                document,
                value,
                "max",
                propertyPointer
            );
            property.step = optionalNumber(
                document,
                value,
                "step",
                propertyPointer
            );
            property.precision = optionalInt(
                document,
                value,
                "precision",
                propertyPointer
            );
            if (const Json* fraction = optionalField(value, "fraction")) {
                property.fraction = boolValue(
                    document,
                    *fraction,
                    childPointer(propertyPointer, "fraction"),
                    "Property fraction"
                );
            }

            parsePropertyTypeDetails(
                document,
                value,
                propertyPointer,
                property
            );
            result.emplace(key, std::move(property));
        }
        return result;
    }

    void parsePropertyTypeDetails(
        const Document& document,
        const Json& source,
        std::string_view pointer,
        ProjectProperty& property
    ) const {
        const Json* value = optionalField(source, "value");
        const std::string valuePointer = childPointer(pointer, "value");
        switch (property.type) {
            case PropertyType::boolean:
                property.value = Value{.storage = value == nullptr
                    ? Value::Storage(false)
                    : Value::Storage(boolValue(
                          document,
                          *value,
                          valuePointer,
                          "Boolean property value"
                      ))};
                break;

            case PropertyType::slider: {
                const Json& requiredValue = value != nullptr
                    ? *value
                    : requiredField(document, source, "value", pointer);
                const double number = numberValue(
                    document,
                    requiredValue,
                    valuePointer,
                    "Slider property value"
                );
                property.value = Value{.storage = number};
                break;
            }

            case PropertyType::combo: {
                const Json& options = requiredField(
                    document,
                    source,
                    "options",
                    pointer
                );
                const std::string optionsPointer = childPointer(pointer, "options");
                requireArray(document, options, optionsPointer, "Combo options");
                std::unordered_set<std::string> optionValues;
                for (std::size_t index = 0; index < options.size(); ++index) {
                    const Json& option = options[index];
                    const std::string optionPointer = childPointer(
                        optionsPointer,
                        index
                    );
                    if (!option.is_object()) {
                        continue;
                    }
                    const Json& rawValue = requiredField(
                        document,
                        option,
                        "value",
                        optionPointer
                    );
                    std::string normalized;
                    if (rawValue.is_string()) {
                        normalized = rawValue.get<std::string>();
                    } else {
                        normalized = std::to_string(integerValue(
                            document,
                            rawValue,
                            childPointer(optionPointer, "value"),
                            "Combo option value"
                        ));
                    }
                    if (!optionValues.emplace(normalized).second) {
                        continue;
                    }
                    property.options.push_back(PropertyOption{
                        .value = std::move(normalized),
                        .label = requiredString(
                            document,
                            option,
                            "label",
                            optionPointer
                        ),
                    });
                }
                const Json& requiredValue = value != nullptr
                    ? *value
                    : requiredField(document, source, "value", pointer);
                std::string selected;
                if (requiredValue.is_string()) {
                    selected = requiredValue.get<std::string>();
                } else {
                    selected = std::to_string(integerValue(
                        document,
                        requiredValue,
                        valuePointer,
                        "Combo property value"
                    ));
                }
                property.value = Value{.storage = std::move(selected)};
                break;
            }

            case PropertyType::color:
            case PropertyType::sceneTexture:
            case PropertyType::textInput:
            case PropertyType::userShortcut:
            {
                const Json& requiredValue = value != nullptr
                    ? *value
                    : requiredField(document, source, "value", pointer);
                property.value = Value{.storage = stringValue(
                    document,
                    requiredValue,
                    valuePointer,
                    "Property value"
                )};
                break;
            }

            case PropertyType::file:
            case PropertyType::directory:
                if (value != nullptr) {
                    property.value = Value{.storage = stringValue(
                        document,
                        *value,
                        valuePointer,
                        "Property value"
                    )};
                } else {
                    property.value = Value{.storage = std::string()};
                }
                break;

            case PropertyType::text:
            case PropertyType::group:
                if (value != nullptr) {
                    property.value = parsePropertyValue(
                        document,
                        *value,
                        valuePointer
                    );
                }
                break;
        }
    }

    DynamicValue parseDynamic(
        const Document& document,
        const Json& source,
        std::string_view pointer,
        DynamicValueParseMode mode = DynamicValueParseMode::standard,
        std::size_t depth = 0
    ) const {
        if (depth > maximumValueDepth) {
            fail(
                document,
                std::string(pointer),
                SceneModelErrorCode::invalidValue,
                "Dynamic value nesting exceeds the supported depth"
            );
        }

        DynamicValue result;
        // Linux DynamicValueParser treats every object as a wrapper and
        // requires its authored `value` before reading optional bindings.
        const bool wrapper = source.is_object();
        if (!wrapper) {
            result.value = runtimeValue(document, source, pointer, mode);
        } else {
            const Json& initial = requiredField(
                document,
                source,
                "value",
                pointer
            );
            result.value = runtimeValue(
                document,
                initial,
                childPointer(pointer, "value"),
                mode
            );

            if (const Json* user = optionalField(source, "user")) {
                UserBinding binding;
                const std::string userPointer = childPointer(pointer, "user");
                if (user->is_string()) {
                    binding.property = stringValue(
                        document,
                        *user,
                        userPointer,
                        "User property reference"
                    );
                } else if (user->is_object()) {
                    binding.property = requiredString(
                        document,
                        *user,
                        "name",
                        userPointer
                    );
                    binding.condition = requiredString(
                        document,
                        *user,
                        "condition",
                        userPointer
                    );
                } else {
                    fail(
                        document,
                        userPointer,
                        SceneModelErrorCode::typeMismatch,
                        "User binding must be a property name or binding object"
                    );
                }
                // Upstream only connects bindings whose project property
                // exists. Unknown names retain the wrapper's literal value.
                if (properties_ != nullptr &&
                    properties_->contains(binding.property)) {
                    result.user = std::move(binding);
                }
            }

            if (const Json* script = optionalField(source, "script")) {
                result.script = stringValue(
                    document,
                    *script,
                    childPointer(pointer, "script"),
                    "Dynamic script source"
                );
            }
            if (result.script) {
                const Json* scriptProperties = optionalField(
                    source,
                    "scriptproperties"
                );
                if (scriptProperties != nullptr && scriptProperties->is_object()) {
                    const std::string propertiesPointer = childPointer(
                        pointer,
                        "scriptproperties"
                    );
                    for (const auto& [key, value] : scriptProperties->items()) {
                        result.scriptProperties.emplace(
                            key,
                            parseDynamic(
                                document,
                                value,
                                childPointer(propertiesPointer, key),
                                DynamicValueParseMode::standard,
                                depth + 1
                            )
                        );
                    }
                }
            }
        }
        return result;
    }

    DynamicValue optionalDynamic(
        const Document& document,
        const Json& object,
        std::string_view key,
        std::string_view parentPointer,
        DynamicValue defaultValue,
        DynamicValueParseMode mode
    ) const {
        const Json* value = optionalField(object, key);
        if (value == nullptr) {
            return defaultValue;
        }
        return parseDynamic(
            document,
            *value,
            childPointer(parentPointer, key),
            mode
        );
    }

    Scene parseScene(
        const std::string& path,
        std::vector<std::string> chain
    ) {
        Document document = loadDocument(path, std::move(chain));
        requireObject(document, document.root, "", "Scene root");
        const Json& cameraJson = requiredField(
            document,
            document.root,
            "camera",
            ""
        );
        const Json& general = requiredField(
            document,
            document.root,
            "general",
            ""
        );
        const Json& objects = requiredField(
            document,
            document.root,
            "objects",
            ""
        );
        requireObject(document, cameraJson, "/camera", "Scene camera");
        requireObject(document, general, "/general", "Scene general section");
        requireArray(document, objects, "/objects", "Scene objects");
        if (objects.empty()) {
            fail(
                document,
                "/objects",
                SceneModelErrorCode::invalidValue,
                "Scene objects must not be empty"
            );
        }

        Scene result;
        result.assetPath = path;
        result.camera.center = parseDynamic(
            document,
            requiredField(document, cameraJson, "center", "/camera"),
            "/camera/center",
            DynamicValueParseMode::standard
        );
        result.camera.eye = parseDynamic(
            document,
            requiredField(document, cameraJson, "eye", "/camera"),
            "/camera/eye",
            DynamicValueParseMode::standard
        );
        result.camera.up = parseDynamic(
            document,
            requiredField(document, cameraJson, "up", "/camera"),
            "/camera/up",
            DynamicValueParseMode::standard
        );

        // Linux parses only these known DynamicValue fields in `general` and
        // gives each one a concrete default in the model. Structural objects
        // such as `orthogonalprojection` are handled separately below.
        const auto generalDynamic = [&](
            std::string_view key,
            DynamicValue defaultValue
        ) {
            result.generalValues.emplace(
                std::string(key),
                optionalDynamic(
                    document,
                    general,
                    key,
                    "/general",
                    std::move(defaultValue),
                    DynamicValueParseMode::standard
                )
            );
        };
        generalDynamic("ambientcolor", literal(std::string("0 0 0")));
        generalDynamic("skylightcolor", literal(std::string("0 0 0")));
        generalDynamic("clearcolor", literal(std::string("1 1 1")));
        generalDynamic("camerafade", literal(false));
        generalDynamic("bloom", literal(false));
        generalDynamic("bloomstrength", literal(0.0));
        generalDynamic("bloomthreshold", literal(0.0));
        generalDynamic("cameraparallax", literal(false));
        generalDynamic("cameraparallaxamount", literal(1.0));
        generalDynamic("cameraparallaxdelay", literal(0.0));
        generalDynamic("cameraparallaxmouseinfluence", literal(1.0));
        generalDynamic("camerashake", literal(false));
        generalDynamic("camerashakeamplitude", literal(0.0));
        generalDynamic("camerashakeroughness", literal(0.0));
        generalDynamic("camerashakespeed", literal(0.0));

        result.camera.preview = optionalBool(
            document,
            general,
            "camerapreview",
            "/general",
            false
        );
        result.camera.nearPlane = optionalDynamic(
            document,
            cameraJson,
            "nearz",
            "/camera",
            literal(0.0),
            DynamicValueParseMode::standard
        );
        result.camera.farPlane = optionalDynamic(
            document,
            cameraJson,
            "farz",
            "/camera",
            literal(1000.0),
            DynamicValueParseMode::standard
        );
        result.camera.fieldOfView = optionalDynamic(
            document,
            cameraJson,
            "fov",
            "/camera",
            literal(50.0),
            DynamicValueParseMode::standard
        );
        const Json& projection = requiredField(
            document,
            general,
            "orthogonalprojection",
            "/general"
        );
        requireObject(
            document,
            projection,
            "/general/orthogonalprojection",
            "Orthogonal projection"
        );
        result.camera.projectionAuto = optionalBool(
            document,
            projection,
            "auto",
            "/general/orthogonalprojection",
            false
        );
        if (!result.camera.projectionAuto) {
            result.camera.projectionWidth = requiredInt(
                document,
                projection,
                "width",
                "/general/orthogonalprojection"
            );
            result.camera.projectionHeight = requiredInt(
                document,
                projection,
                "height",
                "/general/orthogonalprojection"
            );
        }

        result.objects.reserve(objects.size());
        std::unordered_map<int, std::size_t> objectIndices;
        for (std::size_t index = 0; index < objects.size(); ++index) {
            SceneObject object = parseObject(
                document,
                objects[index],
                childPointer("/objects", index)
            );
            if (!objectIndices.emplace(object.base.id, index).second) {
                fail(
                    document,
                    childPointer(childPointer("/objects", index), "id"),
                    SceneModelErrorCode::duplicateId,
                    "Scene contains duplicate object id " +
                        std::to_string(object.base.id)
                );
            }
            result.objects.push_back(std::move(object));
        }
        validateObjectReferences(document, result.objects, objectIndices);

        const DynamicValue& bloom = result.generalValues.at("bloom");
        if (bloom.value.boolean() || bloom.isDynamic()) {
            result.bloomModel = makeLinuxBloomModel();
            result.bloomEffect = makeLinuxBloomEffect();
        }
        return result;
    }

    std::shared_ptr<const Model> makeLinuxBloomModel() {
        auto material = std::make_shared<Material>();
        material->assetPath = "materials/wpenginelinux.json";
        MaterialPass pass;
        pass.blending = BlendingMode::normal;
        pass.culling = CullingMode::disabled;
        pass.depthTest = DepthMode::disabled;
        pass.depthWrite = DepthMode::disabled;
        pass.shader = "genericimage2";
        pass.textures.push_back(TextureSlot{
            .name = std::string("_rt_FullFrameBuffer"),
        });
        material->passes.push_back(std::move(pass));

        auto model = std::make_shared<Model>();
        model->assetPath = "models/wpenginelinux.json";
        model->material = std::move(material);
        return model;
    }

    std::shared_ptr<const Effect> makeLinuxBloomEffect() {
        auto effect = std::make_shared<Effect>();
        effect->assetPath = "effects/wpenginelinux/bloomeffect.json";
        effect->name = "camerabloom_wpengine_linux";
        effect->group = "wpengine_linux_camera";

        const auto addPass = [this, &effect](
            std::string materialPath,
            std::string target,
            std::vector<EffectBind> binds
        ) {
            EffectPass pass;
            pass.material = loadMaterial(
                materialPath,
                {
                    "effects/wpenginelinux/bloomeffect.json#/passes -> " +
                        materialPath,
                }
            );
            pass.target = std::move(target);
            pass.binds = std::move(binds);
            effect->passes.push_back(std::move(pass));
        };

        addPass(
            "materials/util/downsample_quarter_bloom.json",
            "_rt_4FrameBuffer",
            {{.index = 0, .name = "_rt_FullFrameBuffer"}}
        );
        addPass(
            "materials/util/downsample_eighth_blur_v.json",
            "_rt_8FrameBuffer",
            {{.index = 0, .name = "_rt_4FrameBuffer"}}
        );
        addPass(
            "materials/util/blur_h_bloom.json",
            "_rt_Bloom",
            {{.index = 0, .name = "_rt_8FrameBuffer"}}
        );
        addPass(
            "materials/util/combine.json",
            "_rt_FullFrameBuffer",
            {
                {.index = 0, .name = "_rt_imageLayerComposite_-1_a"},
                {.index = 1, .name = "_rt_Bloom"},
            }
        );
        return effect;
    }

    ParticleVector3 particleVector3(
        const Document& document,
        const Json& source,
        std::string_view pointer,
        std::string_view description,
        ParticleVector3 defaultValue,
        bool allowScalar,
        bool allowString
    ) const {
        ParticleVector3 result;
        if (allowScalar && source.is_number()) {
            const double component = numberValue(
                document,
                source,
                pointer,
                description
            );
            return {component, component, component};
        }
        if (allowString && source.is_string()) {
            std::istringstream input(source.get<std::string>());
            if (!(input >> result.x >> result.y >> result.z)) {
                fail(
                    document,
                    std::string(pointer),
                    SceneModelErrorCode::invalidValue,
                    std::string(description) +
                        " must contain exactly three finite numbers"
                );
            }
            input >> std::ws;
            if (!input.eof() || !std::isfinite(result.x) ||
                !std::isfinite(result.y) || !std::isfinite(result.z)) {
                fail(
                    document,
                    std::string(pointer),
                    SceneModelErrorCode::invalidValue,
                    std::string(description) +
                        " must contain exactly three finite numbers"
                );
            }
            return result;
        }
        if (source.is_array()) {
            if (source.size() < 3) {
                return defaultValue;
            }
            result.x = numberValue(
                document,
                source[0],
                childPointer(pointer, 0),
                description
            );
            result.y = numberValue(
                document,
                source[1],
                childPointer(pointer, 1),
                description
            );
            result.z = numberValue(
                document,
                source[2],
                childPointer(pointer, 2),
                description
            );
            return result;
        }
        return defaultValue;
    }

    ParticleVector3 optionalParticleVector3(
        const Document& document,
        const Json& source,
        std::string_view key,
        std::string_view pointer,
        ParticleVector3 defaultValue,
        bool allowScalar = true,
        bool allowString = true
    ) const {
        const Json* value = optionalField(source, key);
        if (value == nullptr || value->is_null()) {
            return defaultValue;
        }
        return particleVector3(
            document,
            *value,
            childPointer(pointer, key),
            std::string("Particle field '") + std::string(key) + "'",
            defaultValue,
            allowScalar,
            allowString
        );
    }

    ParticleVector3 optionalParticleControlPointOffset(
        const Json& source,
        std::string_view key
    ) const {
        const Json* value = optionalField(source, key);
        if (value == nullptr || !value->is_string()) {
            return {};
        }

        // The pinned Linux runtime initializes the vector to zero and lets
        // stream extraction fill as many authored string components as exist.
        ParticleVector3 result;
        std::istringstream input(value->get<std::string>());
        input >> result.x >> result.y >> result.z;
        return result;
    }

    std::optional<int> optionalParticleId(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        const Json* id = optionalField(source, "id");
        if (id == nullptr || id->is_null()) {
            return std::nullopt;
        }
        return intValue(
            document,
            *id,
            childPointer(pointer, "id"),
            "Particle component id"
        );
    }

    DynamicValue optionalParticleDynamic(
        const Document& document,
        const Json& source,
        std::string_view key,
        std::string_view pointer,
        DynamicValue defaultValue,
        DynamicValueParseMode mode
    ) const {
        const Json* value = optionalField(source, key);
        if (value == nullptr || value->is_null()) {
            return defaultValue;
        }
        return parseDynamic(
            document,
            *value,
            childPointer(pointer, key),
            mode
        );
    }

    DynamicValue optionalParticleColor(
        const Document& document,
        const Json& source,
        std::string_view key,
        std::string_view pointer,
        const std::array<double, 4>& defaultValue
    ) const {
        const Json* value = optionalField(source, key);
        if (value == nullptr || value->is_null()) {
            return colorLiteral(defaultValue);
        }
        return parseDynamic(
            document,
            *value,
            childPointer(pointer, key),
            DynamicValueParseMode::color
        );
    }

    std::optional<ParticleEmitter> parseParticleEmitter(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Particle emitter");
        const std::string name = optionalString(
            document, source, "name", pointer, false
        ).value_or("");

        ParticleEmitterBase base;
        base.id = optionalParticleId(document, source, pointer);
        base.directions = optionalParticleVector3(
            document, source, "directions", pointer, {1.0, 1.0, 0.0}
        );
        base.distanceMin = optionalParticleVector3(
            document, source, "distancemin", pointer, {}, true
        );
        base.distanceMax = optionalParticleVector3(
            document,
            source,
            "distancemax",
            pointer,
            {256.0, 256.0, 0.0},
            true
        );
        base.origin = optionalParticleVector3(
            document, source, "origin", pointer, {}
        );
        if (const Json* instantaneous = optionalField(source, "instantaneous");
            instantaneous != nullptr && !instantaneous->is_null()) {
            base.instantaneous = uint32Value(
                document,
                *instantaneous,
                childPointer(pointer, "instantaneous"),
                "Particle instantaneous count"
            );
        }
        if (const Json* rate = optionalField(source, "rate");
            rate != nullptr && !rate->is_null()) {
            base.rate = numberValue(
                document,
                *rate,
                childPointer(pointer, "rate"),
                "Particle emission rate"
            );
        }
        const auto optionalNonnegativeNumber = [&document, &source, pointer](
            std::string_view field,
            double fallback,
            std::string_view description
        ) {
            const Json* value = optionalField(source, field);
            if (value == nullptr || value->is_null()) {
                return fallback;
            }
            const double result = numberValue(
                document,
                *value,
                childPointer(pointer, field),
                description
            );
            if (result < 0.0) {
                fail(
                    document,
                    childPointer(pointer, field),
                    SceneModelErrorCode::invalidValue,
                    std::string(description) + " must be non-negative"
                );
            }
            return result;
        };
        base.delay = optionalNonnegativeNumber(
            "delay", base.delay, "Particle emitter delay"
        );
        base.duration = optionalNonnegativeNumber(
            "duration", base.duration, "Particle emitter duration"
        );
        base.minimumPeriodicDelay = optionalNonnegativeNumber(
            "minperiodicdelay",
            base.minimumPeriodicDelay,
            "Particle emitter minimum periodic delay"
        );
        base.maximumPeriodicDelay = optionalNonnegativeNumber(
            "maxperiodicdelay",
            base.maximumPeriodicDelay,
            "Particle emitter maximum periodic delay"
        );
        base.minimumPeriodicDuration = optionalNonnegativeNumber(
            "minperiodicduration",
            base.minimumPeriodicDuration,
            "Particle emitter minimum periodic duration"
        );
        base.maximumPeriodicDuration = optionalNonnegativeNumber(
            "maxperiodicduration",
            base.maximumPeriodicDuration,
            "Particle emitter maximum periodic duration"
        );
        if (const Json* maximumToEmit = optionalField(
                source, "maxtoemitperperiod"
            ); maximumToEmit != nullptr && !maximumToEmit->is_null()) {
            base.maximumToEmitPerPeriod = uint32Value(
                document,
                *maximumToEmit,
                childPointer(pointer, "maxtoemitperperiod"),
                "Particle emitter maximum emissions per period"
            );
        }
        if (const Json* controlPoint = optionalField(source, "controlpoint");
            controlPoint != nullptr && !controlPoint->is_null()) {
            base.controlPoint = intValue(
                document,
                *controlPoint,
                childPointer(pointer, "controlpoint"),
                "Particle emitter control point"
            );
        }
        if (const Json* flags = optionalField(source, "flags");
            flags != nullptr && !flags->is_null()) {
            base.flags = uint32Value(
                document,
                *flags,
                childPointer(pointer, "flags"),
                "Particle emitter flags"
            );
        }

        if (name == "boxrandom") {
            return ParticleBoxRandomEmitter{.base = std::move(base)};
        }
        if (name == "sphererandom") {
            ParticleSphereRandomEmitter result;
            result.base = std::move(base);
            result.sign = optionalParticleVector3(
                document, source, "sign", pointer, {}, false, false
            );
            if (const Json* speedMin = optionalField(source, "speedmin");
                speedMin != nullptr && !speedMin->is_null()) {
                result.speedMin = numberValue(
                    document,
                    *speedMin,
                    childPointer(pointer, "speedmin"),
                    "Particle sphere minimum speed"
                );
            }
            if (const Json* speedMax = optionalField(source, "speedmax");
                speedMax != nullptr && !speedMax->is_null()) {
                result.speedMax = numberValue(
                    document,
                    *speedMax,
                    childPointer(pointer, "speedmax"),
                    "Particle sphere maximum speed"
                );
            }
            return result;
        }
        // Upstream leaves unknown component names out of the executable list.
        return std::nullopt;
    }

    std::optional<ParticleInitializer> parseParticleInitializer(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Particle initializer");
        const std::string name = optionalString(
            document, source, "name", pointer, false
        ).value_or("");
        const std::optional<int> id = optionalParticleId(
            document, source, pointer
        );

        if (name == "lifetimerandom") {
            return ParticleLifetimeRandomInitializer{
                .id = id,
                .minimum = optionalParticleDynamic(
                    document, source, "min", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .maximum = optionalParticleDynamic(
                    document, source, "max", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "sizerandom") {
            return ParticleSizeRandomInitializer{
                .id = id,
                .minimum = optionalParticleDynamic(
                    document, source, "min", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .maximum = optionalParticleDynamic(
                    document, source, "max", pointer, literal(20.0),
                    DynamicValueParseMode::standard
                ),
                .exponent = optionalParticleDynamic(
                    document, source, "exponent", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "colorrandom") {
            return ParticleColorRandomInitializer{
                .id = id,
                .minimum = optionalParticleColor(
                    document, source, "min", pointer, {0.0, 0.0, 0.0, 1.0}
                ),
                .maximum = optionalParticleColor(
                    document, source, "max", pointer, {1.0, 1.0, 1.0, 1.0}
                ),
            };
        }
        if (name == "alpharandom") {
            return ParticleAlphaRandomInitializer{
                .id = id,
                .minimum = optionalParticleDynamic(
                    document, source, "min", pointer, literal(0.05),
                    DynamicValueParseMode::standard
                ),
                .maximum = optionalParticleDynamic(
                    document, source, "max", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "velocityrandom" || name == "rotationrandom") {
            const bool velocity = name == "velocityrandom";
            DynamicValue minimum = optionalParticleDynamic(
                document,
                source,
                "min",
                pointer,
                literal(std::string(velocity ? "-32 -32 -32" : "0 0 0")),
                DynamicValueParseMode::standard
            );
            DynamicValue maximum = optionalParticleDynamic(
                document,
                source,
                "max",
                pointer,
                literal(std::string(
                    velocity ? "32 32 32" : "0 0 6.283185307179586"
                )),
                DynamicValueParseMode::standard
            );
            if (velocity) {
                return ParticleVelocityRandomInitializer{
                    .id = id,
                    .minimum = std::move(minimum),
                    .maximum = std::move(maximum),
                };
            }
            return ParticleRotationRandomInitializer{
                .id = id,
                .minimum = std::move(minimum),
                .maximum = std::move(maximum),
            };
        }
        if (name == "angularvelocityrandom") {
            return ParticleAngularVelocityRandomInitializer{
                .id = id,
                .minimum = optionalParticleDynamic(
                    document, source, "min", pointer,
                    literal(std::string("0 0 -5")),
                    DynamicValueParseMode::standard
                ),
                .maximum = optionalParticleDynamic(
                    document, source, "max", pointer,
                    literal(std::string("0 0 5")),
                    DynamicValueParseMode::standard
                ),
                .exponent = optionalParticleDynamic(
                    document, source, "exponent", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "turbulentvelocityrandom") {
            return ParticleTurbulentVelocityRandomInitializer{
                .id = id,
                .speedMinimum = optionalParticleDynamic(
                    document, source, "speedmin", pointer, literal(100.0),
                    DynamicValueParseMode::standard
                ),
                .speedMaximum = optionalParticleDynamic(
                    document, source, "speedmax", pointer, literal(250.0),
                    DynamicValueParseMode::standard
                ),
                .scale = optionalParticleDynamic(
                    document, source, "scale", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
                .offset = optionalParticleDynamic(
                    document, source, "offset", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .forward = optionalParticleDynamic(
                    document, source, "forward", pointer,
                    literal(std::string("0 1 0")), DynamicValueParseMode::standard
                ),
                .timeScale = optionalParticleDynamic(
                    document, source, "timescale", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
                .phaseMinimum = optionalParticleDynamic(
                    document, source, "phasemin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .phaseMaximum = optionalParticleDynamic(
                    document, source, "phasemax", pointer, literal(0.1),
                    DynamicValueParseMode::standard
                ),
                .right = optionalParticleDynamic(
                    document, source, "right", pointer,
                    literal(std::string("0 0 1")), DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "mapsequencearoundcontrolpoint") {
            return ParticleMapSequenceAroundControlPointInitializer{
                .id = id,
                .controlPoint = optionalParticleDynamic(
                    document, source, "controlpoint", pointer, literal(0),
                    DynamicValueParseMode::standard
                ),
                .count = optionalParticleDynamic(
                    document, source, "count", pointer, literal(1),
                    DynamicValueParseMode::standard
                ),
                .speedMinimum = optionalParticleDynamic(
                    document, source, "speedmin", pointer,
                    literal(std::string("0 0 0")),
                    DynamicValueParseMode::standard
                ),
                .speedMaximum = optionalParticleDynamic(
                    document, source, "speedmax", pointer,
                    literal(std::string("100 100 100")),
                    DynamicValueParseMode::standard
                ),
            };
        }
        return std::nullopt;
    }

    std::optional<ParticleOperator> parseParticleOperator(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Particle operator");
        const std::string name = optionalString(
            document, source, "name", pointer, false
        ).value_or("");
        const std::optional<int> id = optionalParticleId(
            document, source, pointer
        );
        if (name == "movement") {
            return ParticleMovementOperator{
                .id = id,
                .drag = optionalParticleDynamic(
                    document, source, "drag", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .gravity = optionalParticleDynamic(
                    document,
                    source,
                    "gravity",
                    pointer,
                    literal(std::string("0 0 0")),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "alphafade") {
            return ParticleAlphaFadeOperator{
                .id = id,
                .fadeInTime = optionalParticleDynamic(
                    document, source, "fadeintime", pointer, literal(0.5),
                    DynamicValueParseMode::standard
                ),
                .fadeOutTime = optionalParticleDynamic(
                    document, source, "fadeouttime", pointer, literal(0.5),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "angularmovement") {
            return ParticleAngularMovementOperator{
                .id = id,
                .drag = optionalParticleDynamic(
                    document, source, "drag", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .force = optionalParticleDynamic(
                    document, source, "force", pointer,
                    literal(std::string("0 0 0")), DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "oscillateposition") {
            return ParticleOscillatePositionOperator{
                .id = id,
                .frequencyMinimum = optionalParticleDynamic(
                    document, source, "frequencymin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .frequencyMaximum = optionalParticleDynamic(
                    document, source, "frequencymax", pointer, literal(5.0),
                    DynamicValueParseMode::standard
                ),
                .scaleMinimum = optionalParticleDynamic(
                    document, source, "scalemin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .scaleMaximum = optionalParticleDynamic(
                    document, source, "scalemax", pointer, literal(10.0),
                    DynamicValueParseMode::standard
                ),
                .phaseMinimum = optionalParticleDynamic(
                    document, source, "phasemin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .phaseMaximum = optionalParticleDynamic(
                    document, source, "phasemax", pointer,
                    literal(6.283185307179586), DynamicValueParseMode::standard
                ),
                .mask = optionalParticleDynamic(
                    document, source, "mask", pointer,
                    literal(std::string("1 1 0")), DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "oscillatealpha") {
            return ParticleOscillateAlphaOperator{
                .id = id,
                .frequencyMinimum = optionalParticleDynamic(
                    document, source, "frequencymin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .frequencyMaximum = optionalParticleDynamic(
                    document, source, "frequencymax", pointer, literal(10.0),
                    DynamicValueParseMode::standard
                ),
                .scaleMinimum = optionalParticleDynamic(
                    document, source, "scalemin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .scaleMaximum = optionalParticleDynamic(
                    document, source, "scalemax", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
                .phaseMinimum = optionalParticleDynamic(
                    document, source, "phasemin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .phaseMaximum = optionalParticleDynamic(
                    document, source, "phasemax", pointer,
                    literal(6.283185307179586), DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "oscillatesize") {
            return ParticleOscillateSizeOperator{
                .id = id,
                .frequencyMinimum = optionalParticleDynamic(
                    document, source, "frequencymin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .frequencyMaximum = optionalParticleDynamic(
                    document, source, "frequencymax", pointer, literal(10.0),
                    DynamicValueParseMode::standard
                ),
                .scaleMinimum = optionalParticleDynamic(
                    document, source, "scalemin", pointer, literal(0.8),
                    DynamicValueParseMode::standard
                ),
                .scaleMaximum = optionalParticleDynamic(
                    document, source, "scalemax", pointer, literal(1.2),
                    DynamicValueParseMode::standard
                ),
                .phaseMinimum = optionalParticleDynamic(
                    document, source, "phasemin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .phaseMaximum = optionalParticleDynamic(
                    document, source, "phasemax", pointer,
                    literal(6.283185307179586), DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "sizechange") {
            return ParticleSizeChangeOperator{
                .id = id,
                .startTime = optionalParticleDynamic(
                    document, source, "starttime", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .endTime = optionalParticleDynamic(
                    document, source, "endtime", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
                .startValue = optionalParticleDynamic(
                    document, source, "startvalue", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
                .endValue = optionalParticleDynamic(
                    document, source, "endvalue", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "alphachange") {
            return ParticleAlphaChangeOperator{
                .id = id,
                .startTime = optionalParticleDynamic(
                    document, source, "starttime", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .endTime = optionalParticleDynamic(
                    document, source, "endtime", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
                .startValue = optionalParticleDynamic(
                    document, source, "startvalue", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
                .endValue = optionalParticleDynamic(
                    document, source, "endvalue", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "colorchange") {
            return ParticleColorChangeOperator{
                .id = id,
                .startTime = optionalParticleDynamic(
                    document, source, "starttime", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .endTime = optionalParticleDynamic(
                    document, source, "endtime", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
                .startValue = optionalParticleDynamic(
                    document, source, "startvalue", pointer,
                    literal(std::string("1 1 1")),
                    DynamicValueParseMode::standard
                ),
                .endValue = optionalParticleDynamic(
                    document, source, "endvalue", pointer,
                    literal(std::string("1 1 1")),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "turbulence") {
            return ParticleTurbulenceOperator{
                .id = id,
                .scale = optionalParticleDynamic(
                    document, source, "scale", pointer, literal(0.005),
                    DynamicValueParseMode::standard
                ),
                .speedMinimum = optionalParticleDynamic(
                    document, source, "speedmin", pointer, literal(500.0),
                    DynamicValueParseMode::standard
                ),
                .speedMaximum = optionalParticleDynamic(
                    document, source, "speedmax", pointer, literal(1000.0),
                    DynamicValueParseMode::standard
                ),
                .timeScale = optionalParticleDynamic(
                    document, source, "timescale", pointer, literal(0.01),
                    DynamicValueParseMode::standard
                ),
                .mask = optionalParticleDynamic(
                    document, source, "mask", pointer,
                    literal(std::string("1 1 0")),
                    DynamicValueParseMode::standard
                ),
                .phaseMinimum = optionalParticleDynamic(
                    document, source, "phasemin", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .phaseMaximum = optionalParticleDynamic(
                    document, source, "phasemax", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .audioProcessingMode = optionalParticleDynamic(
                    document, source, "audioprocessingmode", pointer,
                    literal(0), DynamicValueParseMode::standard
                ),
                .audioProcessingBounds = optionalParticleDynamic(
                    document, source, "audioprocessingbounds", pointer,
                    literal(std::string("0 1")),
                    DynamicValueParseMode::standard
                ),
                .audioProcessingExponent = optionalParticleDynamic(
                    document, source, "audioprocessingexponent", pointer,
                    literal(1.0), DynamicValueParseMode::standard
                ),
                .audioProcessingFrequencyStart = optionalParticleDynamic(
                    document, source, "audioprocessingfrequencystart", pointer,
                    literal(0), DynamicValueParseMode::standard
                ),
                .audioProcessingFrequencyEnd = optionalParticleDynamic(
                    document, source, "audioprocessingfrequencyend", pointer,
                    literal(15), DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "vortex" || name == "vortex_v2") {
            return ParticleVortexOperator{
                .id = id,
                .controlPoint = optionalInt(
                    document, source, "controlpoint", pointer
                ).value_or(0),
                .flags = optionalUInt32(
                    document, source, "flags", pointer
                ).value_or(0),
                .axis = optionalParticleDynamic(
                    document, source, "axis", pointer,
                    literal(std::string("0 0 1")),
                    DynamicValueParseMode::standard
                ),
                .offset = optionalParticleDynamic(
                    document, source, "offset", pointer,
                    literal(std::string("0 0 0")),
                    DynamicValueParseMode::standard
                ),
                .distanceInner = optionalParticleDynamic(
                    document, source, "distanceinner", pointer, literal(500.0),
                    DynamicValueParseMode::standard
                ),
                .distanceOuter = optionalParticleDynamic(
                    document, source, "distanceouter", pointer, literal(650.0),
                    DynamicValueParseMode::standard
                ),
                .speedInner = optionalParticleDynamic(
                    document, source, "speedinner", pointer, literal(2500.0),
                    DynamicValueParseMode::standard
                ),
                .speedOuter = optionalParticleDynamic(
                    document, source, "speedouter", pointer, literal(0.0),
                    DynamicValueParseMode::standard
                ),
                .centerForce = optionalParticleDynamic(
                    document, source, "centerforce", pointer, literal(1.0),
                    DynamicValueParseMode::standard
                ),
                .ringRadius = optionalParticleDynamic(
                    document, source, "ringradius", pointer, literal(300.0),
                    DynamicValueParseMode::standard
                ),
                .ringWidth = optionalParticleDynamic(
                    document, source, "ringwidth", pointer, literal(50.0),
                    DynamicValueParseMode::standard
                ),
                .ringPullDistance = optionalParticleDynamic(
                    document, source, "ringpulldistance", pointer, literal(50.0),
                    DynamicValueParseMode::standard
                ),
                .ringPullForce = optionalParticleDynamic(
                    document, source, "ringpullforce", pointer, literal(10.0),
                    DynamicValueParseMode::standard
                ),
                .audioProcessingMode = optionalParticleDynamic(
                    document, source, "audioprocessingmode", pointer,
                    literal(0), DynamicValueParseMode::standard
                ),
                .audioProcessingBounds = optionalParticleDynamic(
                    document, source, "audioprocessingbounds", pointer,
                    literal(std::string("0 1")),
                    DynamicValueParseMode::standard
                ),
            };
        }
        if (name == "controlpointattract") {
            const int controlPoint = optionalInt(
                document, source, "controlpoint", pointer
            ).value_or(0);
            return ParticleControlPointAttractOperator{
                .id = id,
                .controlPoint = controlPoint,
                .origin = optionalParticleDynamic(
                    document, source, "origin", pointer,
                    literal(std::string("0 0 0")), DynamicValueParseMode::standard
                ),
                .scale = optionalParticleDynamic(
                    document, source, "scale", pointer, literal(100.0),
                    DynamicValueParseMode::standard
                ),
                .threshold = optionalParticleDynamic(
                    document, source, "threshold", pointer, literal(1000.0),
                    DynamicValueParseMode::standard
                ),
            };
        }
        return std::nullopt;
    }

    ParticleSpriteRenderer parseParticleRenderer(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Particle renderer");
        const std::string name = optionalString(
            document, source, "name", pointer, false
        ).value_or("sprite");
        ParticleSpriteRenderer result;
        result.id = optionalParticleId(document, source, pointer);
        result.name = name;
        result.length = optionalNumber(
            document, source, "length", pointer
        ).value_or(name == "ropetrail" ? 1.0 : 0.05);
        result.maxLength = optionalNumber(
            document, source, "maxlength", pointer
        ).value_or(10.0);
        result.minLength = optionalNumber(
            document, source, "minlength", pointer
        ).value_or(0.0);
        result.subdivision = optionalNumber(
            document, source, "subdivision", pointer
        ).value_or(name == "rope" ? 4.0 : 1.0);
        result.segments = optionalNumber(
            document, source, "segments", pointer
        ).value_or(4.0);
        result.uvScale = optionalNumber(
            document, source, "uvscale", pointer
        ).value_or(1.0);
        result.uvScrolling = optionalBool(
            document, source, "uvscrolling", pointer, false
        );
        result.uvSmoothing = optionalBool(
            document, source, "uvsmoothing", pointer, true
        );
        result.fadeAlpha = optionalBool(
            document, source, "fadealpha", pointer, false
        );
        result.fadeSize = optionalBool(
            document, source, "fadesize", pointer, false
        );
        // The pinned Linux renderer always uses screen-facing sprites and
        // does not consume authored orientation/axis metadata.
        return result;
    }

    ParticleChild parseParticleChild(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Particle child");

        ParticleChild result;
        result.particlePath = optionalString(
            document, source, "particle", pointer
        ).value_or("");
        result.type = optionalString(
            document, source, "type", pointer
        ).value_or("static");
        result.name = optionalString(
            document, source, "name", pointer
        ).value_or("");
        result.maxCount = optionalInt(
            document, source, "maxcount", pointer
        ).value_or(20);
        result.controlPointStartIndex = optionalInt(
            document, source, "controlpointstartindex", pointer
        ).value_or(0);
        result.probability = optionalNumber(
            document, source, "probability", pointer
        ).value_or(1.0);
        result.angles = optionalParticleVector3(
            document, source, "angles", pointer, {}
        );
        result.origin = optionalParticleVector3(
            document, source, "origin", pointer, {}
        );
        result.scale = optionalParticleVector3(
            document, source, "scale", pointer, {1.0, 1.0, 1.0}
        );
        return result;
    }

    std::shared_ptr<const ParticleDefinition> parseParticleDefinition(
        const Document& document,
        const Json& source,
        std::string_view pointer,
        std::string assetPath
    ) {
        requireObject(document, source, pointer, "Particle definition");

        auto result = std::make_shared<ParticleDefinition>();
        result->assetPath = std::move(assetPath);
        if (const Json* startTime = optionalField(source, "starttime");
            startTime != nullptr && !startTime->is_null()) {
            result->startTime = numberValue(
                document,
                *startTime,
                childPointer(pointer, "starttime"),
                "Particle start time"
            );
            if (!std::isfinite(result->startTime) || result->startTime < 0.0) {
                fail(
                    document,
                    childPointer(pointer, "starttime"),
                    SceneModelErrorCode::invalidValue,
                    "Particle start time must be finite and non-negative"
                );
            }
        }
        if (const Json* mode = optionalField(source, "animationmode");
            mode != nullptr && !mode->is_null()) {
            result->animationMode = stringValue(
                document,
                *mode,
                childPointer(pointer, "animationmode"),
                "Particle animation mode",
                false
            );
            if (result->animationMode != "sequence" &&
                result->animationMode != "once" &&
                result->animationMode != "randomframe") {
                result->animationMode = "sequence";
            }
        }
        if (const Json* multiplier = optionalField(source, "sequencemultiplier");
            multiplier != nullptr && !multiplier->is_null()) {
            result->sequenceMultiplier = numberValue(
                document,
                *multiplier,
                childPointer(pointer, "sequencemultiplier"),
                "Particle sequence multiplier"
            );
            if (result->sequenceMultiplier <= 0.0) {
                result->sequenceMultiplier = 1.0;
            }
        }

        if (const Json* maxCount = optionalField(source, "maxcount");
            maxCount != nullptr && !maxCount->is_null()) {
            result->maxCount = uint32Value(
                document,
                *maxCount,
                childPointer(pointer, "maxcount"),
                "Particle maximum count"
            );
            if (result->maxCount == 0) {
                result->maxCount = linuxDefaultParticlePoolSize;
            }
        }
        if (const Json* flags = optionalField(source, "flags");
            flags != nullptr && !flags->is_null()) {
            result->flags = uint32Value(
                document,
                *flags,
                childPointer(pointer, "flags"),
                "Particle flags"
            );
        }
        if (const Json* controlPoints = optionalField(source, "controlpoint");
            controlPoints != nullptr && !controlPoints->is_null()) {
            const std::string collectionPointer = childPointer(
                pointer, "controlpoint"
            );
            if (controlPoints->is_array()) {
                result->controlPoints.reserve(controlPoints->size());
            }
            for (std::size_t index = 0;
                 controlPoints->is_array() && index < controlPoints->size();
                 ++index) {
                const Json& sourceControlPoint = (*controlPoints)[index];
                const std::string controlPointPointer = childPointer(
                    collectionPointer, index
                );
                requireObject(
                    document, sourceControlPoint, controlPointPointer,
                    "Particle control point"
                );
                ParticleControlPoint controlPoint;
                controlPoint.id = optionalInt(
                    document, sourceControlPoint, "id", controlPointPointer
                ).value_or(-1);
                if (controlPoint.id < 0 ||
                    controlPoint.id >= particleControlPointSlotCount) {
                    continue;
                }
                if (const Json* controlPointFlags = optionalField(
                        sourceControlPoint, "flags"
                    ); controlPointFlags != nullptr && !controlPointFlags->is_null()) {
                    controlPoint.flags = uint32Value(
                        document,
                        *controlPointFlags,
                        childPointer(controlPointPointer, "flags"),
                        "Particle control point flags"
                    );
                }
                controlPoint.offset = optionalParticleControlPointOffset(
                    sourceControlPoint, "offset"
                );
                controlPoint.lockToPointer =
                    (controlPoint.flags & 1U) != 0U;
                result->controlPoints.push_back(std::move(controlPoint));
            }
        }

        const std::string materialPath = requiredString(
            document, source, "material", pointer, false
        );
        result->material = loadMaterial(
            materialPath,
            referenceChain(
                document,
                childPointer(pointer, "material"),
                materialPath
            )
        );

        if (const Json* emitters = optionalField(source, "emitter");
            emitters != nullptr && !emitters->is_null()) {
            const std::string collectionPointer = childPointer(pointer, "emitter");
            if (emitters->is_array()) {
                result->emitters.reserve(emitters->size());
            }
            for (std::size_t index = 0;
                 emitters->is_array() && index < emitters->size(); ++index) {
                auto emitter = parseParticleEmitter(
                    document,
                    (*emitters)[index],
                    childPointer(collectionPointer, index)
                );
                if (emitter) {
                    result->emitters.push_back(std::move(*emitter));
                }
            }
        }
        if (const Json* initializers = optionalField(source, "initializer");
            initializers != nullptr && !initializers->is_null()) {
            const std::string collectionPointer = childPointer(pointer, "initializer");
            if (initializers->is_array()) {
                result->initializers.reserve(initializers->size());
            }
            for (std::size_t index = 0;
                 initializers->is_array() && index < initializers->size();
                 ++index) {
                auto initializer = parseParticleInitializer(
                    document,
                    (*initializers)[index],
                    childPointer(collectionPointer, index)
                );
                if (initializer) {
                    result->initializers.push_back(std::move(*initializer));
                }
            }
        }
        if (const Json* operators = optionalField(source, "operator");
            operators != nullptr && !operators->is_null()) {
            const std::string collectionPointer = childPointer(pointer, "operator");
            if (operators->is_array()) {
                result->operators.reserve(operators->size());
            }
            for (std::size_t index = 0;
                 operators->is_array() && index < operators->size(); ++index) {
                auto particleOperator = parseParticleOperator(
                    document,
                    (*operators)[index],
                    childPointer(collectionPointer, index)
                );
                if (particleOperator) {
                    result->operators.push_back(std::move(*particleOperator));
                }
            }
        }
        if (const Json* children = optionalField(source, "children");
            children != nullptr && children->is_array()) {
            const std::string collectionPointer = childPointer(pointer, "children");
            requireArray(document, *children, collectionPointer, "Particle children");
            result->children.reserve(children->size());
            for (std::size_t index = 0; index < children->size(); ++index) {
                result->children.push_back(parseParticleChild(
                    document,
                    (*children)[index],
                    childPointer(collectionPointer, index)
                ));
            }
        }
        if (const Json* renderers = optionalField(source, "renderer");
            renderers != nullptr && !renderers->is_null()) {
            const std::string collectionPointer = childPointer(pointer, "renderer");
            if (renderers->is_array() && !renderers->empty()) {
                result->renderers.reserve(renderers->size());
                for (std::size_t index = 0; index < renderers->size(); ++index) {
                    result->renderers.push_back(parseParticleRenderer(
                        document,
                        (*renderers)[index],
                        childPointer(collectionPointer, index)
                    ));
                }
                result->renderer = result->renderers.front();
            }
        }
        if (result->renderers.empty()) {
            result->renderers.push_back(result->renderer);
        }
        return result;
    }

    std::shared_ptr<const ParticleDefinition> loadParticleDefinition(
        const std::string& path,
        std::vector<std::string> chain
    ) {
        if (const auto cached = particleCache_.find(path);
            cached != particleCache_.end()) {
            return cached->second;
        }
        if (!loadingParticles_.emplace(path).second) {
            throw SceneModelError(
                SceneModelErrorCode::referenceCycle,
                path,
                "",
                std::move(chain),
                "Particle definition reference cycle detected"
            );
        }
        try {
            Document document = loadDocument(path, std::move(chain));
            auto result = parseParticleDefinition(
                document, document.root, "", path
            );
            loadingParticles_.erase(path);
            particleCache_.emplace(path, result);
            return result;
        } catch (...) {
            loadingParticles_.erase(path);
            throw;
        }
    }

    ParticleInstanceOverride parseParticleInstanceOverride(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Particle instance override");
        return ParticleInstanceOverride{
            .id = optionalParticleId(document, source, pointer),
            .enabled = optionalParticleDynamic(
                document, source, "enabled", pointer, literal(true),
                DynamicValueParseMode::standard
            ),
            .alpha = optionalParticleDynamic(
                document, source, "alpha", pointer, literal(1.0),
                DynamicValueParseMode::standard
            ),
            .size = optionalParticleDynamic(
                document, source, "size", pointer, literal(1.0),
                DynamicValueParseMode::standard
            ),
            .lifetime = optionalParticleDynamic(
                document, source, "lifetime", pointer, literal(1.0),
                DynamicValueParseMode::standard
            ),
            .rate = optionalParticleDynamic(
                document, source, "rate", pointer, literal(1.0),
                DynamicValueParseMode::standard
            ),
            .speed = optionalParticleDynamic(
                document, source, "speed", pointer, literal(1.0),
                DynamicValueParseMode::standard
            ),
            .count = optionalParticleDynamic(
                document, source, "count", pointer, literal(1.0),
                DynamicValueParseMode::standard
            ),
            .color = optionalParticleDynamic(
                document, source, "color", pointer,
                literal(std::string("1 1 1")), DynamicValueParseMode::standard
            ),
            .colorMultiplier = optionalParticleDynamic(
                document, source, "colorn", pointer,
                literal(std::string("1 1 1")), DynamicValueParseMode::standard
            ),
        };
    }

    ParticleObject parseParticle(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) {
        const Json& definition = requiredField(
            document, source, "particle", pointer
        );
        ParticleObject result;
        result.parallaxDepth = optionalDynamic(
            document,
            source,
            "parallaxDepth",
            pointer,
            literal(std::string("0 0")),
            DynamicValueParseMode::standard
        );
        result.instanceOverride = ParticleInstanceOverride{
            .enabled = literal(false),
            .alpha = literal(1.0),
            .size = literal(1.0),
            .lifetime = literal(1.0),
            .rate = literal(1.0),
            .speed = literal(1.0),
            .count = literal(1.0),
            .color = literal(1.0),
            .colorMultiplier = literal(1.0),
        };
        if (const Json* instanceOverride = optionalField(source, "instanceoverride");
            instanceOverride != nullptr && !instanceOverride->is_null()) {
            result.instanceOverride = parseParticleInstanceOverride(
                document,
                *instanceOverride,
                childPointer(pointer, "instanceoverride")
            );
        }
        const std::string definitionPointer = childPointer(pointer, "particle");
        if (definition.is_string()) {
            const std::string path = stringValue(
                document,
                definition,
                definitionPointer,
                "Particle definition reference",
                false
            );
            result.definition = loadParticleDefinition(
                path,
                referenceChain(document, definitionPointer, path)
            );
        } else if (definition.is_object()) {
            result.definition = parseParticleDefinition(
                document,
                definition,
                definitionPointer,
                document.path + "#" + definitionPointer
            );
        } else {
            fail(
                document,
                definitionPointer,
                SceneModelErrorCode::typeMismatch,
                "Particle definition must be an inline object or asset path"
            );
        }
        return result;
    }

    SceneObject parseObject(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) {
        requireObject(document, source, pointer, "Scene object");
        SceneObject result;
        result.base.id = requiredInt(document, source, "id", pointer);
        const Json& objectName = requiredField(document, source, "name", pointer);
        if (objectName.is_string()) {
            result.base.name = stringValue(
                document, objectName, childPointer(pointer, "name"),
                "Object name"
            );
        } else if (objectName.is_number_integer() ||
                   objectName.is_number_unsigned()) {
            result.base.name = std::to_string(intValue(
                document, objectName, childPointer(pointer, "name"),
                "Object name"
            ));
        } else {
            fail(
                document,
                childPointer(pointer, "name"),
                SceneModelErrorCode::typeMismatch,
                "Object name must be a string or integer"
            );
        }
        result.base.parent = optionalInt(document, source, "parent", pointer);
        result.base.disablePropagation = optionalBool(
            document,
            source,
            "disablepropagation",
            pointer,
            false
        );
        result.base.origin = optionalDynamic(
            document,
            source,
            "origin",
            pointer,
            literal(std::string("0 0 0")),
            DynamicValueParseMode::standard
        );
        result.base.scale = optionalDynamic(
            document,
            source,
            "scale",
            pointer,
            literal(std::string("1 1 1")),
            DynamicValueParseMode::standard
        );
        result.base.angles = optionalDynamic(
            document,
            source,
            "angles",
            pointer,
            literal(std::string("0 0 0")),
            DynamicValueParseMode::standard
        );
        result.base.visible = optionalDynamic(
            document,
            source,
            "visible",
            pointer,
            literal(true),
            DynamicValueParseMode::standard
        );
        result.base.solid = optionalBool(
            document, source, "solid", pointer, false
        );
        result.base.lockTransforms = optionalBool(
            document, source, "locktransforms", pointer, false
        );

        if (const Json* dependencies = optionalField(source, "dependencies")) {
            const std::string dependenciesPointer = childPointer(
                pointer,
                "dependencies"
            );
            requireArray(
                document,
                *dependencies,
                dependenciesPointer,
                "Object dependencies"
            );
            result.base.dependencies.reserve(dependencies->size());
            for (std::size_t index = 0; index < dependencies->size(); ++index) {
                const Json& dependencySource = (*dependencies)[index];
                const std::string dependencyPointer = childPointer(
                    dependenciesPointer,
                    index
                );
                ObjectDependency dependency;
                if (dependencySource.is_object()) {
                    dependency.id = requiredInt(
                        document,
                        dependencySource,
                        "id",
                        dependencyPointer
                    );
                    dependency.index = optionalInt(
                        document,
                        dependencySource,
                        "index",
                        dependencyPointer
                    );
                    if (dependency.index && *dependency.index < 0) {
                        fail(
                            document,
                            childPointer(dependencyPointer, "index"),
                            SceneModelErrorCode::invalidValue,
                            "Object dependency index must not be negative"
                        );
                    }
                    dependency.type = optionalString(
                        document,
                        dependencySource,
                        "type",
                        dependencyPointer,
                        false
                    );
                } else {
                    dependency.id = intValue(
                        document,
                        dependencySource,
                        dependencyPointer,
                        "Object dependency"
                    );
                }
                result.base.dependencies.push_back(std::move(dependency));
            }
        }

        const Json* imageValue = optionalField(source, "image");
        const Json* textValue = optionalField(source, "text");
        const Json* soundValue = optionalField(source, "sound");
        const Json* particleValue = optionalField(source, "particle");
        const bool hasImage = imageValue != nullptr && imageValue->is_string();
        const bool hasText = textValue != nullptr;
        const bool hasSound = soundValue != nullptr && soundValue->is_array();
        const bool hasParticle = particleValue != nullptr;

        // Match upstream's deterministic discriminator precedence. Exported
        // scenes can retain stale fields from a previous object type.
        if (hasImage) {
            result.data = parseImage(document, source, pointer);
        } else if (hasSound) {
            result.data = parseSound(document, source, pointer);
        } else if (hasParticle) {
            result.data = parseParticle(document, source, pointer);
        } else if (hasText) {
            result.data = parseText(document, source, pointer);
        } else {
            result.data = GroupObject{};
        }
        return result;
    }

    ImageObject parseImage(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) {
        ImageObject result;
        const std::string modelPath = stringValue(
            document,
            requiredField(document, source, "image", pointer),
            childPointer(pointer, "image"),
            "Image model reference",
            false
        );
        result.model = loadModel(
            modelPath,
            referenceChain(document, childPointer(pointer, "image"), modelPath)
        );
        result.alpha = optionalDynamic(
            document,
            source,
            "alpha",
            pointer,
            literal(1.0),
            DynamicValueParseMode::standard
        );
        result.color = optionalDynamic(
            document,
            source,
            "color",
            pointer,
            colorLiteral({1.0, 1.0, 1.0, 1.0}),
            DynamicValueParseMode::color
        );
        result.size = optionalDynamic(
            document,
            source,
            "size",
            pointer,
            literal(std::string("0 0")),
            DynamicValueParseMode::standard
        );
        result.parallaxDepth = optionalDynamic(
            document,
            source,
            "parallaxDepth",
            pointer,
            literal(std::string("0 0")),
            DynamicValueParseMode::standard
        );
        result.brightness = optionalDynamic(
            document,
            source,
            "brightness",
            pointer,
            literal(1.0),
            DynamicValueParseMode::standard
        );
        result.colorBlendMode = optionalDynamic(
            document,
            source,
            "colorBlendMode",
            pointer,
            literal(std::int64_t(0)),
            DynamicValueParseMode::standard
        );
        if (result.colorBlendMode.value.number() > 0.0 ||
            result.colorBlendMode.isDynamic()) {
            result.colorBlendMaterial = loadMaterial(
                "materials/util/effectpassthrough.json",
                referenceChain(
                    document,
                    childPointer(pointer, "colorBlendMode"),
                    "materials/util/effectpassthrough.json"
                )
            );
        }
        result.horizontalAlignment = optionalString(
            document,
            source,
            "horizontalalign",
            pointer
        ).value_or(optionalString(
            document,
            source,
            "alignment",
            pointer
        ).value_or("center"));
        result.copyBackground = optionalBool(
            document,
            source,
            "copybackground",
            pointer,
            false
        );
        result.perspective = optionalBool(
            document,
            source,
            "perspective",
            pointer,
            false
        );
        result.ledSource = optionalBool(
            document,
            source,
            "ledsource",
            pointer,
            false
        );

        if (const Json* effects = optionalField(source, "effects")) {
            const std::string effectsPointer = childPointer(pointer, "effects");
            requireArray(document, *effects, effectsPointer, "Image effects");
            result.effects.reserve(effects->size());
            bool needsMagentaCompositeTintMaterial = false;
            for (std::size_t index = 0; index < effects->size(); ++index) {
                result.effects.push_back(parseImageEffect(
                    document,
                    (*effects)[index],
                    childPointer(effectsPointer, index)
                ));
                for (const EffectPassOverride& passOverride :
                     result.effects.back().passOverrides) {
                    const auto composite = passOverride.combos.find("COMPOSITE");
                    if (composite != passOverride.combos.end() &&
                        composite->second == 2 &&
                        passOverride.constants.contains("compositecolor")) {
                        needsMagentaCompositeTintMaterial = true;
                        break;
                    }
                }
            }
            if (needsMagentaCompositeTintMaterial) {
                result.magentaCompositeTintMaterial = loadMaterial(
                    "materials/effects/tint.json",
                    referenceChain(
                        document,
                        effectsPointer,
                        "materials/effects/tint.json"
                    )
                );
            }
        }

        if (const Json* instance = optionalField(source, "instance")) {
            const std::string instancePointer = childPointer(pointer, "instance");
            requireObject(document, *instance, instancePointer, "Image instance");
            if (const Json* textures = optionalField(*instance, "textures")) {
                result.instanceTextures = parseTextureSlots(
                    document,
                    *textures,
                    childPointer(instancePointer, "textures")
                );
            }
            if (const Json* textures = optionalField(*instance, "usertextures")) {
                result.instanceUserTextures = parseTextureSlots(
                    document,
                    *textures,
                    childPointer(instancePointer, "usertextures")
                );
            }
        }
        return result;
    }

    TextObject parseText(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        TextObject result;
        result.text = parseDynamic(
            document,
            requiredField(document, source, "text", pointer),
            childPointer(pointer, "text"),
            DynamicValueParseMode::standard
        );
        result.font = optionalString(document, source, "font", pointer)
                          .value_or("");
        result.pointSize = optionalDynamic(
            document,
            source,
            "pointsize",
            pointer,
            literal(32.0),
            DynamicValueParseMode::standard
        );
        result.size = optionalDynamic(
            document,
            source,
            "size",
            pointer,
            literal(std::string("0 0")),
            DynamicValueParseMode::standard
        );
        result.color = optionalDynamic(
            document,
            source,
            "color",
            pointer,
            colorLiteral({1.0, 1.0, 1.0, 1.0}),
            DynamicValueParseMode::color
        );
        result.alpha = optionalDynamic(
            document,
            source,
            "alpha",
            pointer,
            literal(1.0),
            DynamicValueParseMode::standard
        );
        result.padding = optionalDynamic(
            document,
            source,
            "padding",
            pointer,
            literal(std::string("0 0")),
            DynamicValueParseMode::standard
        );
        result.spacing = optionalDynamic(
            document,
            source,
            "spacing",
            pointer,
            literal(std::string("0 0")),
            DynamicValueParseMode::standard
        );
        result.horizontalAlignment = optionalString(
            document,
            source,
            "horizontalalign",
            pointer
        ).value_or(optionalString(
            document,
            source,
            "alignment",
            pointer
        ).value_or("center"));
        result.verticalAlignment = optionalString(
            document,
            source,
            "verticalalign",
            pointer
        ).value_or("center");
        result.perspective = optionalBool(
            document,
            source,
            "perspective",
            pointer,
            false
        );
        result.limitRows = optionalBool(
            document,
            source,
            "limitrows",
            pointer,
            false
        );
        result.limitUseEllipsis = optionalBool(
            document,
            source,
            "limituseellipsis",
            pointer,
            false
        );
        result.limitWidth = optionalBool(
            document,
            source,
            "limitwidth",
            pointer,
            false
        );
        result.maxRows = optionalInt(document, source, "maxrows", pointer)
                             .value_or(0);
        result.maxWidth = optionalNumber(document, source, "maxwidth", pointer)
                              .value_or(0.0);
        return result;
    }

    SoundObject parseSound(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        SoundObject result;
        const Json& sounds = requiredField(document, source, "sound", pointer);
        const std::string soundsPointer = childPointer(pointer, "sound");
        requireArray(document, sounds, soundsPointer, "Sound sources");
        if (sounds.empty()) {
            fail(
                document,
                soundsPointer,
                SceneModelErrorCode::invalidValue,
                "Sound sources must not be empty"
            );
        }
        result.sounds.reserve(sounds.size());
        for (std::size_t index = 0; index < sounds.size(); ++index) {
            result.sounds.push_back(stringValue(
                document,
                sounds[index],
                childPointer(soundsPointer, index),
                "Sound source",
                false
            ));
        }
        result.playbackMode = optionalString(
            document,
            source,
            "playbackmode",
            pointer
        );
        result.volume = optionalDynamic(
            document,
            source,
            "volume",
            pointer,
            literal(1.0),
            DynamicValueParseMode::standard
        );
        result.startSilent = optionalBool(
            document,
            source,
            "startsilent",
            pointer,
            false
        );
        result.muteInEditor = optionalBool(
            document,
            source,
            "muteineditor",
            pointer,
            false
        );
        result.minimumTime = optionalNumber(document, source, "mintime", pointer)
                                 .value_or(0.0);
        result.maximumTime = optionalNumber(document, source, "maxtime", pointer)
                                 .value_or(0.0);
        if (result.minimumTime > result.maximumTime) {
            fail(
                document,
                std::string(pointer),
                SceneModelErrorCode::invalidValue,
                "Sound minimum time exceeds maximum time"
            );
        }
        return result;
    }

    void validateObjectReferences(
        const Document& document,
        const std::vector<SceneObject>& objects,
        const std::unordered_map<int, std::size_t>& indices
    ) const {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const auto& object = objects[index];
            const std::string pointer = childPointer("/objects", index);
            if (object.base.parent && !indices.contains(*object.base.parent)) {
                fail(
                    document,
                    childPointer(pointer, "parent"),
                    SceneModelErrorCode::danglingReference,
                    "Object parent references unknown id " +
                        std::to_string(*object.base.parent)
                );
            }
            for (std::size_t dependencyIndex = 0;
                 dependencyIndex < object.base.dependencies.size();
                 ++dependencyIndex) {
                const int dependencyId =
                    object.base.dependencies[dependencyIndex].id;
                if (!indices.contains(dependencyId)) {
                    fail(
                        document,
                        childPointer(
                            childPointer(pointer, "dependencies"),
                            dependencyIndex
                        ),
                        SceneModelErrorCode::danglingReference,
                        "Object dependency references unknown id " +
                            std::to_string(dependencyId)
                    );
                }
            }
        }
    }

    std::shared_ptr<const Model> loadModel(
        const std::string& path,
        std::vector<std::string> chain
    ) {
        if (const auto cached = modelCache_.find(path);
            cached != modelCache_.end()) {
            return cached->second;
        }
        if (!loadingModels_.emplace(path).second) {
            throw SceneModelError(
                SceneModelErrorCode::referenceCycle,
                path,
                "",
                std::move(chain),
                "Model reference cycle detected"
            );
        }
        try {
            Document document = loadDocument(path, std::move(chain));
            requireObject(document, document.root, "", "Model root");
            auto result = std::make_shared<Model>();
            result->assetPath = path;
            const std::string materialPath = requiredString(
                document,
                document.root,
                "material",
                "",
                false
            );
            result->material = loadMaterial(
                materialPath,
                referenceChain(document, "/material", materialPath)
            );
            result->solidLayer = optionalBool(
                document,
                document.root,
                "solidlayer",
                "",
                false
            );
            result->fullscreen = optionalBool(
                document,
                document.root,
                "fullscreen",
                "",
                false
            );
            result->passthrough = optionalBool(
                document,
                document.root,
                "passthrough",
                "",
                false
            );
            result->projectLayer = optionalBool(
                document,
                document.root,
                "projectlayer",
                "",
                false
            );
            result->autoSize = optionalBool(
                document,
                document.root,
                "autosize",
                "",
                false
            );
            result->noPadding = optionalBool(
                document,
                document.root,
                "nopadding",
                "",
                false
            );
            result->width = optionalInt(document, document.root, "width", "");
            result->height = optionalInt(document, document.root, "height", "");
            if (const Json* puppet = optionalField(document.root, "puppet")) {
                const std::string puppetPath = stringValue(
                    document,
                    *puppet,
                    "/puppet",
                    "Puppet model path",
                    false
                );
                const std::vector<std::string> puppetChain = referenceChain(
                    document,
                    "/puppet",
                    puppetPath
                );
                try {
                    const ResolvedAsset asset = resolver_.resolve(puppetPath);
                    result->puppetMesh = std::make_shared<const PuppetMesh>(
                        PuppetMeshParser::parse(asset.bytes, puppetPath)
                    );
                } catch (const FormatError& error) {
                    const SceneModelErrorCode code =
                        error.code() == FormatErrorCode::assetNotFound
                            ? SceneModelErrorCode::danglingReference
                            : SceneModelErrorCode::assetFailure;
                    throw SceneModelError(
                        code,
                        puppetPath,
                        "/puppet",
                        puppetChain,
                        error.what()
                    );
                }
            }
            if (const Json* cropOffset = optionalField(document.root, "cropoffset")) {
                result->cropOffset = parseDynamic(
                    document,
                    *cropOffset,
                    "/cropoffset",
                    DynamicValueParseMode::standard
                );
            }
            if ((result->width && *result->width <= 0) ||
                (result->height && *result->height <= 0)) {
                fail(
                    document,
                    "",
                    SceneModelErrorCode::invalidValue,
                    "Model dimensions must be greater than zero"
                );
            }
            loadingModels_.erase(path);
            modelCache_.emplace(path, result);
            return result;
        } catch (...) {
            loadingModels_.erase(path);
            throw;
        }
    }

    std::shared_ptr<const Material> loadMaterial(
        const std::string& path,
        std::vector<std::string> chain
    ) {
        if (const auto cached = materialCache_.find(path);
            cached != materialCache_.end()) {
            return cached->second;
        }
        if (!loadingMaterials_.emplace(path).second) {
            throw SceneModelError(
                SceneModelErrorCode::referenceCycle,
                path,
                "",
                std::move(chain),
                "Material reference cycle detected"
            );
        }
        try {
            Document document = loadDocument(path, std::move(chain));
            requireObject(document, document.root, "", "Material root");
            const Json& passes = requiredField(
                document,
                document.root,
                "passes",
                ""
            );
            requireArray(document, passes, "/passes", "Material passes");
            if (passes.empty()) {
                fail(
                    document,
                    "/passes",
                    SceneModelErrorCode::invalidValue,
                    "Material passes must not be empty"
                );
            }
            auto result = std::make_shared<Material>();
            result->assetPath = path;
            result->passes.reserve(passes.size());
            for (std::size_t index = 0; index < passes.size(); ++index) {
                result->passes.push_back(parseMaterialPass(
                    document,
                    passes[index],
                    childPointer("/passes", index)
                ));
            }
            loadingMaterials_.erase(path);
            materialCache_.emplace(path, result);
            return result;
        } catch (...) {
            loadingMaterials_.erase(path);
            throw;
        }
    }

    MaterialPass parseMaterialPass(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Material pass");
        MaterialPass result;
        result.blending = parseBlending(
            document,
            optionalString(document, source, "blending", pointer)
                .value_or("normal"),
            childPointer(pointer, "blending")
        );
        result.culling = parseCulling(
            document,
            optionalString(document, source, "cullmode", pointer)
                .value_or("nocull"),
            childPointer(pointer, "cullmode")
        );
        result.depthTest = parseDepth(
            document,
            optionalString(document, source, "depthtest", pointer)
                .value_or("disabled"),
            childPointer(pointer, "depthtest"),
            "depth test"
        );
        result.depthWrite = parseDepth(
            document,
            optionalString(document, source, "depthwrite", pointer)
                .value_or("disabled"),
            childPointer(pointer, "depthwrite"),
            "depth write"
        );
        result.shader = requiredString(
            document,
            source,
            "shader",
            pointer,
            false
        );
        if (const Json* textures = optionalField(source, "textures")) {
            result.textures = parseTextureSlots(
                document,
                *textures,
                childPointer(pointer, "textures")
            );
        }
        if (const Json* textures = optionalField(source, "usertextures")) {
            result.userTextures = parseTextureSlots(
                document,
                *textures,
                childPointer(pointer, "usertextures")
            );
        }
        if (const Json* combos = optionalField(source, "combos")) {
            result.combos = parseCombos(
                document,
                *combos,
                childPointer(pointer, "combos")
            );
        }
        if (const Json* constants = optionalField(
                source,
                "constantshadervalues"
            )) {
            result.constants = parseConstants(
                document,
                *constants,
                childPointer(pointer, "constantshadervalues")
            );
        }
        return result;
    }

    BlendingMode parseBlending(
        const Document& document,
        std::string_view value,
        std::string pointer
    ) const {
        if (value == "normal") {
            return BlendingMode::normal;
        }
        if (value == "translucent") {
            return BlendingMode::translucent;
        }
        if (value == "additive") {
            return BlendingMode::additive;
        }
        fail(
            document,
            std::move(pointer),
            SceneModelErrorCode::invalidValue,
            "Unknown blending mode '" + std::string(value) + "'"
        );
    }

    CullingMode parseCulling(
        const Document& document,
        std::string_view value,
        std::string pointer
    ) const {
        if (value == "normal") {
            return CullingMode::normal;
        }
        if (value == "nocull") {
            return CullingMode::disabled;
        }
        fail(
            document,
            std::move(pointer),
            SceneModelErrorCode::invalidValue,
            "Unknown culling mode '" + std::string(value) + "'"
        );
    }

    DepthMode parseDepth(
        const Document& document,
        std::string_view value,
        std::string pointer,
        std::string_view description
    ) const {
        if (value == "disabled") {
            return DepthMode::disabled;
        }
        if (value == "enabled") {
            return DepthMode::enabled;
        }
        fail(
            document,
            std::move(pointer),
            SceneModelErrorCode::invalidValue,
            "Unknown " + std::string(description) + " mode '" +
                std::string(value) + "'"
        );
    }

    TextureSlots parseTextureSlots(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireArray(document, source, pointer, "Texture slots");
        TextureSlots result;
        result.reserve(source.size());
        for (std::size_t index = 0; index < source.size(); ++index) {
            const Json& slot = source[index];
            if (slot.is_null()) {
                result.push_back(TextureSlot{});
            } else if (slot.is_object()) {
                const std::string slotPointer = childPointer(pointer, index);
                const std::string name = requiredString(
                    document,
                    slot,
                    "name",
                    slotPointer,
                    false
                );
                Value parsed = parseValue(document, slot, slotPointer);
                result.push_back(TextureSlot{
                    .name = name,
                    .metadata = std::get<Value::Object>(std::move(parsed.storage)),
                });
            } else {
                result.push_back(TextureSlot{
                    .name = stringValue(
                        document,
                        slot,
                        childPointer(pointer, index),
                        "Texture slot",
                        false
                    ),
                });
            }
        }
        return result;
    }

    ComboMap parseCombos(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Shader combos");
        ComboMap result;
        for (const auto& [key, value] : source.items()) {
            result.emplace(
                key,
                intValue(
                    document,
                    value,
                    childPointer(pointer, key),
                    "Shader combo"
                )
            );
        }
        return result;
    }

    ConstantMap parseConstants(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Shader constants");
        ConstantMap result;
        for (const auto& [key, value] : source.items()) {
            result.emplace(
                key,
                parseDynamic(document, value, childPointer(pointer, key))
            );
        }
        return result;
    }

    ImageEffect parseImageEffect(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) {
        requireObject(document, source, pointer, "Image effect");
        ImageEffect result;
        result.id = optionalInt(document, source, "id", pointer);
        result.name = optionalString(document, source, "name", pointer)
                          .value_or("");
        result.visible = optionalDynamic(
            document,
            source,
            "visible",
            pointer,
            literal(true),
            DynamicValueParseMode::standard
        );
        const std::string effectPath = requiredString(
            document,
            source,
            "file",
            pointer,
            false
        );
        result.effect = loadEffect(
            effectPath,
            referenceChain(
                document,
                childPointer(pointer, "file"),
                effectPath
            )
        );
        if (const Json* passes = optionalField(source, "passes")) {
            const std::string passesPointer = childPointer(pointer, "passes");
            requireArray(document, *passes, passesPointer, "Effect pass overrides");
            if (passes->size() > result.effect->passes.size()) {
                fail(
                    document,
                    passesPointer,
                    SceneModelErrorCode::danglingReference,
                    "Effect has more pass overrides than its definition"
                );
            }
            result.passOverrides.reserve(passes->size());
            for (std::size_t index = 0; index < passes->size(); ++index) {
                result.passOverrides.push_back(parseEffectOverride(
                    document,
                    (*passes)[index],
                    childPointer(passesPointer, index)
                ));
            }
        }
        return result;
    }

    EffectPassOverride parseEffectOverride(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        requireObject(document, source, pointer, "Effect pass override");
        EffectPassOverride result;
        result.id = optionalInt(document, source, "id", pointer);
        if (const Json* combos = optionalField(source, "combos")) {
            result.combos = parseCombos(
                document,
                *combos,
                childPointer(pointer, "combos")
            );
        }
        if (const Json* constants = optionalField(
                source,
                "constantshadervalues"
            )) {
            result.constants = parseConstants(
                document,
                *constants,
                childPointer(pointer, "constantshadervalues")
            );
        }
        if (const Json* textures = optionalField(source, "textures")) {
            result.textures = parseTextureSlots(
                document,
                *textures,
                childPointer(pointer, "textures")
            );
        }
        if (const Json* textures = optionalField(source, "usertextures")) {
            result.userTextures = parseTextureSlots(
                document,
                *textures,
                childPointer(pointer, "usertextures")
            );
        }
        return result;
    }

    std::shared_ptr<const Effect> loadEffect(
        const std::string& path,
        std::vector<std::string> chain
    ) {
        if (const auto cached = effectCache_.find(path);
            cached != effectCache_.end()) {
            return cached->second;
        }
        if (!loadingEffects_.emplace(path).second) {
            throw SceneModelError(
                SceneModelErrorCode::referenceCycle,
                path,
                "",
                std::move(chain),
                "Effect reference cycle detected"
            );
        }
        try {
            Document document = loadDocument(path, std::move(chain));
            requireObject(document, document.root, "", "Effect root");
            auto result = std::make_shared<Effect>();
            result->assetPath = path;
            result->name = optionalString(document, document.root, "name", "")
                               .value_or("");
            result->description = optionalString(
                                      document,
                                      document.root,
                                      "description",
                                      ""
                                  )
                                      .value_or("");
            result->group = optionalString(document, document.root, "group", "")
                                .value_or("");
            result->preview = optionalString(
                                  document,
                                  document.root,
                                  "preview",
                                  ""
                              )
                                  .value_or("");
            if (const Json* dependencies = optionalField(
                    document.root,
                    "dependencies"
                )) {
                for (std::size_t index = 0;
                     dependencies->is_array() && index < dependencies->size();
                     ++index) {
                    result->dependencies.push_back(stringValue(
                        document,
                        (*dependencies)[index],
                        childPointer("/dependencies", index),
                        "Effect dependency",
                        false
                    ));
                }
            }
            if (const Json* framebuffers = optionalField(document.root, "fbos")) {
                result->framebuffers = parseFramebuffers(
                    document,
                    *framebuffers,
                    "/fbos"
                );
            }
            const Json& passes = requiredField(
                document,
                document.root,
                "passes",
                ""
            );
            if (passes.is_array()) {
                result->passes.reserve(passes.size());
            }
            for (std::size_t index = 0;
                 passes.is_array() && index < passes.size(); ++index) {
                result->passes.push_back(parseEffectPass(
                    document,
                    passes[index],
                    childPointer("/passes", index)
                ));
            }
            loadingEffects_.erase(path);
            effectCache_.emplace(path, result);
            return result;
        } catch (...) {
            loadingEffects_.erase(path);
            throw;
        }
    }

    std::vector<FramebufferDefinition> parseFramebuffers(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) const {
        std::vector<FramebufferDefinition> result;
        if (source.is_array()) {
            result.reserve(source.size());
        }
        for (std::size_t index = 0;
             source.is_array() && index < source.size(); ++index) {
            const Json& value = source[index];
            const std::string itemPointer = childPointer(pointer, index);
            requireObject(document, value, itemPointer, "Effect framebuffer");
            FramebufferDefinition framebuffer;
            framebuffer.name = requiredString(
                document,
                value,
                "name",
                itemPointer,
                false
            );
            framebuffer.format = optionalString(
                                     document,
                                     value,
                                     "format",
                                     itemPointer,
                                     false
                                 )
                                     .value_or("rgba8888");
            framebuffer.scale = optionalNumber(
                                    document,
                                    value,
                                    "scale",
                                    itemPointer
                                )
                                    .value_or(1.0);
            framebuffer.unique = optionalBool(
                document,
                value,
                "unique",
                itemPointer,
                false
            );
            framebuffer.fit = optionalInt(
                document,
                value,
                "fit",
                itemPointer
            );
            framebuffer.width = optionalInt(
                document,
                value,
                "width",
                itemPointer
            );
            framebuffer.height = optionalInt(
                document,
                value,
                "height",
                itemPointer
            );
            framebuffer.uvs = optionalString(
                document,
                value,
                "uvs",
                itemPointer,
                false
            );
            result.push_back(std::move(framebuffer));
        }
        return result;
    }

    EffectPass parseEffectPass(
        const Document& document,
        const Json& source,
        std::string_view pointer
    ) {
        requireObject(document, source, pointer, "Effect pass");
        EffectPass result;
        if (const Json* material = optionalField(source, "material")) {
            const std::string path = stringValue(
                document,
                *material,
                childPointer(pointer, "material"),
                "Effect material reference",
                false
            );
            result.material = loadMaterial(
                path,
                referenceChain(
                    document,
                    childPointer(pointer, "material"),
                    path
                )
            );
        }
        if (const Json* command = optionalField(source, "command")) {
            const std::string name = stringValue(
                document,
                *command,
                childPointer(pointer, "command"),
                "Effect command",
                false
            );
            if (name == "copy") {
                result.command = EffectCommand::copy;
            } else {
                result.command = EffectCommand::swap;
            }
            result.source = requiredString(
                document,
                source,
                "source",
                pointer,
                false
            );
            result.target = requiredString(
                document,
                source,
                "target",
                pointer,
                false
            );
        } else {
            result.source = optionalString(
                document,
                source,
                "source",
                pointer,
                false
            );
            result.target = optionalString(
                document,
                source,
                "target",
                pointer,
                false
            );
        }
        result.compose = optionalBool(
            document,
            source,
            "compose",
            pointer,
            false
        );
        if (const Json* binds = optionalField(source, "bind")) {
            const std::string bindsPointer = childPointer(pointer, "bind");
            for (std::size_t index = 0;
                 binds->is_array() && index < binds->size(); ++index) {
                const Json& bind = (*binds)[index];
                const std::string bindPointer = childPointer(bindsPointer, index);
                requireObject(document, bind, bindPointer, "Effect bind");
                EffectBind parsed{
                    .index = requiredInt(document, bind, "index", bindPointer),
                    .name = requiredString(
                        document,
                        bind,
                        "name",
                        bindPointer,
                        false
                    ),
                };
                result.binds.push_back(std::move(parsed));
            }
        }
        // Upstream stores bind/source/target names verbatim. Resolution is a
        // render-planning concern because copy sources may also be assets and
        // an invalid pass must not prevent the rest of the scene from loading.
        return result;
    }

    const AssetResolver& resolver_;
    const std::map<std::string, ProjectProperty>* properties_ = nullptr;
    std::unordered_map<std::string, std::shared_ptr<const Model>> modelCache_;
    std::unordered_map<std::string, std::shared_ptr<const Material>> materialCache_;
    std::unordered_map<std::string, std::shared_ptr<const Effect>> effectCache_;
    std::unordered_map<
        std::string,
        std::shared_ptr<const ParticleDefinition>
    > particleCache_;
    std::unordered_set<std::string> loadingModels_;
    std::unordered_set<std::string> loadingMaterials_;
    std::unordered_set<std::string> loadingEffects_;
    std::unordered_set<std::string> loadingParticles_;
};

}  // namespace

SceneProject SceneModelLoader::load(
    const AssetResolver& resolver,
    std::string_view projectPath
) {
    return Parser(resolver).parse(projectPath);
}

}  // namespace we::scene
