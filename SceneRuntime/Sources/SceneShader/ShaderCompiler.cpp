#include <SceneShader/ShaderCompiler.hpp>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Include/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_glsl.hpp>
#include <spirv_msl.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace we::scene {
namespace {

constexpr std::size_t maximumShaderSourceBytes = 16 * 1024 * 1024;
constexpr EShMessages wallpaperEngineShaderMessages =
    static_cast<EShMessages>(EShMsgDefault | EShMsgHlslCompatibleGlsl);

const TBuiltInResource builtInResources = {
    .maxLights = 32,
    .maxClipPlanes = 6,
    .maxTextureUnits = 32,
    .maxTextureCoords = 32,
    .maxVertexAttribs = 64,
    .maxVertexUniformComponents = 4096,
    .maxVaryingFloats = 64,
    .maxVertexTextureImageUnits = 32,
    .maxCombinedTextureImageUnits = 80,
    .maxTextureImageUnits = 32,
    .maxFragmentUniformComponents = 4096,
    .maxDrawBuffers = 32,
    .maxVertexUniformVectors = 128,
    .maxVaryingVectors = 8,
    .maxFragmentUniformVectors = 16,
    .maxVertexOutputVectors = 16,
    .maxFragmentInputVectors = 15,
    .minProgramTexelOffset = -8,
    .maxProgramTexelOffset = 7,
    .maxClipDistances = 8,
    .maxComputeWorkGroupCountX = 65535,
    .maxComputeWorkGroupCountY = 65535,
    .maxComputeWorkGroupCountZ = 65535,
    .maxComputeWorkGroupSizeX = 1024,
    .maxComputeWorkGroupSizeY = 1024,
    .maxComputeWorkGroupSizeZ = 64,
    .maxComputeUniformComponents = 1024,
    .maxComputeTextureImageUnits = 16,
    .maxComputeImageUniforms = 8,
    .maxComputeAtomicCounters = 8,
    .maxComputeAtomicCounterBuffers = 1,
    .maxVaryingComponents = 60,
    .maxVertexOutputComponents = 64,
    .maxGeometryInputComponents = 64,
    .maxGeometryOutputComponents = 128,
    .maxFragmentInputComponents = 128,
    .maxImageUnits = 8,
    .maxCombinedImageUnitsAndFragmentOutputs = 8,
    .maxCombinedShaderOutputResources = 8,
    .maxImageSamples = 0,
    .maxVertexImageUniforms = 0,
    .maxTessControlImageUniforms = 0,
    .maxTessEvaluationImageUniforms = 0,
    .maxGeometryImageUniforms = 0,
    .maxFragmentImageUniforms = 8,
    .maxCombinedImageUniforms = 8,
    .maxGeometryTextureImageUnits = 16,
    .maxGeometryOutputVertices = 256,
    .maxGeometryTotalOutputComponents = 1024,
    .maxGeometryUniformComponents = 1024,
    .maxGeometryVaryingComponents = 64,
    .maxTessControlInputComponents = 128,
    .maxTessControlOutputComponents = 128,
    .maxTessControlTextureImageUnits = 16,
    .maxTessControlUniformComponents = 1024,
    .maxTessControlTotalOutputComponents = 4096,
    .maxTessEvaluationInputComponents = 128,
    .maxTessEvaluationOutputComponents = 128,
    .maxTessEvaluationTextureImageUnits = 16,
    .maxTessEvaluationUniformComponents = 1024,
    .maxTessPatchComponents = 120,
    .maxPatchVertices = 32,
    .maxTessGenLevel = 64,
    .maxViewports = 16,
    .maxVertexAtomicCounters = 0,
    .maxTessControlAtomicCounters = 0,
    .maxTessEvaluationAtomicCounters = 0,
    .maxGeometryAtomicCounters = 0,
    .maxFragmentAtomicCounters = 8,
    .maxCombinedAtomicCounters = 8,
    .maxAtomicCounterBindings = 1,
    .maxVertexAtomicCounterBuffers = 0,
    .maxTessControlAtomicCounterBuffers = 0,
    .maxTessEvaluationAtomicCounterBuffers = 0,
    .maxGeometryAtomicCounterBuffers = 0,
    .maxFragmentAtomicCounterBuffers = 1,
    .maxCombinedAtomicCounterBuffers = 1,
    .maxAtomicCounterBufferSize = 16384,
    .maxTransformFeedbackBuffers = 4,
    .maxTransformFeedbackInterleavedComponents = 64,
    .maxCullDistances = 8,
    .maxCombinedClipAndCullDistances = 8,
    .maxSamples = 4,
    .maxMeshOutputVerticesNV = 256,
    .maxMeshOutputPrimitivesNV = 512,
    .maxMeshWorkGroupSizeX_NV = 32,
    .maxMeshWorkGroupSizeY_NV = 1,
    .maxMeshWorkGroupSizeZ_NV = 1,
    .maxTaskWorkGroupSizeX_NV = 32,
    .maxTaskWorkGroupSizeY_NV = 1,
    .maxTaskWorkGroupSizeZ_NV = 1,
    .maxMeshViewCountNV = 4,
    .limits = {
        .nonInductiveForLoops = true,
        .whileLoops = true,
        .doWhileLoops = true,
        .generalUniformIndexing = true,
        .generalAttributeMatrixVectorIndexing = true,
        .generalVaryingIndexing = true,
        .generalSamplerIndexing = true,
        .generalVariableIndexing = true,
        .generalConstantMatrixVectorIndexing = true,
    },
};

class GlslangProcess final {
public:
    GlslangProcess() {
        if (!glslang::InitializeProcess()) {
            throw ShaderCompileError(
                ShaderCompilePhase::input,
                "glslang process initialization failed"
            );
        }
    }

