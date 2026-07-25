#include <SceneCore/AssetResolver.hpp>

#include <SceneCore/BinaryReader.hpp>
#include <SceneCore/FormatError.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <utility>

namespace we::scene {
namespace {

namespace fs = std::filesystem;

// Derived from linux-wallpaperengine WallpaperApplication.cpp at
// b016d7d1fdcf4e5fd2f9c9fa420a8aaa07fee02d (GPL-3.0). These are runtime
// assets, not a renderer shortcut: Wallpaper Engine effect `copy` commands
// execute this shader through the same preprocessing/translation path as any
// authored material pass.
constexpr std::string_view copyCommandFragmentShader = R"(
uniform sampler2D g_Texture0;
in vec2 v_TexCoord;
void main () {
out_FragColor = texture (g_Texture0, v_TexCoord);
}
)";

constexpr std::string_view copyCommandVertexShader = R"(
in vec3 a_Position;
in vec2 a_TexCoord;
out vec2 v_TexCoord;
void main () {
gl_Position = vec4 (a_Position, 1.0);
v_TexCoord = a_TexCoord;
}
)";

fs::path canonicalDirectory(const fs::path& path, std::string_view label) {
    std::error_code error;
    const fs::path canonical = fs::canonical(path, error);
    if (error) {
        throw FormatError(
            FormatErrorCode::ioFailure,
            path.string(),
            FormatError::noOffset,
            "Unable to resolve " + std::string(label) + ": " + error.message()
        );
    }
    if (!fs::is_directory(canonical, error) || error) {
        throw FormatError(
            FormatErrorCode::invalidArgument,
            canonical.string(),
            FormatError::noOffset,
            error ? "Unable to inspect " + std::string(label) + ": " +
                    error.message()
                  : std::string(label) + " is not a directory"
        );
    }
    return canonical;
}

bool pathIsWithin(const fs::path& root, const fs::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    while (rootPart != root.end() && candidatePart != candidate.end()) {
        if (*rootPart != *candidatePart) {
            return false;
        }
        ++rootPart;
        ++candidatePart;
    }
    return rootPart == root.end();
}

}  // namespace

