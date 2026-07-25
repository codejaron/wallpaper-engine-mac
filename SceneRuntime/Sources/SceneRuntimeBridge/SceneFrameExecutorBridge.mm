#include <SceneRuntimeBridge/SceneRuntimeBridge.h>

#include "SceneRuntimeBridgeInternal.hpp"

#include <SceneCore/FormatError.hpp>
#include <SceneShader/ShaderCompiler.hpp>

#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>

using namespace we::scene;
using namespace we::scene::bridge;

namespace {

bool requireExecutor(
    WESceneFrameExecutorRef executor,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (executor != nullptr && executor->executor) {
        return true;
    }
    assignError(
        outError,
        WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Scene frame executor is required"
    );
    return false;
}

WESceneRuntimeErrorCode shaderErrorCode(
    ShaderCompilePhase phase
) noexcept {
    switch (phase) {
        case ShaderCompilePhase::input:
            return WE_SCENE_RUNTIME_ERROR_SHADER_INPUT_INVALID;
        case ShaderCompilePhase::vertexParse:
        case ShaderCompilePhase::fragmentParse:
            return WE_SCENE_RUNTIME_ERROR_SHADER_PARSE_FAILURE;
        case ShaderCompilePhase::link:
            return WE_SCENE_RUNTIME_ERROR_SHADER_LINK_FAILURE;
        case ShaderCompilePhase::spirvGeneration:
        case ShaderCompilePhase::crossCompilation:
            return WE_SCENE_RUNTIME_ERROR_SHADER_TRANSLATION_FAILURE;
    }
    return WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE;
}

WESceneFramePlanIssueSeverity issueSeverity(
    FramePlanIssueSeverity severity
) noexcept {
    switch (severity) {
        case FramePlanIssueSeverity::warning:
            return WE_SCENE_FRAME_ISSUE_WARNING;
        case FramePlanIssueSeverity::skipPass:
            return WE_SCENE_FRAME_ISSUE_SKIP_PASS;
        case FramePlanIssueSeverity::skipObject:
            return WE_SCENE_FRAME_ISSUE_SKIP_OBJECT;
        case FramePlanIssueSeverity::frameFatal:
            return WE_SCENE_FRAME_ISSUE_FRAME_FATAL;
    }
    std::terminate();
}

std::optional<gl::PresentationScaling> presentationScaling(
    WEScenePresentationScaling scaling
) noexcept {
    switch (scaling) {
        case WE_SCENE_PRESENTATION_STRETCH:
            return gl::PresentationScaling::stretch;
        case WE_SCENE_PRESENTATION_ASPECT_FIT:
            return gl::PresentationScaling::aspectFit;
        case WE_SCENE_PRESENTATION_ASPECT_FILL:
            return gl::PresentationScaling::aspectFill;
    }
    return std::nullopt;
}

int renderExecutor(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    std::optional<FrameProjectionSize> drawableFallback,
    std::optional<gl::PresentationScaling> scaling,
    WESceneRuntimeErrorRef* outError
) {
    clearError(outError);
    if (!requireExecutor(executor, outError)) return 0;
    if (inputs == nullptr) {
        executor->executor->invalidateFrame();
        assignError(
            outError,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene frame inputs are required"
        );
        return 0;
    }
    try {
        const gl::FrameInputs frameInputs{
            .pointerPosition = {inputs->pointer_x, inputs->pointer_y},
            .timeSeconds = inputs->time_seconds,
            .frameTimeSeconds = inputs->frame_time_seconds,
        };
        if (drawableFallback) {
            if (!scaling) {
                executor->executor->invalidateFrame();
                assignError(
                    outError,
                    WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE,
                    "Drawable rendering has no presentation scaling mode"
                );
                return 0;
            }
            executor->executor->render(
                frameInputs, drawableFallback->width, drawableFallback->height,
                *scaling
            );
        } else {
            executor->executor->render(frameInputs);
        }
        return 1;
    } catch (const gl::Error& error) {
        if (error.code() == gl::ErrorCode::invalidArgument) {
            assignError(outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT, error.what());
        } else {
            assignGLError(outError, error);
        }
    } catch (const ShaderCompileError& error) {
        assignError(outError, shaderErrorCode(error.phase()), error.what());
    } catch (const FormatError& error) {
        assignError(
            outError,
            error.code() == FormatErrorCode::assetNotFound
                ? WE_SCENE_RUNTIME_ERROR_ASSET_NOT_FOUND
                : WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID,
            error.what()
        );
    } catch (const SceneModelError& error) {
        assignModelError(outError, error);
    } catch (const std::exception& error) {
        assignExceptionError(outError, "rendering a scene frame", error.what());
    } catch (...) {
        assignExceptionError(outError, "rendering a scene frame", nullptr);
    }
    return 0;
}

}  // namespace