    ~GlslangProcess() {
        glslang::FinalizeProcess();
    }

    GlslangProcess(const GlslangProcess&) = delete;
    GlslangProcess& operator=(const GlslangProcess&) = delete;
};

void ensureGlslangInitialized() {
    static const GlslangProcess process;
    static_cast<void>(process);
}

std::mutex& compilerMutex() {
    static std::mutex mutex;
    return mutex;
}

void validateSource(std::string_view source, std::string_view name) {
    if (source.empty()) {
        throw ShaderCompileError(
            ShaderCompilePhase::input,
            std::string(name) + " shader source is empty"
        );
    }
    if (source.size() > maximumShaderSourceBytes) {
        throw ShaderCompileError(
            ShaderCompilePhase::input,
            std::string(name) + " shader source exceeds the 16 MiB limit"
        );
    }
    if (source.find('\0') != std::string_view::npos) {
        throw ShaderCompileError(
            ShaderCompilePhase::input,
            std::string(name) + " shader source contains a null byte"
        );
    }
}

struct ShaderSourceBinding final {
    ShaderSourceBinding(std::string_view source, std::string_view sourceName)
        : sourcePointer(source.data()),
          sourceLength(static_cast<int>(source.size())),
          name(sourceName),
          namePointer(name.c_str()) {}

    ShaderSourceBinding(const ShaderSourceBinding&) = delete;
    ShaderSourceBinding& operator=(const ShaderSourceBinding&) = delete;
    ShaderSourceBinding(ShaderSourceBinding&&) = delete;
    ShaderSourceBinding& operator=(ShaderSourceBinding&&) = delete;

    const char* sourcePointer;
    int sourceLength;
    std::string name;
    const char* namePointer;
};

void configureShader(
    glslang::TShader& shader,
    EShLanguage stage,
    ShaderSourceBinding& binding
) {
    shader.setStringsWithLengthsAndNames(
        &binding.sourcePointer,
        &binding.sourceLength,
        &binding.namePointer,
        1
    );
    shader.setEntryPoint("main");
    shader.setEnvInput(
        glslang::EShSourceGlsl,
        stage,
        glslang::EShClientOpenGL,
        330
    );
    shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    shader.setAutoMapLocations(true);
    shader.setAutoMapBindings(true);
}

std::string shaderLog(glslang::TShader& shader) {
    std::string result = shader.getInfoLog();
    const char* debugLog = shader.getInfoDebugLog();
    if (debugLog != nullptr && *debugLog != '\0') {
        if (!result.empty()) {
            result += '\n';
        }
        result += debugLog;
    }
    return result.empty() ? "glslang returned no diagnostic" : result;
}

std::string programLog(glslang::TProgram& program) {
    std::string result = program.getInfoLog();
    const char* debugLog = program.getInfoDebugLog();
    if (debugLog != nullptr && *debugLog != '\0') {
        if (!result.empty()) {
            result += '\n';
        }
        result += debugLog;
    }
    return result.empty() ? "glslang returned no diagnostic" : result;
}

std::string activeShaderSource(
    std::string_view source,
    std::string_view sourceName,
    EShLanguage stage,
    ShaderCompilePhase phase
) {
    glslang::TShader shader(stage);
    ShaderSourceBinding binding(source, sourceName);
    configureShader(shader, stage, binding);
    glslang::TShader::ForbidIncluder includer;
    std::string result;
    if (!shader.preprocess(
            &builtInResources,
            100,
            ENoProfile,
            false,
            false,
            wallpaperEngineShaderMessages,
            &result,
            includer)) {
        throw ShaderCompileError(
            phase,
            std::string(sourceName) + " shader preprocessing failed: " +
                shaderLog(shader)
        );
    }
    if (result.empty()) {
        throw ShaderCompileError(
            phase,
            std::string(sourceName) + " shader preprocessing produced empty source"
        );
    }
    return result;
}

struct ShaderToken final {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool identifier = false;
};

std::vector<ShaderToken> shaderTokens(std::string_view source) {
    std::vector<ShaderToken> result;
    for (std::size_t index = 0; index < source.size();) {
        const unsigned char current =
            static_cast<unsigned char>(source[index]);
        if (std::isspace(current) != 0) {
            ++index;
            continue;
        }
        if (source[index] == '/' && index + 1 < source.size()) {
            if (source[index + 1] == '/') {
                index += 2;
                while (index < source.size() && source[index] != '\n') {
                    ++index;
                }
                continue;
            }
            if (source[index + 1] == '*') {
                index += 2;
                while (index + 1 < source.size() &&
                       !(source[index] == '*' && source[index + 1] == '/')) {
                    ++index;
                }
                if (index + 1 >= source.size()) {
                    throw ShaderCompileError(
                        ShaderCompilePhase::input,
                        "Unterminated block comment in active shader source"
                    );
                }
                index += 2;
                continue;
            }
        }
        if (source[index] == '"' || source[index] == '\'') {
            const char quote = source[index++];
            while (index < source.size()) {
                if (source[index] == '\\' && index + 1 < source.size()) {
                    index += 2;
                    continue;
                }
                if (source[index++] == quote) break;
            }
            continue;
        }
        const bool identifier = std::isalpha(current) != 0 ||
            source[index] == '_';
        if (identifier) {
            const std::size_t begin = index++;
            while (index < source.size()) {
                const unsigned char value =
                    static_cast<unsigned char>(source[index]);
                if (std::isalnum(value) == 0 && source[index] != '_') break;
                ++index;
            }
            result.push_back({begin, index, true});
            continue;
        }
        const std::size_t begin = index++;
        if (index < source.size()) {
            const std::string_view pair = source.substr(begin, 2);
            if (pair == "==" || pair == "!=" || pair == "<=" ||
                pair == ">=" || pair == "+=" || pair == "-=" ||
                pair == "*=" || pair == "/=" || pair == "&&" ||
                pair == "||" || pair == "++" || pair == "--" ||
                pair == "<<" || pair == ">>") {
                ++index;
            }
        }
        result.push_back({begin, index, false});
    }
    return result;
}

bool tokenEquals(
    std::string_view source,
    const ShaderToken& token,
    std::string_view expected
) {
    return source.substr(token.begin, token.end - token.begin) == expected;
}

struct SourceRange final {
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::optional<std::size_t> firstFunctionDefinition(
    std::string_view source,
    const std::vector<ShaderToken>& tokens
) {
    int braces = 0;
    std::size_t statementBegin = 0;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokenEquals(source, tokens[index], "{")) {
            if (braces == 0 && index > 0 &&
                tokenEquals(source, tokens[index - 1], ")")) {
                return tokens[statementBegin].begin;
            }
            ++braces;
        } else if (tokenEquals(source, tokens[index], "}")) {
            --braces;
            if (braces == 0) statementBegin = index + 1;
        } else if (braces == 0 &&
                   tokenEquals(source, tokens[index], ";")) {
            statementBegin = index + 1;
        }
    }
    return std::nullopt;
}

