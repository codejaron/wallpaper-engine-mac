#include <SceneShader/ShaderPreprocessor.hpp>

#include <SceneCore/FormatError.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace we::scene {
namespace {

constexpr std::size_t maximumIncludeDepth = 32;
constexpr std::size_t maximumExpandedSourceBytes = 32 * 1024 * 1024;

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string upper(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<char>(std::toupper(character)));
    }
    return result;
}

[[noreturn]] void preprocessError(
    std::string_view source,
    std::string message
) {
    throw FormatError(
        FormatErrorCode::malformedMetadata,
        std::string(source),
        FormatError::noOffset,
        std::move(message)
    );
}

struct LoadedSource {
    std::string name;
    std::string contents;
};

class SourceExpander final {
public:
    SourceExpander(const AssetResolver& resolver, std::string rootName)
        : resolver_(resolver), rootName_(std::move(rootName)) {}

    [[nodiscard]] std::string expand(
        std::string_view source,
        std::string_view sourceName
    ) {
        stack_.clear();
        return expandImpl(source, sourceName, 0);
    }

private:
    [[nodiscard]] LoadedSource loadInclude(
        std::string_view includeName,
        std::string_view currentName
    ) const {
        const std::filesystem::path currentPath(currentName);
        const std::filesystem::path currentDirectory = currentPath.parent_path();
        std::vector<std::string> candidates;
        const std::string include(includeName);
        auto addCandidate = [&candidates](const std::filesystem::path& path) {
            const std::string value = path.generic_string();
            if (value.empty() ||
                std::find(candidates.begin(), candidates.end(), value) !=
                    candidates.end()) {
                return;
            }
            candidates.push_back(value);
        };

        if (!currentDirectory.empty()) {
            addCandidate(currentDirectory / include);
        }
        if (include.starts_with("shaders/")) {
            addCandidate(include);
        } else {
            addCandidate(std::filesystem::path("shaders") / include);
            addCandidate(include);
        }

        std::string lastError;
        for (const std::string& candidate : candidates) {
            try {
                return {
                    candidate,
                    resolver_.readString(candidate),
                };
            } catch (const FormatError& error) {
                if (error.code() != FormatErrorCode::assetNotFound) {
                    throw;
                }
                lastError = error.what();
            }
        }

        preprocessError(
            currentName,
            "Shader include '" + include + "' was not found" +
                (lastError.empty() ? std::string() : ": " + lastError)
        );
    }

    [[nodiscard]] std::string expandImpl(
        std::string_view source,
        std::string_view sourceName,
        std::size_t depth
    ) {
        if (depth > maximumIncludeDepth) {
            preprocessError(
                sourceName,
                "Shader include depth exceeds " +
                    std::to_string(maximumIncludeDepth)
            );
        }
        const std::string logicalName(sourceName);
        if (std::find(stack_.begin(), stack_.end(), logicalName) != stack_.end()) {
            std::string chain;
            for (const std::string& item : stack_) {
                if (!chain.empty()) {
                    chain += " -> ";
                }
                chain += item;
            }
            chain += " -> " + logicalName;
            preprocessError(sourceName, "Shader include cycle: " + chain);
        }

        stack_.push_back(logicalName);
        std::istringstream lines{std::string(source)};
        std::string result;
        std::string line;
        while (std::getline(lines, line)) {
            const std::string leading = trim(line);
            if (!leading.starts_with("#include")) {
                result += line;
                result.push_back('\n');
                continue;
            }

            const std::string rest = trim(
                std::string_view(leading).substr(std::string("#include").size())
            );
            if (rest.size() < 2 || rest.front() != '"') {
                preprocessError(sourceName, "Malformed #include directive");
            }
            const std::size_t closingQuote = rest.find('"', 1);
            if (closingQuote == std::string::npos ||
                !trim(std::string_view(rest).substr(closingQuote + 1)).empty()) {
                preprocessError(sourceName, "Malformed #include directive");
            }
            const LoadedSource included = loadInclude(
                std::string_view(rest).substr(1, closingQuote - 1),
                sourceName
            );
            result += "// begin include \"" + included.name + "\"\n";
            result += expandImpl(included.contents, included.name, depth + 1);
            result += "// end include \"" + included.name + "\"\n";
            if (result.size() > maximumExpandedSourceBytes) {
                preprocessError(
                    sourceName,
                    "Expanded shader source exceeds the 32 MiB limit"
                );
            }
        }
        stack_.pop_back();
        return result;
    }

    const AssetResolver& resolver_;
    std::string rootName_;
    std::vector<std::string> stack_;
};

