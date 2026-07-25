#include <SceneShader/ShaderPreprocessor.hpp>

#include <SceneCore/FormatError.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <locale>
#include <regex>
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

std::string processRequires(
    std::string source,
    std::string_view sourceName
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
        result += "// begin generated module LightingV1\n"
                  "vec3 PerformLighting_V1(vec3 worldPos, vec3 albedo, vec3 normal, vec3 viewDir,\n"
                  "    vec3 specularTint, vec3 baseReflectance, float roughness, float metallic)\n"
                  "{\n"
                  "    return vec3(0.0);\n"
                  "}\n"
                  "// end generated module LightingV1\n";
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
              "#define texSample2D texture\n"
              "#define texSample2DLod textureLod\n"
              "#define log10(x) (log2(x) * 0.301029995663981)\n"
              "#define atan2 atan\n"
              "#define fmod(x, y) ((x)-(y)*trunc((x)/(y)))\n"
              "#define ddx dFdx\n"
              "#define ddy(x) dFdy(-(x))\n"
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

std::string applyLinkedVaryingCompatibility(
    std::string vertex,
    const std::string& fragment
) {
    const std::regex fragmentVec4(R"(\bvarying\s+vec4\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    const std::regex fragmentVec2(R"(\bvarying\s+vec2\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    const std::regex assignment(
        R"((^|\n)([ \t]*)([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;\n]+);)"
    );
    for (std::sregex_iterator it(fragment.begin(), fragment.end(), fragmentVec4),
         end;
         it != end;
         ++it) {
        const std::string name = (*it)[1].str();
        const std::regex declaration(
            "\\bvarying\\s+vec2\\s+" + name + "\\s*;"
        );
        if (!std::regex_search(vertex, declaration)) {
            continue;
        }
        vertex = std::regex_replace(
            vertex,
            declaration,
            "varying vec4 " + name + ";"
        );
        std::string adjusted;
        std::size_t offset = 0;
        for (std::sregex_iterator assignmentIt(vertex.begin(), vertex.end(), assignment);
             assignmentIt != end;
             ++assignmentIt) {
            const std::smatch& match = *assignmentIt;
            if (match[3].str() != name) {
                continue;
            }
            const std::size_t position = static_cast<std::size_t>(match.position());
            adjusted += vertex.substr(offset, position - offset);
            adjusted += match[1].str() + match[2].str() + name + " = vec4(" +
                match[4].str() + ", 0.0, 1.0);";
            offset = position + static_cast<std::size_t>(match.length());
        }
        if (offset != 0) {
            adjusted += vertex.substr(offset);
            vertex = std::move(adjusted);
        }
    }
    // Wallpaper Engine's shader frontend permits a vertex stage to expose a
    // wider varying than the fragment stage consumes. GLSL requires exact
    // interface types, so narrow the producer to the fragment's declared
    // contract. If the vertex source truly uses the discarded components,
    // normal compilation still fails explicitly instead of guessing values.
    for (std::sregex_iterator it(fragment.begin(), fragment.end(), fragmentVec2),
         end;
         it != end;
         ++it) {
        const std::string name = (*it)[1].str();
        const std::regex declaration(
            "\\bvarying\\s+vec[34]\\s+" + name + "\\s*;"
        );
        vertex = std::regex_replace(
            vertex,
            declaration,
            "varying vec2 " + name + ";"
        );
    }
    return vertex;
}

std::string applyFragmentTexCoordCompatibility(std::string source) {
    const std::regex wide(R"(\bvarying\s+vec[34]\s+v_TexCoord\s*;)");
    const std::regex beforeCast(R"(\bv_TexCoord\b(\s*[-+*/]\s*CAST2\s*\())");
    const std::regex afterCast(R"((CAST2\s*\([^)]+\)\s*[-+*/]\s*)\bv_TexCoord\b)");
    if (!std::regex_search(source, wide) ||
        (!std::regex_search(source, beforeCast) &&
         !std::regex_search(source, afterCast))) {
        return source;
    }
    source = std::regex_replace(source, beforeCast, "v_TexCoord.xy$1");
    return std::regex_replace(source, afterCast, "$1v_TexCoord.xy");
}

PreprocessedShader makeShader(
    bool fragment,
    std::string source,
    std::string name,
    const ShaderPreprocessOptions& options
) {
    source = removeVersionDirectives(processRequires(std::move(source), name));
    std::map<std::string, int> combos;
    parseCombos(source, options, combos);
    PreprocessedShader result;
    result.name = std::move(name);
    result.combos = combos;
    result.parameters = parseParameters(source, result.name);
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
    result.vertex.source = applyLinkedVaryingCompatibility(
        std::move(result.vertex.source),
        result.fragment.source
    );
    result.fragment.source = applyFragmentTexCoordCompatibility(
        std::move(result.fragment.source)
    );
    return result;
}

}  // namespace we::scene