std::string hoistLateUniformDeclarations(
    std::string source,
    std::string_view sourceName,
    ShaderCompilePhase phase
) {
    const std::vector<ShaderToken> tokens = shaderTokens(source);
    const std::optional<std::size_t> firstFunction =
        firstFunctionDefinition(source, tokens);
    if (!firstFunction.has_value()) return source;

    std::vector<SourceRange> lateUniforms;
    int braces = 0;
    std::size_t statementBegin = 0;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokenEquals(source, tokens[index], "{")) {
            ++braces;
            continue;
        }
        if (tokenEquals(source, tokens[index], "}")) {
            --braces;
            if (braces == 0) statementBegin = index + 1;
            continue;
        }
        if (braces != 0) continue;
        if (tokenEquals(source, tokens[index], ";")) {
            statementBegin = index + 1;
            continue;
        }
        if (!tokenEquals(source, tokens[index], "uniform") ||
            tokens[index].begin < *firstFunction) {
            continue;
        }

        std::size_t beginIndex = index;
        if (statementBegin < index &&
            tokenEquals(source, tokens[statementBegin], "layout")) {
            beginIndex = statementBegin;
        }

        int declarationBraces = 0;
        int declarationParentheses = 0;
        int declarationBrackets = 0;
        std::size_t endIndex = index + 1;
        for (; endIndex < tokens.size(); ++endIndex) {
            if (tokenEquals(source, tokens[endIndex], "{")) {
                ++declarationBraces;
            } else if (tokenEquals(source, tokens[endIndex], "}")) {
                --declarationBraces;
            } else if (tokenEquals(source, tokens[endIndex], "(")) {
                ++declarationParentheses;
            } else if (tokenEquals(source, tokens[endIndex], ")")) {
                --declarationParentheses;
            } else if (tokenEquals(source, tokens[endIndex], "[")) {
                ++declarationBrackets;
            } else if (tokenEquals(source, tokens[endIndex], "]")) {
                --declarationBrackets;
            } else if (declarationBraces == 0 &&
                       declarationParentheses == 0 &&
                       declarationBrackets == 0 &&
                       tokenEquals(source, tokens[endIndex], ";")) {
                break;
            }
        }
        if (endIndex == tokens.size()) {
            throw ShaderCompileError(
                phase,
                std::string(sourceName) +
                    " shader has a global uniform declaration without a "
                    "terminating semicolon"
            );
        }
        lateUniforms.push_back({
            .begin = tokens[beginIndex].begin,
            .end = tokens[endIndex].end,
        });
        index = endIndex;
        statementBegin = endIndex + 1;
    }
    if (lateUniforms.empty()) return source;

    std::string declarations;
    for (const SourceRange& range : lateUniforms) {
        declarations.append(source, range.begin, range.end - range.begin);
        declarations.push_back('\n');
    }
    for (auto range = lateUniforms.rbegin(); range != lateUniforms.rend();
         ++range) {
        source.replace(range->begin, range->end - range->begin, "\n");
    }
    source.insert(*firstFunction, declarations);
    return source;
}

struct VaryingDeclaration final {
    std::string name;
    int width = 0;
    std::size_t typeBegin = 0;
    std::size_t typeEnd = 0;
};

