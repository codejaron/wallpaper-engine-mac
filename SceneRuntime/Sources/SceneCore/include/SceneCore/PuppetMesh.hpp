#ifndef WE_SCENE_CORE_PUPPET_MESH_HPP
#define WE_SCENE_CORE_PUPPET_MESH_HPP

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace we::scene {

enum class PuppetModelVersion {
    mdlv0013,
    mdlv0021,
    mdlv0023,
};

struct PuppetVertex final {
    float position[3]{};
    float texCoord[2]{};
    std::uint32_t boneIndices[4]{};
    float boneWeights[4]{};
};

struct PuppetBone final {
    int parent = -1;
    std::array<float, 16> bindLocalMatrix{};
};

struct PuppetTransformSample final {
    std::array<float, 3> translation{};
    std::array<float, 3> rotation{};
    std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
};

struct PuppetAnimation final {
    int id = 0;
    std::string name;
    std::string playbackMode;
    float framesPerSecond = 0.0F;
    std::uint32_t frameCount = 0;
    std::vector<std::vector<PuppetTransformSample>> boneTracks;
};

struct PuppetAnimationLayerInput final {
    int animationId = 0;
    double timeSeconds = 0.0;
    float blend = 1.0F;
    bool additive = false;
};

struct PuppetMesh final {
    std::string source;
    PuppetModelVersion version = PuppetModelVersion::mdlv0021;
    std::vector<PuppetVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<PuppetBone> bones;
    std::vector<PuppetAnimation> animations;

    [[nodiscard]] const PuppetAnimation* animation(int id) const noexcept;
};

class PuppetMeshParser final {
public:
    // Parses a static Wallpaper Engine MDLV mesh block. The returned mesh owns
    // all decoded data and is safe to share between frame plans and GL
    // preparation.
    [[nodiscard]] static PuppetMesh parse(
        std::span<const std::uint8_t> bytes,
        std::string source
    );
};

/// Evaluates authored animation layers and applies weighted skeletal skinning.
/// With no bones or active layers, the returned positions equal the bind mesh.
[[nodiscard]] std::vector<std::array<float, 3>> evaluatePuppetPositions(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
);

}  // namespace we::scene

#endif
