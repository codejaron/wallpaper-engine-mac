#include <SceneRuntimeBridge/SceneRuntimeBridge.h>

#include "SceneRuntimeBridgeInternal.hpp"

#include <SceneCore/FormatError.hpp>
#include <SceneShader/ShaderCompiler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <vector>

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

gl::PresentationViewport presentationViewport(
    const WEScenePresentationViewport& viewport
) noexcept {
    return {
        .canvasWidth = viewport.virtual_canvas_width,
        .canvasHeight = viewport.virtual_canvas_height,
        .viewportX = viewport.viewport_x,
        .viewportY = viewport.viewport_y,
        .viewportWidth = viewport.viewport_width,
        .viewportHeight = viewport.viewport_height,
        .drawableWidth = viewport.drawable_width,
        .drawableHeight = viewport.drawable_height,
    };
}

bool requirePresentationViewport(
    WESceneFrameExecutorRef executor,
    const WEScenePresentationViewport* viewport,
    bool invalidateFrame,
    gl::PresentationViewport& result,
    WESceneRuntimeErrorRef* outError
) {
    if (viewport == nullptr) {
        if (invalidateFrame) executor->executor->invalidateFrame();
        assignError(
            outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene presentation viewport is required"
        );
        return false;
    }
    try {
        result = presentationViewport(*viewport);
        gl::validatePresentationViewport(result);
        return true;
    } catch (const gl::Error& error) {
        if (invalidateFrame) executor->executor->invalidateFrame();
        if (error.code() == gl::ErrorCode::invalidArgument) {
            assignError(
                outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT, error.what()
            );
        } else {
            assignGLError(outError, error);
        }
        return false;
    }
}

std::optional<AudioSpectrumFrame> audioSpectrumFrame(
    const WESceneAudioSpectrumInputs* inputs
) {
    if (inputs == nullptr) return std::nullopt;
    if (inputs->spectrum_16_left == nullptr ||
        inputs->spectrum_16_right == nullptr ||
        inputs->spectrum_32_left == nullptr ||
        inputs->spectrum_32_right == nullptr ||
        inputs->spectrum_64_left == nullptr ||
        inputs->spectrum_64_right == nullptr) {
        throw gl::Error(
            gl::ErrorCode::invalidArgument,
            "Audio spectrum input requires all six fixed-size arrays"
        );
    }
    AudioSpectrumFrame result;
    std::copy_n(
        inputs->spectrum_16_left, result.spectrum16Left.size(),
        result.spectrum16Left.begin()
    );
    std::copy_n(
        inputs->spectrum_16_right, result.spectrum16Right.size(),
        result.spectrum16Right.begin()
    );
    std::copy_n(
        inputs->spectrum_32_left, result.spectrum32Left.size(),
        result.spectrum32Left.begin()
    );
    std::copy_n(
        inputs->spectrum_32_right, result.spectrum32Right.size(),
        result.spectrum32Right.begin()
    );
    std::copy_n(
        inputs->spectrum_64_left, result.spectrum64Left.size(),
        result.spectrum64Left.begin()
    );
    std::copy_n(
        inputs->spectrum_64_right, result.spectrum64Right.size(),
        result.spectrum64Right.begin()
    );
    return result;
}

script::ScriptMediaPlaybackState mediaPlaybackState(
    WESceneMediaPlaybackState state
) {
    switch (state) {
        case WE_SCENE_MEDIA_STOPPED:
            return script::ScriptMediaPlaybackState::stopped;
        case WE_SCENE_MEDIA_PLAYING:
            return script::ScriptMediaPlaybackState::playing;
        case WE_SCENE_MEDIA_PAUSED:
            return script::ScriptMediaPlaybackState::paused;
    }
    throw gl::Error(
        gl::ErrorCode::invalidArgument,
        "Scene media snapshot has an unknown playback state"
    );
}