std::map<std::string, VaryingDeclaration> varyingDeclarations(
    std::string_view source,
    const std::vector<ShaderToken>& tokens,
    std::string_view qualifier,
    std::string_view sourceName,
    ShaderCompilePhase phase
) {
    std::map<std::string, VaryingDeclaration> result;
    int braces = 0;
    int parentheses = 0;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokenEquals(source, tokens[index], "{")) {
            ++braces;
            continue;
        }
        if (tokenEquals(source, tokens[index], "}")) {
            --braces;
            continue;
        }
        if (tokenEquals(source, tokens[index], "(")) {
            ++parentheses;
            continue;
        }
        if (tokenEquals(source, tokens[index], ")")) {
            --parentheses;
            continue;
        }
        if (braces != 0 || parentheses != 0 ||
            !tokenEquals(source, tokens[index], qualifier)) {
            continue;
        }
        std::size_t typeIndex = index + 1;
        while (typeIndex < tokens.size() &&
               (tokenEquals(source, tokens[typeIndex], "lowp") ||
                tokenEquals(source, tokens[typeIndex], "mediump") ||
                tokenEquals(source, tokens[typeIndex], "highp"))) {
            ++typeIndex;
        }
        if (typeIndex + 2 >= tokens.size()) continue;
        const std::string_view type = source.substr(
            tokens[typeIndex].begin,
            tokens[typeIndex].end - tokens[typeIndex].begin
        );
        const int width = type == "vec2" ? 2 : type == "vec3" ? 3
            : type == "vec4" ? 4 : 0;
        if (width == 0 || !tokens[typeIndex + 1].identifier ||
            !tokenEquals(source, tokens[typeIndex + 2], ";")) {
            continue;
        }
        VaryingDeclaration declaration{
            .name = std::string(source.substr(
                tokens[typeIndex + 1].begin,
                tokens[typeIndex + 1].end - tokens[typeIndex + 1].begin
            )),
            .width = width,
            .typeBegin = tokens[typeIndex].begin,
            .typeEnd = tokens[typeIndex].end,
        };
        const auto [existing, inserted] = result.emplace(
            declaration.name, declaration
        );
        if (!inserted && existing->second.width != width) {
            throw ShaderCompileError(
                phase,
                std::string(sourceName) + " shader declares active varying '" +
                    declaration.name + "' with multiple vector widths"
            );
        }
    }
    return result;
}

struct SourceReplacement final {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string value;
};

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

std::string expandedVaryingExpression(
    std::string_view expression,
    int sourceWidth,
    int targetWidth
) {
    std::string result = "vec" + std::to_string(targetWidth) + "(";
    result += trim(expression);
    if (sourceWidth == 2 && targetWidth >= 3) result += ", 0.0";
    if (targetWidth == 4) result += ", 1.0";
    result += ")";
    return result;
}

std::string applyLinkedVaryingSemantics(
    std::string vertex,
    const std::string& fragment,
    std::string_view vertexName,
    std::string_view fragmentName
) {
    const std::vector<ShaderToken> vertexTokens = shaderTokens(vertex);
    const std::vector<ShaderToken> fragmentTokens = shaderTokens(fragment);
    const auto vertexVaryings = varyingDeclarations(
        vertex,
        vertexTokens,
        "out",
        vertexName,
        ShaderCompilePhase::vertexParse
    );
    const auto fragmentVaryings = varyingDeclarations(
        fragment,
        fragmentTokens,
        "in",
        fragmentName,
        ShaderCompilePhase::fragmentParse
    );
    std::vector<SourceReplacement> replacements;
    for (const auto& [name, consumer] : fragmentVaryings) {
        const auto producer = vertexVaryings.find(name);
        if (producer == vertexVaryings.end() ||
            producer->second.width == consumer.width) {
            continue;
        }
        replacements.push_back({
            .begin = producer->second.typeBegin,
            .end = producer->second.typeEnd,
            .value = "vec" + std::to_string(consumer.width),
        });
        if (producer->second.width >= consumer.width) continue;

        for (std::size_t index = 0; index + 2 < vertexTokens.size(); ++index) {
            if (!vertexTokens[index].identifier ||
                !tokenEquals(vertex, vertexTokens[index], name) ||
                !tokenEquals(vertex, vertexTokens[index + 1], "=")) {
                continue;
            }
            int parentheses = 0;
            int brackets = 0;
            std::size_t endIndex = index + 2;
            for (; endIndex < vertexTokens.size(); ++endIndex) {
                if (tokenEquals(vertex, vertexTokens[endIndex], "(")) {
                    ++parentheses;
                } else if (tokenEquals(vertex, vertexTokens[endIndex], ")")) {
                    --parentheses;
                } else if (tokenEquals(vertex, vertexTokens[endIndex], "[")) {
                    ++brackets;
                } else if (tokenEquals(vertex, vertexTokens[endIndex], "]")) {
                    --brackets;
                } else if (parentheses == 0 && brackets == 0 &&
                           tokenEquals(vertex, vertexTokens[endIndex], ";")) {
                    break;
                }
            }
            if (endIndex == vertexTokens.size()) {
                throw ShaderCompileError(
                    ShaderCompilePhase::vertexParse,
                    std::string(vertexName) +
                        " shader assignment to linked varying '" + name +
                        "' has no terminating semicolon"
                );
            }
            replacements.push_back({
                .begin = vertexTokens[index + 2].begin,
                .end = vertexTokens[endIndex].begin,
                .value = expandedVaryingExpression(
                    std::string_view(vertex).substr(
                        vertexTokens[index + 2].begin,
                        vertexTokens[endIndex].begin -
                            vertexTokens[index + 2].begin
                    ),
                    producer->second.width,
                    consumer.width
                ),
            });
            index = endIndex;
        }
    }
    std::sort(
        replacements.begin(), replacements.end(),
        [](const SourceReplacement& lhs, const SourceReplacement& rhs) {
            return lhs.begin > rhs.begin;
        }
    );
    std::size_t previousBegin = vertex.size();
    for (const SourceReplacement& replacement : replacements) {
        if (replacement.end > previousBegin ||
            replacement.begin > replacement.end ||
            replacement.end > vertex.size()) {
            throw ShaderCompileError(
                ShaderCompilePhase::vertexParse,
                std::string(vertexName) +
                    " shader linked varying semantic rewrites overlap"
            );
        }
        vertex.replace(
            replacement.begin,
            replacement.end - replacement.begin,
            replacement.value
        );
        previousBegin = replacement.begin;
    }
    return vertex;
}

