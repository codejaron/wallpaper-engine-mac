#ifndef WE_SCENE_CORE_RUNTIME_HPP
#define WE_SCENE_CORE_RUNTIME_HPP

#include <cstdint>
#include <memory>
#include <string>

namespace we::scene {

class AssetResolver;

enum class RuntimeErrorCode : std::int32_t {
    none = 0,
    invalidArgument = 1,
    assetsDirectoryNotFound = 2,
    assetsPathNotDirectory = 3,
    assetsLayoutInvalid = 4,
    scenePackageNotFound = 5,
    scenePackageNotRegularFile = 6,
    scenePackageUnreadable = 7,
    filesystemFailure = 8,
    internalFailure = 9,
    scenePackageInvalid = 10,
    gifScenePackageInvalid = 11,
    assetResolverFailure = 12,
};

struct RuntimeError {
    RuntimeErrorCode code = RuntimeErrorCode::none;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != RuntimeErrorCode::none;
    }
};

struct RuntimeConfiguration {
    std::string assetsDirectory;
    std::string scenePackagePath;
};

// Owns validated external inputs and the asset resolver. Scene graph loading
// and rendering are separate lifecycle layers built on top of this runtime.
class Runtime final {
public:
    [[nodiscard]] static std::unique_ptr<Runtime> create(
        const RuntimeConfiguration& configuration,
        RuntimeError& error
    );

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    ~Runtime();

    [[nodiscard]] const std::string& assetsDirectory() const noexcept;
    [[nodiscard]] const std::string& scenePackagePath() const noexcept;
    [[nodiscard]] const AssetResolver& assetResolver() const noexcept;

private:
    Runtime(
        std::string assetsDirectory,
        std::string scenePackagePath,
        std::unique_ptr<AssetResolver> assetResolver
    );

    std::string assetsDirectory_;
    std::string scenePackagePath_;
    std::unique_ptr<AssetResolver> assetResolver_;
};

}  // namespace we::scene

#endif
