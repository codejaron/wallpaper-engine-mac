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

constexpr std::uint32_t maximumMetalTextureDimension = 16'384;
constexpr std::uint32_t maximumPresentationDimension =
    static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());

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

std::optional<metal::PresentationScaling> presentationScaling(
    WEScenePresentationScaling scaling
) noexcept {
    switch (scaling) {
        case WE_SCENE_PRESENTATION_STRETCH:
            return metal::PresentationScaling::stretch;
        case WE_SCENE_PRESENTATION_ASPECT_FIT:
            return metal::PresentationScaling::aspectFit;
        case WE_SCENE_PRESENTATION_ASPECT_FILL:
            return metal::PresentationScaling::aspectFill;
        case WE_SCENE_PRESENTATION_AUTOMATIC:
            return metal::PresentationScaling::automatic;
    }
    return std::nullopt;
}

std::optional<metal::PhysicalRenderQuality> physicalRenderQuality(
    WEScenePhysicalRenderQuality quality
) noexcept {
    switch (quality) {
        case WE_SCENE_PHYSICAL_RENDER_BALANCED:
            return metal::PhysicalRenderQuality::balanced;
        case WE_SCENE_PHYSICAL_RENDER_POWER_SAVING:
            return metal::PhysicalRenderQuality::powerSaving;
        case WE_SCENE_PHYSICAL_RENDER_ULTRA:
            return metal::PhysicalRenderQuality::ultra;
    }
    return std::nullopt;
}

bool requirePhysicalRenderTarget(
    WESceneFrameExecutorRef executor,
    const WEScenePhysicalRenderTarget* target,
    bool invalidateFrame,
    metal::PhysicalRenderTarget& result,
    WESceneRuntimeErrorRef* outError
) {
    if (target == nullptr) {
        if (invalidateFrame) executor->executor->invalidateFrame();
        assignError(
            outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Physical render target is required"
        );
        return false;
    }
    const auto quality = physicalRenderQuality(target->quality);
    if (target->backing_width == 0 || target->backing_height == 0 ||
        target->backing_width > maximumMetalTextureDimension ||
        target->backing_height > maximumMetalTextureDimension ||
        !quality) {
        if (invalidateFrame) executor->executor->invalidateFrame();
        assignError(
            outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Physical render target dimensions and quality must be valid"
        );
        return false;
    }
    result = {
        .backingWidth = target->backing_width,
        .backingHeight = target->backing_height,
        .quality = *quality,
    };
    return true;
}