struct TranslatedMetalStage final {
    std::string source;
    std::vector<TranslatedMetalShaderPair::UniformBinding> uniforms;
    std::vector<TranslatedMetalShaderPair::TextureBinding> textures;
    std::vector<TranslatedMetalShaderPair::VertexAttribute> attributes;
};

TranslatedMetalShaderPair::ValueType metalValueType(
    const spirv_cross::SPIRType& type
) {
    using ValueType = TranslatedMetalShaderPair::ValueType;
    using BaseType = spirv_cross::SPIRType::BaseType;
    if (type.columns == 3 && type.vecsize == 3 &&
        type.basetype == BaseType::Float) {
        return ValueType::float3x3;
    }
    if (type.columns == 4 && type.vecsize == 4 &&
        type.basetype == BaseType::Float) {
        return ValueType::float4x4;
    }
    if (type.columns != 1) return ValueType::unsupported;
    if (type.basetype == BaseType::Boolean && type.vecsize == 1) {
        return ValueType::boolean;
    }
    if (type.basetype == BaseType::Int && type.vecsize == 1) {
        return ValueType::int32;
    }
    if (type.basetype == BaseType::UInt && type.vecsize == 1) {
        return ValueType::uint32;
    }
    if (type.basetype != BaseType::Float) return ValueType::unsupported;
    switch (type.vecsize) {
        case 1: return ValueType::float1;
        case 2: return ValueType::float2;
        case 3: return ValueType::float3;
        case 4: return ValueType::float4;
        default: return ValueType::unsupported;
    }
}

std::uint32_t literalArrayLength(const spirv_cross::SPIRType& type) {
    std::uint64_t result = 1;
    for (std::size_t index = 0; index < type.array.size(); ++index) {
        if (index >= type.array_size_literal.size() ||
            !type.array_size_literal[index] || type.array[index] == 0) {
            return 0;
        }
        result *= type.array[index];
        if (result > std::numeric_limits<std::uint32_t>::max()) return 0;
    }
    return static_cast<std::uint32_t>(result);
}

std::string reflectedResourceName(
    const spirv_cross::CompilerMSL& compiler,
    const spirv_cross::Resource& resource
) {
    if (!resource.name.empty()) return resource.name;
    const std::string name = compiler.get_name(resource.id);
    return name.empty() ? compiler.get_fallback_name(resource.id) : name;
}

std::uint32_t metalLocationCount(const spirv_cross::SPIRType& type) {
    std::uint64_t count = std::max<std::uint32_t>(type.columns, 1);
    for (const std::uint32_t length : type.array) {
        count *= std::max<std::uint32_t>(length, 1);
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            throw ShaderCompileError(
                ShaderCompilePhase::crossCompilation,
                "Shader interface location count overflows uint32_t"
            );
        }
    }
    return static_cast<std::uint32_t>(count);
}

template <typename Resources>
void assignMetalInterfaceLocations(
    spirv_cross::CompilerMSL& compiler,
    const Resources& resources
) {
    std::set<std::uint32_t> usedLocations;
    std::vector<const spirv_cross::Resource*> unassigned;
    for (const auto& resource : resources) {
        const auto& type = compiler.get_type(resource.type_id);
        if (!compiler.has_decoration(resource.id, spv::DecorationLocation)) {
            unassigned.push_back(&resource);
            continue;
        }
        const std::uint32_t first = compiler.get_decoration(
            resource.id, spv::DecorationLocation
        );
        const std::uint32_t count = metalLocationCount(type);
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            usedLocations.insert(first + offset);
        }
    }
    std::ranges::sort(
        unassigned,
        [&](const auto* left, const auto* right) {
            return reflectedResourceName(compiler, *left) <
                reflectedResourceName(compiler, *right);
        }
    );
    for (const auto* resource : unassigned) {
        const std::uint32_t count = metalLocationCount(
            compiler.get_type(resource->type_id)
        );
        std::uint32_t first = 0;
        for (;;) {
            bool available = true;
            for (std::uint32_t offset = 0; offset < count; ++offset) {
                if (usedLocations.contains(first + offset)) {
                    available = false;
                    first += offset + 1;
                    break;
                }
            }
            if (available) break;
        }
        compiler.set_decoration(
            resource->id, spv::DecorationLocation, first
        );
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            usedLocations.insert(first + offset);
        }
    }
}

struct MetalInterfaceResource {
    std::string name;
    std::uint32_t locationCount = 1;
};

using MetalInterfaceLocationMap = std::map<std::string, std::uint32_t>;

template <typename Resources>
std::vector<MetalInterfaceResource> metalInterfaceResources(
    const spirv_cross::CompilerMSL& compiler,
    const Resources& resources
) {
    std::vector<MetalInterfaceResource> result;
    result.reserve(resources.size());
    for (const auto& resource : resources) {
        result.push_back({
            .name = reflectedResourceName(compiler, resource),
            .locationCount = metalLocationCount(
                compiler.get_type(resource.type_id)
            ),
        });
    }
    return result;
}

