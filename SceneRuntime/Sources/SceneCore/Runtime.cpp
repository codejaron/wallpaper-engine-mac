#include <SceneCore/Runtime.hpp>

#include <SceneCore/AssetResolver.hpp>
#include <SceneCore/FormatError.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace we::scene {
namespace {

namespace fs = std::filesystem;

void assignError(
    RuntimeError& error,
    RuntimeErrorCode code,
    std::string message
) {
    error.code = code;
    error.message = std::move(message);
}

bool pathExists(
    const fs::path& path,
    RuntimeError& error,
    RuntimeErrorCode missingCode,
    const char* description
) {
    std::error_code filesystemError;
    const bool exists = fs::exists(path, filesystemError);
    if (filesystemError) {
        assignError(
            error,
            RuntimeErrorCode::filesystemFailure,
            std::string("Unable to inspect ") + description + " '" +
                path.string() + "': " + filesystemError.message()
        );
        return false;
    }

    if (!exists) {
        assignError(
            error,
            missingCode,
            std::string(description) + " does not exist: '" + path.string() + "'"
        );
        return false;
    }

    return true;
}

std::string canonicalPath(const fs::path& path, RuntimeError& error) {
    std::error_code filesystemError;
    fs::path canonical = fs::weakly_canonical(path, filesystemError);
    if (filesystemError) {
        assignError(
            error,
            RuntimeErrorCode::filesystemFailure,
            "Unable to resolve path '" + path.string() + "': " +
                filesystemError.message()
        );
        return {};
    }

    return canonical.string();
}

}  // namespace

std::unique_ptr<Runtime> Runtime::create(
    const RuntimeConfiguration& configuration,
    RuntimeError& error
) {
    error = {};

    if (configuration.assetsDirectory.empty()) {
        assignError(
            error,
            RuntimeErrorCode::invalidArgument,
            "The Wallpaper Engine assets directory is required"
        );
        return nullptr;
    }
    if (configuration.scenePackagePath.empty()) {
        assignError(
            error,
            RuntimeErrorCode::invalidArgument,
            "The scene package path is required"
        );
        return nullptr;
    }

    const fs::path assetsPath(configuration.assetsDirectory);
    if (!pathExists(
            assetsPath,
            error,
            RuntimeErrorCode::assetsDirectoryNotFound,
            "Wallpaper Engine assets directory"
        )) {
        return nullptr;
    }

    std::error_code filesystemError;
    const bool assetsIsDirectory = fs::is_directory(assetsPath, filesystemError);
    if (filesystemError) {
        assignError(
            error,
            RuntimeErrorCode::filesystemFailure,
            "Unable to inspect Wallpaper Engine assets directory '" +
                assetsPath.string() + "': " + filesystemError.message()
        );
        return nullptr;
    }
    if (!assetsIsDirectory) {
        assignError(
            error,
            RuntimeErrorCode::assetsPathNotDirectory,
            "Wallpaper Engine assets path is not a directory: '" +
                assetsPath.string() + "'"
        );
        return nullptr;
    }

    const fs::path shadersPath = assetsPath / "shaders";
    if (!pathExists(
            shadersPath,
            error,
            RuntimeErrorCode::assetsLayoutInvalid,
            "Wallpaper Engine shaders directory"
        )) {
        return nullptr;
    }
    const bool shadersIsDirectory = fs::is_directory(shadersPath, filesystemError);
    if (filesystemError || !shadersIsDirectory) {
        assignError(
            error,
            filesystemError ? RuntimeErrorCode::filesystemFailure
                            : RuntimeErrorCode::assetsLayoutInvalid,
            filesystemError
                ? "Unable to inspect Wallpaper Engine shaders directory '" +
                    shadersPath.string() + "': " + filesystemError.message()
                : "Wallpaper Engine assets directory has no shaders directory: '" +
                    assetsPath.string() + "'"
        );
        return nullptr;
    }

    const fs::path scenePackagePath(configuration.scenePackagePath);
    if (!pathExists(
            scenePackagePath,
            error,
            RuntimeErrorCode::scenePackageNotFound,
            "Scene package"
        )) {
        return nullptr;
    }

    filesystemError.clear();
    const bool scenePackageIsFile =
        fs::is_regular_file(scenePackagePath, filesystemError);
    if (filesystemError) {
        assignError(
            error,
            RuntimeErrorCode::filesystemFailure,
            "Unable to inspect scene package '" + scenePackagePath.string() +
                "': " + filesystemError.message()
        );
        return nullptr;
    }
    if (!scenePackageIsFile) {
        assignError(
            error,
            RuntimeErrorCode::scenePackageNotRegularFile,
            "Scene package is not a regular file: '" + scenePackagePath.string() + "'"
        );
        return nullptr;
    }

    std::ifstream scenePackage(scenePackagePath, std::ios::binary);
    if (!scenePackage.is_open()) {
        assignError(
            error,
            RuntimeErrorCode::scenePackageUnreadable,
            "Scene package is not readable: '" + scenePackagePath.string() + "'"
        );
        return nullptr;
    }

    std::string canonicalAssets = canonicalPath(assetsPath, error);
    if (error) {
        return nullptr;
    }
    std::string canonicalScenePackage = canonicalPath(scenePackagePath, error);
    if (error) {
        return nullptr;
    }

    try {
        auto resolver = std::make_unique<AssetResolver>(AssetResolver::create(
            canonicalAssets,
            canonicalScenePackage
        ));
        return std::unique_ptr<Runtime>(new Runtime(
            std::move(canonicalAssets),
            std::move(canonicalScenePackage),
            std::move(resolver)
        ));
    } catch (const FormatError& formatError) {
        RuntimeErrorCode code = RuntimeErrorCode::assetResolverFailure;
        if (formatError.source() == canonicalScenePackage) {
            code = RuntimeErrorCode::scenePackageInvalid;
        } else if (fs::path(formatError.source()).filename() == "gifscene.pkg") {
            code = RuntimeErrorCode::gifScenePackageInvalid;
        }
        assignError(
            error,
            code,
            formatError.what()
        );
        return nullptr;
    }
}

Runtime::Runtime(
    std::string assetsDirectory,
    std::string scenePackagePath,
    std::unique_ptr<AssetResolver> assetResolver
)
    : assetsDirectory_(std::move(assetsDirectory)),
      scenePackagePath_(std::move(scenePackagePath)),
      assetResolver_(std::move(assetResolver)) {}

Runtime::~Runtime() = default;

const std::string& Runtime::assetsDirectory() const noexcept {
    return assetsDirectory_;
}

const std::string& Runtime::scenePackagePath() const noexcept {
    return scenePackagePath_;
}

const AssetResolver& Runtime::assetResolver() const noexcept {
    return *assetResolver_;
}

}  // namespace we::scene
