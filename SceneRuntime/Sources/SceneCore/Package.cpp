#include <SceneCore/Package.hpp>

#include <SceneCore/BinaryReader.hpp>
#include <SceneCore/FormatError.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <sstream>
#include <utility>

namespace we::scene {
namespace {

constexpr std::size_t maximumPackageEntries = 100'000;
constexpr std::size_t maximumPackagePathLength = 16 * 1024;

}  // namespace

std::string normalizeAssetPath(std::string_view path) {
    if (path.empty()) {
        throw FormatError(
            FormatErrorCode::unsafePath,
            {},
            FormatError::noOffset,
            "Asset path must not be empty"
        );
    }
    if (path.find('\0') != std::string_view::npos) {
        throw FormatError(
            FormatErrorCode::unsafePath,
            {},
            FormatError::noOffset,
            "Asset path must not contain a null byte"
        );
    }

    std::string portable(path);
    std::replace(portable.begin(), portable.end(), '\\', '/');
    const std::filesystem::path candidate(portable);
    if (candidate.is_absolute() || candidate.has_root_name() ||
        candidate.has_root_directory()) {
        throw FormatError(
            FormatErrorCode::unsafePath,
            portable,
            FormatError::noOffset,
            "Absolute asset paths are not allowed"
        );
    }

    std::filesystem::path normalized;
    for (const auto& component : candidate) {
        if (component == "." || component.empty()) {
            continue;
        }
        if (component == "..") {
            throw FormatError(
                FormatErrorCode::unsafePath,
                portable,
                FormatError::noOffset,
                "Parent traversal is not allowed in asset paths"
            );
        }
        normalized /= component;
    }

    const std::string result = normalized.generic_string();
    if (result.empty()) {
        throw FormatError(
            FormatErrorCode::unsafePath,
            portable,
            FormatError::noOffset,
            "Asset path resolves to an empty path"
        );
    }
    return result;
}

PackageArchive PackageArchive::parse(
    std::vector<std::uint8_t> bytes,
    std::string source
) {
    BinaryReader reader(bytes, source);
    PackageArchive archive;
    archive.source_ = std::move(source);

    archive.version_ = reader.readSizedString(64);
    if (!archive.version_.starts_with("PKGV")) {
        throw FormatError(
            FormatErrorCode::invalidMagic,
            archive.source_,
            sizeof(std::uint32_t),
            "Expected a PKGV package header, got '" + archive.version_ + "'"
        );
    }

    const std::uint32_t entryCount = reader.readUInt32();
    if (entryCount > maximumPackageEntries) {
        std::ostringstream message;
        message << "Package declares " << entryCount
                << " entries, exceeding the limit of "
                << maximumPackageEntries;
        throw FormatError(
            FormatErrorCode::invalidValue,
            archive.source_,
            reader.position() - sizeof(std::uint32_t),
            message.str()
        );
    }

    archive.entries_.reserve(entryCount);
    for (std::uint32_t index = 0; index < entryCount; ++index) {
        const std::size_t entryOffset = reader.position();
        PackageEntry entry;
        const std::string rawPath = reader.readSizedString(
            maximumPackagePathLength
        );
        try {
            entry.path = normalizeAssetPath(rawPath);
        } catch (const FormatError& error) {
            throw FormatError(
                error.code(),
                archive.source_,
                entryOffset,
                "Invalid package entry path '" + rawPath + "': " + error.what()
            );
        }
        entry.offset = reader.readUInt32();
        entry.length = reader.readUInt32();

        if (archive.entryIndex_.contains(entry.path)) {
            throw FormatError(
                FormatErrorCode::duplicateEntry,
                archive.source_,
                entryOffset,
                "Package contains duplicate entry '" + entry.path + "'"
            );
        }
        archive.entryIndex_.emplace(entry.path, archive.entries_.size());
        archive.entries_.push_back(std::move(entry));
    }

    archive.baseOffset_ = reader.position();
    const std::size_t payloadSize = bytes.size() - archive.baseOffset_;
    for (const auto& entry : archive.entries_) {
        const std::size_t offset = entry.offset;
        const std::size_t length = entry.length;
        if (offset > payloadSize || length > payloadSize - offset) {
            std::ostringstream message;
            message << "Package entry '" << entry.path
                    << "' points outside the payload (offset " << offset
                    << ", length " << length << ", payload "
                    << payloadSize << ')';
            throw FormatError(
                FormatErrorCode::invalidOffset,
                archive.source_,
                archive.baseOffset_,
                message.str()
            );
        }
    }

    archive.bytes_ = std::move(bytes);
    return archive;
}

PackageArchive PackageArchive::open(const std::filesystem::path& path) {
    return parse(readBinaryFile(path), path.string());
}

const std::string& PackageArchive::version() const noexcept {
    return version_;
}

const std::string& PackageArchive::source() const noexcept {
    return source_;
}

std::size_t PackageArchive::baseOffset() const noexcept {
    return baseOffset_;
}

const std::vector<PackageEntry>& PackageArchive::entries() const noexcept {
    return entries_;
}

bool PackageArchive::contains(std::string_view path) const {
    return entryIndex_.contains(normalizeAssetPath(path));
}

std::span<const std::uint8_t> PackageArchive::data(
    std::string_view path
) const {
    const std::string normalized = normalizeAssetPath(path);
    const auto found = entryIndex_.find(normalized);
    if (found == entryIndex_.end()) {
        throw FormatError(
            FormatErrorCode::assetNotFound,
            source_,
            FormatError::noOffset,
            "Package entry not found: '" + normalized + "'"
        );
    }

    const PackageEntry& entry = entries_[found->second];
    return std::span<const std::uint8_t>(bytes_).subspan(
        baseOffset_ + entry.offset,
        entry.length
    );
}

}  // namespace we::scene