extern "C" WESceneFrameExecutorRef we_scene_frame_executor_create(
    WESceneFrameGraphRef graph,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (graph == nullptr || !graph->graph) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene frame graph is required"
        );
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneFrameExecutor>();
        handle->executor = std::make_unique<gl::FramePlanExecutor>(graph->graph);
        return handle.release();
    } catch (const gl::Error& error) {
        assignGLError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "creating the scene frame executor", error.what());
    } catch (...) {
        assignExceptionError(out_error, "creating the scene frame executor", nullptr);
    }
    return nullptr;
}

extern "C" WESceneFrameExecutorRef we_scene_frame_executor_create_with_cgl_context(
    WESceneFrameGraphRef graph,
    void* cgl_context,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (graph == nullptr || !graph->graph || cgl_context == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Scene frame graph and CGL context are required");
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneFrameExecutor>();
        handle->executor = std::make_unique<gl::FramePlanExecutor>(
            graph->graph, static_cast<CGLContextObj>(cgl_context)
        );
        return handle.release();
    } catch (const gl::Error& error) {
        assignGLError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "creating the borrowed-context scene executor", error.what());
    } catch (...) {
        assignExceptionError(out_error, "creating the borrowed-context scene executor", nullptr);
    }
    return nullptr;
}

extern "C" void we_scene_frame_executor_destroy(
    WESceneFrameExecutorRef executor
) {
    delete executor;
}

extern "C" int we_scene_frame_executor_render(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    WESceneRuntimeErrorRef* out_error
) {
    return renderExecutor(
        executor, inputs, std::nullopt, std::nullopt, out_error
    );
}

extern "C" int we_scene_frame_executor_render_for_drawable(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (drawable_width == 0 || drawable_height == 0 ||
        drawable_width > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()) ||
        drawable_height > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max())) {
        executor->executor->invalidateFrame();
        assignError(
            out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Drawable dimensions must be non-zero and fit OpenGL's signed range"
        );
        return 0;
    }
    const auto mode = presentationScaling(scaling);
    if (!mode) {
        executor->executor->invalidateFrame();
        assignError(
            out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Unknown scene presentation scaling mode"
        );
        return 0;
    }
    return renderExecutor(
        executor, inputs,
        FrameProjectionSize{.width = drawable_width, .height = drawable_height},
        mode,
        out_error
    );
}

extern "C" int we_scene_frame_executor_replay_for_drawable(
    WESceneFrameExecutorRef executor,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (drawable_width == 0 || drawable_height == 0 ||
        drawable_width > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()) ||
        drawable_height > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max())) {
        executor->executor->invalidateFrame();
        assignError(
            out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Drawable dimensions must be non-zero and fit OpenGL's signed range"
        );
        return 0;
    }
    try {
        executor->executor->replay(drawable_width, drawable_height);
        return 1;
    } catch (const gl::Error& error) {
        assignGLError(out_error, error);
    } catch (const ShaderCompileError& error) {
        assignError(out_error, shaderErrorCode(error.phase()), error.what());
    } catch (const FormatError& error) {
        assignError(
            out_error,
            error.code() == FormatErrorCode::assetNotFound
                ? WE_SCENE_RUNTIME_ERROR_ASSET_NOT_FOUND
                : WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID,
            error.what()
        );
    } catch (const SceneModelError& error) {
        assignModelError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "replaying an evaluated scene frame", error.what());
    } catch (...) {
        assignExceptionError(out_error, "replaying an evaluated scene frame", nullptr);
    }
    return 0;
}

extern "C" int we_scene_frame_executor_present(
    WESceneFrameExecutorRef executor,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (drawable_width == 0 || drawable_height == 0 ||
        drawable_width > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()) ||
        drawable_height > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max())) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Drawable dimensions must be non-zero and fit OpenGL's signed range");
        return 0;
    }
    const auto mode = presentationScaling(scaling);
    if (!mode) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Unknown scene presentation scaling mode");
        return 0;
    }
    try {
        executor->executor->present(drawable_width, drawable_height, *mode);
        return 1;
    } catch (const gl::Error& error) {
        assignGLError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "presenting a scene frame", error.what());
    } catch (...) {
        assignExceptionError(out_error, "presenting a scene frame", nullptr);
    }
    return 0;
}

extern "C" uint32_t we_scene_frame_executor_width(
    WESceneFrameExecutorRef executor
) {
    return executor != nullptr && executor->executor
        ? executor->executor->width() : 0;
}

extern "C" uint32_t we_scene_frame_executor_height(
    WESceneFrameExecutorRef executor
) {
    return executor != nullptr && executor->executor
        ? executor->executor->height() : 0;
}

extern "C" size_t we_scene_frame_executor_rgba8_byte_count(
    WESceneFrameExecutorRef executor
) {
    return executor != nullptr && executor->executor
        ? executor->executor->rgba8ByteCount() : 0;
}

