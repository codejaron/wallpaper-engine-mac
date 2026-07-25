#ifndef WE_SCENE_SHADER_COMPILER_HPP
#define WE_SCENE_SHADER_COMPILER_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

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

struct TranslatedShaderPair {
    std::string vertex;
    std::string fragment;
};

class ShaderCompiler final {
public:
    [[nodiscard]] static TranslatedShaderPair translate(
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