script::ScriptMediaSnapshot mediaSnapshot(
    const WESceneMediaSnapshot& input
) {
    if ((input.available != 0 && input.available != 1) ||
        (input.has_thumbnail != 0 && input.has_thumbnail != 1)) {
        throw gl::Error(
            gl::ErrorCode::invalidArgument,
            "Scene media availability and thumbnail flags must be zero or one"
        );
    }
    const bool available = input.available == 1;
    const auto requireString = [&](const char* value, const char* field) {
        if (value == nullptr && available) {
            throw gl::Error(
                gl::ErrorCode::invalidArgument,
                std::string("Available Scene media snapshot requires ") + field
            );
        }
        return value == nullptr ? std::string{} : std::string(value);
    };
    const auto color = [&](const double (&components)[3], const char* field) {
        std::array<double, 3> result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            const double component = components[index];
            if (available && (!std::isfinite(component) ||
                              component < 0.0 || component > 1.0)) {
                throw gl::Error(
                    gl::ErrorCode::invalidArgument,
                    std::string("Scene media ") + field +
                        " must contain finite values in [0, 1]"
                );
            }
            result[index] = available ? component : 0.0;
        }
        return result;
    };
    if (available &&
        (!std::isfinite(input.position) || input.position < 0.0 ||
         !std::isfinite(input.duration) || input.duration < 0.0)) {
        throw gl::Error(
            gl::ErrorCode::invalidArgument,
            "Scene media timeline values must be finite and non-negative"
        );
    }
    return {
        .revision = input.revision,
        .available = available,
        .playbackState = mediaPlaybackState(input.playback_state),
        .title = requireString(input.title, "title"),
        .artist = requireString(input.artist, "artist"),
        .contentType = requireString(input.content_type, "content type"),
        .albumTitle = requireString(input.album_title, "album title"),
        .subTitle = requireString(input.sub_title, "subtitle"),
        .albumArtist = requireString(input.album_artist, "album artist"),
        .genres = requireString(input.genres, "genres"),
        .position = available ? input.position : 0.0,
        .duration = available ? input.duration : 0.0,
        .hasThumbnail = available && input.has_thumbnail == 1,
        .primaryColor = color(input.primary_color, "primary color"),
        .secondaryColor = color(input.secondary_color, "secondary color"),
        .tertiaryColor = color(input.tertiary_color, "tertiary color"),
        .textColor = color(input.text_color, "text color"),
        .highContrastColor = color(
            input.high_contrast_color, "high contrast color"
        ),
    };
}

bool requireAudioSpectrum(
    WESceneFrameExecutorRef executor,
    const WESceneAudioSpectrumInputs* audioSpectrum,
    WESceneRuntimeErrorRef* outError
) {
    if (audioSpectrum != nullptr) return true;
    if (executor != nullptr && executor->executor) {
        executor->executor->invalidateFrame();
    }
    assignError(
        outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Audio spectrum input is required"
    );
    return false;
}

int renderExecutor(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audioSpectrum,
    std::optional<gl::PresentationViewport> presentation,
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
            .audioSpectrum = audioSpectrumFrame(audioSpectrum),
        };
        if (presentation) {
            if (!scaling) {
                executor->executor->invalidateFrame();
                assignError(
                    outError,
                    WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE,
                    "Viewport rendering has no presentation scaling mode"
                );
                return 0;
            }
            executor->executor->render(
                frameInputs, *presentation, *scaling
            );
        } else {
            executor->executor->render(frameInputs);
        }
        return 1;
    } catch (const gl::Error& error) {
        executor->executor->invalidateFrame();
        if (error.code() == gl::ErrorCode::invalidArgument) {
            assignError(outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT, error.what());
        } else {
            assignGLError(outError, error);
        }
    } catch (const ShaderCompileError& error) {
        executor->executor->invalidateFrame();
        assignError(outError, shaderErrorCode(error.phase()), error.what());
    } catch (const FormatError& error) {
        executor->executor->invalidateFrame();
        assignError(
            outError,
            error.code() == FormatErrorCode::assetNotFound
                ? WE_SCENE_RUNTIME_ERROR_ASSET_NOT_FOUND
                : WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID,
            error.what()
        );
    } catch (const SceneModelError& error) {
        executor->executor->invalidateFrame();
        assignModelError(outError, error);
    } catch (const std::exception& error) {
        executor->executor->invalidateFrame();
        assignExceptionError(outError, "rendering a scene frame", error.what());
    } catch (...) {
        executor->executor->invalidateFrame();
        assignExceptionError(outError, "rendering a scene frame", nullptr);
    }
    return 0;
}

int renderExecutorForDrawable(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audioSpectrum,
    bool audioSpectrumRequired,
    uint32_t drawableWidth,
    uint32_t drawableHeight,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* outError
) {
    clearError(outError);
    if (!requireExecutor(executor, outError)) return 0;
    if (audioSpectrumRequired &&
        !requireAudioSpectrum(executor, audioSpectrum, outError)) {
        return 0;
    }
    if (drawableWidth == 0 || drawableHeight == 0 ||
        drawableWidth > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()) ||
        drawableHeight > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max())) {
        executor->executor->invalidateFrame();
        assignError(
            outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Drawable dimensions must be non-zero and fit OpenGL's signed range"
        );
        return 0;
    }
    const auto mode = presentationScaling(scaling);
    if (!mode) {
        executor->executor->invalidateFrame();
        assignError(
            outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Unknown scene presentation scaling mode"
        );
        return 0;
    }
    return renderExecutor(
        executor, inputs, audioSpectrum,
        gl::drawablePresentationViewport(drawableWidth, drawableHeight),
        mode,
        outError
    );
}

