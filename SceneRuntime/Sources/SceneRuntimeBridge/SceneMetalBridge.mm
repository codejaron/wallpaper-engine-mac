#include <SceneRuntimeBridge/SceneMetalBridge.h>

#include <SceneMetal/SceneMetal.hpp>

#include <cstdio>
#include <exception>
#include <memory>
#include <new>
#include <span>

struct WESceneMetalRenderer {
    std::unique_ptr<we::scene::metal::OffscreenRenderer> renderer;
};

struct WESceneMetalError {
    WESceneMetalErrorCode code;
    char message[1024];
};

namespace {

void clearError(WESceneMetalErrorRef* outError) noexcept {
    if (outError != nullptr) {
        *outError = nullptr;
    }
}

WESceneMetalErrorCode bridgeErrorCode(we::scene::metal::ErrorCode code) noexcept {
    using Code = we::scene::metal::ErrorCode;
    switch (code) {
        case Code::invalidArgument:
            return WE_SCENE_METAL_ERROR_INVALID_ARGUMENT;
        case Code::contextCreation:
            return WE_SCENE_METAL_ERROR_CONTEXT_CREATION;
        case Code::unsupportedContext:
            return WE_SCENE_METAL_ERROR_UNSUPPORTED_CONTEXT;
        case Code::shaderCompilation:
            return WE_SCENE_METAL_ERROR_SHADER_COMPILATION;
        case Code::programLink:
            return WE_SCENE_METAL_ERROR_PROGRAM_LINK;
        case Code::framebufferCreation:
            return WE_SCENE_METAL_ERROR_FRAMEBUFFER_CREATION;
        case Code::draw:
            return WE_SCENE_METAL_ERROR_DRAW;
        case Code::readback:
            return WE_SCENE_METAL_ERROR_READBACK;
        case Code::internalFailure:
            return WE_SCENE_METAL_ERROR_INTERNAL_FAILURE;
        case Code::textureDecode:
            return WE_SCENE_METAL_ERROR_TEXTURE_DECODE;
        case Code::textureUpload:
            return WE_SCENE_METAL_ERROR_TEXTURE_UPLOAD;
        case Code::resourceValidation:
            return WE_SCENE_METAL_ERROR_RESOURCE_VALIDATION;
    }
    return WE_SCENE_METAL_ERROR_INTERNAL_FAILURE;
}

void assignError(
    WESceneMetalErrorRef* outError,
    WESceneMetalErrorCode code,
    const char* message
) noexcept {
    if (outError == nullptr) {
        return;
    }
    std::unique_ptr<WESceneMetalError> error(new (std::nothrow) WESceneMetalError);
    if (!error) {
        return;
    }
    error->code = code;
    std::snprintf(
        error->message,
        sizeof(error->message),
        "%s",
        message != nullptr ? message : "unknown SceneMetal failure"
    );
    *outError = error.release();
}

template <typename Function>
int perform(
    WESceneMetalErrorRef* outError,
    Function&& function
) noexcept {
    clearError(outError);
    try {
        function();
        return 1;
    } catch (const we::scene::metal::Error& error) {
        assignError(outError, bridgeErrorCode(error.code()), error.what());
        return 0;
    } catch (const std::exception& error) {
        assignError(outError, WE_SCENE_METAL_ERROR_INTERNAL_FAILURE, error.what());
        return 0;
    } catch (...) {
        assignError(
            outError,
            WE_SCENE_METAL_ERROR_INTERNAL_FAILURE,
            "unknown C++ exception"
        );
        return 0;
    }
}

}  // namespace

extern "C" WESceneMetalRendererRef we_scene_metal_renderer_create(
    uint32_t width,
    uint32_t height,
    WESceneMetalErrorRef* out_error
) {
    clearError(out_error);
    try {
        auto result = std::make_unique<WESceneMetalRenderer>();
        result->renderer =
            std::make_unique<we::scene::metal::OffscreenRenderer>(width, height);
        return result.release();
    } catch (const we::scene::metal::Error& error) {
        assignError(out_error, bridgeErrorCode(error.code()), error.what());
        return nullptr;
    } catch (const std::exception& error) {
        assignError(out_error, WE_SCENE_METAL_ERROR_INTERNAL_FAILURE, error.what());
        return nullptr;
    } catch (...) {
        assignError(
            out_error,
            WE_SCENE_METAL_ERROR_INTERNAL_FAILURE,
            "unknown C++ exception"
        );
        return nullptr;
    }
}

extern "C" void we_scene_metal_renderer_destroy(WESceneMetalRendererRef renderer) {
    delete renderer;
}

extern "C" size_t we_scene_metal_renderer_rgba8_byte_count(
    WESceneMetalRendererRef renderer
) {
    return renderer != nullptr && renderer->renderer
        ? renderer->renderer->rgba8ByteCount()
        : 0;
}

extern "C" int we_scene_metal_renderer_compile_program(
    WESceneMetalRendererRef renderer,
    const char* vertex_source,
    const char* fragment_source,
    WESceneMetalErrorRef* out_error
) {
    if (renderer == nullptr || !renderer->renderer ||
        vertex_source == nullptr || fragment_source == nullptr) {
        clearError(out_error);
        assignError(
            out_error,
            WE_SCENE_METAL_ERROR_INVALID_ARGUMENT,
            "Renderer and both shader sources are required"
        );
        return 0;
    }
    return perform(out_error, [&] {
        renderer->renderer->compileProgram(vertex_source, fragment_source);
    });
}

extern "C" int we_scene_metal_renderer_draw(
    WESceneMetalRendererRef renderer,
    WESceneMetalErrorRef* out_error
) {
    if (renderer == nullptr || !renderer->renderer) {
        clearError(out_error);
        assignError(
            out_error,
            WE_SCENE_METAL_ERROR_INVALID_ARGUMENT,
            "Renderer is required"
        );
        return 0;
    }
    return perform(out_error, [&] { renderer->renderer->draw(); });
}

extern "C" int we_scene_metal_renderer_read_rgba8(
    WESceneMetalRendererRef renderer,
    uint8_t* output,
    size_t output_length,
    WESceneMetalErrorRef* out_error
) {
    if (renderer == nullptr || !renderer->renderer || output == nullptr) {
        clearError(out_error);
        assignError(
            out_error,
            WE_SCENE_METAL_ERROR_INVALID_ARGUMENT,
            "Renderer and RGBA8 output buffer are required"
        );
        return 0;
    }
    return perform(out_error, [&] {
        renderer->renderer->readRGBA8(
            std::span<std::uint8_t>(output, output_length)
        );
    });
}

extern "C" WESceneMetalErrorCode we_scene_metal_error_code(
    WESceneMetalErrorRef error
) {
    return error != nullptr ? error->code : WE_SCENE_METAL_ERROR_NONE;
}

extern "C" const char* we_scene_metal_error_message(WESceneMetalErrorRef error) {
    return error != nullptr ? error->message : nullptr;
}

extern "C" void we_scene_metal_error_destroy(WESceneMetalErrorRef error) {
    delete error;
}
