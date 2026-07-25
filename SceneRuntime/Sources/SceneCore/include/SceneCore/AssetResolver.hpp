#ifndef WE_SCENE_CORE_ASSET_RESOLVER_HPP
#define WE_SCENE_CORE_ASSET_RESOLVER_HPP

#include <SceneCore/Package.hpp>
#include <SceneCore/Texture.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace we::scene {

enum class AssetSource {
    virtualMemory,
    projectDirectory,
    scenePackage,
    gifScenePackage,
    officialAssets,
};

struct ResolvedAsset {
    std::string logicalPath;
    AssetSource source = AssetSource::virtualMemory;
    std::optional<std::filesystem::path> physicalPath;
    std::vector<std::uint8_t> bytes;
};

class AssetResolver final {
public:
    [[nodiscard]] static AssetResolver create(
        const std::filesystem::path& assetsDirectory,
        const std::filesystem::path& scenePackagePath
    );

    void addVirtual(
        std::string_view path,
        std::span<const std::uint8_t> bytes
    );
    void addVirtual(std::string_view path, std::string_view contents);

    [[nodiscard]] bool contains(std::string_view path) const;
    [[nodiscard]] ResolvedAsset resolve(std::string_view path) const;
    [[nodiscard]] std::string readString(std::string_view path) const;
    [[nodiscard]] Texture parseTexture(std::string_view path) const;

    [[nodiscard]] const PackageArchive& scenePackage() const noexcept;
    [[nodiscard]] const std::optional<PackageArchive>& gifScenePackage()
        const noexcept;
    [[nodiscard]] const std::filesystem::path& assetsDirectory()
        const noexcept;
    [[nodiscard]] const std::filesystem::path& projectDirectory()
        const noexcept;

private:
    [[nodiscard]] std::optional<ResolvedAsset> resolveDirectory(
        const std::filesystem::path& root,
        const std::string& normalizedPath,
        AssetSource source
    ) const;
    [[nodiscard]] std::optional<ResolvedAsset> resolvePackage(
        const PackageArchive& package,
        const std::string& normalizedPath,
        AssetSource source
    ) const;

    std::filesystem::path assetsDirectory_;
    std::filesystem::path projectDirectory_;
    PackageArchive scenePackage_;
    std::optional<PackageArchive> gifScenePackage_;
    std::unordered_map<std::string, std::vector<std::uint8_t>> virtualFiles_;
};

[[nodiscard]] const char* assetSourceName(AssetSource source) noexcept;

}  // namespace we::scene

#endif