struct LinkedMetalInterfaceLocations {
    MetalInterfaceLocationMap vertexOutputs;
    MetalInterfaceLocationMap fragmentInputs;
};

LinkedMetalInterfaceLocations buildLinkedMetalInterfaceLocations(
    const std::vector<std::uint32_t>& vertexSpirv,
    const std::vector<std::uint32_t>& fragmentSpirv
) {
    spirv_cross::CompilerMSL vertexCompiler(vertexSpirv);
    spirv_cross::CompilerMSL fragmentCompiler(fragmentSpirv);
    const auto vertexResources = vertexCompiler.get_shader_resources();
    const auto fragmentResources = fragmentCompiler.get_shader_resources();
    const auto vertexOutputs = metalInterfaceResources(
        vertexCompiler, vertexResources.stage_outputs
    );
    const auto fragmentInputs = metalInterfaceResources(
        fragmentCompiler, fragmentResources.stage_inputs
    );
    std::map<std::string, std::uint32_t> locationCounts;
    const auto mergeResources = [&](const auto& resources) {
        for (const auto& resource : resources) {
            auto [location, inserted] = locationCounts.try_emplace(
                resource.name, resource.locationCount
            );
            if (!inserted) {
                location->second = std::max(
                    location->second, resource.locationCount
                );
            }
        }
    };
    mergeResources(vertexOutputs);
    mergeResources(fragmentInputs);

    // Wallpaper Engine varyings are one linked vertex/pixel interface. Assign
    // both sides from the same symbol table instead of retaining glslang's
    // independent per-stage auto locations.
    LinkedMetalInterfaceLocations result;
    std::uint32_t location = 0;
    for (const auto& locationEntry : locationCounts) {
        const auto& name = locationEntry.first;
        const auto locationCount = locationEntry.second;
        if (std::ranges::any_of(vertexOutputs, [&](const auto& resource) {
                return resource.name == name;
            })) {
            result.vertexOutputs.emplace(name, location);
        }
        if (std::ranges::any_of(fragmentInputs, [&](const auto& resource) {
                return resource.name == name;
            })) {
            result.fragmentInputs.emplace(name, location);
        }
        if (location > std::numeric_limits<std::uint32_t>::max() -
                locationCount) {
            throw ShaderCompileError(
                ShaderCompilePhase::crossCompilation,
                "Linked shader interface location count overflows uint32_t"
            );
        }
        location += locationCount;
    }
    return result;
}

LinkedMetalInterfaceLocations linkMetalInterfaceLocations(
    const std::vector<std::uint32_t>& vertexSpirv,
    const std::vector<std::uint32_t>& fragmentSpirv
) {
    try {
        return buildLinkedMetalInterfaceLocations(vertexSpirv, fragmentSpirv);
    } catch (const ShaderCompileError&) {
        throw;
    } catch (const std::exception& error) {
        throw ShaderCompileError(
            ShaderCompilePhase::crossCompilation,
            "Linked shader Metal interface reflection failed: " +
                std::string(error.what())
        );
    }
}

template <typename Resources>
void applyMetalInterfaceLocations(
    spirv_cross::CompilerMSL& compiler,
    const Resources& resources,
    const MetalInterfaceLocationMap& locations
) {
    for (const auto& resource : resources) {
        const auto location = locations.find(
            reflectedResourceName(compiler, resource)
        );
        if (location == locations.end()) continue;
        compiler.set_decoration(
            resource.id, spv::DecorationLocation, location->second
        );
    }
}

std::vector<std::uint32_t> generateSpirv(
    const glslang::TIntermediate& intermediate,
    std::string_view stageName
) {
    try {
        std::vector<std::uint32_t> spirv;
        glslang::GlslangToSpv(intermediate, spirv);
        if (!spirv.empty()) return spirv;
        throw ShaderCompileError(
            ShaderCompilePhase::spirvGeneration,
            std::string(stageName) + " shader generated empty SPIR-V"
        );
    } catch (const ShaderCompileError&) {
        throw;
    } catch (const std::exception& error) {
        throw ShaderCompileError(
            ShaderCompilePhase::spirvGeneration,
            std::string(stageName) + " shader SPIR-V generation failed: " +
                error.what()
        );
    }
}

void configureMetalResourceBindings(
    spirv_cross::CompilerMSL& compiler,
    const spirv_cross::ShaderResources& resources,
    spv::ExecutionModel executionModel
) {
    std::uint32_t descriptorBinding = 0;
    std::uint32_t bufferIndex = 0;
    std::uint32_t textureIndex = 0;
    std::uint32_t samplerIndex = 0;
    const auto bindBuffer = [&](const spirv_cross::Resource& resource) {
        compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 0);
        compiler.set_decoration(
            resource.id, spv::DecorationBinding, descriptorBinding
        );
        compiler.add_msl_resource_binding({
            .stage = executionModel,
            .basetype = compiler.get_type(resource.type_id).basetype,
            .desc_set = 0,
            .binding = descriptorBinding++,
            .count = 1,
            .msl_buffer = bufferIndex++,
        });
    };
    for (const auto& resource : resources.gl_plain_uniforms) {
        bindBuffer(resource);
    }
    for (const auto& resource : resources.uniform_buffers) {
        bindBuffer(resource);
    }
    for (const auto& resource : resources.sampled_images) {
        compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 0);
        compiler.set_decoration(
            resource.id, spv::DecorationBinding, descriptorBinding
        );
        compiler.add_msl_resource_binding({
            .stage = executionModel,
            .basetype = compiler.get_type(resource.type_id).basetype,
            .desc_set = 0,
            .binding = descriptorBinding++,
            .count = 1,
            .msl_texture = textureIndex++,
            .msl_sampler = samplerIndex++,
        });
    }
}