int renderExecutorForViewport(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audioSpectrum,
    bool audioSpectrumRequired,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* outError
) {
    clearError(outError);
    if (!requireExecutor(executor, outError)) return 0;
    if (audioSpectrumRequired &&
        !requireAudioSpectrum(executor, audioSpectrum, outError)) {
        return 0;
    }
    gl::PresentationViewport nativeViewport;
    if (!requirePresentationViewport(
            executor, viewport, true, nativeViewport, outError)) return 0;
    const auto mode = presentationScaling(scaling);
    if (!mode) {
        executor->executor->invalidateFrame();
        assignError(
            outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Unknown scene presentation scaling mode"
        );
        return 0;
    }
    return renderExecutor(
        executor, inputs, audioSpectrum, nativeViewport, mode, outError
    );
}

int replayExecutor(
    WESceneFrameExecutorRef executor,
    const gl::PresentationViewport& viewport,
    WESceneRuntimeErrorRef* outError
) {
    try {
        executor->executor->replay(viewport);
        return 1;
    } catch (const gl::Error& error) {
        assignGLError(outError, error);
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
        assignExceptionError(
            outError, "replaying an evaluated scene frame", error.what()
        );
    } catch (...) {
        assignExceptionError(
            outError, "replaying an evaluated scene frame", nullptr
        );
    }
    return 0;
}

int presentExecutor(
    WESceneFrameExecutorRef executor,
    const gl::PresentationViewport& viewport,
    gl::PresentationScaling scaling,
    WESceneRuntimeErrorRef* outError
) {
    try {
        executor->executor->present(viewport, scaling);
        return 1;
    } catch (const gl::Error& error) {
        assignGLError(outError, error);
    } catch (const std::exception& error) {
        assignExceptionError(outError, "presenting a scene frame", error.what());
    } catch (...) {
        assignExceptionError(outError, "presenting a scene frame", nullptr);
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

extern "C" int we_scene_frame_executor_set_pointer_state(
    WESceneFrameExecutorRef executor,
    int active,
    int left_down,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if ((active != 0 && active != 1) ||
        (left_down != 0 && left_down != 1)) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene pointer state flags must be zero or one"
        );
        return 0;
    }
    executor->executor->setPointerState(active == 1, left_down == 1);
    return 1;
}

extern "C" int we_scene_frame_executor_set_screensaver_state(
    WESceneFrameExecutorRef executor,
    int is_screensaver,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (is_screensaver != 0 && is_screensaver != 1) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene screensaver state must be zero or one"
        );
        return 0;
    }
    executor->executor->setScreensaverState(is_screensaver == 1);
    return 1;
}

extern "C" int we_scene_frame_executor_set_sound_runtime_states(
    WESceneFrameExecutorRef executor,
    const WESceneSoundRuntimeStateInput* states,
    size_t state_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (state_count != 0 && states == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene sound runtime states require storage when count is non-zero"
        );
        return 0;
    }
    try {
        std::vector<script::ScriptSoundRuntimeSnapshot> snapshots;
        snapshots.reserve(state_count);
        std::set<int32_t> object_ids;
        for (size_t index = 0; index < state_count; ++index) {
            const WESceneSoundRuntimeStateInput& input = states[index];
            if (!std::isfinite(input.position) || input.position < 0.0) {
                throw gl::Error(
                    gl::ErrorCode::invalidArgument,
                    "Scene sound runtime position must be finite and non-negative"
                );
            }
            if (!object_ids.emplace(input.object_id).second) {
                throw gl::Error(
                    gl::ErrorCode::invalidArgument,
                    "Scene sound runtime states contain a duplicate object id"
                );
            }
            script::ScriptSoundRuntimeState state;
            switch (input.state) {
                case WE_SCENE_SOUND_RUNTIME_STOPPED:
                    state = script::ScriptSoundRuntimeState::stopped;
                    break;
                case WE_SCENE_SOUND_RUNTIME_PLAYING:
                    state = script::ScriptSoundRuntimeState::playing;
                    break;
                case WE_SCENE_SOUND_RUNTIME_PAUSED:
                    state = script::ScriptSoundRuntimeState::paused;
                    break;
                case WE_SCENE_SOUND_RUNTIME_ENDED:
                    state = script::ScriptSoundRuntimeState::ended;
                    break;
                default:
                    throw gl::Error(
                        gl::ErrorCode::invalidArgument,
                        "Scene sound runtime state is unknown"
                    );
            }
            snapshots.push_back({
                .layerId = input.object_id,
                .state = state,
                .positionSeconds = input.position,
            });
        }
        executor->executor->setSoundRuntimeStates(std::move(snapshots));
        return 1;
    } catch (const gl::Error& error) {
        if (error.code() == gl::ErrorCode::invalidArgument) {
            assignError(
                out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT, error.what()
            );
        } else {
            assignGLError(out_error, error);
        }
    } catch (const std::exception& error) {
        assignExceptionError(
            out_error, "copying Scene sound runtime states", error.what()
        );
    } catch (...) {
        assignExceptionError(
            out_error, "copying Scene sound runtime states", nullptr
        );
    }
    return 0;
}