constexpr int maximumLightingV1LightCount = 4;

int lightingComboValue(
    const std::map<std::string, int>& combos,
    std::string_view name
) {
    const auto found = combos.find(std::string(name));
    return found == combos.end() ? 0 : found->second;
}

int lightingV1LightCount(
    const std::map<std::string, int>& combos,
    std::string_view name,
    std::string_view sourceName
) {
    const int value = lightingComboValue(combos, name);
    if (value < 0 || value > maximumLightingV1LightCount) {
        preprocessError(
            sourceName,
            "LightingV1 combo '" + std::string(name) +
                "' must be between 0 and " +
                std::to_string(maximumLightingV1LightCount)
        );
    }
    return value;
}

std::string lightingV1Module(
    const std::map<std::string, int>& combos,
    std::string_view sourceName
) {
    if (lightingComboValue(combos, "LIGHTING") == 0) return {};

    const int point = lightingV1LightCount(
        combos, "LIGHTS_POINT", sourceName
    );
    const int spot = lightingV1LightCount(
        combos, "LIGHTS_SPOT", sourceName
    );
    const int tube = lightingV1LightCount(
        combos, "LIGHTS_TUBE", sourceName
    );
    const int directional = lightingV1LightCount(
        combos, "LIGHTS_DIRECTIONAL", sourceName
    );
    const int pointShadow = lightingV1LightCount(
        combos, "LIGHTS_POINT_SHADOW", sourceName
    );
    const int spotShadowCookie = lightingV1LightCount(
        combos, "LIGHTS_SPOT_SHADOW_COOKIE", sourceName
    );
    const int spotCookie = lightingV1LightCount(
        combos, "LIGHTS_SPOT_COOKIE", sourceName
    );
    const int spotShadow = lightingV1LightCount(
        combos, "LIGHTS_SPOT_SHADOW", sourceName
    );
    const int directionalShadow = lightingV1LightCount(
        combos, "LIGHTS_DIRECTIONAL_SHADOW", sourceName
    );
    const int spotFeatureCount =
        spotShadowCookie + spotCookie + spotShadow;
    if (pointShadow > point || spotFeatureCount > spot ||
        directionalShadow > directional) {
        preprocessError(
            sourceName,
            "LightingV1 shadow/cookie counts exceed their light totals"
        );
    }
    // The official generator advances the directional projection base by one
    // while consuming three consecutive cascades. Its ABI therefore only
    // permits one shadow-casting directional light.
    if (directionalShadow > 1) {
        preprocessError(
            sourceName,
            "LightingV1 supports at most one shadow-casting directional light"
        );
    }

    std::ostringstream result;
    const auto uniformArray = [&result](
        std::string_view type,
        std::string_view name,
        int count
    ) {
        if (count == 0) return;
        result << "uniform " << type << ' ' << name << '[' << count
               << "];\n";
    };
    uniformArray("vec4", "g_LPoint_Color", point);
    uniformArray("vec4", "g_LPoint_Origin", point);
    uniformArray("vec4", "g_LSpot_Color", spot);
    uniformArray("vec4", "g_LSpot_Origin", spot);
    uniformArray("vec4", "g_LSpot_Direction", spot);
    uniformArray("vec4", "g_LSpot_Exponent", spot);
    uniformArray("vec4", "g_LTube_Color", tube);
    uniformArray("vec4", "g_LTube_OriginA", tube);
    uniformArray("vec4", "g_LTube_OriginB", tube);
    uniformArray("vec4", "g_LDirectional_Color", directional);
    uniformArray("vec4", "g_LDirectional_Direction", directional);
    uniformArray(
        "mat4", "g_LFeature_ShadowProjection",
        spotFeatureCount + directionalShadow * 3
    );
    uniformArray(
        "vec4", "g_LFeature_ShadowProjectionTransform",
        spotFeatureCount + directionalShadow * 3
    );
    uniformArray(
        "vec4", "g_LFeature_ShadowPointProjection", pointShadow
    );
    uniformArray(
        "vec4", "g_LFeature_ShadowPointProjectionTransform", pointShadow
    );

    result <<
        "vec3 PerformLighting_V1(vec3 worldPos, vec3 color, vec3 normal, "
        "vec3 viewVector, vec3 specularTint, vec3 ambient, float roughness, "
        "float metallic)\n"
        "{\n"
        "\tvec3 light = CAST3(0.0);\n";
    const auto lightBlock = [&result](int index, std::string_view body) {
        result << "\t{\n\t\tconst uint i = " << index << "u;\n"
               << body << "\t}\n";
    };
    for (int index = 0; index < pointShadow; ++index) {
        lightBlock(
            index,
            "\t\tvec3 lightDelta = g_LPoint_Origin[i].xyz - worldPos;\n"
            "\t\tvec4 projectedCoords = CalculateProjectedCoordsPoint("
            "worldPos, g_LPoint_Origin[i].xyz, "
            "g_LFeature_ShadowPointProjection[i], "
            "g_LFeature_ShadowPointProjectionTransform[i]);\n"
            "\t\tfloat shadowFactor = PerformPointShadowMapping("
            "projectedCoords);\n"
            "\t\tlight += ComputePBRLightShadow(normal, lightDelta, "
            "viewVector, color, g_LPoint_Color[i].rgb, "
            "g_LPoint_Color[i].w, g_LPoint_Origin[i].w, specularTint, "
            "ambient, roughness, metallic, shadowFactor);\n"
        );
    }
    for (int index = pointShadow; index < point; ++index) {
        lightBlock(
            index,
            "\t\tvec3 lightDelta = g_LPoint_Origin[i].xyz - worldPos;\n"
            "\t\tlight += ComputePBRLightShadow(normal, lightDelta, "
            "viewVector, color, g_LPoint_Color[i].rgb, "
            "g_LPoint_Color[i].w, g_LPoint_Origin[i].w, specularTint, "
            "ambient, roughness, metallic, 1.0);\n"
        );
    }
    for (int index = 0; index < spotShadowCookie; ++index) {
        lightBlock(
            index,
            "\t\tvec3 lightDelta = g_LSpot_Origin[i].xyz - worldPos;\n"
            "\t\tvec3 projectedCoords = CalculateProjectedCoords("
            "worldPos, g_LFeature_ShadowProjection[i]);\n"
            "\t\tfloat shadowFactor = PerformShadowMapping("
            "projectedCoords, g_LFeature_ShadowProjectionTransform[i]);\n"
            "\t\tvec3 cookieColor = texSample2D(COOKIE_SAMPLER, "
            "projectedCoords.xy).rgb;\n"
            "\t\tlight += ComputePBRLightShadow(normal, lightDelta, "
            "viewVector, color, g_LSpot_Color[i].rgb * cookieColor, "
            "g_LSpot_Color[i].w, g_LSpot_Exponent[i].x, specularTint, "
            "ambient, roughness, metallic, shadowFactor);\n"
        );
    }
    for (int index = spotShadowCookie;
         index < spotShadowCookie + spotCookie; ++index) {
        lightBlock(
            index,
            "\t\tvec3 lightDelta = g_LSpot_Origin[i].xyz - worldPos;\n"
            "\t\tvec3 projectedCoords = CalculateProjectedCoords("
            "worldPos, g_LFeature_ShadowProjection[i]);\n"
            "\t\tvec3 cookieColor = texSample2D(COOKIE_SAMPLER, "
            "projectedCoords.xy).rgb;\n"
            "\t\tlight += ComputePBRLightShadow(normal, lightDelta, "
            "viewVector, color, g_LSpot_Color[i].rgb * cookieColor, "
            "g_LSpot_Color[i].w, g_LSpot_Exponent[i].x, specularTint, "
            "ambient, roughness, metallic, 1.0);\n"
        );
    }
    for (int index = spotShadowCookie + spotCookie;
         index < spotFeatureCount; ++index) {
        lightBlock(
            index,
            "\t\tvec3 lightDelta = g_LSpot_Origin[i].xyz - worldPos;\n"
            "\t\tfloat spotCookie = -dot(normalize(lightDelta), "
            "g_LSpot_Direction[i].xyz);\n"
            "\t\tspotCookie = smoothstep(g_LSpot_Direction[i].w, "
            "g_LSpot_Origin[i].w, spotCookie);\n"
            "\t\tvec3 projectedCoords = CalculateProjectedCoords("
            "worldPos, g_LFeature_ShadowProjection[i]);\n"
            "\t\tfloat shadowFactor = PerformShadowMapping("
            "projectedCoords, g_LFeature_ShadowProjectionTransform[i]);\n"
            "\t\tlight += ComputePBRLightShadow(normal, lightDelta, "
            "viewVector, color, g_LSpot_Color[i].rgb * spotCookie, "
            "g_LSpot_Color[i].w, g_LSpot_Exponent[i].x, specularTint, "
            "ambient, roughness, metallic, shadowFactor);\n"
        );
    }
    for (int index = spotFeatureCount; index < spot; ++index) {
        lightBlock(
            index,
            "\t\tvec3 lightDelta = g_LSpot_Origin[i].xyz - worldPos;\n"
            "\t\tfloat spotCookie = -dot(normalize(lightDelta), "
            "g_LSpot_Direction[i].xyz);\n"
            "\t\tspotCookie = smoothstep(g_LSpot_Direction[i].w, "
            "g_LSpot_Origin[i].w, spotCookie);\n"
            "\t\tlight += ComputePBRLightShadow(normal, lightDelta, "
            "viewVector, color, g_LSpot_Color[i].rgb * spotCookie, "
            "g_LSpot_Color[i].w, g_LSpot_Exponent[i].x, specularTint, "
            "ambient, roughness, metallic, 1.0);\n"
        );
    }
    for (int index = 0; index < tube; ++index) {
        lightBlock(
            index,
            "\t\tvec3 lightDelta = PointSegmentDelta(worldPos, "
            "g_LTube_OriginA[i].xyz, g_LTube_OriginB[i].xyz);\n"
            "\t\tlight += ComputePBRLightShadow(normal, lightDelta, "
            "viewVector, color, g_LTube_Color[i].rgb, "
            "g_LTube_Color[i].w, g_LTube_OriginA[i].w, specularTint, "
            "ambient, roughness, metallic, 1.0);\n"
        );
    }
    for (int index = 0; index < directionalShadow; ++index) {
        const int projection = spotFeatureCount + index;
        result << "\t{\n\t\tconst uint i = " << index << "u;\n"
               << "\t\tvec4 projectedCoords1 = "
                  "CalculateProjectedCoordsCascades(worldPos, "
                  "g_LFeature_ShadowProjection[" << projection << "]);\n"
               << "\t\tvec4 projectedCoords2 = "
                  "CalculateProjectedCoordsCascades(worldPos, "
                  "g_LFeature_ShadowProjection[" << projection + 1 << "]);\n"
               << "\t\tvec4 projectedCoords3 = "
                  "CalculateProjectedCoordsCascades(worldPos, "
                  "g_LFeature_ShadowProjection[" << projection + 2 << "]);\n"
               << "\t\tvec3 projectedCoords = mix(projectedCoords1.xyz, "
                  "projectedCoords2.xyz, projectedCoords1.w);\n"
               << "\t\tprojectedCoords = mix(projectedCoords, "
                  "projectedCoords3.xyz, projectedCoords2.w);\n"
               << "\t\tvec4 atlasTransform = mix("
                  "g_LFeature_ShadowProjectionTransform[" << projection
               << "], g_LFeature_ShadowProjectionTransform[" << projection + 1
               << "], projectedCoords1.w);\n"
               << "\t\tatlasTransform = mix(atlasTransform, "
                  "g_LFeature_ShadowProjectionTransform[" << projection + 2
               << "], projectedCoords2.w);\n"
               << "\t\tfloat shadowFactor = max(projectedCoords3.w, "
                  "PerformShadowMapping(projectedCoords, atlasTransform));\n"
               << "\t\tlight += ComputePBRLightShadowInfinite(normal, "
                  "g_LDirectional_Direction[i].xyz, viewVector, color, "
                  "g_LDirectional_Color[i].rgb, specularTint, ambient, "
                  "roughness, metallic, shadowFactor);\n\t}\n";
    }
    for (int index = directionalShadow; index < directional; ++index) {
        lightBlock(
            index,
            "\t\tlight += ComputePBRLightShadowInfinite(normal, "
            "g_LDirectional_Direction[i].xyz, viewVector, color, "
            "g_LDirectional_Color[i].rgb, specularTint, ambient, "
            "roughness, metallic, 1.0);\n"
        );
    }
    result << "\treturn light;\n}\n";
    return result.str();
}

