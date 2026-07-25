#ifndef WE_SCENE_CORE_PACKAGE_HPP
#define WE_SCENE_CORE_PACKAGE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace we::scene {

struct PackageEntry {
    std::string path;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

class PackageArchive final {
public:
    [[nodiscard]] static PackageArchive parse(
        std::vector<std::uint8_t> bytes,
        std::string source
    );
    [[nodiscard]] static PackageArchive open(
        const std::filesystem::path& path
    );

    [[nodiscard]] const std::string& version() const noexcept;
    [[nodiscard]] const std::string& source() const noexcept;
    [[nodiscard]] std::size_t baseOffset() const noexcept;
    [[nodiscard]] const std::vector<PackageEntry>& entries() const noexcept;
    [[nodiscard]] bool contains(std::string_view path) const;
    [[nodiscard]] std::span<const std::uint8_t> data(
        std::string_view path
    ) const;

private:
    std::string version_;
    std::string source_;
    std::size_t baseOffset_ = 0;
    std::vector<std::uint8_t> bytes_;
    std::vector<PackageEntry> entries_;
    std::unordered_map<std::string, std::size_t> entryIndex_;
};

[[nodiscard]] std::string normalizeAssetPath(std::string_view path);

}  // namespace we::scene

#endif
