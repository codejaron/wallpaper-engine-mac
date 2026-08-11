#ifndef WE_SCENE_RUNTIME_BRIDGE_INTERNAL_HPP
#define WE_SCENE_RUNTIME_BRIDGE_INTERNAL_HPP

#include <SceneCore/Runtime.hpp>
#include <SceneFrameGraph/SceneFrameGraph.hpp>
#include <SceneMetal/FramePlanExecutor.hpp>
#include <SceneMetal/SceneMetal.hpp>
#include <SceneGraph/SceneGraph.hpp>
#include <SceneModel/SceneModel.hpp>
#include <SceneRuntimeBridge/SceneRuntimeBridge.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct WESceneRuntime {
    std::shared_ptr<const we::scene::Runtime> runtime;
};

struct WESceneRuntimeError {
    WESceneRuntimeErrorCode code = WE_SCENE_RUNTIME_ERROR_NONE;
    std::string message;
    std::string assetPath;
    std::string jsonPointer;
    std::vector<std::string> referenceChain;
};

struct WESceneModel {
    std::shared_ptr<we::scene::SceneModel> model;
    std::mutex scratchMutex;
    std::string valueScratch;
};

struct WESceneGraph {
    std::shared_ptr<we::scene::SceneGraph> graph;
};

struct WESceneGraphSnapshot {
    we::scene::SceneGraphSnapshot snapshot;
};

struct WESceneFrameGraph {
    std::shared_ptr<we::scene::SceneFrameGraph> graph;
};

struct WESceneFramePlan {
    we::scene::FramePlan plan;
};

struct WESceneFrameExecutor {
    std::unique_ptr<we::scene::metal::FramePlanExecutor> executor;
};