std::string processRequires(
    std::string source,
    std::string_view sourceName,
    const std::map<std::string, int>& combos
) {
    std::istringstream lines(source);
    std::string result;
    std::string line;
    while (std::getline(lines, line)) {
        const std::string leading = trim(line);
        if (!leading.starts_with("#require")) {
            result += line;
            result.push_back('\n');
            continue;
        }
        const std::string module = trim(
            std::string_view(leading).substr(std::string("#require").size())
        );
        if (module.empty() || module.find_first_of(" \t") != std::string::npos) {
            preprocessError(sourceName, "Malformed #require directive");
        }
        if (module != "LightingV1") {
            preprocessError(
                sourceName,
                "Unsupported #require module '" + module + "'"
            );
        }
        result += "// begin generated module LightingV1\n";
        result += lightingV1Module(combos, sourceName);
        result += "// end generated module LightingV1\n";
        result += "// #require LightingV1\n";
    }
    return result;
}

std::string removeVersionDirectives(std::string source) {
    std::istringstream lines(source);
    std::string result;
    std::string line;
    while (std::getline(lines, line)) {
        if (!trim(line).starts_with("#version")) {
            result += line;
            result.push_back('\n');
        }
    }
    return result;
}

void parseCombos(
    const std::string& source,
    const ShaderPreprocessOptions& options,
    std::map<std::string, int>& combos
) {
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t marker = line.find("// [COMBO]");
        if (marker == std::string::npos) {
            continue;
        }
        const std::string jsonText = trim(
            std::string_view(line).substr(marker + std::string("// [COMBO]").size())
        );
        try {
            const nlohmann::json metadata = nlohmann::json::parse(jsonText);
            if (!metadata.is_object() || !metadata.contains("combo") ||
                !metadata["combo"].is_string()) {
                preprocessError(line, "Combo metadata has no string 'combo' field");
            }
            const std::string name = metadata["combo"].get<std::string>();
            int value = 0;
            if (metadata.contains("default")) {
                if (!metadata["default"].is_number_integer()) {
                    preprocessError(
                        line,
                        "Combo '" + name + "' default must be an integer"
                    );
                }
                value = metadata["default"].get<int>();
            }
            if (const auto overrideValue = options.overrideCombos.find(name);
                overrideValue != options.overrideCombos.end()) {
                value = overrideValue->second;
            } else if (const auto configured = options.combos.find(name);
                       configured != options.combos.end()) {
                value = configured->second;
            }
            combos.insert_or_assign(name, value);
        } catch (const FormatError&) {
            throw;
        } catch (const nlohmann::json::exception& error) {
            preprocessError(
                line,
                "Invalid combo metadata: " + std::string(error.what())
            );
        }
    }
    for (const auto& [name, value] : options.combos) {
        combos.insert_or_assign(name, value);
    }
    for (const auto& [name, value] : options.overrideCombos) {
        combos.insert_or_assign(name, value);
    }
}

