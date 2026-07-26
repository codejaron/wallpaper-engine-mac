#ifndef WE_SCENE_SHADER_PREPROCESSOR_HPP
#define WE_SCENE_SHADER_PREPROCESSOR_HPP

#include <SceneCore/AssetResolver.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace we::scene {

struct ShaderPreprocessOptions {
    std::map<std::string, int> combos;
    std::map<std::string, int> overrideCombos;
    std::map<std::string, std::string> constants;
};

using ShaderParameterDefault = std::variant<
    bool,
    std::int64_t,
    double,
    std::string,
    std::vector<double>
>;

struct ShaderParameterMetadata {
    enum class Stage { vertex, fragment };

    Stage stage = Stage::vertex;
    std::string type;
    std::string name;
    std::optional<std::string> material;
    std::optional<ShaderParameterDefault> defaultValue;
    std::string json;
};

struct PreprocessedShader {
    std::string name;
    std::string source;
    std::map<std::string, int> combos;
    std::vector<ShaderParameterMetadata> parameters;
};

struct PreprocessedShaderPair {
    PreprocessedShader vertex;
    PreprocessedShader fragment;
};

class ShaderPreprocessor final {
public:
    explicit ShaderPreprocessor(const AssetResolver& resolver) noexcept;

    [[nodiscard]] PreprocessedShaderPair preprocessFiles(
        std::string_view vertexPath,
        std::string_view fragmentPath,
        const ShaderPreprocessOptions& options = {}
    ) const;

    [[nodiscard]] PreprocessedShaderPair preprocessSources(
        std::string_view vertexSource,
        std::string_view fragmentSource,
        std::string_view vertexName,
        std::string_view fragmentName,
        const ShaderPreprocessOptions& options = {}
    ) const;

private:
    const AssetResolver& resolver_;
};

}  // namespace we::scene

#endif