TranslatedMetalStage crossCompileMetal(
    std::vector<std::uint32_t> spirv,
    std::string_view stageName,
    spv::ExecutionModel executionModel,
    std::string_view entryPoint,
    const MetalInterfaceLocationMap& linkedInterfaceLocations
) {
    try {
        spirv_cross::CompilerMSL compiler(std::move(spirv));
        compiler.rename_entry_point("main", std::string(entryPoint), executionModel);
        spirv_cross::CompilerGLSL::Options commonOptions;
        // Wallpaper Engine GLSL uses [-w, w] clip-space depth. Metal uses
        // [0, w], so perform the conversion in generated vertex code instead
        // of changing the authored camera/projection contract.
        commonOptions.vertex.fixup_clipspace = true;
        // Metal's viewport origin is top-left while Wallpaper Engine's
        // authored GLSL projection is bottom-left. Flip clip-space Y so the
        // established top-left framebuffer contract remains unchanged.
        commonOptions.vertex.flip_vert_y = true;
        compiler.set_common_options(commonOptions);
        spirv_cross::CompilerMSL::Options metalOptions;
        metalOptions.platform = spirv_cross::CompilerMSL::Options::macOS;
        metalOptions.set_msl_version(2, 4);
        compiler.set_msl_options(metalOptions);
        const spirv_cross::ShaderResources resources =
            compiler.get_shader_resources();
        if (executionModel == spv::ExecutionModelVertex) {
            applyMetalInterfaceLocations(
                compiler, resources.stage_outputs, linkedInterfaceLocations
            );
        } else if (executionModel == spv::ExecutionModelFragment) {
            applyMetalInterfaceLocations(
                compiler, resources.stage_inputs, linkedInterfaceLocations
            );
        }
        assignMetalInterfaceLocations(compiler, resources.stage_inputs);
        assignMetalInterfaceLocations(compiler, resources.stage_outputs);
        configureMetalResourceBindings(compiler, resources, executionModel);
        if (executionModel == spv::ExecutionModelVertex) {
            for (const auto& resource : resources.stage_inputs) {
                const auto& type = compiler.get_type(resource.type_id);
                compiler.add_msl_shader_input({
                    .location = compiler.get_decoration(
                        resource.id, spv::DecorationLocation
                    ),
                    .vecsize = type.vecsize,
                });
            }
        }
        if (executionModel == spv::ExecutionModelFragment) {
            for (const auto& resource : resources.stage_outputs) {
                const auto& type = compiler.get_type(resource.type_id);
                compiler.add_msl_shader_output({
                    .location = compiler.get_decoration(
                        resource.id, spv::DecorationLocation
                    ),
                    .vecsize = type.vecsize,
                });
            }
        }
        std::string output = compiler.compile();
        if (output.empty()) {
            throw ShaderCompileError(
                ShaderCompilePhase::crossCompilation,
                std::string(stageName) + " shader generated empty MSL"
            );
        }
        TranslatedMetalStage result{.source = std::move(output)};
        const auto appendUniform = [&](const spirv_cross::Resource& resource,
                                       bool uniformBlock) {
            const auto& type = compiler.get_type(resource.type_id);
            const std::uint32_t bufferIndex =
                compiler.get_automatic_msl_resource_binding(resource.id);
            if (bufferIndex == std::numeric_limits<std::uint32_t>::max()) {
                return;
            }
            result.uniforms.push_back({
                .name = reflectedResourceName(compiler, resource),
                .type = metalValueType(type),
                .bufferIndex = bufferIndex,
                .arrayLength = literalArrayLength(type),
                .isArray = !type.array.empty(),
                .uniformBlock = uniformBlock,
            });
        };
        for (const auto& resource : resources.gl_plain_uniforms) {
            appendUniform(resource, false);
        }
        for (const auto& resource : resources.uniform_buffers) {
            appendUniform(resource, true);
        }
        for (const auto& resource : resources.sampled_images) {
            const std::uint32_t textureIndex =
                compiler.get_automatic_msl_resource_binding(resource.id);
            const std::uint32_t samplerIndex = compiler
                .get_automatic_msl_resource_binding_secondary(resource.id);
            if (textureIndex == std::numeric_limits<std::uint32_t>::max() ||
                samplerIndex == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }
            const auto& type = compiler.get_type(resource.type_id);
            const auto& sampledType = compiler.get_type(type.image.type);
            result.textures.push_back({
                .name = reflectedResourceName(compiler, resource),
                .textureIndex = textureIndex,
                .samplerIndex = samplerIndex,
                .supportedFloat2D = type.image.dim == spv::Dim2D &&
                    !type.image.arrayed && !type.image.ms &&
                    type.array.empty() &&
                    sampledType.basetype == spirv_cross::SPIRType::Float,
            });
        }
        for (const auto& resource : resources.stage_inputs) {
            const auto& type = compiler.get_type(resource.type_id);
            const std::uint32_t location = compiler.get_decoration(
                resource.id, spv::DecorationLocation
            );
            if (!compiler.is_msl_shader_input_used(location)) continue;
            result.attributes.push_back({
                .name = reflectedResourceName(compiler, resource),
                .location = location,
                .componentCount = type.vecsize,
            });
        }
        return result;
    } catch (const ShaderCompileError&) {
        throw;
    } catch (const std::exception& error) {
        throw ShaderCompileError(
            ShaderCompilePhase::crossCompilation,
            std::string(stageName) + " shader Metal cross-compilation failed: " +
                error.what()
        );
    }
}

}  // namespace