extern "C" int we_scene_frame_executor_last_model_revision(
    WESceneFrameExecutorRef executor,
    uint64_t* out_revision,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) {
        return 0;
    }
    if (!requireOutput(out_revision, out_error, "revision")) {
        return 0;
    }
    const auto revision = executor->executor->lastModelRevision();
    if (!revision.has_value()) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE,
            "No scene frame has been rendered"
        );
        return 0;
    }
    *out_revision = *revision;
    return 1;
}

extern "C" int we_scene_frame_executor_sound_count(
    WESceneFrameExecutorRef executor,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error) ||
        !requireOutput(out_count, out_error, "sound count")) return 0;
    const auto* sounds = executor->executor->lastSounds();
    if (sounds == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE,
                    "No scene frame has been rendered");
        return 0;
    }
    *out_count = sounds->size();
    return 1;
}

extern "C" int we_scene_frame_executor_sound_info(
    WESceneFrameExecutorRef executor,
    size_t index,
    WESceneFrameSoundInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error) ||
        !requireOutput(out_info, out_error, "sound information")) return 0;
    const auto* sounds = executor->executor->lastSounds();
    if (sounds == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE,
                    "No scene frame has been rendered");
        return 0;
    }
    if (index >= sounds->size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame sound index is out of range");
        return 0;
    }
    const FrameSoundDescriptor& value = sounds->at(index);
    *out_info = {};
    out_info->object_index = value.objectIndex;
    out_info->object_id = value.objectId;
    out_info->visible = value.visible ? 1 : 0;
    out_info->source_count = value.sources.size();
    out_info->playback_mode = value.playbackMode == FrameSoundPlaybackMode::loop
        ? WE_SCENE_FRAME_SOUND_PLAYBACK_LOOP : WE_SCENE_FRAME_SOUND_PLAYBACK_ONCE;
    out_info->volume = value.volume;
    out_info->start_silent = value.startSilent ? 1 : 0;
    out_info->mute_in_editor = value.muteInEditor ? 1 : 0;
    out_info->minimum_time = value.minimumTime;
    out_info->maximum_time = value.maximumTime;
    return 1;
}

extern "C" int we_scene_frame_executor_sound_source(
    WESceneFrameExecutorRef executor,
    size_t sound_index,
    size_t source_index,
    const char** out_source,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error) ||
        !requireOutput(out_source, out_error, "sound source")) return 0;
    const auto* sounds = executor->executor->lastSounds();
    if (sounds == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE,
                    "No scene frame has been rendered");
        return 0;
    }
    if (sound_index >= sounds->size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame sound index is out of range");
        return 0;
    }
    const auto& sources = sounds->at(sound_index).sources;
    if (source_index >= sources.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame sound source index is out of range");
        return 0;
    }
    *out_source = sources[source_index].c_str();
    return 1;
}

extern "C" int we_scene_frame_executor_issue_count(
    WESceneFrameExecutorRef executor,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error) ||
        !requireOutput(out_count, out_error, "executor issue count")) return 0;
    const auto* issues = executor->executor->lastIssues();
    if (issues == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE,
                    "No scene frame has been rendered");
        return 0;
    }
    *out_count = issues->size();
    return 1;
}

extern "C" int we_scene_frame_executor_issue_info(
    WESceneFrameExecutorRef executor,
    size_t index,
    WESceneFrameExecutorIssueInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error) ||
        !requireOutput(out_info, out_error, "executor issue information")) return 0;
    const auto* issues = executor->executor->lastIssues();
    if (issues == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_FRAME_EXECUTOR_INVALID_STATE,
                    "No scene frame has been rendered");
        return 0;
    }
    if (index >= issues->size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame executor issue index is out of range");
        return 0;
    }
    const gl::FrameExecutionIssue& value = issues->at(index);
    *out_info = {};
    out_info->severity = issueSeverity(value.severity);
    out_info->object_index = value.objectIndex;
    out_info->object_id = value.objectId;
    out_info->operation_index = value.operationIndex;
    out_info->message = value.message.c_str();
    return 1;
}

extern "C" int we_scene_frame_executor_read_rgba8(
    WESceneFrameExecutorRef executor,
    uint8_t* output,
    size_t output_length,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) {
        return 0;
    }
    const auto required = executor->executor->rgba8ByteCount();
    if (output == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "RGBA8 output buffer is required"
        );
        return 0;
    }
    if (output_length != required) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "RGBA8 output buffer length must equal the executor byte count"
        );
        return 0;
    }
    try {
        executor->executor->readRGBA8(std::span<std::uint8_t>(output, output_length));
        return 1;
    } catch (const gl::Error& error) {
        assignGLError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "reading a rendered scene frame", error.what());
    } catch (...) {
        assignExceptionError(out_error, "reading a rendered scene frame", nullptr);
    }
    return 0;
}
