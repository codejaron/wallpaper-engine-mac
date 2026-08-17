#ifndef WE_SCENE_CORE_PUPPET_MESH_HPP
#define WE_SCENE_CORE_PUPPET_MESH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    float morphMapIndex = 0.0F;
    float normal[3]{};
    float tangent[4]{};
    float texCoord[4]{};
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
    struct MorphCurve final {
        std::uint16_t id = 0;
        std::vector<float> values;
    };
    struct MorphTrack final {
        std::uint32_t flags = 0;
        float scale = 0.0F;
        std::vector<MorphCurve> curves;
    };
    std::vector<std::vector<float>> blendMapTracks;
    std::vector<std::vector<float>> bonesAlphaTracks;
    std::vector<std::vector<float>> v6ScalarTracks;
    std::vector<MorphTrack> morphTracks;
};

struct PuppetMorphModifier final {
    std::uint32_t boneIndex = 0;
    std::uint32_t ruleMode = 0;
    float ruleStart = 0.0F;
    float ruleEnd = 0.0F;
};

struct PuppetMorphTarget final {
    std::uint64_t shapeIdentity = 0;
    std::string name;
    std::vector<std::array<std::uint16_t, 3>> basePositions;
    std::vector<std::array<std::uint16_t, 3>> payload400;
    std::vector<std::array<std::uint16_t, 3>> payload800;
    std::vector<std::uint16_t> payload1000;
    std::optional<PuppetMorphModifier> modifier;
};

struct PuppetMorphData final {
    float scale = 0.0F;
    std::uint32_t vertexCount = 0;
    std::vector<PuppetMorphTarget> targets;
};

struct PuppetVertexPartRecord final {
    std::array<std::uint32_t, 3> words{};
};

struct PuppetDrawPartRecord final {
    std::array<std::uint32_t, 4> words{};
};

struct PuppetMaskRecord final {
    std::uint32_t id = 0;
    std::string materialPath;
    std::vector<std::uint32_t> firstPartIndices;
    std::vector<std::uint32_t> secondPartIndices;
};

struct PuppetSubmesh final {
    std::vector<std::string> materialPaths;
    std::uint32_t auxiliaryFlags = 0;
    std::optional<std::uint32_t> blendMapRowCount;
    std::uint32_t vertexFlags = 0;
    std::vector<PuppetVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<PuppetVertexPartRecord> vertexPartRecords;
    std::vector<PuppetDrawPartRecord> drawPartRecords;
    std::vector<PuppetMaskRecord> masks;
    std::optional<PuppetMorphData> morph;
};

struct PuppetMorphState final {
    std::size_t submeshIndex = 0;
    std::array<std::uint32_t, 12> offsets{};
    std::array<float, 12> weights{};
};

struct PuppetAnimationLayerInput final {
    int animationId = 0;
    double timeSeconds = 0.0;
    float blend = 1.0F;
    bool additive = false;
};

inline constexpr std::size_t puppetMaximumMorphTargets = 11;
inline constexpr std::size_t puppetMaximumBlendMapScalars = 16;
inline constexpr std::uint32_t puppetSubmeshBlendMapFlag = 0x2U;
inline constexpr std::uint32_t puppetSubmeshBonesAlphaFlag = 0x4U;

// Wallpaper Engine exposes Puppet transforms to shaders as mat4x3 values. The
// twelve components are stored as four consecutive vec3 columns so applying
// the matrix to (x, y, z, 1) matches the engine's row-vector MDL math.
using PuppetSkinMatrix = std::array<float, 12>;
using PuppetBlendMapState =
    std::array<float, puppetMaximumBlendMapScalars>;
using PuppetBonesAlphaState = std::vector<float>;

struct PuppetPoseState final {
    std::vector<PuppetSkinMatrix> skinMatrices;
    std::vector<PuppetSkinMatrix> inverseCurrentGlobalMatrices;
};

struct PuppetMorphModifierState final {
    std::size_t submeshIndex = 0;
    std::array<PuppetSkinMatrix, puppetMaximumMorphTargets> transforms{};
    std::array<std::array<float, 3>, puppetMaximumMorphTargets> rules{};
};

struct PuppetMesh final {
    std::string source;
    PuppetModelVersion version = PuppetModelVersion::mdlv0021;
    std::vector<PuppetSubmesh> submeshes;
    std::optional<std::size_t> blendMapSubmeshIndex;
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

/// Evaluates authored animation layers once and returns every shader transform
/// derived from the resulting skeletal pose.
[[nodiscard]] PuppetPoseState evaluatePuppetPose(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
);

[[nodiscard]] std::vector<PuppetSkinMatrix> evaluatePuppetSkinMatrices(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
);

[[nodiscard]] PuppetBlendMapState evaluatePuppetBlendMap(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
);

[[nodiscard]] PuppetBonesAlphaState evaluatePuppetBonesAlpha(
    const PuppetMesh& mesh,
    std::span<const PuppetAnimationLayerInput> layers
);

[[nodiscard]] std::vector<std::array<float, 3>> applyPuppetSkinMatrices(
    const PuppetMesh& mesh,
    std::size_t submeshIndex,
    std::span<const PuppetSkinMatrix> matrices
);

[[nodiscard]] std::vector<std::array<float, 3>> evaluatePuppetPositions(
    const PuppetMesh& mesh,
    std::size_t submeshIndex,
    std::span<const PuppetAnimationLayerInput> layers
);

[[nodiscard]] std::optional<PuppetMorphState> evaluatePuppetMorphState(
    const PuppetMesh& mesh,
    std::size_t submeshIndex,
    std::span<const PuppetAnimationLayerInput> layers
);

[[nodiscard]] std::optional<PuppetMorphModifierState>
evaluatePuppetMorphModifierState(
    const PuppetMesh& mesh,
    std::size_t submeshIndex,
    const PuppetPoseState& pose
);

}  // namespace we::scene

#endif