ShaderCompileError::ShaderCompileError(
    ShaderCompilePhase phase,
    std::string message
)
    : std::runtime_error(std::move(message)), phase_(phase) {}

ShaderCompilePhase ShaderCompileError::phase() const noexcept {
    return phase_;
}

TranslatedMetalShaderPair ShaderCompiler::translateToMetal(
    std::string_view vertexSource,
    std::string_view fragmentSource,
    std::string_view vertexName,
    std::string_view fragmentName
) {
    std::lock_guard lock(compilerMutex());
    validateSource(vertexSource, vertexName);
    validateSource(fragmentSource, fragmentName);
    ensureGlslangInitialized();

    std::string activeVertex = activeShaderSource(
        vertexSource,
        vertexName,
        EShLangVertex,
        ShaderCompilePhase::vertexParse
    );
    std::string activeFragment = activeShaderSource(
        fragmentSource,
        fragmentName,
        EShLangFragment,
        ShaderCompilePhase::fragmentParse
    );
    activeVertex = hoistLateUniformDeclarations(
        std::move(activeVertex),
        vertexName,
        ShaderCompilePhase::vertexParse
    );
    activeFragment = hoistLateUniformDeclarations(
        std::move(activeFragment),
        fragmentName,
        ShaderCompilePhase::fragmentParse
    );
    activeVertex = applyLinkedVaryingSemantics(
        std::move(activeVertex),
        activeFragment,
        vertexName,
        fragmentName
    );

    glslang::TShader vertexShader(EShLangVertex);
    glslang::TShader fragmentShader(EShLangFragment);
    ShaderSourceBinding vertexBinding(activeVertex, vertexName);
    ShaderSourceBinding fragmentBinding(activeFragment, fragmentName);
    configureShader(vertexShader, EShLangVertex, vertexBinding);
    configureShader(fragmentShader, EShLangFragment, fragmentBinding);

    if (!vertexShader.parse(
            &builtInResources,
            100,
            false,
            wallpaperEngineShaderMessages)) {
        throw ShaderCompileError(
            ShaderCompilePhase::vertexParse,
            std::string(vertexName) + " shader parsing failed: " +
                shaderLog(vertexShader)
        );
    }
    if (!fragmentShader.parse(
            &builtInResources,
            100,
            false,
            wallpaperEngineShaderMessages)) {
        throw ShaderCompileError(
            ShaderCompilePhase::fragmentParse,
            std::string(fragmentName) + " shader parsing failed: " +
                shaderLog(fragmentShader)
        );
    }

    glslang::TProgram program;
    program.addShader(&vertexShader);
    program.addShader(&fragmentShader);
    if (!program.link(wallpaperEngineShaderMessages)) {
        throw ShaderCompileError(
            ShaderCompilePhase::link,
            "shader program linking failed: " + programLog(program)
        );
    }
    if (!program.mapIO()) {
        throw ShaderCompileError(
            ShaderCompilePhase::link,
            "shader interface mapping failed: " + programLog(program)
        );
    }

    const glslang::TIntermediate* vertexIntermediate =
        program.getIntermediate(EShLangVertex);
    const glslang::TIntermediate* fragmentIntermediate =
        program.getIntermediate(EShLangFragment);
    if (vertexIntermediate == nullptr || fragmentIntermediate == nullptr) {
        throw ShaderCompileError(
            ShaderCompilePhase::spirvGeneration,
            "linked shader program has no stage intermediate"
        );
    }

    constexpr std::string_view vertexEntryPoint = "we_scene_vertex_main";
    constexpr std::string_view fragmentEntryPoint = "we_scene_fragment_main";
    std::vector<std::uint32_t> vertexSpirv = generateSpirv(
        *vertexIntermediate, vertexName
    );
    std::vector<std::uint32_t> fragmentSpirv = generateSpirv(
        *fragmentIntermediate, fragmentName
    );
    const LinkedMetalInterfaceLocations linkedLocations =
        linkMetalInterfaceLocations(vertexSpirv, fragmentSpirv);
    TranslatedMetalStage vertex = crossCompileMetal(
            std::move(vertexSpirv),
            vertexName,
            spv::ExecutionModelVertex,
            vertexEntryPoint,
            linkedLocations.vertexOutputs
        );
    TranslatedMetalStage fragment = crossCompileMetal(
            std::move(fragmentSpirv),
            fragmentName,
            spv::ExecutionModelFragment,
            fragmentEntryPoint,
            linkedLocations.fragmentInputs
        );
    return {
        .vertex = std::move(vertex.source),
        .fragment = std::move(fragment.source),
        .vertexEntryPoint = std::string(vertexEntryPoint),
        .fragmentEntryPoint = std::string(fragmentEntryPoint),
        .vertexUniforms = std::move(vertex.uniforms),
        .fragmentUniforms = std::move(fragment.uniforms),
        .vertexTextures = std::move(vertex.textures),
        .fragmentTextures = std::move(fragment.textures),
        .vertexAttributes = std::move(vertex.attributes),
    };
}

const char* ShaderCompiler::glslangRevision() noexcept {
    return "b775500a153f5ceb0e4b6f366b79c4c57521bb62";
}

const char* ShaderCompiler::spirvCrossRevision() noexcept {
    return "ad4d02220b01c1800e5a4e6671d6d8ca8ab07783";
}

}  // namespace we::scene
