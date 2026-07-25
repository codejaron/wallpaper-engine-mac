#include <SceneRuntimeBridge/SceneGLBridge.h>

#include <SceneGL/SceneGL.hpp>

#include <cstdio>
#include <exception>
#include <memory>
#include <new>
#include <span>

struct WESceneGLRenderer {
    std::unique_ptr<we::scene::gl::OffscreenRenderer> renderer;
};

struct WESceneGLError {
    WESceneGLErrorCode code;
    char message[1024];
};

namespace {

void clearError(WESceneGLErrorRef* outError) noexcept {
    if (outError != nullptr) {
        *outError = nullptr;
    }
}

WESceneGLErrorCode bridgeErrorCode(we::scene::gl::ErrorCode code) noexcept {
    using Code = we::scene::gl::ErrorCode;
    switch (code) {
        case Code::invalidArgument:
            return WE_SCENE_GL_ERROR_INVALID_ARGUMENT;
        case Code::contextCreation:
            return WE_SCENE_GL_ERROR_CONTEXT_CREATION;
        case Code::unsupportedContext:
            return WE_SCENE_GL_ERROR_UNSUPPORTED_CONTEXT;
        case Code::shaderCompilation:
            return WE_SCENE_GL_ERROR_SHADER_COMPILATION;
        case Code::programLink:
            return WE_SCENE_GL_ERROR_PROGRAM_LINK;
        case Code::framebufferCreation:
            return WE_SCENE_GL_ERROR_FRAMEBUFFER_CREATION;
        case Code::draw:
            return WE_SCENE_GL_ERROR_DRAW;
        case Code::readback:
            return WE_SCENE_GL_ERROR_READBACK;
        case Code::internalFailure:
            return WE_SCENE_GL_ERROR_INTERNAL_FAILURE;
        case Code::textureDecode:
            return WE_SCENE_GL_ERROR_TEXTURE_DECODE;
        case Code::textureUpload:
            return WE_SCENE_GL_ERROR_TEXTURE_UPLOAD;
        case Code::resourceValidation:
            return WE_SCENE_GL_ERROR_RESOURCE_VALIDATION;
    }
    return WE_SCENE_GL_ERROR_INTERNAL_FAILURE;
}

void assignError(
    WESceneGLErrorRef* outError,
    WESceneGLErrorCode code,
    const char* message
) noexcept {
    if (outError == nullptr) {
        return;
    }
    std::unique_ptr<WESceneGLError> error(new (std::nothrow) WESceneGLError);
    if (!error) {
        return;
    }
    error->code = code;
    std::snprintf(
        error->message,
        sizeof(error->message),
        "%s",
        message != nullptr ? message : "unknown SceneGL failure"
    );
    *outError = error.release();
}

template <typename Function>
int perform(
    WESceneGLErrorRef* outError,
    Function&& function
) noexcept {
    clearError(outError);
    try {
        function();
        return 1;
    } catch (const we::scene::gl::Error& error) {
        assignError(outError, bridgeErrorCode(error.code()), error.what());
        return 0;
    } catch (const std::exception& error) {
        assignError(outError, WE_SCENE_GL_ERROR_INTERNAL_FAILURE, error.what());
        return 0;
    } catch (...) {
        assignError(
            outError,
            WE_SCENE_GL_ERROR_INTERNAL_FAILURE,
            "unknown C++ exception"
        );
        return 0;
    }
}

}  // namespace

extern "C" WESceneGLRendererRef we_scene_gl_renderer_create(
    uint32_t width,
    uint32_t height,
    WESceneGLErrorRef* out_error
) {
    clearError(out_error);
    try {
        auto result = std::make_unique<WESceneGLRenderer>();
        result->renderer =
            std::make_unique<we::scene::gl::OffscreenRenderer>(width, height);
        return result.release();
    } catch (const we::scene::gl::Error& error) {
        assignError(out_error, bridgeErrorCode(error.code()), error.what());
        return nullptr;
    } catch (const std::exception& error) {
        assignError(out_error, WE_SCENE_GL_ERROR_INTERNAL_FAILURE, error.what());
        return nullptr;
    } catch (...) {
        assignError(
            out_error,
            WE_SCENE_GL_ERROR_INTERNAL_FAILURE,
            "unknown C++ exception"
        );
        return nullptr;
    }
}

extern "C" void we_scene_gl_renderer_destroy(WESceneGLRendererRef renderer) {
    delete renderer;
}

extern "C" size_t we_scene_gl_renderer_rgba8_byte_count(
    WESceneGLRendererRef renderer
) {
    return renderer != nullptr && renderer->renderer
        ? renderer->renderer->rgba8ByteCount()
        : 0;
}

extern "C" int we_scene_gl_renderer_compile_program(
    WESceneGLRendererRef renderer,
    const char* vertex_source,
    const char* fragment_source,
    WESceneGLErrorRef* out_error
) {
    if (renderer == nullptr || !renderer->renderer ||
        vertex_source == nullptr || fragment_source == nullptr) {
        clearError(out_error);
        assignError(
            out_error,
            WE_SCENE_GL_ERROR_INVALID_ARGUMENT,
            "Renderer and both shader sources are required"
        );
        return 0;
    }
    return perform(out_error, [&] {
        renderer->renderer->compileProgram(vertex_source, fragment_source);
    });
}

extern "C" int we_scene_gl_renderer_draw(
    WESceneGLRendererRef renderer,
    WESceneGLErrorRef* out_error
) {
    if (renderer == nullptr || !renderer->renderer) {
        clearError(out_error);
        assignError(
            out_error,
            WE_SCENE_GL_ERROR_INVALID_ARGUMENT,
            "Renderer is required"
        );
        return 0;
    }
    return perform(out_error, [&] { renderer->renderer->draw(); });
}

extern "C" int we_scene_gl_renderer_read_rgba8(
    WESceneGLRendererRef renderer,
    uint8_t* output,
    size_t output_length,
    WESceneGLErrorRef* out_error
) {
    if (renderer == nullptr || !renderer->renderer || output == nullptr) {
        clearError(out_error);
        assignError(
            out_error,
            WE_SCENE_GL_ERROR_INVALID_ARGUMENT,
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

extern "C" WESceneGLErrorCode we_scene_gl_error_code(
    WESceneGLErrorRef error
) {
    return error != nullptr ? error->code : WE_SCENE_GL_ERROR_NONE;
}

extern "C" const char* we_scene_gl_error_message(WESceneGLErrorRef error) {
    return error != nullptr ? error->message : nullptr;
}

extern "C" void we_scene_gl_error_destroy(WESceneGLErrorRef error) {
    delete error;
}