AssetResolver AssetResolver::create(
    const fs::path& assetsDirectory,
    const fs::path& scenePackagePath
) {
    AssetResolver resolver;
    resolver.assetsDirectory_ = canonicalDirectory(
        assetsDirectory,
        "Wallpaper Engine assets directory"
    );
    resolver.projectDirectory_ = canonicalDirectory(
        scenePackagePath.parent_path(),
        "scene project directory"
    );

    std::error_code error;
    const fs::path canonicalPackage = fs::canonical(scenePackagePath, error);
    if (error || !fs::is_regular_file(canonicalPackage, error) || error) {
        throw FormatError(
            FormatErrorCode::invalidArgument,
            scenePackagePath.string(),
            FormatError::noOffset,
            error ? "Unable to inspect scene package: " + error.message()
                  : "Scene package is not a regular file"
        );
    }
    if (!pathIsWithin(resolver.projectDirectory_, canonicalPackage)) {
        throw FormatError(
            FormatErrorCode::unsafePath,
            canonicalPackage.string(),
            FormatError::noOffset,
            "Scene package resolves outside its project directory"
        );
    }
    resolver.scenePackage_ = PackageArchive::open(canonicalPackage);
    resolver.addVirtual(
        "shaders/commands/copy.frag", copyCommandFragmentShader
    );
    resolver.addVirtual(
        "shaders/commands/copy.vert", copyCommandVertexShader
    );

    const fs::path gifPackage = resolver.projectDirectory_ / "gifscene.pkg";
    error.clear();
    const bool gifExists = fs::exists(gifPackage, error);
    if (error) {
        throw FormatError(
            FormatErrorCode::ioFailure,
            gifPackage.string(),
            FormatError::noOffset,
            "Unable to inspect optional gifscene.pkg: " + error.message()
        );
    }
    if (gifExists) {
        const fs::path canonicalGifPackage = fs::canonical(gifPackage, error);
        if (error) {
            throw FormatError(
                FormatErrorCode::ioFailure,
                gifPackage.string(),
                FormatError::noOffset,
                "Unable to resolve gifscene.pkg: " + error.message()
            );
        }
        if (!pathIsWithin(resolver.projectDirectory_, canonicalGifPackage)) {
            throw FormatError(
                FormatErrorCode::unsafePath,
                gifPackage.string(),
                FormatError::noOffset,
                "gifscene.pkg resolves outside its project directory"
            );
        }
        if (!fs::is_regular_file(canonicalGifPackage, error) || error) {
            throw FormatError(
                FormatErrorCode::invalidArgument,
                gifPackage.string(),
                FormatError::noOffset,
                error ? "Unable to inspect gifscene.pkg: " + error.message()
                      : "gifscene.pkg is not a regular file"
            );
        }
        try {
            resolver.gifScenePackage_ = PackageArchive::parse(
                readBinaryFile(canonicalGifPackage),
                gifPackage.string()
            );
        } catch (const FormatError& formatError) {
            if (formatError.source() == gifPackage.string()) {
                throw;
            }
            throw FormatError(
                formatError.code(),
                gifPackage.string(),
                formatError.offset(),
                formatError.what()
            );
        }
    }

    const fs::path shaders = resolver.assetsDirectory_ / "shaders";
    error.clear();
    if (!fs::is_directory(shaders, error) || error) {
        throw FormatError(
            FormatErrorCode::invalidArgument,
            resolver.assetsDirectory_.string(),
            FormatError::noOffset,
            error ? "Unable to inspect assets shaders directory: " +
                    error.message()
                  : "Wallpaper Engine assets directory has no shaders directory"
        );
    }
    return resolver;
}

void AssetResolver::addVirtual(
    std::string_view path,
    std::span<const std::uint8_t> bytes
) {
    virtualFiles_.insert_or_assign(
        normalizeAssetPath(path),
        std::vector<std::uint8_t>(bytes.begin(), bytes.end())
    );
}

void AssetResolver::addVirtual(
    std::string_view path,
    std::string_view contents
) {
    addVirtual(
        path,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(contents.data()),
            contents.size()
        )
    );
}

bool AssetResolver::contains(std::string_view path) const {
    try {
        static_cast<void>(resolve(path));
        return true;
    } catch (const FormatError& error) {
        if (error.code() == FormatErrorCode::assetNotFound) {
            return false;
        }
        throw;
    }
}

ResolvedAsset AssetResolver::resolve(std::string_view path) const {
    const std::string normalized = normalizeAssetPath(path);

    if (const auto found = virtualFiles_.find(normalized);
        found != virtualFiles_.end()) {
        return {
            .logicalPath = normalized,
            .source = AssetSource::virtualMemory,
            .physicalPath = std::nullopt,
            .bytes = found->second,
        };
    }
    if (auto asset = resolveDirectory(
            projectDirectory_, normalized, AssetSource::projectDirectory
        )) {
        return std::move(*asset);
    }
    if (auto asset = resolvePackage(
            scenePackage_, normalized, AssetSource::scenePackage
        )) {
        return std::move(*asset);
    }
    if (gifScenePackage_) {
        if (auto asset = resolvePackage(
                *gifScenePackage_, normalized, AssetSource::gifScenePackage
            )) {
            return std::move(*asset);
        }
    }
    if (auto asset = resolveDirectory(
            assetsDirectory_, normalized, AssetSource::officialAssets
        )) {
        return std::move(*asset);
    }

    throw FormatError(
        FormatErrorCode::assetNotFound,
        normalized,
        FormatError::noOffset,
        "Asset was not found in virtual memory, project files, scene.pkg, "
        "gifscene.pkg, or the configured official assets directory"
    );
}