extern "C" int we_scene_frame_executor_set_media_snapshot(
    WESceneFrameExecutorRef executor,
    const WESceneMediaSnapshot* snapshot,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (snapshot == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene media snapshot is required"
        );
        return 0;
    }
    try {
        executor->executor->setMediaSnapshot(mediaSnapshot(*snapshot));
        return 1;
    } catch (const gl::Error& error) {
        if (error.code() == gl::ErrorCode::invalidArgument) {
            assignError(
                out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT, error.what()
            );
        } else {
            assignGLError(out_error, error);
        }
    } catch (const std::exception& error) {
        assignExceptionError(
            out_error, "copying a Scene media snapshot", error.what()
        );
    } catch (...) {
        assignExceptionError(
            out_error, "copying a Scene media snapshot", nullptr
        );
    }
    return 0;
}

extern "C" int we_scene_frame_executor_clear_media_snapshot(
    WESceneFrameExecutorRef executor,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    executor->executor->setMediaSnapshot(std::nullopt);
    return 1;
}

extern "C" int we_scene_frame_executor_render(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    WESceneRuntimeErrorRef* out_error
) {
    return renderExecutor(
        executor, inputs, nullptr, std::nullopt, std::nullopt, out_error
    );
}

extern "C" int we_scene_frame_executor_render_with_audio_spectrum(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (!requireAudioSpectrum(executor, audio_spectrum, out_error)) return 0;
    return renderExecutor(
        executor, inputs, audio_spectrum, std::nullopt, std::nullopt, out_error
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
    return renderExecutorForDrawable(
        executor, inputs, nullptr, false,
        drawable_width, drawable_height, scaling, out_error
    );
}

extern "C" int we_scene_frame_executor_render_for_drawable_with_audio_spectrum(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
) {
    return renderExecutorForDrawable(
        executor, inputs, audio_spectrum, true,
        drawable_width, drawable_height, scaling, out_error
    );
}

extern "C" int we_scene_frame_executor_render_for_viewport(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
) {
    return renderExecutorForViewport(
        executor, inputs, nullptr, false, viewport, scaling, out_error
    );
}

extern "C" int
we_scene_frame_executor_render_for_viewport_with_audio_spectrum(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
) {
    return renderExecutorForViewport(
        executor, inputs, audio_spectrum, true, viewport, scaling, out_error
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
    return replayExecutor(
        executor,
        gl::drawablePresentationViewport(drawable_width, drawable_height),
        out_error
    );
}

extern "C" int we_scene_frame_executor_replay_for_viewport(
    WESceneFrameExecutorRef executor,
    const WEScenePresentationViewport* viewport,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    gl::PresentationViewport nativeViewport;
    if (!requirePresentationViewport(
            executor, viewport, true, nativeViewport, out_error)) return 0;
    return replayExecutor(executor, nativeViewport, out_error);
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
    return presentExecutor(
        executor,
        gl::drawablePresentationViewport(drawable_width, drawable_height),
        *mode,
        out_error
    );
}

extern "C" int we_scene_frame_executor_present_for_viewport(
    WESceneFrameExecutorRef executor,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    gl::PresentationViewport nativeViewport;
    if (!requirePresentationViewport(
            executor, viewport, false, nativeViewport, out_error)) return 0;
    const auto mode = presentationScaling(scaling);
    if (!mode) {
        assignError(
            out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Unknown scene presentation scaling mode"
        );
        return 0;
    }
    return presentExecutor(
        executor, nativeViewport, *mode, out_error
    );
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
    out_info->playback_command = WE_SCENE_FRAME_SOUND_COMMAND_NONE;
    out_info->playback_command_generation = 0;
    if (!value.playbackCommand) return 1;
    out_info->playback_command_generation = value.playbackCommand->generation;
    switch (value.playbackCommand->action) {
        case FrameSoundPlaybackCommandAction::play:
            out_info->playback_command = WE_SCENE_FRAME_SOUND_COMMAND_PLAY;
            break;
        case FrameSoundPlaybackCommandAction::pause:
            out_info->playback_command = WE_SCENE_FRAME_SOUND_COMMAND_PAUSE;
            break;
        case FrameSoundPlaybackCommandAction::stop:
            out_info->playback_command = WE_SCENE_FRAME_SOUND_COMMAND_STOP;
            break;
    }
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