metal::PresentationViewport presentationViewport(
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
    metal::PresentationViewport& result,
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
        metal::validatePresentationViewport(result);
        return true;
    } catch (const metal::Error& error) {
        if (invalidateFrame) executor->executor->invalidateFrame();
        if (error.code() == metal::ErrorCode::invalidArgument) {
            assignError(
                outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT, error.what()
            );
        } else {
            assignMetalError(outError, error);
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
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
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
    throw metal::Error(
        metal::ErrorCode::invalidArgument,
        "Scene media snapshot has an unknown playback state"
    );
}

script::ScriptMediaSnapshot mediaSnapshot(
    const WESceneMediaSnapshot& input
) {
    if ((input.available != 0 && input.available != 1) ||
        (input.has_thumbnail != 0 && input.has_thumbnail != 1)) {
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
            "Scene media availability and thumbnail flags must be zero or one"
        );
    }
    const bool available = input.available == 1;
    const auto requireString = [&](const char* value, const char* field) {
        if (value == nullptr && available) {
            throw metal::Error(
                metal::ErrorCode::invalidArgument,
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
                throw metal::Error(
                    metal::ErrorCode::invalidArgument,
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
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
            "Scene media timeline values must be finite and non-negative"
        );
    }
    return {
        .statusRevision = input.status_revision,
        .metadataRevision = input.metadata_revision,
        .playbackRevision = input.playback_revision,
        .timelineRevision = input.timeline_revision,
        .thumbnailRevision = input.thumbnail_revision,
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

metal::MediaThumbnailRGBA8 mediaThumbnailRGBA8(
    const WESceneMediaThumbnailRGBA8& input
) {
    if (input.width == 0 || input.height == 0) {
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
            "Scene media thumbnail dimensions must be greater than zero"
        );
    }
    if (input.pixels == nullptr) {
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
            "Scene media thumbnail pixels are required"
        );
    }
    const std::size_t minimumRowBytes =
        static_cast<std::size_t>(input.width) * 4;
    if (input.bytes_per_row < minimumRowBytes) {
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
            "Scene media thumbnail row bytes are smaller than one RGBA8 row"
        );
    }
    if (static_cast<std::size_t>(input.bytes_per_row) >
        std::numeric_limits<std::size_t>::max() / input.height) {
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
            "Scene media thumbnail storage length overflows size_t"
        );
    }
    const std::size_t borrowedLength =
        static_cast<std::size_t>(input.bytes_per_row) * input.height;
    if (input.pixel_length != borrowedLength) {
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
            "Scene media thumbnail pixel length does not match its row layout"
        );
    }
    if (minimumRowBytes >
        std::numeric_limits<std::size_t>::max() / input.height) {
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
            "Scene media thumbnail packed length overflows size_t"
        );
    }
    const std::size_t packedLength = minimumRowBytes * input.height;
    if (packedLength > 256 * 1024 * 1024) {
        throw metal::Error(
            metal::ErrorCode::invalidArgument,
            "Scene media thumbnail exceeds the 256 MiB allocation limit"
        );
    }

    metal::MediaThumbnailRGBA8 result{
        .revision = input.revision,
        .width = input.width,
        .height = input.height,
        .pixels = std::vector<std::uint8_t>(packedLength),
    };
    for (std::uint32_t row = 0; row < input.height; ++row) {
        std::copy_n(
            input.pixels + static_cast<std::size_t>(row) *
                input.bytes_per_row,
            minimumRowBytes,
            result.pixels.data() + static_cast<std::size_t>(row) *
                minimumRowBytes
        );
    }
    return result;
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
    std::optional<metal::PresentationViewport> presentation,
    std::optional<metal::PresentationScaling> scaling,
    std::optional<metal::PhysicalRenderTarget> physicalTarget,
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
        const metal::FrameInputs frameInputs{
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
            if (physicalTarget) {
                executor->executor->render(
                    frameInputs, *presentation, *scaling, *physicalTarget
                );
            } else {
                executor->executor->render(
                    frameInputs, *presentation, *scaling
                );
            }
        } else {
            if (physicalTarget) {
                executor->executor->invalidateFrame();
                assignError(
                    outError,
                    WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Physical rendering requires an explicit presentation target"
                );
                return 0;
            }
            executor->executor->render(frameInputs);
        }
        return 1;
    } catch (const metal::Error& error) {
        executor->executor->invalidateFrame();
        if (error.code() == metal::ErrorCode::invalidArgument) {
            assignError(outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT, error.what());
        } else {
            assignMetalError(outError, error);
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
    std::optional<metal::PhysicalRenderTarget> physicalTarget,
    WESceneRuntimeErrorRef* outError
) {
    clearError(outError);
    if (!requireExecutor(executor, outError)) return 0;
    if (audioSpectrumRequired &&
        !requireAudioSpectrum(executor, audioSpectrum, outError)) {
        return 0;
    }
    if (drawableWidth == 0 || drawableHeight == 0 ||
        drawableWidth > maximumPresentationDimension ||
        drawableHeight > maximumPresentationDimension) {
        executor->executor->invalidateFrame();
        assignError(
            outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Drawable dimensions must be non-zero and fit the runtime dimension range"
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
        metal::drawablePresentationViewport(drawableWidth, drawableHeight),
        mode,
        physicalTarget,
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
    std::optional<metal::PhysicalRenderTarget> physicalTarget,
    WESceneRuntimeErrorRef* outError
) {
    clearError(outError);
    if (!requireExecutor(executor, outError)) return 0;
    if (audioSpectrumRequired &&
        !requireAudioSpectrum(executor, audioSpectrum, outError)) {
        return 0;
    }
    metal::PresentationViewport nativeViewport;
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
        executor, inputs, audioSpectrum, nativeViewport, mode,
        physicalTarget, outError
    );
}

int replayExecutor(
    WESceneFrameExecutorRef executor,
    const metal::PresentationViewport& viewport,
    std::optional<metal::PresentationScaling> scaling,
    std::optional<metal::PhysicalRenderTarget> physicalTarget,
    WESceneRuntimeErrorRef* outError
) {
    try {
        if (physicalTarget) {
            if (!scaling) {
                assignError(
                    outError, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Physical replay requires a presentation scaling mode"
                );
                return 0;
            }
            executor->executor->replay(viewport, *scaling, *physicalTarget);
        } else {
            executor->executor->replay(viewport);
        }
        return 1;
    } catch (const metal::Error& error) {
        assignMetalError(outError, error);
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
    void* metalDrawable,
    const metal::PresentationViewport& viewport,
    metal::PresentationScaling scaling,
    WESceneRuntimeErrorRef* outError
) {
    try {
        executor->executor->present(metalDrawable, viewport, scaling);
        return 1;
    } catch (const metal::Error& error) {
        assignMetalError(outError, error);
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
        handle->executor = std::make_unique<metal::FramePlanExecutor>(graph->graph);
        return handle.release();
    } catch (const metal::Error& error) {
        assignMetalError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "creating the scene frame executor", error.what());
    } catch (...) {
        assignExceptionError(out_error, "creating the scene frame executor", nullptr);
    }
    return nullptr;
}

extern "C" WESceneFrameExecutorRef we_scene_frame_executor_create_with_metal_device(
    WESceneFrameGraphRef graph,
    void* metal_device,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (graph == nullptr || !graph->graph || metal_device == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Scene frame graph and Metal device are required");
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneFrameExecutor>();
        handle->executor = std::make_unique<metal::FramePlanExecutor>(
            graph->graph, metal_device
        );
        return handle.release();
    } catch (const metal::Error& error) {
        assignMetalError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "creating the Metal scene executor", error.what());
    } catch (...) {
        assignExceptionError(out_error, "creating the Metal scene executor", nullptr);
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
                throw metal::Error(
                    metal::ErrorCode::invalidArgument,
                    "Scene sound runtime position must be finite and non-negative"
                );
            }
            if (!object_ids.emplace(input.object_id).second) {
                throw metal::Error(
                    metal::ErrorCode::invalidArgument,
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
                    throw metal::Error(
                        metal::ErrorCode::invalidArgument,
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
    } catch (const metal::Error& error) {
        if (error.code() == metal::ErrorCode::invalidArgument) {
            assignError(
                out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT, error.what()
            );
        } else {
            assignMetalError(out_error, error);
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
    } catch (const metal::Error& error) {
        if (error.code() == metal::ErrorCode::invalidArgument) {
            assignError(
                out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT, error.what()
            );
        } else {
            assignMetalError(out_error, error);
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

extern "C" int we_scene_frame_executor_set_media_thumbnail_rgba8(
    WESceneFrameExecutorRef executor,
    const WESceneMediaThumbnailRGBA8* thumbnail,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (thumbnail == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene media thumbnail is required"
        );
        return 0;
    }
    try {
        executor->executor->setMediaThumbnail(
            mediaThumbnailRGBA8(*thumbnail)
        );
        return 1;
    } catch (const metal::Error& error) {
        if (error.code() == metal::ErrorCode::invalidArgument) {
            assignError(
                out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                error.what()
            );
        } else {
            assignMetalError(out_error, error);
        }
    } catch (const std::exception& error) {
        assignExceptionError(
            out_error, "copying a Scene media thumbnail", error.what()
        );
    } catch (...) {
        assignExceptionError(
            out_error, "copying a Scene media thumbnail", nullptr
        );
    }
    return 0;
}

extern "C" int we_scene_frame_executor_clear_media_thumbnail(
    WESceneFrameExecutorRef executor,
    uint64_t revision,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    try {
        executor->executor->clearMediaThumbnail(revision);
        return 1;
    } catch (const metal::Error& error) {
        if (error.code() == metal::ErrorCode::invalidArgument) {
            assignError(
                out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                error.what()
            );
        } else {
            assignMetalError(out_error, error);
        }
    } catch (const std::exception& error) {
        assignExceptionError(
            out_error, "clearing a Scene media thumbnail", error.what()
        );
    } catch (...) {
        assignExceptionError(
            out_error, "clearing a Scene media thumbnail", nullptr
        );
    }
    return 0;
}

extern "C" int we_scene_frame_executor_render(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    WESceneRuntimeErrorRef* out_error
) {
    return renderExecutor(
        executor, inputs, nullptr, std::nullopt, std::nullopt,
        std::nullopt, out_error
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
        executor, inputs, audio_spectrum, std::nullopt, std::nullopt,
        std::nullopt, out_error
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
        drawable_width, drawable_height, scaling, std::nullopt, out_error
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
        drawable_width, drawable_height, scaling, std::nullopt, out_error
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
        executor, inputs, nullptr, false, viewport, scaling,
        std::nullopt, out_error
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
        executor, inputs, audio_spectrum, true, viewport, scaling,
        std::nullopt, out_error
    );
}

extern "C" int
we_scene_frame_executor_render_for_drawable_with_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    metal::PhysicalRenderTarget nativeTarget;
    if (!requirePhysicalRenderTarget(
            executor, physical_target, true, nativeTarget, out_error
        )) return 0;
    return renderExecutorForDrawable(
        executor, inputs, nullptr, false,
        drawable_width, drawable_height, scaling, nativeTarget, out_error
    );
}

extern "C" int
we_scene_frame_executor_render_for_drawable_with_audio_spectrum_and_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    metal::PhysicalRenderTarget nativeTarget;
    if (!requirePhysicalRenderTarget(
            executor, physical_target, true, nativeTarget, out_error
        )) return 0;
    return renderExecutorForDrawable(
        executor, inputs, audio_spectrum, true,
        drawable_width, drawable_height, scaling, nativeTarget, out_error
    );
}

extern "C" int
we_scene_frame_executor_render_for_viewport_with_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    metal::PhysicalRenderTarget nativeTarget;
    if (!requirePhysicalRenderTarget(
            executor, physical_target, true, nativeTarget, out_error
        )) return 0;
    return renderExecutorForViewport(
        executor, inputs, nullptr, false, viewport, scaling,
        nativeTarget, out_error
    );
}

extern "C" int
we_scene_frame_executor_render_for_viewport_with_audio_spectrum_and_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WESceneFrameInputs* inputs,
    const WESceneAudioSpectrumInputs* audio_spectrum,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    metal::PhysicalRenderTarget nativeTarget;
    if (!requirePhysicalRenderTarget(
            executor, physical_target, true, nativeTarget, out_error
        )) return 0;
    return renderExecutorForViewport(
        executor, inputs, audio_spectrum, true, viewport, scaling,
        nativeTarget, out_error
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
        drawable_width > maximumPresentationDimension ||
        drawable_height > maximumPresentationDimension) {
        executor->executor->invalidateFrame();
        assignError(
            out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Drawable dimensions must be non-zero and fit the runtime dimension range"
        );
        return 0;
    }
    return replayExecutor(
        executor,
        metal::drawablePresentationViewport(drawable_width, drawable_height),
        std::nullopt,
        std::nullopt,
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
    metal::PresentationViewport nativeViewport;
    if (!requirePresentationViewport(
            executor, viewport, true, nativeViewport, out_error)) return 0;
    return replayExecutor(
        executor, nativeViewport, std::nullopt, std::nullopt, out_error
    );
}

extern "C" int
we_scene_frame_executor_replay_for_drawable_with_physical_render_target(
    WESceneFrameExecutorRef executor,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (drawable_width == 0 || drawable_height == 0 ||
        drawable_width > maximumPresentationDimension ||
        drawable_height > maximumPresentationDimension) {
        executor->executor->invalidateFrame();
        assignError(
            out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Drawable dimensions must be non-zero and fit the runtime dimension range"
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
    metal::PhysicalRenderTarget nativeTarget;
    if (!requirePhysicalRenderTarget(
            executor, physical_target, true, nativeTarget, out_error
        )) return 0;
    return replayExecutor(
        executor,
        metal::drawablePresentationViewport(drawable_width, drawable_height),
        *mode,
        nativeTarget,
        out_error
    );
}

extern "C" int
we_scene_frame_executor_replay_for_viewport_with_physical_render_target(
    WESceneFrameExecutorRef executor,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    const WEScenePhysicalRenderTarget* physical_target,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    metal::PresentationViewport nativeViewport;
    if (!requirePresentationViewport(
            executor, viewport, true, nativeViewport, out_error
        )) return 0;
    const auto mode = presentationScaling(scaling);
    if (!mode) {
        executor->executor->invalidateFrame();
        assignError(
            out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Unknown scene presentation scaling mode"
        );
        return 0;
    }
    metal::PhysicalRenderTarget nativeTarget;
    if (!requirePhysicalRenderTarget(
            executor, physical_target, true, nativeTarget, out_error
        )) return 0;
    return replayExecutor(
        executor, nativeViewport, *mode, nativeTarget, out_error
    );
}

extern "C" int we_scene_frame_executor_present(
    WESceneFrameExecutorRef executor,
    void* metal_drawable,
    uint32_t drawable_width,
    uint32_t drawable_height,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (metal_drawable == nullptr || drawable_width == 0 ||
        drawable_height == 0) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Metal drawable and non-zero dimensions are required");
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
        metal_drawable,
        metal::drawablePresentationViewport(drawable_width, drawable_height),
        *mode,
        out_error
    );
}

extern "C" int we_scene_frame_executor_present_for_viewport(
    WESceneFrameExecutorRef executor,
    void* metal_drawable,
    const WEScenePresentationViewport* viewport,
    WEScenePresentationScaling scaling,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (metal_drawable == nullptr) {
        assignError(
            out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Metal drawable is required"
        );
        return 0;
    }
    metal::PresentationViewport nativeViewport;
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
        executor, metal_drawable, nativeViewport, *mode, out_error
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

extern "C" int we_scene_frame_executor_framebuffer_resource_stats(
    WESceneFrameExecutorRef executor,
    WESceneFramebufferResourceStats* out_stats,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error) ||
        !requireOutput(out_stats, out_error, "framebuffer resource statistics")) {
        return 0;
    }
    const metal::FramebufferResourceStats stats =
        executor->executor->framebufferResourceStats();
    *out_stats = {
        .framebuffer_count = stats.framebufferCount,
        .color_attachment_count = stats.colorAttachmentCount,
        .depth_attachment_count = stats.depthAttachmentCount,
        .inactive_framebuffer_count = stats.inactiveFramebufferCount,
        .inactive_color_attachment_count = stats.inactiveColorAttachmentCount,
        .inactive_depth_attachment_count = stats.inactiveDepthAttachmentCount,
    };
    return 1;
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
    switch (value.playbackMode) {
        case FrameSoundPlaybackMode::loop:
            out_info->playback_mode = WE_SCENE_FRAME_SOUND_PLAYBACK_LOOP;
            break;
        case FrameSoundPlaybackMode::random:
            out_info->playback_mode = WE_SCENE_FRAME_SOUND_PLAYBACK_RANDOM;
            break;
        case FrameSoundPlaybackMode::single:
            out_info->playback_mode = WE_SCENE_FRAME_SOUND_PLAYBACK_SINGLE;
            break;
    }
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
    const metal::FrameExecutionIssue& value = issues->at(index);
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
    } catch (const metal::Error& error) {
        assignMetalError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "reading a rendered scene frame", error.what());
    } catch (...) {
        assignExceptionError(out_error, "reading a rendered scene frame", nullptr);
    }
    return 0;
}

extern "C" int we_scene_frame_executor_read_rgba8_async(
    WESceneFrameExecutorRef executor,
    void* context,
    WESceneFrameExecutorRGBA8Callback callback,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireExecutor(executor, out_error)) return 0;
    if (callback == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "RGBA8 readback callback is required"
        );
        return 0;
    }
    try {
        executor->executor->readRGBA8Async(
            [context, callback](
                std::vector<std::uint8_t> pixels,
                std::string errorMessage
            ) noexcept {
                callback(
                    context,
                    pixels.empty() ? nullptr : pixels.data(),
                    pixels.size(),
                    errorMessage.empty() ? nullptr : errorMessage.c_str()
                );
            }
        );
        return 1;
    } catch (const metal::Error& error) {
        assignMetalError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(
            out_error,
            "scheduling rendered scene frame readback",
            error.what()
        );
    } catch (...) {
        assignExceptionError(
            out_error,
            "scheduling rendered scene frame readback",
            nullptr
        );
    }
    return 0;
}