std::optional<std::size_t> vectorComponentCount(std::string_view type) {
    if (type == "vec2") {
        return 2;
    }
    if (type == "vec3") {
        return 3;
    }
    if (type == "vec4") {
        return 4;
    }
    return std::nullopt;
}

std::vector<double> parseVectorDefault(
    std::string_view value,
    std::size_t expectedCount,
    std::string_view parameterName,
    std::string_view sourceName
) {
    const auto malformed = [&]() -> void {
        preprocessError(
            sourceName,
            "Uniform metadata default for vector '" +
                std::string(parameterName) + "' must contain exactly " +
                std::to_string(expectedCount) + " numbers"
        );
    };

    std::vector<double> components;
    std::size_t position = 0;
    bool expectsValue = true;
    while (position < value.size()) {
        bool skippedWhitespace = false;
        while (position < value.size() &&
               std::isspace(
                   static_cast<unsigned char>(value[position])
               ) != 0) {
            skippedWhitespace = true;
            ++position;
        }
        if (position == value.size()) {
            if (expectsValue) malformed();
            break;
        }

        if (!expectsValue) {
            if (value[position] == ',') {
                ++position;
                expectsValue = true;
                continue;
            }
            if (!skippedWhitespace) malformed();
            expectsValue = true;
        }
        if (value[position] == ',') malformed();

        const std::size_t tokenStart = position;
        while (position < value.size() && value[position] != ',' &&
               std::isspace(
                   static_cast<unsigned char>(value[position])
               ) == 0) {
            ++position;
        }
        std::istringstream tokenStream{
            std::string(value.substr(tokenStart, position - tokenStart))
        };
        tokenStream.imbue(std::locale::classic());
        double component = 0.0;
        if (!(tokenStream >> component)) malformed();
        if (tokenStream.peek() != std::char_traits<char>::eof()) malformed();

        if (components.size() == expectedCount) {
            malformed();
        }
        if (!std::isfinite(component)) {
            preprocessError(
                sourceName,
                "Uniform metadata default for vector '" +
                    std::string(parameterName) + "' must contain finite numbers"
            );
        }
        components.push_back(component);
        expectsValue = false;
    }
    if (expectsValue || components.size() != expectedCount) malformed();
    return components;
}