namespace we::scene::bridge {

inline void clearError(WESceneRuntimeErrorRef* outError) noexcept {
    if (outError != nullptr) {
        *outError = nullptr;
    }
}

inline void assignError(
    WESceneRuntimeErrorRef* outError,
    WESceneRuntimeErrorCode code,
    std::string_view message
) noexcept {
    if (outError == nullptr) {
        return;
    }
    try {
        auto error = std::make_unique<WESceneRuntimeError>();
        error->code = code;
        error->message.assign(message);
        *outError = error.release();
    } catch (...) {
        *outError = nullptr;
    }
}

inline void assignStructuredError(
    WESceneRuntimeErrorRef* outError,
    WESceneRuntimeErrorCode code,
    std::string_view message,
    std::string_view assetPath,
    std::string_view jsonPointer,
    const std::vector<std::string>& referenceChain
) noexcept {
    if (outError == nullptr) {
        return;
    }
    try {
        auto error = std::make_unique<WESceneRuntimeError>();
        error->code = code;
        error->message.assign(message);
        error->assetPath.assign(assetPath);
        error->jsonPointer.assign(jsonPointer);
        error->referenceChain = referenceChain;
        *outError = error.release();
    } catch (...) {
        *outError = nullptr;
    }
}

inline void assignExceptionError(
    WESceneRuntimeErrorRef* outError,
    const char* operation,
    const char* detail
) noexcept {
    if (outError == nullptr) {
        return;
    }
    try {
        std::string message = "Unexpected failure while ";
        message += operation != nullptr ? operation : "performing an operation";
        message += ": ";
        message += detail != nullptr ? detail : "unknown C++ exception";
        assignError(
            outError,
            WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE,
            message
        );
    } catch (...) {
        *outError = nullptr;
    }
}

inline WESceneRuntimeErrorCode modelErrorCode(
    SceneModelErrorCode code
) noexcept {
    switch (code) {
        case SceneModelErrorCode::invalidJson:
            return WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_JSON;
        case SceneModelErrorCode::missingField:
            return WE_SCENE_RUNTIME_ERROR_SCENE_MISSING_FIELD;
        case SceneModelErrorCode::typeMismatch:
            return WE_SCENE_RUNTIME_ERROR_SCENE_TYPE_MISMATCH;
        case SceneModelErrorCode::invalidValue:
            return WE_SCENE_RUNTIME_ERROR_SCENE_INVALID_VALUE;
        case SceneModelErrorCode::unsupportedProject:
            return WE_SCENE_RUNTIME_ERROR_SCENE_UNSUPPORTED_PROJECT;
        case SceneModelErrorCode::unsupportedObject:
            return WE_SCENE_RUNTIME_ERROR_SCENE_UNSUPPORTED_OBJECT;
        case SceneModelErrorCode::duplicateId:
            return WE_SCENE_RUNTIME_ERROR_SCENE_DUPLICATE_ID;
        case SceneModelErrorCode::danglingReference:
            return WE_SCENE_RUNTIME_ERROR_SCENE_DANGLING_REFERENCE;
        case SceneModelErrorCode::referenceCycle:
            return WE_SCENE_RUNTIME_ERROR_SCENE_REFERENCE_CYCLE;
        case SceneModelErrorCode::assetFailure:
            return WE_SCENE_RUNTIME_ERROR_SCENE_ASSET_FAILURE;
    }
    return WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE;
}

inline void assignModelError(
    WESceneRuntimeErrorRef* outError,
    const SceneModelError& error
) noexcept {
    assignStructuredError(
        outError,
        modelErrorCode(error.code()),
        error.what(),
        error.assetPath(),
        error.jsonPointer(),
        error.referenceChain()
    );
}

inline WESceneRuntimeErrorCode metalErrorCode(
    metal::ErrorCode code
) noexcept {
    switch (code) {
        case metal::ErrorCode::invalidArgument:
            return WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE;
        case metal::ErrorCode::contextCreation:
            return WE_SCENE_RUNTIME_ERROR_METAL_CONTEXT_CREATION;
        case metal::ErrorCode::unsupportedContext:
            return WE_SCENE_RUNTIME_ERROR_METAL_UNSUPPORTED_CONTEXT;
        case metal::ErrorCode::shaderCompilation:
            return WE_SCENE_RUNTIME_ERROR_METAL_SHADER_COMPILATION;
        case metal::ErrorCode::programLink:
            return WE_SCENE_RUNTIME_ERROR_METAL_PROGRAM_LINK;
        case metal::ErrorCode::framebufferCreation:
            return WE_SCENE_RUNTIME_ERROR_METAL_FRAMEBUFFER_CREATION;
        case metal::ErrorCode::draw:
            return WE_SCENE_RUNTIME_ERROR_METAL_DRAW;
        case metal::ErrorCode::readback:
            return WE_SCENE_RUNTIME_ERROR_METAL_READBACK;
        case metal::ErrorCode::internalFailure:
            return WE_SCENE_RUNTIME_ERROR_METAL_INTERNAL_FAILURE;
        case metal::ErrorCode::textureDecode:
            return WE_SCENE_RUNTIME_ERROR_METAL_TEXTURE_DECODE;
        case metal::ErrorCode::textureUpload:
            return WE_SCENE_RUNTIME_ERROR_METAL_TEXTURE_UPLOAD;
        case metal::ErrorCode::resourceValidation:
            return WE_SCENE_RUNTIME_ERROR_METAL_RESOURCE_VALIDATION;
    }
    return WE_SCENE_RUNTIME_ERROR_METAL_INTERNAL_FAILURE;
}

inline void assignMetalError(
    WESceneRuntimeErrorRef* outError,
    const metal::Error& error
) noexcept {
    assignError(outError, metalErrorCode(error.code()), error.what());
}

inline bool requireOutput(
    const void* output,
    WESceneRuntimeErrorRef* outError,
    const char* name
) noexcept {
    if (output != nullptr) {
        return true;
    }
    try {
        assignError(
            outError,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            std::string("Output ") + name + " is required"
        );
    } catch (...) {
        if (outError != nullptr) {
            *outError = nullptr;
        }
    }
    return false;
}

}  // namespace we::scene::bridge

#endif
