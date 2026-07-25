#ifndef WE_SCENE_CORE_PUPPET_MESH_HPP
#define WE_SCENE_CORE_PUPPET_MESH_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace we::scene {

enum class PuppetModelVersion {
    mdlv0021,
    mdlv0023,
};

struct PuppetVertex final {
    float position[3]{};
    float texCoord[2]{};
};

struct PuppetMesh final {
    std::string source;
    PuppetModelVersion version = PuppetModelVersion::mdlv0021;
    std::vector<PuppetVertex> vertices;
    std::vector<std::uint16_t> indices;
};

class PuppetMeshParser final {
public:
    // Parses the static MDLV mesh block used by Wallpaper Engine's Linux
    // renderer. The returned mesh owns all decoded data and is safe to share
    // between frame plans and GL preparation.
    [[nodiscard]] static PuppetMesh parse(
        std::span<const std::uint8_t> bytes,
        std::string source
    );
};

}  // namespace we::scene

#endif