std::string AssetResolver::readString(std::string_view path) const {
    const auto asset = resolve(path);
    return {
        reinterpret_cast<const char*>(asset.bytes.data()),
        asset.bytes.size(),
    };
}

Texture AssetResolver::parseTexture(std::string_view path) const {
    const ResolvedAsset textureAsset = resolve(path);
    std::optional<std::string> metadata;
    const std::string metadataPath = normalizeAssetPath(path) + "-json";
    try {
        metadata = readString(metadataPath);
    } catch (const FormatError& error) {
        if (error.code() != FormatErrorCode::assetNotFound) {
            throw;
        }
    }

    const std::string source = std::string(assetSourceName(textureAsset.source)) +
        ":" + textureAsset.logicalPath;
    return TextureParser::parse(
        textureAsset.bytes,
        source,
        metadata ? std::optional<std::string_view>(*metadata) : std::nullopt
    );
}

const PackageArchive& AssetResolver::scenePackage() const noexcept {
    return scenePackage_;
}

const std::optional<PackageArchive>& AssetResolver::gifScenePackage()
    const noexcept {
    return gifScenePackage_;
}

const fs::path& AssetResolver::assetsDirectory() const noexcept {
    return assetsDirectory_;
}

const fs::path& AssetResolver::projectDirectory() const noexcept {
    return projectDirectory_;
}

std::optional<ResolvedAsset> AssetResolver::resolveDirectory(
    const fs::path& root,
    const std::string& normalizedPath,
    AssetSource source
) const {
    const fs::path candidate = root / fs::path(normalizedPath);
    std::error_code error;
    const bool exists = fs::exists(candidate, error);
    if (error) {
        throw FormatError(
            FormatErrorCode::ioFailure,
            candidate.string(),
            FormatError::noOffset,
            "Unable to inspect asset path: " + error.message()
        );
    }
    if (!exists) {
        return std::nullopt;
    }

    const fs::path canonical = fs::canonical(candidate, error);
    if (error) {
        throw FormatError(
            FormatErrorCode::ioFailure,
            candidate.string(),
            FormatError::noOffset,
            "Unable to resolve asset path: " + error.message()
        );
    }
    if (!pathIsWithin(root, canonical)) {
        throw FormatError(
            FormatErrorCode::unsafePath,
            candidate.string(),
            FormatError::noOffset,
            "Asset path resolves outside its mounted directory"
        );
    }
    const bool isRegularFile = fs::is_regular_file(canonical, error);
    if (error) {
        throw FormatError(
            FormatErrorCode::ioFailure,
            canonical.string(),
            FormatError::noOffset,
            "Unable to inspect resolved asset: " + error.message()
        );
    }
    if (!isRegularFile) {
        return std::nullopt;
    }

    return ResolvedAsset {
        .logicalPath = normalizedPath,
        .source = source,
        .physicalPath = canonical,
        .bytes = readBinaryFile(canonical),
    };
}

std::optional<ResolvedAsset> AssetResolver::resolvePackage(
    const PackageArchive& package,
    const std::string& normalizedPath,
    AssetSource source
) const {
    if (!package.contains(normalizedPath)) {
        return std::nullopt;
    }
    const auto data = package.data(normalizedPath);
    return ResolvedAsset {
        .logicalPath = normalizedPath,
        .source = source,
        .physicalPath = std::nullopt,
        .bytes = std::vector<std::uint8_t>(data.begin(), data.end()),
    };
}

const char* assetSourceName(AssetSource source) noexcept {
    switch (source) {
        case AssetSource::virtualMemory:
            return "virtual";
        case AssetSource::projectDirectory:
            return "project";
        case AssetSource::scenePackage:
            return "scene.pkg";
        case AssetSource::gifScenePackage:
            return "gifscene.pkg";
        case AssetSource::officialAssets:
            return "official-assets";
    }
    return "unknown";
}

}  // namespace we::scene
