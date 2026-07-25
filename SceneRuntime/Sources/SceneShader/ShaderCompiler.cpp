#include <SceneShader/ShaderCompiler.hpp>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Include/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_glsl.hpp>

#include <array>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace we::scene {
namespace {

constexpr std::size_t maximumShaderSourceBytes = 16 * 1024 * 1024;

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

std::string crossCompile(
    const glslang::TIntermediate& intermediate,
    std::string_view stageName
) {
    try {
        std::vector<std::uint32_t> spirv;
        glslang::GlslangToSpv(intermediate, spirv);
        if (spirv.empty()) {
            throw ShaderCompileError(
                ShaderCompilePhase::spirvGeneration,
                std::string(stageName) + " shader generated empty SPIR-V"
            );
        }

        spirv_cross::CompilerGLSL compiler(std::move(spirv));
        spirv_cross::CompilerGLSL::Options options;
        options.version = 330;
        options.es = false;
        // macOS exposes OpenGL 4.1 Core.  Binding layout qualifiers require
        // GLSL 4.20/ARB_shading_language_420pack, so sampler units are bound
        // explicitly by the renderer instead of being baked into GLSL.
        options.enable_420pack_extension = false;
        compiler.set_common_options(options);
        std::string output = compiler.compile();
        if (output.empty()) {
            throw ShaderCompileError(
                ShaderCompilePhase::crossCompilation,
                std::string(stageName) + " shader generated empty GLSL"
            );
        }
        return output;
    } catch (const ShaderCompileError&) {
        throw;
    } catch (const std::exception& error) {
        throw ShaderCompileError(
            ShaderCompilePhase::crossCompilation,
            std::string(stageName) + " shader cross-compilation failed: " +
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

TranslatedShaderPair ShaderCompiler::translate(
    std::string_view vertexSource,
    std::string_view fragmentSource,
    std::string_view vertexName,
    std::string_view fragmentName
) {
    std::lock_guard lock(compilerMutex());
    validateSource(vertexSource, vertexName);
    validateSource(fragmentSource, fragmentName);
    ensureGlslangInitialized();

    glslang::TShader vertexShader(EShLangVertex);
    glslang::TShader fragmentShader(EShLangFragment);
    ShaderSourceBinding vertexBinding(vertexSource, vertexName);
    ShaderSourceBinding fragmentBinding(fragmentSource, fragmentName);
    configureShader(
        vertexShader,
        EShLangVertex,
        vertexBinding
    );
    configureShader(
        fragmentShader,
        EShLangFragment,
        fragmentBinding
    );

    if (!vertexShader.parse(&builtInResources, 100, false, EShMsgDefault)) {
        throw ShaderCompileError(
            ShaderCompilePhase::vertexParse,
            std::string(vertexName) + " shader parsing failed: " +
                shaderLog(vertexShader)
        );
    }
    if (!fragmentShader.parse(&builtInResources, 100, false, EShMsgDefault)) {
        throw ShaderCompileError(
            ShaderCompilePhase::fragmentParse,
            std::string(fragmentName) + " shader parsing failed: " +
                shaderLog(fragmentShader)
        );
    }

    glslang::TProgram program;
    program.addShader(&vertexShader);
    program.addShader(&fragmentShader);
    if (!program.link(EShMsgDefault)) {
        throw ShaderCompileError(
            ShaderCompilePhase::link,
            "shader program linking failed: " + programLog(program)
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

    return {
        .vertex = crossCompile(*vertexIntermediate, vertexName),
        .fragment = crossCompile(*fragmentIntermediate, fragmentName),
    };
}

const char* ShaderCompiler::glslangRevision() noexcept {
    return "b775500a153f5ceb0e4b6f366b79c4c57521bb62";
}

const char* ShaderCompiler::spirvCrossRevision() noexcept {
    return "ad4d02220b01c1800e5a4e6671d6d8ca8ab07783";
}

}  // namespace we::scene