ShaderParameterDefault parseParameterDefault(
    const nlohmann::json& value,
    std::string_view parameterType,
    std::string_view parameterName,
    std::string_view sourceName
) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_unsigned()) {
        const std::uint64_t unsignedValue = value.get<std::uint64_t>();
        if (unsignedValue >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            preprocessError(
                sourceName,
                "Uniform metadata default for '" + std::string(parameterName) +
                    "' exceeds the supported integer range"
            );
        }
        return static_cast<std::int64_t>(unsignedValue);
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            preprocessError(
                sourceName,
                "Uniform metadata default for '" + std::string(parameterName) +
                    "' must be finite"
            );
        }
        return number;
    }
    if (value.is_string()) {
        std::string stringValue = value.get<std::string>();
        if (const auto componentCount = vectorComponentCount(parameterType)) {
            return parseVectorDefault(
                stringValue,
                *componentCount,
                parameterName,
                sourceName
            );
        }
        return std::move(stringValue);
    }

    preprocessError(
        sourceName,
        "Uniform metadata default for '" + std::string(parameterName) +
            "' must be a boolean, integer, number, or string; found " +
            value.type_name()
    );
}

std::vector<ShaderParameterMetadata> parseParameters(
    const std::string& source,
    std::string_view sourceName
) {
    std::vector<ShaderParameterMetadata> result;
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t uniform = line.find("uniform ");
        const std::size_t semicolon = line.find(';');
        const std::size_t comment = line.find("//", semicolon);
        if (uniform == std::string::npos || semicolon == std::string::npos ||
            comment == std::string::npos || comment < semicolon) {
            continue;
        }
        std::istringstream declaration(
            line.substr(uniform + std::string("uniform ").size())
        );
        ShaderParameterMetadata parameter;
        if (!(declaration >> parameter.type >> parameter.name)) {
            continue;
        }
        parameter.name = parameter.name.substr(
            0,
            parameter.name.find(';')
        );
        parameter.json = trim(std::string_view(line).substr(comment + 2));
        try {
            const nlohmann::json metadata = nlohmann::json::parse(parameter.json);
            if (!metadata.is_object()) {
                preprocessError(
                    sourceName,
                    "Uniform metadata for '" + parameter.name +
                        "' must be a JSON object"
                );
            }
            if (const auto material = metadata.find("material");
                material != metadata.end()) {
                if (!material->is_string()) {
                    preprocessError(
                        sourceName,
                        "Uniform metadata material for '" + parameter.name +
                            "' must be a string"
                    );
                }
                parameter.material = material->get<std::string>();
            }
            if (const auto combo = metadata.find("combo");
                combo != metadata.end()) {
                if (!combo->is_string()) {
                    preprocessError(
                        sourceName,
                        "Uniform metadata combo for '" + parameter.name +
                            "' must be a string"
                    );
                }
                parameter.combo = combo->get<std::string>();
            }
            if (const auto defaultValue = metadata.find("default");
                defaultValue != metadata.end()) {
                parameter.defaultValue = parseParameterDefault(
                    *defaultValue,
                    parameter.type,
                    parameter.name,
                    sourceName
                );
            }
        } catch (const FormatError&) {
            throw;
        } catch (const nlohmann::json::exception& error) {
            preprocessError(
                sourceName,
                "Invalid uniform metadata for '" + parameter.name + "': " +
                    error.what()
            );
        }
        result.push_back(std::move(parameter));
    }
    return result;
}

