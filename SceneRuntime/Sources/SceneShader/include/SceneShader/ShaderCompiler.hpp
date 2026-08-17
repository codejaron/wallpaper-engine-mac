#ifndef WE_SCENE_SHADER_COMPILER_HPP
#define WE_SCENE_SHADER_COMPILER_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace we::scene {

enum class ShaderCompilePhase : std::uint32_t {
    input = 0,
    vertexParse = 1,
    fragmentParse = 2,
    link = 3,
    spirvGeneration = 4,
    crossCompilation = 5,
};

class ShaderCompileError final : public std::runtime_error {
public:
    ShaderCompileError(ShaderCompilePhase phase, std::string message);

    [[nodiscard]] ShaderCompilePhase phase() const noexcept;

private:
    ShaderCompilePhase phase_;
};

struct TranslatedMetalShaderPair {
    enum class TextureDimension : std::uint32_t {
        unsupported = 0,
        texture2D = 2,
        texture3D = 3,
    };

    enum class ValueType : std::uint32_t {
        boolean,
        int32,
        uint32,
        float1,
        float2,
        float3,
        float4,
        float3x3,
        float4x3,
        float4x4,
        unsupported,
    };

    struct UniformBinding {
        std::string name;
        ValueType type = ValueType::unsupported;
        std::uint32_t bufferIndex = 0;
        std::uint32_t arrayLength = 1;
        bool isArray = false;
        bool uniformBlock = false;
    };

    struct TextureBinding {
        std::string name;
        std::uint32_t textureIndex = 0;
        std::uint32_t samplerIndex = 0;
        TextureDimension dimension = TextureDimension::unsupported;
        bool comparison = false;
    };

    struct VertexAttribute {
        std::string name;
        std::uint32_t location = 0;
        std::uint32_t componentCount = 0;
    };

    std::string vertex;
    std::string fragment;
    std::string vertexEntryPoint;
    std::string fragmentEntryPoint;
    std::vector<UniformBinding> vertexUniforms;
    std::vector<UniformBinding> fragmentUniforms;
    std::vector<TextureBinding> vertexTextures;
    std::vector<TextureBinding> fragmentTextures;
    std::vector<VertexAttribute> vertexAttributes;
};

class ShaderCompiler final {
public:
    [[nodiscard]] static TranslatedMetalShaderPair translateToMetal(
        std::string_view vertexSource,
        std::string_view fragmentSource,
        std::string_view vertexName = "vertex",
        std::string_view fragmentName = "fragment"
    );

    [[nodiscard]] static const char* glslangRevision() noexcept;
    [[nodiscard]] static const char* spirvCrossRevision() noexcept;
};

}  // namespace we::scene

#endif