bool samplerParameterType(std::string_view type) {
    return type.starts_with("sampler") || type.starts_with("isampler") ||
        type.starts_with("usampler");
}

std::optional<int> samplerTextureSlot(std::string_view name) {
    constexpr std::string_view prefix = "g_Texture";
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return std::nullopt;
    }
    int slot = 0;
    for (const char digit : name.substr(prefix.size())) {
        if (digit < '0' || digit > '9') return std::nullopt;
        slot = slot * 10 + (digit - '0');
        if (slot >= 32) return std::nullopt;
    }
    return slot;
}

void applyTextureLinkedCombos(
    const std::vector<ShaderParameterMetadata>& parameters,
    const ShaderPreprocessOptions& options,
    std::map<std::string, int>& combos
) {
    for (const ShaderParameterMetadata& parameter : parameters) {
        if (!parameter.combo || !samplerParameterType(parameter.type)) {
            continue;
        }
        const std::optional<int> slot = samplerTextureSlot(parameter.name);
        if (!slot) continue;

        // Explicit material and renderer overrides are authoritative. A
        // sampler-linked combo is the discovered/default layer beneath them.
        const bool explicitlyConfigured =
            options.overrideCombos.contains(*parameter.combo) ||
            options.combos.contains(*parameter.combo);
        if (options.textureSlots.contains(*slot)) {
            if (!explicitlyConfigured) {
                combos.insert_or_assign(*parameter.combo, 1);
            }
            continue;
        }

        // A few stock shaders use require/requireany with an integer default
        // to select a sampler-backed variant even when no explicit texture
        // slot exists. Preserve that small part of the official discovery
        // contract without treating texture-name defaults as combo values.
        if (explicitlyConfigured || parameter.json.empty()) continue;
        const nlohmann::json metadata = nlohmann::json::parse(parameter.json);
        const auto require = metadata.find("require");
        if (require == metadata.end() || !require->is_object()) continue;
        const bool requireAny = metadata.value("requireany", false);
        bool required = false;
        if (requireAny) {
            // This mirrors the upstream renderer's discovery rule: a
            // require-any sampler is discovered when one condition is not
            // currently satisfied.
            for (const auto& [name, value] : require->items()) {
                const auto found = combos.find(name);
                if (found == combos.end() ||
                    options.overrideCombos.contains(name) ||
                    found->second != value.get<int>()) {
                    required = true;
                    break;
                }
            }
        } else {
            required = true;
            for (const auto& [name, value] : require->items()) {
                const auto found = combos.find(name);
                if ((found != combos.end() ||
                     options.overrideCombos.contains(name)) &&
                    found != combos.end() && found->second == value.get<int>()) {
                    required = false;
                    break;
                }
            }
        }
        if (!required) continue;
        const auto defaultValue = metadata.find("default");
        if (defaultValue != metadata.end() &&
            defaultValue->is_number_integer()) {
            combos.insert_or_assign(
                *parameter.combo,
                defaultValue->get<int>()
            );
        }
    }
}

std::string compatibilityHeader(
    bool fragment,
    const std::map<std::string, int>& combos,
    const std::map<std::string, std::string>& constants,
    std::string_view name
) {
    std::string result = "#version 330\n";
    result += "// Processed Wallpaper Engine shader: " + std::string(name) + "\n";
    result += "precision highp float;\n"
              "#define mul(x, y) ((y) * (x))\n"
              "#define max(x, y) max(y, x)\n"
              "#define lerp mix\n"
              "#define frac fract\n"
              "#define CASTF(x) (float(x))\n"
              "#define CASTU(x) (uint(x))\n"
              "#define CAST2(x) (vec2(x))\n"
              "#define CAST3(x) (vec3(x))\n"
              "#define CAST4(x) (vec4(x))\n"
              "#define CAST3X3(x) (mat3(x))\n"
              "#define float2 vec2\n"
              "#define float3 vec3\n"
              "#define float4 vec4\n"
              "#define int2 ivec2\n"
              "#define int3 ivec3\n"
              "#define int4 ivec4\n"
              "#define saturate(x) (clamp(x, 0.0, 1.0))\n"
              // Wallpaper Engine's shader ABI uses distinct sampler names for
              // depth comparison and integer-addressed render targets. Keep
              // those names in the source so metadata/reflection can retain
              // their contract, then lower them to core GLSL 3.30 types.
              "#define sampler2DComparison sampler2DShadow\n"
              "#define sampler2DBackBuffer sampler2D\n"
              "#define texSample2D texture\n"
              "#define texSample2DLod textureLod\n"
              // Wallpaper Engine's 3D LUT intrinsic returns RGB rather than
              // the four-component storage texel returned by GLSL texture().
              "#define texSample3D(sampler, coordinate) texture((sampler), (coordinate)).rgb\n"
              "#define texSample2DCompare(sampler, coordinate, compare) (vec4(texture((sampler), vec3((coordinate), (compare)))))\n"
              "#define texSample2DBackBuffer(sampler, coordinate, resolution) texelFetch((sampler), ivec2((coordinate) * (resolution)), 0)\n"
              "#define texLoad2D(sampler, coordinate, resolution) texelFetch((sampler), ivec2((coordinate) * (resolution)), 0)\n"
              "#define log10(x) (log2(x) * 0.301029995663981)\n"
              "#define atan2 atan\n"
              "#define fmod(x, y) ((x)-(y)*trunc((x)/(y)))\n"
              "#define ddx dFdx\n"
              "#define ddy(x) dFdy(-(x))\n"
              "#define clip(x) if ((x) < 0.0) discard\n"
              "#define GLSL 1\n";
    if (fragment) {
        result += "out vec4 out_FragColor;\n#define varying in\n";
    } else {
        result += "#define attribute in\n#define varying out\n";
    }
    for (const auto& [nameValue, value] : constants) {
        result += "#define " + upper(nameValue) + " " + value + "\n";
    }
    for (const auto& [nameValue, value] : combos) {
        result += "#define " + upper(nameValue) + " " +
            std::to_string(value) + "\n";
    }
    result.push_back('\n');
    return result;
}

PreprocessedShader makeShader(
    bool fragment,
    std::string source,
    std::string name,
    const ShaderPreprocessOptions& options
) {
    std::map<std::string, int> combos;
    parseCombos(source, options, combos);
    source = removeVersionDirectives(
        processRequires(std::move(source), name, combos)
    );
    PreprocessedShader result;
    result.name = std::move(name);
    result.combos = combos;
    result.parameters = parseParameters(source, result.name);
    for (ShaderParameterMetadata& parameter : result.parameters) {
        parameter.stage = fragment
            ? ShaderParameterMetadata::Stage::fragment
            : ShaderParameterMetadata::Stage::vertex;
    }
    result.source = compatibilityHeader(
        fragment,
        result.combos,
        options.constants,
        result.name
    ) + source;
    std::size_t position = 0;
    while ((position = result.source.find("gl_FragColor", position)) !=
           std::string::npos) {
        result.source.replace(position, std::string("gl_FragColor").size(), "out_FragColor");
        position += std::string("out_FragColor").size();
    }
    return result;
}

}  // namespace

ShaderPreprocessor::ShaderPreprocessor(const AssetResolver& resolver) noexcept
    : resolver_(resolver) {}

PreprocessedShaderPair ShaderPreprocessor::preprocessFiles(
    std::string_view vertexPath,
    std::string_view fragmentPath,
    const ShaderPreprocessOptions& options
) const {
    const ResolvedAsset vertex = resolver_.resolve(vertexPath);
    const ResolvedAsset fragment = resolver_.resolve(fragmentPath);
    SourceExpander vertexExpander(resolver_, vertex.logicalPath);
    SourceExpander fragmentExpander(resolver_, fragment.logicalPath);
    return preprocessSources(
        vertexExpander.expand(
            std::string_view(
                reinterpret_cast<const char*>(vertex.bytes.data()),
                vertex.bytes.size()
            ),
            vertex.logicalPath
        ),
        fragmentExpander.expand(
            std::string_view(
                reinterpret_cast<const char*>(fragment.bytes.data()),
                fragment.bytes.size()
            ),
            fragment.logicalPath
        ),
        vertex.logicalPath,
        fragment.logicalPath,
        options
    );
}

PreprocessedShaderPair ShaderPreprocessor::preprocessSources(
    std::string_view vertexSource,
    std::string_view fragmentSource,
    std::string_view vertexName,
    std::string_view fragmentName,
    const ShaderPreprocessOptions& options
) const {
    SourceExpander vertexExpander(resolver_, std::string(vertexName));
    SourceExpander fragmentExpander(resolver_, std::string(fragmentName));
    std::string expandedVertex = vertexExpander.expand(vertexSource, vertexName);
    std::string expandedFragment = fragmentExpander.expand(
        fragmentSource, fragmentName
    );

    // Wallpaper Engine treats a vertex/fragment pair as one shader program:
    // combo metadata discovered in either stage is visible to both. Official
    // effects commonly declare a combo only beside the fragment parameter
    // that controls it while using the same macro to guard vertex varyings.
    // Resolving each stage independently therefore creates incompatible
    // interfaces even though both sources are valid as a linked pair.
    std::map<std::string, int> linkedCombos;
    parseCombos(expandedVertex, options, linkedCombos);
    parseCombos(expandedFragment, options, linkedCombos);
    ShaderPreprocessOptions linkedOptions = options;
    linkedOptions.combos = std::move(linkedCombos);
    std::vector<ShaderParameterMetadata> linkedParameters = parseParameters(
        expandedVertex, vertexName
    );
    std::vector<ShaderParameterMetadata> fragmentParameters = parseParameters(
        expandedFragment, fragmentName
    );
    linkedParameters.insert(
        linkedParameters.end(), fragmentParameters.begin(), fragmentParameters.end()
    );
    applyTextureLinkedCombos(
        linkedParameters, options, linkedOptions.combos
    );

    PreprocessedShaderPair result {
        makeShader(
            false,
            std::move(expandedVertex),
            std::string(vertexName),
            linkedOptions
        ),
        makeShader(
            true,
            std::move(expandedFragment),
            std::string(fragmentName),
            linkedOptions
        ),
    };
    return result;
}

}  // namespace we::scene
