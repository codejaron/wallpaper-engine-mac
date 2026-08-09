#include <SceneGL/FramePlanExecutor.hpp>
#include <SceneGL/FramebufferPlanRequirements.hpp>

#include "SceneGLDevice.hpp"
#include "SceneGLPresentation.hpp"
#include "TextCoverageRenderer.hpp"

#include <SceneCore/FormatError.hpp>
#include <SceneCore/PuppetMesh.hpp>
#include <SceneCore/Runtime.hpp>
#include <SceneShader/ShaderCompiler.hpp>
#include <SceneShader/ShaderPreprocessor.hpp>
#include <SceneText/SceneText.hpp>

#include <OpenGL/gl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <os/log.h>

namespace we::scene::gl {
namespace {

using FrameTraceClock = std::chrono::steady_clock;

[[nodiscard]] bool frameTraceEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("WE_SCENE_FRAME_TRACE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

[[nodiscard]] bool frameStatsEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("WE_SCENE_FRAME_STATS");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

void frameStatsLog(const char* format, ...) noexcept {
    if (!frameStatsEnabled()) return;
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "[SceneFrameStats] %s\n", message);
}

void frameTraceLog(const char* format, ...) noexcept {
    if (!frameTraceEnabled()) return;
    char message[2048];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    os_log_with_type(
        OS_LOG_DEFAULT,
        OS_LOG_TYPE_INFO,
        "[SceneFrameTrace] %{public}s",
        message
    );
}

[[nodiscard]] double frameTraceMilliseconds(
    FrameTraceClock::time_point start,
    FrameTraceClock::time_point end = FrameTraceClock::now()
) noexcept {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

constexpr double wallpaperEnginePointSizeToPixels = 4.0;
constexpr double maximumWallpaperEngineTextPixelSize = 1024.0;

double textPixelSize(double pointSize) {
    const double scaled = pointSize * wallpaperEnginePointSizeToPixels;
    if (!std::isfinite(scaled) || scaled <= 0.0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text raster point size must be finite and greater than zero"
        );
    }
    return std::clamp(
        std::round(scaled),
        1.0,
        maximumWallpaperEngineTextPixelSize
    );
}

struct TextLayout final {
    double alignmentX = 0.0;
    double alignmentY = 0.0;
};

[[nodiscard]] TextLayout textLayout(
    const FrameTextDescriptor& descriptor,
    const text::RasterizedText& rasterized
) {
    const std::array<double, 4> layoutComponents{
        descriptor.size.x, descriptor.size.y,
        descriptor.padding.x, descriptor.padding.y,
    };
    for (const double component : layoutComponents) {
        if (!std::isfinite(component) || component < 0.0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Text size and padding must be finite non-negative values"
            );
        }
    }

    const double logicalLeft = rasterized.logicalLeftFromBitmap;
    const double logicalTop = rasterized.logicalTopFromBitmap;
    const double logicalWidth = rasterized.typographicWidth;
    const double logicalHeight = rasterized.typographicHeight;
    const auto horizontalAnchor = [logicalLeft, logicalWidth](
        std::string_view alignment
    ) {
        if (alignment == "left") return -logicalLeft;
        if (alignment == "center") {
            return -(logicalLeft + logicalWidth * 0.5);
        }
        if (alignment == "right") {
            return -(logicalLeft + logicalWidth);
        }
        throw Error(
            ErrorCode::resourceValidation,
            "Unsupported text alignment '" + std::string(alignment) + "'"
        );
    };
    const auto verticalAnchor = [logicalTop, logicalHeight](
        std::string_view alignment
    ) {
        if (alignment == "top") {
            return -(logicalTop + logicalHeight);
        }
        if (alignment == "center") {
            return -(logicalTop + logicalHeight * 0.5);
        }
        if (alignment == "bottom") return -logicalTop;
        throw Error(
            ErrorCode::resourceValidation,
            "Unsupported text alignment '" + std::string(alignment) + "'"
        );
    };
    return {
        .alignmentX = horizontalAnchor(descriptor.horizontalAlignment),
        .alignmentY = verticalAnchor(descriptor.verticalAlignment),
    };
}

struct ActiveUniform final {
    std::string name;
    GLenum type = 0;
    GLint size = 0;
    GLint blockIndex = -1;
    GLint location = -1;
    bool isArray = false;
};

struct PreparedAudioSpectrumUniform final {
    GLint location = -1;
    GLsizei activeElementCount = 0;
};

struct ProgramResource final {
    GLuint program = 0;
    std::vector<ShaderParameterMetadata> parameters;
    std::vector<ActiveUniform> uniforms;
};

static_assert(std::is_nothrow_move_assignable_v<ProgramResource>);

struct Vertex final {
    float position[3];
    float texCoord[2];
};

struct ParticleVertex final {
    float position[3];
    float texCoordRotationSize[4];
    float color[4];
    float velocityLifetime[4];
    float rotationXY[2];
};

static_assert(sizeof(ParticleVertex) == sizeof(float) * 17);

// genericropeparticle consumes the thick-format 26-float vertex contract used
// by linux-wallpaperengine's CParticle::renderRope().  Keep a separate POD
// layout so the sprite path can continue to use the compact 17-float format.
struct RopeParticleVertex final {
    float positionVec4[4];
    float texCoordVec4[4];
    float texCoordVec4C1[4];
    float texCoordVec4C2[4];
    float texCoordVec4C3[4];
    float texCoordC4[2];
    float color[4];
};

static_assert(sizeof(RopeParticleVertex) == sizeof(float) * 26);

struct ParticleAtlasMetadata final {
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    std::uint32_t frames = 0;
    float duration = 0.0F;
    float frameAspect = 1.0F;

    [[nodiscard]] bool enabled() const noexcept { return frames > 0; }
};

struct ParticleDrawBatch final {
    std::vector<ParticleVertex> vertices;
    std::vector<RopeParticleVertex> ropeVertices;
    std::vector<std::uint32_t> indices;
    ParticleAtlasMetadata atlas;
    bool rope = false;
    float ropeSegmentMaxCountUnscaled = 0.0F;
    float ropeSegmentTimeOffset = 0.0F;
    float ropeSegmentMaxCount = 0.0F;
};

struct ResolvedFrameInputs final {
    FrameVector2 pointerPosition;
    FrameVector2 pointerPositionLast;
    FrameVector2 effectPointerPosition;
    FrameVector2 effectPointerPositionLast;
    bool pointerActive = false;
    bool pointerLeftDown = false;
    double timeSeconds = 0.0;
    double frameTimeSeconds = 0.0;
    std::optional<bool> isScreensaver;
    float daytime = 0.0F;
    double timeOfDay = 0.0;
    std::optional<AudioSpectrumFrame> audioSpectrum;
    std::optional<script::ScriptMediaSnapshot> mediaSnapshot;
};

struct CursorHit final {
    int layerId = 0;
    double worldX = 0.0;
    double worldY = 0.0;
    double worldZ = 0.0;
    double localX = 0.0;
    double localY = 0.0;
    double localZ = 0.0;
};

struct CursorProjection final {
    CursorHit hit;
    bool inside = false;
};

[[nodiscard]] double centeredWallpaperY(
    double wallpaperY,
    std::uint32_t sceneHeight
) noexcept {
    // Wallpaper Engine scene coordinates grow upward from the bottom edge.
    // The offscreen scene is presented with one final vertical flip, so
    // authored positions must be mirrored here exactly once.
    return static_cast<double>(sceneHeight) * 0.5 - wallpaperY;
}

[[nodiscard]] FrameVector2 linuxEffectPointer(
    FrameVector2 bottomLeftPointer
) noexcept {
    return {
        .x = bottomLeftPointer.x,
        .y = 1.0 - bottomLeftPointer.y,
    };
}

// Project the canonical bottom-left pointer into one image's local space.  The
// projection is deliberately usable outside the image bounds as well: a
// pressed layer keeps receiving drag/up events after the pointer leaves it.
[[nodiscard]] std::optional<CursorProjection> projectCursorImage(
    const FramePlan& plan,
    const FrameImageDescriptor& image,
    FrameVector2 pointer
) {
    if (!std::isfinite(pointer.x) || !std::isfinite(pointer.y) ||
        plan.width == 0 || plan.height == 0) {
        return std::nullopt;
    }
    const double worldX = std::clamp(pointer.x, 0.0, 1.0) *
        static_cast<double>(plan.width);
    const double worldY = std::clamp(pointer.y, 0.0, 1.0) *
        static_cast<double>(plan.height);
    const auto& transform = image.worldTransform;
    const double scaledWidth = image.size.x * transform.scale.x;
    const double scaledHeight = image.size.y * transform.scale.y;
    if (!std::isfinite(worldX) || !std::isfinite(worldY) ||
        !std::isfinite(scaledWidth) || !std::isfinite(scaledHeight) ||
        image.size.x <= 0.0 || image.size.y <= 0.0 ||
        transform.scale.x == 0.0 || transform.scale.y == 0.0 ||
        !std::isfinite(transform.origin.x) ||
        !std::isfinite(transform.origin.y) ||
        !std::isfinite(transform.origin.z) ||
        !std::isfinite(transform.angles.z)) {
        return std::nullopt;
    }

    // Keep pointer projection paired with prepareDraw's authored-scene to
    // offscreen-scene conversion, including the final presentation flip.
    double centerX = transform.origin.x -
        static_cast<double>(plan.width) * 0.5;
    double centerY = centeredWallpaperY(transform.origin.y, plan.height);
    if (image.horizontalAlignment.find("left") != std::string::npos) {
        centerX += scaledWidth * 0.5;
    } else if (image.horizontalAlignment.find("right") != std::string::npos) {
        centerX -= scaledWidth * 0.5;
    }
    if (image.horizontalAlignment.find("top") != std::string::npos) {
        centerY += scaledHeight * 0.5;
    } else if (image.horizontalAlignment.find("bottom") != std::string::npos) {
        centerY -= scaledHeight * 0.5;
    }

    const double sceneX = worldX - static_cast<double>(plan.width) * 0.5;
    const double sceneY = centeredWallpaperY(worldY, plan.height);
    const double dx = sceneX - centerX;
    const double dy = sceneY - centerY;
    const double cosine = std::cos(transform.angles.z);
    const double sine = std::sin(transform.angles.z);
    // prepareDraw applies R(-angle); inverse it with R(+angle).
    const double rotatedX = cosine * dx - sine * dy;
    const double rotatedY = sine * dx + cosine * dy;
    const double localX = rotatedX / transform.scale.x + image.size.x * 0.5;
    const double localY = rotatedY / transform.scale.y + image.size.y * 0.5;
    const double epsilon = 1e-9;
    return CursorProjection{
        .hit = CursorHit{
            .layerId = image.objectId,
            .worldX = worldX,
            .worldY = worldY,
            .worldZ = transform.origin.z,
            .localX = std::clamp(localX, 0.0, image.size.x),
            .localY = std::clamp(localY, 0.0, image.size.y),
            .localZ = 0.0,
        },
        .inside = localX >= -epsilon && localX <= image.size.x + epsilon &&
            localY >= -epsilon && localY <= image.size.y + epsilon,
    };
}

[[nodiscard]] std::optional<CursorHit> hitTestInteractiveImage(
    const FramePlan& plan,
    const FrameImageDescriptor& image,
    FrameVector2 pointer
) {
    if (!image.visible || !image.cursorInteractive) return std::nullopt;
    const auto projection = projectCursorImage(plan, image, pointer);
    if (!projection || !projection->inside) return std::nullopt;
    return projection->hit;
}

[[nodiscard]] std::vector<CursorHit> hitTestInteractiveLayers(
    const FramePlan& plan,
    FrameVector2 pointer
) {
    // Cursor callbacks are layer-local. Preserve front-to-back render order,
    // but dispatch to every interactive layer under the pointer; overlapping
    // controls keep independent SceneScript state in Wallpaper Engine.
    std::vector<CursorHit> hits;
    std::set<int> seenLayerIds;
    for (auto iterator = plan.images.rbegin(); iterator != plan.images.rend(); ++iterator) {
        if (const auto hit = hitTestInteractiveImage(plan, *iterator, pointer);
            hit && seenLayerIds.insert(hit->layerId).second) {
            hits.push_back(*hit);
        }
    }
    return hits;
}

[[nodiscard]] std::optional<CursorHit> projectCursorLayer(
    const FramePlan& plan,
    int layerId,
    FrameVector2 pointer
) {
    for (auto iterator = plan.images.rbegin(); iterator != plan.images.rend(); ++iterator) {
        if (iterator->objectId != layerId) continue;
        const auto projection = projectCursorImage(plan, *iterator, pointer);
        if (projection) return projection->hit;
    }
    return std::nullopt;
}

struct TextureAnimationSelection final {
    std::size_t imageIndex = 0;
    std::array<float, 2> translation{0.0F, 0.0F};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 0.0F};
    bool animated = false;
};

TextureAnimationSelection textureAnimationSelection(
    const AssetTextureResource& texture,
    const TextureFrame& selected
) {
    TextureAnimationSelection selection;
    selection.imageIndex = selected.frameNumber;
    if (selection.imageIndex >= texture.images.size() ||
        selection.imageIndex >= texture.imageWidths.size() ||
        selection.imageIndex >= texture.imageHeights.size()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Animated texture frame references an image outside the uploaded resource"
        );
    }
    const std::uint32_t width = texture.imageWidths[selection.imageIndex];
    const std::uint32_t height = texture.imageHeights[selection.imageIndex];
    if (width == 0 || height == 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Animated texture frame references an image with zero dimensions"
        );
    }
    const float inverseWidth = 1.0F / static_cast<float>(width);
    const float inverseHeight = 1.0F / static_cast<float>(height);
    selection.translation = {
        selected.x * inverseWidth,
        selected.y * inverseHeight,
    };
    selection.rotation = {
        selected.width * inverseWidth,
        selected.widthAux * inverseWidth,
        selected.heightAux * inverseHeight,
        selected.height * inverseHeight,
    };
    selection.animated = true;
    return selection;
}

TextureAnimationSelection selectTextureAnimationFrame(
    const AssetTextureResource& texture,
    std::size_t frame
) {
    if (!texture.isAnimated() || texture.frames.empty()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Texture animation controller targets a static texture"
        );
    }
    if (frame >= texture.frames.size()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Texture animation controller frame is outside the uploaded timeline"
        );
    }
    return textureAnimationSelection(texture, texture.frames[frame]);
}

TextureAnimationSelection selectTextureAnimation(
    const AssetTextureResource& texture,
    double timeSeconds
) {
    TextureAnimationSelection selection;
    if (!texture.isAnimated() || texture.frames.empty()) return selection;

    double duration = 0.0;
    const TextureFrame* lastPositiveDurationFrame = nullptr;
    for (const auto& frame : texture.frames) {
        if (!std::isfinite(frame.frameTime) || frame.frameTime < 0.0F) {
            throw Error(
                ErrorCode::resourceValidation,
                "Animated texture contains a negative or non-finite frame duration"
            );
        }
        duration += static_cast<double>(frame.frameTime);
        if (frame.frameTime > 0.0F) lastPositiveDurationFrame = &frame;
    }
    if (!std::isfinite(duration) || duration <= 0.0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Animated texture has a non-positive or non-finite total duration"
        );
    }

    const double phase = std::fmod(timeSeconds, duration);
    const TextureFrame* selected = nullptr;
    double remaining = phase;
    for (const auto& frame : texture.frames) {
        remaining -= static_cast<double>(frame.frameTime);
        if (remaining <= 0.0) {
            selected = &frame;
            break;
        }
    }
    // fmod is strictly below duration, but accumulated floating-point error can
    // leave a positive remainder. The final positive-duration timeline entry
    // owns that tail; trailing zero-duration entries do not occupy any time.
    if (selected == nullptr) selected = lastPositiveDurationFrame;

    return textureAnimationSelection(texture, *selected);
}

void applyTexture0FormatCombo(
    ComboMap& combos,
    TextureFormat format
) {
    switch (format) {
        case TextureFormat::rg88:
            combos.insert_or_assign(
                "TEX0FORMAT", static_cast<int>(TextureFormat::rg88)
            );
            break;
        case TextureFormat::r8:
            combos.insert_or_assign(
                "TEX0FORMAT", static_cast<int>(TextureFormat::r8)
            );
            break;
        default:
            break;
    }
}

PixelFormat pixelFormat(FramebufferFormat format) {
    switch (format) {
        case FramebufferFormat::rgba8: return PixelFormat::rgba8;
        case FramebufferFormat::r8: return PixelFormat::r8;
        case FramebufferFormat::rg16f: return PixelFormat::rg16f;
        case FramebufferFormat::r16f: return PixelFormat::r16f;
    }
    throw Error(ErrorCode::resourceValidation, "Unknown frame-plan format");
}

TextureWrap textureWrap(FramebufferWrapMode wrap) {
    switch (wrap) {
        case FramebufferWrapMode::clampToEdge: return TextureWrap::clampToEdge;
        case FramebufferWrapMode::clampToBorder: return TextureWrap::clampToBorder;
        case FramebufferWrapMode::repeat: return TextureWrap::repeat;
    }
    throw Error(ErrorCode::resourceValidation, "Unknown frame-plan wrap mode");
}

bool sameDescriptor(
    const FramebufferDescriptor& lhs,
    const FramebufferDescriptor& rhs
) {
    return lhs.resource == rhs.resource && lhs.format == rhs.format &&
        lhs.wrapMode == rhs.wrapMode && lhs.width == rhs.width &&
        lhs.height == rhs.height && lhs.scale == rhs.scale &&
        lhs.unique == rhs.unique;
}

bool framebufferRequirementSatisfies(
    const FramebufferAllocationRequirement& cached,
    const FramebufferAllocationRequirement& required
) {
    return sameDescriptor(cached.descriptor, required.descriptor) &&
        (!required.requiresDepthAttachment ||
         cached.requiresDepthAttachment);
}

static_assert(std::is_nothrow_move_assignable_v<FramebufferResource>);
static_assert(std::is_nothrow_swappable_v<FramebufferDescriptor>);
static_assert(
    std::is_nothrow_move_assignable_v<FramebufferAllocationRequirement>
);
static_assert(
    std::is_nothrow_swappable_v<FramebufferAllocationRequirement>
);
static_assert(
    std::is_nothrow_swappable_v<std::map<std::string, std::string>>
);
static_assert(noexcept(std::declval<Device::Session&>().destroyFramebuffer(
    std::declval<FramebufferResource&>()
)));

}  // namespace

namespace {

constexpr std::uint32_t maximumPresentationDimension =
    static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max());

[[nodiscard]] std::uint64_t rectEnd(
    std::uint32_t origin,
    std::uint32_t extent
) noexcept {
    return static_cast<std::uint64_t>(origin) + extent;
}

[[nodiscard]] PresentationRect intersectRect(
    const PresentationRect& lhs,
    const PresentationRect& rhs
) noexcept {
    const std::uint64_t left = std::max<std::uint64_t>(lhs.x, rhs.x);
    const std::uint64_t bottom = std::max<std::uint64_t>(lhs.y, rhs.y);
    const std::uint64_t right = std::min(
        rectEnd(lhs.x, lhs.width), rectEnd(rhs.x, rhs.width)
    );
    const std::uint64_t top = std::min(
        rectEnd(lhs.y, lhs.height), rectEnd(rhs.y, rhs.height)
    );
    if (right <= left || top <= bottom) return {};
    return {
        .x = static_cast<std::uint32_t>(left),
        .y = static_cast<std::uint32_t>(bottom),
        .width = static_cast<std::uint32_t>(right - left),
        .height = static_cast<std::uint32_t>(top - bottom),
    };
}

[[nodiscard]] std::uint32_t mapRectEdge(
    std::uint32_t value,
    std::uint32_t inputOrigin,
    std::uint32_t inputExtent,
    std::uint32_t outputOrigin,
    std::uint32_t outputExtent
) {
    const long double fraction = static_cast<long double>(value - inputOrigin) /
        static_cast<long double>(inputExtent);
    const long double mapped = static_cast<long double>(outputOrigin) +
        fraction * static_cast<long double>(outputExtent);
    const auto rounded = static_cast<std::int64_t>(std::llround(mapped));
    const std::int64_t lower = outputOrigin;
    const std::int64_t upper = static_cast<std::int64_t>(outputOrigin) +
        outputExtent;
    return static_cast<std::uint32_t>(std::clamp(rounded, lower, upper));
}

void ensureMappedInterval(
    std::uint32_t& start,
    std::uint32_t& end,
    std::uint32_t lower,
    std::uint32_t upper
) noexcept {
    if (end > start) return;
    if (start < upper) {
        end = start + 1;
    } else {
        start = upper - 1;
        end = upper;
    }
    start = std::max(start, lower);
}

}  // namespace

PresentationViewport drawablePresentationViewport(
    std::uint32_t drawableWidth,
    std::uint32_t drawableHeight
) {
    return {
        .canvasWidth = drawableWidth,
        .canvasHeight = drawableHeight,
        .viewportX = 0,
        .viewportY = 0,
        .viewportWidth = drawableWidth,
        .viewportHeight = drawableHeight,
        .drawableWidth = drawableWidth,
        .drawableHeight = drawableHeight,
    };
}

void validatePresentationViewport(const PresentationViewport& viewport) {
    if (viewport.canvasWidth == 0 || viewport.canvasHeight == 0 ||
        viewport.canvasWidth > maximumPresentationDimension ||
        viewport.canvasHeight > maximumPresentationDimension) {
        throw Error(
            ErrorCode::invalidArgument,
            "Virtual canvas dimensions must be non-zero and fit OpenGL's signed range"
        );
    }
    if (viewport.viewportWidth == 0 || viewport.viewportHeight == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Display viewport dimensions must be greater than zero"
        );
    }
    if (rectEnd(viewport.viewportX, viewport.viewportWidth) >
            viewport.canvasWidth ||
        rectEnd(viewport.viewportY, viewport.viewportHeight) >
            viewport.canvasHeight) {
        throw Error(
            ErrorCode::invalidArgument,
            "Display viewport must fit entirely within the virtual canvas"
        );
    }
    if (viewport.drawableWidth == 0 || viewport.drawableHeight == 0 ||
        viewport.drawableWidth > maximumPresentationDimension ||
        viewport.drawableHeight > maximumPresentationDimension) {
        throw Error(
            ErrorCode::invalidArgument,
            "Drawable dimensions must be non-zero and fit OpenGL's signed range"
        );
    }
}

PresentationTransform makePresentationTransform(
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    const PresentationViewport& viewport,
    PresentationScaling scaling
) {
    validatePresentationViewport(viewport);
    if (sourceWidth == 0 || sourceHeight == 0 ||
        sourceWidth > maximumPresentationDimension ||
        sourceHeight > maximumPresentationDimension) {
        throw Error(
            ErrorCode::invalidArgument,
            "Scene presentation dimensions must be non-zero and fit OpenGL's signed range"
        );
    }
    PresentationTransform result{
        .sourceWidth = sourceWidth,
        .sourceHeight = sourceHeight,
        .viewport = viewport,
        .source = {.width = sourceWidth, .height = sourceHeight},
        .canvasDestination = {
            .width = viewport.canvasWidth,
            .height = viewport.canvasHeight,
        },
    };
    switch (scaling) {
        case PresentationScaling::stretch:
            break;
        case PresentationScaling::aspectFit: {
            const double scaleX = static_cast<double>(viewport.canvasWidth) /
                sourceWidth;
            const double scaleY = static_cast<double>(viewport.canvasHeight) /
                sourceHeight;
            const double scale = std::min(scaleX, scaleY);
            result.canvasDestination.width = std::clamp<std::uint32_t>(
                static_cast<std::uint32_t>(std::lround(sourceWidth * scale)),
                1,
                viewport.canvasWidth
            );
            result.canvasDestination.height = std::clamp<std::uint32_t>(
                static_cast<std::uint32_t>(std::lround(sourceHeight * scale)),
                1,
                viewport.canvasHeight
            );
            result.canvasDestination.x =
                (viewport.canvasWidth - result.canvasDestination.width) / 2;
            result.canvasDestination.y =
                (viewport.canvasHeight - result.canvasDestination.height) / 2;
            break;
        }
        // Wallpaper Engine on Windows calls this default alignment "Cover":
        // preserve aspect ratio, fill the display, and crop the overflow.
        // Automatic is the host-facing name for that compatibility default.
        case PresentationScaling::automatic:
        case PresentationScaling::aspectFill: {
            const double sourceAspect =
                static_cast<double>(sourceWidth) / sourceHeight;
            const double canvasAspect =
                static_cast<double>(viewport.canvasWidth) /
                viewport.canvasHeight;
            if (sourceAspect > canvasAspect) {
                result.source.width = std::clamp<std::uint32_t>(
                    static_cast<std::uint32_t>(std::lround(
                        sourceHeight * canvasAspect
                    )),
                    1,
                    sourceWidth
                );
                result.source.x = (sourceWidth - result.source.width) / 2;
            } else if (sourceAspect < canvasAspect) {
                result.source.height = std::clamp<std::uint32_t>(
                    static_cast<std::uint32_t>(std::lround(
                        sourceWidth / canvasAspect
                    )),
                    1,
                    sourceHeight
                );
                result.source.y = (sourceHeight - result.source.height) / 2;
            }
            break;
        }
        default:
            throw Error(
                ErrorCode::invalidArgument,
                "Unknown scene presentation scaling mode"
            );
    }
    return result;
}

namespace {

[[nodiscard]] std::uint32_t scaledPhysicalDimension(
    std::uint32_t logicalDimension,
    std::uint32_t logicalOutputDimension,
    std::uint32_t physicalOutputDimension,
    const char* description
) {
    if (logicalDimension == 0 || logicalOutputDimension == 0 ||
        physicalOutputDimension == 0) {
        throw Error(
            ErrorCode::resourceValidation,
            std::string(description) + " requires non-zero logical and physical dimensions"
        );
    }
    const long double scaled = static_cast<long double>(logicalDimension) *
        physicalOutputDimension / logicalOutputDimension;
    if (!std::isfinite(scaled) || scaled > maximumPresentationDimension) {
        throw Error(
            ErrorCode::resourceValidation,
            std::string(description) + " exceeds OpenGL's signed dimension range"
        );
    }
    return std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::llround(scaled)),
        1,
        maximumPresentationDimension
    );
}

[[nodiscard]] std::uint32_t roundedPhysicalOutputDimension(
    std::uint32_t logicalDimension,
    double scale,
    const char* description
) {
    const double scaled = static_cast<double>(logicalDimension) * scale;
    if (!std::isfinite(scaled) || scaled > maximumPresentationDimension) {
        throw Error(
            ErrorCode::resourceValidation,
            std::string(description) + " exceeds OpenGL's signed dimension range"
        );
    }
    return std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::llround(scaled)),
        1,
        maximumPresentationDimension
    );
}

}  // namespace

PhysicalRenderSize physicalRenderSize(
    const FramePlan& logicalPlan,
    const PhysicalRenderTarget& target,
    PresentationScaling scaling
) {
    if (logicalPlan.width == 0 || logicalPlan.height == 0 ||
        logicalPlan.width > maximumPresentationDimension ||
        logicalPlan.height > maximumPresentationDimension ||
        target.backingWidth == 0 || target.backingHeight == 0 ||
        target.backingWidth > maximumPresentationDimension ||
        target.backingHeight > maximumPresentationDimension) {
        throw Error(
            ErrorCode::invalidArgument,
            "Logical and physical render dimensions must be non-zero and fit OpenGL's signed range"
        );
    }
    const double scaleX = static_cast<double>(target.backingWidth) /
        logicalPlan.width;
    const double scaleY = static_cast<double>(target.backingHeight) /
        logicalPlan.height;
    const double targetScale =
        scaling == PresentationScaling::aspectFit
            ? std::min(scaleX, scaleY)
            : std::max(scaleX, scaleY);
    const long double logicalPixelCount =
        static_cast<long double>(logicalPlan.width) * logicalPlan.height;
    constexpr long double balancedPixelCeiling =
        static_cast<long double>(1920) * 1080;
    const long double backingPixelCount = std::min(
        static_cast<long double>(target.backingWidth) * target.backingHeight,
        balancedPixelCeiling
    );
    const double pixelBudgetScale = static_cast<double>(std::sqrt(
        backingPixelCount / logicalPixelCount
    ));
    // Balanced is a backing-pixel budget, not an axis budget. Cover rendering
    // used to choose the larger axis ratio and could therefore allocate and
    // shade a source larger than the drawable only to crop those pixels during
    // presentation. Preserve the authored aspect ratio while bounding total
    // scene pixels by both the author output and the actual backing surface.
    const double balancedScale = std::min({
        1.0,
        targetScale,
        pixelBudgetScale,
    });
    PhysicalRenderSize balanced{
        .width = roundedPhysicalOutputDimension(
            logicalPlan.width, balancedScale, "Balanced physical render width"
        ),
        .height = roundedPhysicalOutputDimension(
            logicalPlan.height, balancedScale, "Balanced physical render height"
        ),
    };
    switch (target.quality) {
        case PhysicalRenderQuality::balanced:
            return balanced;
        case PhysicalRenderQuality::powerSaving:
            return {
                .width = roundedPhysicalOutputDimension(
                    balanced.width, 0.5, "Power-saving physical render width"
                ),
                .height = roundedPhysicalOutputDimension(
                    balanced.height, 0.5, "Power-saving physical render height"
                ),
            };
    }
    throw Error(
        ErrorCode::invalidArgument,
        "Unknown physical render quality"
    );
}

FramePlan withPhysicalRenderSize(
    const FramePlan& logicalPlan,
    PhysicalRenderSize physicalSize
) {
    if (physicalSize.width == 0 || physicalSize.height == 0 ||
        physicalSize.width > maximumPresentationDimension ||
        physicalSize.height > maximumPresentationDimension) {
        throw Error(
            ErrorCode::invalidArgument,
            "Physical render dimensions must be non-zero and fit OpenGL's signed range"
        );
    }
    FramePlan result = logicalPlan;
    bool foundOutput = false;
    for (FramebufferDescriptor& descriptor : result.framebuffers) {
        if (descriptor.width == 0 || descriptor.height == 0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Logical framebuffer descriptor has zero dimensions"
            );
        }
        descriptor.width = scaledPhysicalDimension(
            descriptor.width,
            logicalPlan.width,
            physicalSize.width,
            "Physical framebuffer width"
        );
        descriptor.height = scaledPhysicalDimension(
            descriptor.height,
            logicalPlan.height,
            physicalSize.height,
            "Physical framebuffer height"
        );
        if (descriptor.resource == logicalPlan.output) {
            descriptor.width = physicalSize.width;
            descriptor.height = physicalSize.height;
            foundOutput = true;
        }
    }
    if (!foundOutput) {
        throw Error(
            ErrorCode::resourceValidation,
            "Logical frame plan output has no framebuffer descriptor"
        );
    }
    result.width = physicalSize.width;
    result.height = physicalSize.height;
    return result;
}

FrameVector2 PresentationTransform::map(FrameVector2 drawablePoint) const {
    drawablePoint.x = std::clamp(drawablePoint.x, 0.0, 1.0);
    drawablePoint.y = std::clamp(drawablePoint.y, 0.0, 1.0);
    const double localPixelX = drawablePoint.x * viewport.drawableWidth;
    const double localPixelY = drawablePoint.y * viewport.drawableHeight;
    const double canvasPixelX = viewport.viewportX +
        localPixelX * viewport.viewportWidth / viewport.drawableWidth;
    const double canvasPixelY = viewport.viewportY +
        localPixelY * viewport.viewportHeight / viewport.drawableHeight;
    const double contentLeft = canvasDestination.x;
    const double contentBottom = canvasDestination.y;
    const double contentRight = rectEnd(
        canvasDestination.x, canvasDestination.width
    );
    const double contentTop = rectEnd(
        canvasDestination.y, canvasDestination.height
    );
    const double clampedX = std::clamp(
        canvasPixelX, contentLeft, contentRight
    );
    const double clampedY = std::clamp(
        canvasPixelY, contentBottom, contentTop
    );
    const double sourceX = source.x +
        (clampedX - canvasDestination.x) * source.width /
            canvasDestination.width;
    const double sourceY = source.y +
        (clampedY - canvasDestination.y) * source.height /
            canvasDestination.height;
    return {
        sourceX / sourceWidth,
        sourceY / sourceHeight,
    };
}

PresentationSlice PresentationTransform::slice() const {
    const PresentationRect display{
        .x = viewport.viewportX,
        .y = viewport.viewportY,
        .width = viewport.viewportWidth,
        .height = viewport.viewportHeight,
    };
    const PresentationRect visible = intersectRect(
        canvasDestination, display
    );
    if (visible.width == 0 || visible.height == 0) return {};

    std::uint32_t sourceLeft = mapRectEdge(
        visible.x,
        canvasDestination.x,
        canvasDestination.width,
        source.x,
        source.width
    );
    std::uint32_t sourceRight = mapRectEdge(
        visible.x + visible.width,
        canvasDestination.x,
        canvasDestination.width,
        source.x,
        source.width
    );
    std::uint32_t sourceBottom = mapRectEdge(
        visible.y,
        canvasDestination.y,
        canvasDestination.height,
        source.y,
        source.height
    );
    std::uint32_t sourceTop = mapRectEdge(
        visible.y + visible.height,
        canvasDestination.y,
        canvasDestination.height,
        source.y,
        source.height
    );
    ensureMappedInterval(
        sourceLeft, sourceRight, source.x, source.x + source.width
    );
    ensureMappedInterval(
        sourceBottom, sourceTop, source.y, source.y + source.height
    );

    std::uint32_t destinationLeft = mapRectEdge(
        visible.x,
        viewport.viewportX,
        viewport.viewportWidth,
        0,
        viewport.drawableWidth
    );
    std::uint32_t destinationRight = mapRectEdge(
        visible.x + visible.width,
        viewport.viewportX,
        viewport.viewportWidth,
        0,
        viewport.drawableWidth
    );
    std::uint32_t destinationBottom = mapRectEdge(
        visible.y,
        viewport.viewportY,
        viewport.viewportHeight,
        0,
        viewport.drawableHeight
    );
    std::uint32_t destinationTop = mapRectEdge(
        visible.y + visible.height,
        viewport.viewportY,
        viewport.viewportHeight,
        0,
        viewport.drawableHeight
    );
    ensureMappedInterval(
        destinationLeft, destinationRight, 0, viewport.drawableWidth
    );
    ensureMappedInterval(
        destinationBottom, destinationTop, 0, viewport.drawableHeight
    );

    return {
        .hasContent = true,
        .source = {
            .x = sourceLeft,
            .y = sourceBottom,
            .width = sourceRight - sourceLeft,
            .height = sourceTop - sourceBottom,
        },
        .destination = {
            .x = destinationLeft,
            .y = destinationBottom,
            .width = destinationRight - destinationLeft,
            .height = destinationTop - destinationBottom,
        },
    };
}

namespace {

std::string planIssues(const FramePlan& plan) {
    std::ostringstream message;
    message << "Frame plan revision " << plan.modelRevision
            << " is not executable";
    for (const auto& issue : plan.issues) {
        message << "\n- " << issue.message;
        if (!issue.assetPath.empty()) message << " [" << issue.assetPath << ']';
        if (!issue.jsonPointer.empty()) message << " at " << issue.jsonPointer;
    }
    return message.str();
}

std::array<float, 16> identityMatrix() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

using Matrix = std::array<float, 16>;

Matrix multiply(const Matrix& lhs, const Matrix& rhs) {
    Matrix result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int index = 0; index < 4; ++index) {
                result[column * 4 + row] +=
                    lhs[index * 4 + row] * rhs[column * 4 + index];
            }
        }
    }
    return result;
}

Matrix inverse(const Matrix& matrix, std::string_view description) {
    std::array<std::array<double, 8>, 4> augmented{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            augmented[row][column] = matrix[column * 4 + row];
        }
        augmented[row][row + 4] = 1.0;
    }

    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row) {
            if (std::abs(augmented[row][column]) >
                std::abs(augmented[pivot][column])) {
                pivot = row;
            }
        }
        const double divisor = augmented[pivot][column];
        if (!std::isfinite(divisor) ||
            std::abs(divisor) <= std::numeric_limits<double>::epsilon()) {
            throw Error(
                ErrorCode::resourceValidation,
                std::string(description) + " is singular"
            );
        }
        if (pivot != column) {
            std::swap(augmented[pivot], augmented[column]);
        }
        for (double& value : augmented[column]) value /= divisor;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column) continue;
            const double factor = augmented[row][column];
            for (std::size_t index = 0; index < 8; ++index) {
                augmented[row][index] -= factor * augmented[column][index];
            }
        }
    }

    Matrix result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const double value = augmented[row][column + 4];
            if (!std::isfinite(value) ||
                value > std::numeric_limits<float>::max() ||
                value < -std::numeric_limits<float>::max()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    std::string(description) +
                        " inverse contains a non-finite or out-of-range value"
                );
            }
            result[column * 4 + row] = static_cast<float>(value);
        }
    }
    return result;
}

float localDaytime() {
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) {
        throw Error(
            ErrorCode::internalFailure,
            "Reading local wall-clock time failed"
        );
    }
    std::tm local{};
    if (::localtime_r(&now, &local) == nullptr) {
        throw Error(
            ErrorCode::internalFailure,
            "Converting local wall-clock time failed"
        );
    }
    return static_cast<float>(local.tm_hour * 60 + local.tm_min) /
        (24.0F * 60.0F);
}

Matrix translation(float x, float y, float z) {
    Matrix result = identityMatrix();
    result[12] = x;
    result[13] = y;
    result[14] = z;
    return result;
}

Matrix scaling(float x, float y, float z) {
    Matrix result{};
    result[0] = x;
    result[5] = y;
    result[10] = z;
    result[15] = 1.0F;
    return result;
}

Matrix rotationX(float radians) {
    Matrix result = identityMatrix();
    result[5] = std::cos(radians);
    result[6] = std::sin(radians);
    result[9] = -std::sin(radians);
    result[10] = std::cos(radians);
    return result;
}

Matrix rotationY(float radians) {
    Matrix result = identityMatrix();
    result[0] = std::cos(radians);
    result[2] = -std::sin(radians);
    result[8] = std::sin(radians);
    result[10] = std::cos(radians);
    return result;
}

Matrix rotationZ(float radians) {
    Matrix result = identityMatrix();
    result[0] = std::cos(radians);
    result[1] = std::sin(radians);
    result[4] = -std::sin(radians);
    result[5] = std::cos(radians);
    return result;
}

Matrix orthographic(float left, float right, float bottom, float top, float near, float far) {
    if (!std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(bottom) || !std::isfinite(top) ||
        !std::isfinite(near) || !std::isfinite(far) ||
        left == right || bottom == top || near == far) {
        throw Error(ErrorCode::resourceValidation, "Degenerate orthographic projection");
    }
    Matrix result{};
    result[0] = 2.0F / (right - left);
    result[5] = 2.0F / (top - bottom);
    result[10] = -2.0F / (far - near);
    result[12] = -(right + left) / (right - left);
    result[13] = -(top + bottom) / (top - bottom);
    result[14] = -(far + near) / (far - near);
    result[15] = 1.0F;
    return result;
}

Matrix perspective(float fieldOfViewRadians, float aspect, float near, float far) {
    if (!std::isfinite(fieldOfViewRadians) ||
        !std::isfinite(aspect) || !std::isfinite(near) ||
        !std::isfinite(far) || fieldOfViewRadians <= 0.0F ||
        fieldOfViewRadians >= 3.14159265358979323846F ||
        aspect <= 0.0F || near <= 0.0F || far <= near) {
        throw Error(
            ErrorCode::resourceValidation,
            "Degenerate particle perspective projection"
        );
    }
    const float tangent = std::tan(fieldOfViewRadians * 0.5F);
    if (!std::isfinite(tangent) || tangent <= 0.0F) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective field of view is invalid"
        );
    }
    Matrix result{};
    result[0] = 1.0F / (aspect * tangent);
    result[5] = 1.0F / tangent;
    result[10] = -(far + near) / (far - near);
    result[11] = -1.0F;
    result[14] = -(2.0F * far * near) / (far - near);
    return result;
}

Vector3 subtract(Vector3 lhs, Vector3 rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vector3 normalized(Vector3 value) {
    const double length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (!std::isfinite(length) || length <= std::numeric_limits<double>::epsilon()) {
        throw Error(ErrorCode::resourceValidation, "Camera contains a zero-length direction");
    }
    return {value.x / length, value.y / length, value.z / length};
}

Vector3 cross(Vector3 lhs, Vector3 rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

double dot(Vector3 lhs, Vector3 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Matrix lookAt(const FrameCameraDescriptor& camera) {
    const Vector3 forward = normalized(subtract(camera.center, camera.eye));
    const Vector3 side = normalized(cross(forward, camera.up));
    const Vector3 up = cross(side, forward);
    Matrix result = identityMatrix();
    result[0] = float(side.x); result[4] = float(side.y); result[8] = float(side.z);
    result[1] = float(up.x); result[5] = float(up.y); result[9] = float(up.z);
    result[2] = float(-forward.x); result[6] = float(-forward.y); result[10] = float(-forward.z);
    result[12] = float(-dot(side, camera.eye));
    result[13] = float(-dot(up, camera.eye));
    result[14] = float(dot(forward, camera.eye));
    return result;
}

void validateCameraVector(Vector3 value, std::string_view name) {
    const auto valid = [](double component) {
        return std::isfinite(component) &&
            component <= std::numeric_limits<float>::max() &&
            component >= -std::numeric_limits<float>::max();
    };
    if (!valid(value.x) || !valid(value.y) || !valid(value.z)) {
        throw Error(
            ErrorCode::resourceValidation,
            "Camera " + std::string(name) + " contains a non-finite or out-of-range component"
        );
    }
}

Matrix sceneOrthographicViewProjection(
    const FrameCameraDescriptor& camera,
    float width,
    float height
) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0F || height <= 0.0F) {
        throw Error(
            ErrorCode::resourceValidation,
            "Camera orthographic projection dimensions must be finite and greater than zero"
        );
    }
    validateCameraVector(camera.center, "center");
    validateCameraVector(camera.eye, "eye");
    validateCameraVector(camera.up, "up");
    if (!std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
        camera.nearPlane == camera.farPlane) {
        throw Error(
            ErrorCode::resourceValidation,
            "Camera near and far planes must be finite and distinct"
        );
    }

    // linux-wallpaperengine b016d7d Camera::setOrthogonalProjection composes
    // the eye translation into the projection before applying glm::lookAt.
    // Keep that ordering for Wallpaper Engine scene-camera parity.
    const Matrix projection = orthographic(
        -width * 0.5F, width * 0.5F,
        -height * 0.5F, height * 0.5F,
        static_cast<float>(camera.nearPlane),
        static_cast<float>(camera.farPlane)
    );
    return multiply(
        multiply(
            projection,
            translation(
                static_cast<float>(camera.eye.x),
                static_cast<float>(camera.eye.y),
                static_cast<float>(camera.eye.z)
            )
        ),
        lookAt(camera)
    );
}

struct ParticleView final {
    Matrix viewProjection;
    std::array<float, 3> eyePosition{};
};

ParticleView particlePerspectiveView(
    const FrameCameraDescriptor& camera,
    float width,
    float height
) {
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0F || height <= 0.0F ||
        !std::isfinite(camera.fieldOfView) ||
        camera.fieldOfView <= 0.0 || camera.fieldOfView >= 180.0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective dimensions or field of view are invalid"
        );
    }
    if (!std::isfinite(camera.nearPlane) ||
        !std::isfinite(camera.farPlane) ||
        camera.nearPlane < 0.0 || camera.farPlane <= 0.0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective clipping planes are invalid"
        );
    }
    constexpr double degreesToRadians =
        0.017453292519943295769236907684886;
    const double fieldOfViewRadians = camera.fieldOfView * degreesToRadians;
    const double tangent = std::tan(fieldOfViewRadians * 0.5);
    const double eyeDistance = (static_cast<double>(height) * 0.5) / tangent;
    if (!std::isfinite(eyeDistance) || eyeDistance <= 0.0 ||
        eyeDistance > std::numeric_limits<float>::max()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective eye distance is invalid"
        );
    }
    constexpr double minimumNearPlane = 0.01;
    const double nearPlane = camera.nearPlane > 0.0
        ? camera.nearPlane : minimumNearPlane;
    const double minimumFarPlane = eyeDistance +
        std::max(static_cast<double>(width), static_cast<double>(height));
    const double farPlane = std::max(camera.farPlane, minimumFarPlane);
    if (!std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
        farPlane <= nearPlane ||
        farPlane > std::numeric_limits<float>::max()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Particle perspective clipping planes are invalid"
        );
    }
    FrameCameraDescriptor particleCamera = camera;
    particleCamera.eye = {0.0, 0.0, eyeDistance};
    particleCamera.center = {0.0, 0.0, 0.0};
    particleCamera.up = {0.0, 1.0, 0.0};
    const Matrix projection = perspective(
        static_cast<float>(fieldOfViewRadians),
        width / height,
        static_cast<float>(nearPlane),
        static_cast<float>(farPlane)
    );
    return {
        .viewProjection = multiply(projection, lookAt(particleCamera)),
        .eyePosition = {0.0F, 0.0F, static_cast<float>(eyeDistance)},
    };
}

FrameVector2 sceneParallaxOffset(
    const FramePlan& plan,
    const FrameVector2& depth,
    const FrameVector2& displacement,
    std::string_view subject
) {
    const double referenceSize = static_cast<double>(plan.width);
    // The smoothed displacement already includes the global parallax amount.
    // A zero authored depth is the stationary camera plane.
    const FrameVector2 offset{
        .x = depth.x * displacement.x * referenceSize,
        .y = depth.y * displacement.y * referenceSize,
    };
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y) ||
        std::abs(offset.x) > std::numeric_limits<float>::max() ||
        std::abs(offset.y) > std::numeric_limits<float>::max()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Camera parallax produced a non-finite or out-of-range " +
                std::string(subject) + " offset"
        );
    }
    return offset;
}

void bindMatrix(GLint location, const std::array<float, 16>& value) {
    if (location >= 0) glUniformMatrix4fv(location, 1, GL_FALSE, value.data());
}

void bindMatrix3(GLint location, const std::array<float, 9>& value) {
    if (location >= 0) glUniformMatrix3fv(location, 1, GL_FALSE, value.data());
}

void bindVector2(GLint location, const std::array<float, 2>& value) {
    if (location >= 0) glUniform2fv(location, 1, value.data());
}

void bindVector4(GLint location, const std::array<float, 4>& value) {
    if (location >= 0) glUniform4fv(location, 1, value.data());
}

void bindVector3(GLint location, const std::array<float, 3>& value) {
    if (location >= 0) glUniform3fv(location, 1, value.data());
}

std::vector<GLfloat> numericComponents(
    const RuntimeValue& value,
    std::size_t count,
    std::string_view name
) {
    if (count > value.vector().size()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Uniform '" + std::string(name) + "' requests too many components"
        );
    }
    std::vector<GLfloat> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double number = value.vector()[index];
        if (!std::isfinite(number) ||
            number > std::numeric_limits<GLfloat>::max() ||
            number < -std::numeric_limits<GLfloat>::max()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + std::string(name) + "' contains a non-finite or out-of-range number"
            );
        }
        result.push_back(static_cast<GLfloat>(number));
    }
    return result;
}

RuntimeValue metadataDefault(const ShaderParameterDefault& input) {
    return std::visit([](const auto& value) -> RuntimeValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::vector<double>>) {
            if (value.size() < 2 || value.size() > 4) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Shader vector default must have two to four components"
                );
            }
            std::array<double, 4> components{};
            std::copy(value.begin(), value.end(), components.begin());
            return RuntimeValue::vector(components, value.size());
        } else if constexpr (std::is_same_v<T, std::string>) {
            return RuntimeValue::initialString(value);
        } else if constexpr (std::is_same_v<T, bool>) {
            return RuntimeValue::boolean(value);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return RuntimeValue::integer(value);
        } else {
            return RuntimeValue::floating(value);
        }
    }, input);
}

bool isSamplerParameter(const ShaderParameterMetadata& metadata) {
    return metadata.type.starts_with("sampler") ||
        metadata.type.starts_with("isampler") ||
        metadata.type.starts_with("usampler");
}

std::vector<ActiveUniform> activeUniforms(GLuint program) {
    GLint count = 0;
    GLint maximumNameLength = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maximumNameLength);
    std::vector<GLchar> buffer(
        static_cast<std::size_t>(std::max(1, maximumNameLength))
    );
    std::vector<ActiveUniform> result;
    result.reserve(static_cast<std::size_t>(std::max(0, count)));
    for (GLint index = 0; index < count; ++index) {
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(
            program, static_cast<GLuint>(index), maximumNameLength, &length,
            &size, &type, buffer.data()
        );
        std::string name(buffer.data(), static_cast<std::size_t>(length));
        const bool isArray = name.ends_with("[0]");
        if (isArray) {
            name.resize(name.size() - 3);
        }
        const GLuint uniformIndex = static_cast<GLuint>(index);
        GLint blockIndex = -1;
        glGetActiveUniformsiv(
            program, 1, &uniformIndex, GL_UNIFORM_BLOCK_INDEX, &blockIndex
        );
        const GLint location = glGetUniformLocation(program, name.c_str());
        result.push_back({
            .name = std::move(name),
            .type = type,
            .size = size,
            .blockIndex = blockIndex,
            .location = location,
            .isArray = isArray,
        });
    }
    return result;
}

const ActiveUniform* activeUniform(
    const ProgramResource& program,
    std::string_view name
) {
    const auto found = std::find_if(
        program.uniforms.begin(), program.uniforms.end(),
        [name](const ActiveUniform& uniform) { return uniform.name == name; }
    );
    return found == program.uniforms.end() ? nullptr : &*found;
}

GLint prepareBuiltinUniform(
    const ProgramResource& program,
    const std::string& name,
    GLenum expectedType
) {
    const ActiveUniform* uniform = activeUniform(program, name);
    if (uniform == nullptr) return -1;
    if (uniform->blockIndex >= 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin uniform '" + name +
                "' must not be declared in a uniform block"
        );
    }
    if (uniform->isArray || uniform->size != 1) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin uniform '" + name + "' must not be an array"
        );
    }
    if (uniform->type != expectedType) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin uniform '" + name + "' has an incompatible type"
        );
    }
    if (uniform->location < 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin uniform '" + name + "' has no bindable location"
        );
    }
    return uniform->location;
}

PreparedAudioSpectrumUniform prepareAudioSpectrumUniform(
    const ProgramResource& program,
    const std::string& name,
    GLint expectedSize
) {
    const ActiveUniform* uniform = activeUniform(program, name);
    if (uniform == nullptr) return {};
    if (uniform->blockIndex >= 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin audio uniform '" + name +
                "' must not be declared in a uniform block"
        );
    }
    // GL_ACTIVE_UNIFORM_SIZE is the number of active array elements, not
    // necessarily the source declaration length. A shader that only reads
    // element zero commonly reflects size 1 even when it declares float[16].
    if (!uniform->isArray || uniform->size <= 0 ||
        uniform->size > expectedSize) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin audio uniform '" + name + "' must be compatible with "
                "the float[" + std::to_string(expectedSize) + "] contract"
        );
    }
    if (uniform->type != GL_FLOAT) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin audio uniform '" + name + "' has an incompatible type"
        );
    }
    if (uniform->location < 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Builtin audio uniform '" + name + "' has no bindable location"
        );
    }
    return {
        .location = uniform->location,
        .activeElementCount = static_cast<GLsizei>(uniform->size),
    };
}

std::optional<int> textureSlot(std::string_view name) {
    constexpr std::string_view prefix = "g_Texture";
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return std::nullopt;
    }
    int slot = 0;
    for (const char digit : name.substr(prefix.size())) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        slot = slot * 10 + (digit - '0');
        if (slot >= 32) {
            return std::nullopt;
        }
    }
    return slot;
}

std::vector<std::pair<FrameTextureCandidateSource, std::string>>
samplerDefaultTextures(
    const std::vector<ShaderParameterMetadata>& parameters,
    std::string_view name
) {
    std::vector<std::pair<FrameTextureCandidateSource, std::string>> result;
    for (const auto& parameter : parameters) {
        if (parameter.name != name || !isSamplerParameter(parameter) ||
            !parameter.defaultValue) {
            continue;
        }
        if (const auto* value =
                std::get_if<std::string>(&*parameter.defaultValue)) {
            result.emplace_back(
                parameter.stage == ShaderParameterMetadata::Stage::vertex
                    ? FrameTextureCandidateSource::shaderVertexDefault
                    : FrameTextureCandidateSource::shaderFragmentDefault,
                *value
            );
            continue;
        }
        throw Error(
            ErrorCode::resourceValidation,
            "Sampler metadata default for '" + std::string(name) +
                "' must be a texture name"
        );
    }
    return result;
}

bool isOpenGLSamplerType(GLenum type) {
    switch (type) {
        case GL_SAMPLER_1D:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_1D_SHADOW:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_2D_RECT:
        case GL_SAMPLER_2D_RECT_SHADOW:
        case GL_SAMPLER_1D_ARRAY:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_1D_ARRAY_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_BUFFER:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
        case GL_INT_SAMPLER_1D:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_2D_RECT:
        case GL_INT_SAMPLER_1D_ARRAY:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_1D:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
        case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
            return true;
        default:
            return false;
    }
}

void configureState(const FrameRenderPass& pass) {
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    switch (pass.blending) {
        case BlendingMode::normal:
            glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
            break;
        case BlendingMode::translucent:
            glBlendFuncSeparate(
                GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
            );
            break;
        case BlendingMode::additive:
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_SRC_ALPHA, GL_ONE);
            break;
    }
    pass.depthTest == DepthMode::enabled ? glEnable(GL_DEPTH_TEST)
                                         : glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(pass.depthWrite == DepthMode::enabled ? GL_TRUE : GL_FALSE);
    glFrontFace(GL_CCW);
    if (pass.culling == CullingMode::disabled) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, pass.writeAlpha ? GL_TRUE : GL_FALSE);
    // Image layers are 2D scene planes and commonly sit at z=0 even when a
    // project declares a small positive nearz. Clamping keeps that plane
    // visible without discarding the camera's near/far range for depth tests.
    const bool sceneGeometry =
        pass.geometry == FrameGeometryKind::imageScene ||
        pass.geometry == FrameGeometryKind::passthroughCapture ||
        pass.geometry == FrameGeometryKind::puppetMesh;
    sceneGeometry ? glEnable(GL_DEPTH_CLAMP) : glDisable(GL_DEPTH_CLAMP);
}

}  // namespace

struct FramePlanExecutor::Impl final {
    struct CachedFramebuffer final {
        FramebufferAllocationRequirement requirement;
        FramebufferResource resource;
    };

    struct RopePathPoint final {
        particle::Vector3 position;
        double size = 0.0;
        particle::Vector3 color{1.0, 1.0, 1.0};
        double alpha = 1.0;
    };

    struct RopeTrailSample final {
        double timeSeconds = 0.0;
        RopePathPoint point;
    };

    struct RopeTrailHistory final {
        std::vector<RopeTrailSample> samples;
    };

    struct ParticleState final {
        std::string assetIdentity;
        particle::ParticleSimulation simulation;
        // Rope trails belong to individual stable particle identities. Keep
        // their history beside the simulation so frame preparation remains
        // transactional and replay copies one authoritative particle state.
        std::map<std::uint64_t, RopeTrailHistory> ropeTrails;
    };

    struct HostTextureSlot final {
        std::optional<MediaThumbnailRGBA8> image;
        GLuint texture = 0;
        bool dirty = false;
    };

    struct ImageAttributeState final {
        GLint position = -1;
        GLint texCoord = -1;
    };

    struct PreparedUniform final {
        enum class Kind { integer, float1, float2, float3, float4 };

        GLint location = -1;
        Kind kind = Kind::float1;
        GLint integer = 0;
        std::array<GLfloat, 4> floating{};
    };

    struct PreparedTextureBinding final {
        int slot = 0;
        FrameResourceRef resource;
        GLuint assetTexture = 0;
        std::array<float, 4> resolution{};
        GLint samplerLocation = -1;
        GLint resolutionLocation = -1;
    };

    struct PreparedCommonUniforms final {
        GLint textureReductionScale = -1;
        GLint lightAmbientColor = -1;
        GLint lightSkylightColor = -1;
        GLint brightness = -1;
        GLint userAlpha = -1;
        GLint alpha = -1;
        GLint color = -1;
        GLint color4 = -1;
        GLint compositeColor = -1;
        GLint time = -1;
        GLint daytime = -1;
        GLint modelViewProjection = -1;
        GLint modelViewProjectionInverse = -1;
        GLint effectModelViewProjection = -1;
        GLint model = -1;
        GLint effectModel = -1;
        GLint normalModel = -1;
        GLint viewProjection = -1;
        GLint pointer = -1;
        GLint pointerLast = -1;
        GLint effectTextureProjection = -1;
        GLint effectTextureProjectionInverse = -1;
        GLint texelSize = -1;
        GLint texelSizeHalf = -1;
        PreparedAudioSpectrumUniform audioSpectrum16Left;
        PreparedAudioSpectrumUniform audioSpectrum16Right;
        PreparedAudioSpectrumUniform audioSpectrum32Left;
        PreparedAudioSpectrumUniform audioSpectrum32Right;
        PreparedAudioSpectrumUniform audioSpectrum64Left;
        PreparedAudioSpectrumUniform audioSpectrum64Right;
        bool userAlphaProvidedByMaterial = false;
        bool compositeColorProvidedByShader = false;
        Matrix modelViewProjectionInverseValue = identityMatrix();
        std::array<float, 3> ambientColorValue{};
        std::array<float, 3> skylightColorValue{};
        float brightnessValue = 1.0F;
        float alphaValue = 1.0F;
        std::array<float, 3> colorValue{};
        std::array<float, 4> color4Value{};
        float timeValue = 0.0F;
        float daytimeValue = 0.0F;
        std::array<float, 2> pointerValue{};
        std::array<float, 2> pointerLastValue{};
        std::array<float, 2> texelSizeValue{};
        std::array<float, 2> texelSizeHalfValue{};
        AudioSpectrumFrame audioSpectrumValue;
    };

    struct CommonRenderableValues final {
        float brightness = 0.0F;
        float alpha = 0.0F;
        std::array<float, 4> color{};
    };

    struct PreparedImageUniforms final {
        GLint texture0Translation = -1;
        GLint texture0Rotation = -1;
        GLint alternateModel = -1;
        GLint alternateViewProjection = -1;
    };

    struct PreparedDraw final {
        FrameRenderPass pass;
        GLuint program = 0;
        std::vector<PreparedTextureBinding> textures;
        TextureAnimationSelection texture0Animation;
        PreparedImageUniforms uniforms;
        PreparedCommonUniforms commonUniforms;
        Matrix model = identityMatrix();
        Matrix viewProjection = identityMatrix();
        Matrix modelViewProjection = identityMatrix();
        std::vector<PreparedUniform> materialUniforms;
        std::array<Vertex, 6> vertices{};
        std::vector<Vertex> puppetVertices;
        std::vector<std::uint16_t> puppetIndices;
        GLint firstVertex = 0;
        GLint positionLocation = -1;
        GLint texCoordLocation = -1;
    };

    struct PreparedClear final {
        FrameResourceRef destination;
        std::array<GLfloat, 4> color{};
    };

    struct PreparedText final {
        FrameResourceRef destination;
        PreparedTextCoverage coverage;
        TextDrawRequest request;
    };

    struct PreparedParticleUniforms final {
        GLint texture0Translation = -1;
        GLint texture0Rotation = -1;
        GLint modelInverse = -1;
        GLint orientationUp = -1;
        GLint orientationRight = -1;
        GLint orientationForward = -1;
        GLint viewUp = -1;
        GLint viewRight = -1;
        GLint eyePosition = -1;
        GLint renderVar0 = -1;
        GLint renderVar1 = -1;
        GLint refractAmount = -1;
    };

    struct PreparedParticle final {
        FrameResourceRef destination;
        BlendingMode blending = BlendingMode::normal;
        CullingMode culling = CullingMode::disabled;
        DepthMode depthTest = DepthMode::disabled;
        DepthMode depthWrite = DepthMode::disabled;
        GLuint program = 0;
        std::vector<PreparedTextureBinding> textures;
        TextureAnimationSelection texture0Animation;
        PreparedParticleUniforms uniforms;
        PreparedCommonUniforms commonUniforms;
        Matrix model = identityMatrix();
        Matrix modelInverse = identityMatrix();
        Matrix viewProjection = identityMatrix();
        Matrix modelViewProjection = identityMatrix();
        std::array<float, 3> eyePosition{};
        std::array<float, 4> renderVar0{};
        std::array<float, 4> renderVar1{};
        std::vector<PreparedUniform> materialUniforms;
        std::array<GLint, 7> attributeLocations{};
        bool rope = false;
        bool refract = false;
        std::size_t operationIndex = 0;
    };

    struct PreparedCamera final {
        std::optional<Matrix> orthographicViewProjection;
        std::optional<ParticleView> particlePerspective;
        std::array<float, 3> eyePosition{};
    };

    struct ParticlePreparation final {
        PreparedParticle operation;
        std::optional<ParticleDrawBatch> batch;
        std::optional<ParticleState> state;
    };

    using PreparedOperation = std::variant<
        PreparedDraw,
        FrameSwapCommand,
        PreparedClear,
        PreparedText,
        PreparedParticle
    >;

    struct PreparedFrame final {
        std::vector<std::optional<PreparedOperation>> operations;
        std::vector<Vertex> imageVertices;
        std::map<std::size_t, ParticleDrawBatch> particleBatches;
        const std::map<std::size_t, ParticleDrawBatch>* frozenParticleBatches =
            nullptr;
        std::map<int, ParticleState> particleStates;
        std::map<std::string, std::string> finalAliases;
        std::vector<FrameExecutionIssue> issues;
    };

    struct ObjectOperationGroup final {
        std::size_t objectIndex = 0;
        int objectId = 0;
        std::vector<std::size_t> operationIndexes;
    };

    struct TextRasterKey final {
        std::string utf8;
        std::string font;
        double pointSize = 0.0;
        double maximumWidth = 0.0;
        std::size_t maximumRows = 0;
        bool useEllipsis = false;
        double characterSpacing = 0.0;
        double lineSpacing = 0.0;
        text::HorizontalAlignment horizontalAlignment =
            text::HorizontalAlignment::center;

        [[nodiscard]] bool operator<(const TextRasterKey& other) const {
            if (font != other.font) return font < other.font;
            if (utf8 != other.utf8) return utf8 < other.utf8;
            if (pointSize != other.pointSize) return pointSize < other.pointSize;
            if (maximumWidth != other.maximumWidth) {
                return maximumWidth < other.maximumWidth;
            }
            if (maximumRows != other.maximumRows) {
                return maximumRows < other.maximumRows;
            }
            if (useEllipsis != other.useEllipsis) {
                return useEllipsis < other.useEllipsis;
            }
            if (characterSpacing != other.characterSpacing) {
                return characterSpacing < other.characterSpacing;
            }
            if (lineSpacing != other.lineSpacing) {
                return lineSpacing < other.lineSpacing;
            }
            return horizontalAlignment < other.horizontalAlignment;
        }
    };

    struct CachedTextRaster final {
        text::RasterizedText rasterized;
        TextCoverageKey coverageKey;
        std::size_t bytes = 0;
        std::uint64_t lastUsed = 0;
    };

    static constexpr std::size_t maximumTextRasterEntries = 64;
    static constexpr std::size_t maximumTextRasterBytes = 32 * 1024 * 1024;

    struct LastFrameState final {
        FramePlan sourcePlan;
        FrameEvaluationState evaluation;
        ResolvedFrameInputs inputs;
        std::map<std::size_t, ParticleDrawBatch> particleBatches;
        std::vector<FrameExecutionIssue> issues;
    };

    explicit Impl(std::shared_ptr<SceneFrameGraph> graph)
        : frameGraph(std::move(graph)) {
        validateGraph();
        initializeDimensions();
    }
    Impl(std::shared_ptr<SceneFrameGraph> graph, CGLContextObj context)
        : frameGraph(std::move(graph)), borrowedContext(context) {
        validateGraph();
        if (borrowedContext == nullptr) {
            throw Error(ErrorCode::invalidArgument, "Borrowed OpenGL context is null");
        }
        initializeDimensions();
    }

    ~Impl() {
        if (!device) return;
        try {
            auto session = device->activate();
            textRenderer.release(session);
            presentationRenderer.release(session);
            releaseParticleGeometry(session);
            releasePuppetGeometry(session);
            session.destroyFramebuffer(particleRefractSnapshot);
            session.destroyTexture(mediaThumbnailCurrent.texture);
            session.destroyTexture(mediaThumbnailPrevious.texture);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FramePlanExecutor failed to release renderer resources: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "FramePlanExecutor failed to release renderer resources with an unknown error\n");
        }
    }

    void validateGraph() const {
        if (!frameGraph) {
            throw Error(ErrorCode::invalidArgument, "FramePlanExecutor requires a frame graph");
        }
    }

    void initializeDimensions() {
        if (frameGraph->requiresDrawableProjectionFallback()) return;
        const FramePlan initial = frameGraph->snapshot();
        updateDimensions(initial.width, initial.height);
    }

    [[nodiscard]] static std::size_t checkedRGBA8ByteCount(
        std::uint32_t newWidth,
        std::uint32_t newHeight
    ) {
        if (newHeight != 0 && static_cast<std::size_t>(newWidth) >
                std::numeric_limits<std::size_t>::max() / newHeight / 4) {
            throw Error(ErrorCode::resourceValidation, "RGBA8 output byte count overflows size_t");
        }
        return static_cast<std::size_t>(newWidth) * newHeight * 4;
    }

    void updateDimensions(std::uint32_t newWidth, std::uint32_t newHeight) {
        const std::size_t newByteCount = checkedRGBA8ByteCount(
            newWidth, newHeight
        );
        width = newWidth;
        height = newHeight;
        byteCount = newByteCount;
    }

    Device& ensureDevice() {
        if (!device) {
            device = borrowedContext == nullptr
                ? std::make_unique<Device>()
                : std::make_unique<Device>(borrowedContext);
        }
        return *device;
    }

    const AssetResolver& resolver() const {
        return frameGraph->graph()->model()->runtime()->assetResolver();
    }

    [[nodiscard]] std::uint64_t nextTextRasterUse() noexcept {
        if (textRasterUseSequence ==
            std::numeric_limits<std::uint64_t>::max()) {
            for (auto& [key, entry] : textRasters) {
                static_cast<void>(key);
                entry.lastUsed = 0;
            }
            textRasterUseSequence = 1;
        } else {
            ++textRasterUseSequence;
        }
        return textRasterUseSequence;
    }

    [[nodiscard]] const CachedTextRaster& cachedTextRaster(
        const FrameTextDescriptor& descriptor
    ) {
        const double pointSize = textPixelSize(descriptor.pointSize);
        text::HorizontalAlignment horizontalAlignment;
        if (descriptor.horizontalAlignment == "left") {
            horizontalAlignment = text::HorizontalAlignment::left;
        } else if (descriptor.horizontalAlignment == "center") {
            horizontalAlignment = text::HorizontalAlignment::center;
        } else if (descriptor.horizontalAlignment == "right") {
            horizontalAlignment = text::HorizontalAlignment::right;
        } else {
            throw Error(
                ErrorCode::resourceValidation,
                "Unsupported text alignment '" +
                    descriptor.horizontalAlignment + "'"
            );
        }
        const double maximumWidth = descriptor.limitWidth
            ? descriptor.maxWidth : 0.0;
        const std::size_t maximumRows = descriptor.limitRows
            ? static_cast<std::size_t>(descriptor.maxRows) : 0;
        TextRasterKey key{
            .utf8 = descriptor.text,
            .font = descriptor.font,
            .pointSize = pointSize,
            .maximumWidth = maximumWidth,
            .maximumRows = maximumRows,
            .useEllipsis = descriptor.limitUseEllipsis,
            .characterSpacing = descriptor.spacing.x,
            .lineSpacing = descriptor.spacing.y,
            .horizontalAlignment = horizontalAlignment,
        };
        if (auto found = textRasters.find(key); found != textRasters.end()) {
            found->second.lastUsed = nextTextRasterUse();
            return found->second;
        }

        text::FontSource font;
        std::optional<ResolvedAsset> fontAsset;
        if (descriptor.font.starts_with("systemfont_")) {
            font = text::FontSource::system("Arial");
        } else {
            fontAsset = resolver().resolve(descriptor.font);
            font = text::FontSource::bytes(fontAsset->bytes);
        }
        text::RasterizedText rasterized = text::rasterize({
            .utf8 = descriptor.text,
            .pointSize = pointSize,
            .font = font,
            .maximumWidth = maximumWidth,
            .maximumRows = maximumRows,
            .useEllipsis = descriptor.limitUseEllipsis,
            .characterSpacing = descriptor.spacing.x,
            .lineSpacing = descriptor.spacing.y,
            .horizontalAlignment = horizontalAlignment,
        });
        const TextCoverageKey coverageKey = TextCoverageRenderer::keyFor(
            rasterized
        );
        const std::size_t bytes = rasterized.coverage.size();
        if (bytes > std::numeric_limits<std::size_t>::max() -
                textRasterBytes) {
            throw Error(
                ErrorCode::internalFailure,
                "Text raster cache byte accounting overflowed"
            );
        }
        const auto [inserted, didInsert] = textRasters.try_emplace(
            std::move(key),
            CachedTextRaster{
                .rasterized = std::move(rasterized),
                .coverageKey = coverageKey,
                .bytes = bytes,
                .lastUsed = nextTextRasterUse(),
            }
        );
        if (!didInsert) {
            throw Error(
                ErrorCode::internalFailure,
                "Text raster cache key changed during insertion"
            );
        }
        textRasterBytes += bytes;
        return inserted->second;
    }

    [[nodiscard]] static std::uint32_t paddedTextEffectDimension(
        std::uint32_t rasterDimension,
        double padding,
        std::string_view name
    ) {
        const double value = static_cast<double>(rasterDimension) +
            padding * 2.0;
        if (!std::isfinite(value) || value < 1.0 ||
            value > static_cast<double>(
                std::numeric_limits<std::uint32_t>::max()
            )) {
            throw Error(
                ErrorCode::resourceValidation,
                "Text effect " + std::string(name) +
                    " must resolve to a finite positive 32-bit value"
            );
        }
        return static_cast<std::uint32_t>(std::ceil(value));
    }

    [[nodiscard]] static std::uint32_t scaledTextEffectDimension(
        double value,
        std::string_view name
    ) {
        if (!std::isfinite(value) || value <= 0.0 ||
            value > static_cast<double>(
                std::numeric_limits<std::uint32_t>::max()
            )) {
            throw Error(
                ErrorCode::resourceValidation,
                "Text effect " + std::string(name) +
                    " must resolve to a finite positive 32-bit value"
            );
        }
        return std::max<std::uint32_t>(
            1,
            static_cast<std::uint32_t>(std::floor(value))
        );
    }

    void resolveTextEffectGeometry(FramePlan& plan) {
        for (const FrameTextEffectDescriptor& effect : plan.textEffects) {
            if (effect.textIndex >= plan.texts.size() ||
                effect.imageIndex >= plan.images.size()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Text effect descriptor references an invalid text or image"
                );
            }
            const FrameTextDescriptor& textDescriptor =
                plan.texts[effect.textIndex];
            FrameImageDescriptor& imageDescriptor =
                plan.images[effect.imageIndex];
            if (textDescriptor.objectId != imageDescriptor.objectId) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Text effect proxy object identity is inconsistent"
                );
            }

            const text::RasterizedText& rasterized =
                cachedTextRaster(textDescriptor).rasterized;
            const TextLayout layout = textLayout(
                textDescriptor, rasterized
            );
            const std::uint32_t layerWidth = paddedTextEffectDimension(
                rasterized.width,
                textDescriptor.padding.x,
                "width"
            );
            const std::uint32_t layerHeight = paddedTextEffectDimension(
                rasterized.height,
                textDescriptor.padding.y,
                "height"
            );
            std::set<std::size_t> resolvedFramebufferIndexes;
            for (const FrameTextEffectFramebufferDescriptor& sizing :
                 effect.framebuffers) {
                if (sizing.framebufferIndex >= plan.framebuffers.size() ||
                    !resolvedFramebufferIndexes.emplace(
                        sizing.framebufferIndex
                    ).second) {
                    throw Error(
                        ErrorCode::resourceValidation,
                        "Text effect descriptor references an invalid or duplicate framebuffer"
                    );
                }
                FramebufferDescriptor& framebuffer =
                    plan.framebuffers[sizing.framebufferIndex];
                switch (sizing.sizing) {
                    case FrameTextEffectFramebufferSizing::relative:
                        if (!std::isfinite(sizing.value) ||
                            sizing.value <= 0.0) {
                            throw Error(
                                ErrorCode::resourceValidation,
                                "Text effect framebuffer scale must be finite and positive"
                            );
                        }
                        framebuffer.width = scaledTextEffectDimension(
                            static_cast<double>(layerWidth) / sizing.value,
                            "framebuffer width"
                        );
                        framebuffer.height = scaledTextEffectDimension(
                            static_cast<double>(layerHeight) / sizing.value,
                            "framebuffer height"
                        );
                        break;
                    case FrameTextEffectFramebufferSizing::fit: {
                        if (!std::isfinite(sizing.value) ||
                            sizing.value <= 0.0) {
                            throw Error(
                                ErrorCode::resourceValidation,
                                "Text effect framebuffer fit must be finite and positive"
                            );
                        }
                        const double fitScale = sizing.value /
                            static_cast<double>(
                                std::max(layerWidth, layerHeight)
                            );
                        framebuffer.width = scaledTextEffectDimension(
                            static_cast<double>(layerWidth) * fitScale,
                            "framebuffer width"
                        );
                        framebuffer.height = scaledTextEffectDimension(
                            static_cast<double>(layerHeight) * fitScale,
                            "framebuffer height"
                        );
                        break;
                    }
                    case FrameTextEffectFramebufferSizing::fixed:
                        break;
                }
            }

            imageDescriptor.size = {
                .x = static_cast<double>(layerWidth),
                .y = static_cast<double>(layerHeight),
            };
            imageDescriptor.worldTransform = textDescriptor.worldTransform;

            // The source bitmap starts at the authored padding inside its
            // effect framebuffer. Move the generated image proxy to the exact
            // center of that padded bitmap rectangle after applying the text
            // layer's scale and Z rotation. This factors the direct text
            // transform without changing alignment when the string changes.
            const double localCenterX = layout.alignmentX -
                textDescriptor.padding.x +
                static_cast<double>(layerWidth) * 0.5;
            const double localCenterY = layout.alignmentY -
                textDescriptor.padding.y +
                static_cast<double>(layerHeight) * 0.5;
            const double scaledCenterX = localCenterX *
                textDescriptor.worldTransform.scale.x;
            const double scaledCenterY = localCenterY *
                textDescriptor.worldTransform.scale.y;
            const double angle = textDescriptor.worldTransform.angles.z;
            if (!std::isfinite(scaledCenterX) ||
                !std::isfinite(scaledCenterY) || !std::isfinite(angle)) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Text effect transform must contain only finite values"
                );
            }
            const double cosine = std::cos(angle);
            const double sine = std::sin(angle);
            imageDescriptor.worldTransform.origin.x +=
                cosine * scaledCenterX + sine * scaledCenterY;
            imageDescriptor.worldTransform.origin.y +=
                -sine * scaledCenterX + cosine * scaledCenterY;
        }
    }

    void trimTextRasterCache() {
        while (textRasters.size() > maximumTextRasterEntries ||
               textRasterBytes > maximumTextRasterBytes) {
            const auto victim = std::min_element(
                textRasters.begin(),
                textRasters.end(),
                [](const auto& left, const auto& right) {
                    return left.second.lastUsed < right.second.lastUsed;
                }
            );
            if (victim == textRasters.end() ||
                victim->second.bytes > textRasterBytes) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Text raster cache metadata is inconsistent"
                );
            }
            textRasterBytes -= victim->second.bytes;
            textRasters.erase(victim);
        }
    }

    void clearNewFramebuffer(
        Device::Session& session,
        FramebufferResource& resource,
        bool clearDepth
    ) {
        glBindFramebuffer(GL_FRAMEBUFFER, resource.framebuffer);
        glViewport(
            0, 0,
            static_cast<GLsizei>(resource.width),
            static_cast<GLsizei>(resource.height)
        );
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT |
                (clearDepth ? GL_DEPTH_BUFFER_BIT : 0));
        session.checkError(
            ErrorCode::framebufferCreation,
            "initializing a new framebuffer to transparent"
        );
    }

    void clearNewDepthAttachment(
        Device::Session& session,
        FramebufferResource& resource
    ) {
        glBindFramebuffer(GL_FRAMEBUFFER, resource.framebuffer);
        glViewport(
            0, 0,
            static_cast<GLsizei>(resource.width),
            static_cast<GLsizei>(resource.height)
        );
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        session.checkError(
            ErrorCode::framebufferCreation,
            "initializing a new framebuffer depth attachment"
        );
    }

    void destroyCachedFramebuffers(
        Device::Session& session,
        std::map<std::string, CachedFramebuffer>& cached
    ) noexcept {
        for (auto& [id, framebuffer] : cached) {
            static_cast<void>(id);
            session.destroyFramebuffer(framebuffer.resource);
        }
    }

    void ensureFramebuffers(
        Device::Session& session,
        const FramePlan& plan,
        const FramebufferPlanRequirements& requirements
    ) {
        const bool outputSizeChanged = width != 0 && height != 0 &&
            (width != plan.width || height != plan.height);
        // Validate and allocate every non-GL part of the next arena before
        // touching the live cache. A malformed plan therefore cannot evict a
        // previously usable framebuffer set.
        for (const auto& [id, requirement] : requirements.descriptors) {
            const FramebufferDescriptor& descriptor = requirement.descriptor;
            if (descriptor.resource.id != id ||
                descriptor.resource.kind != FrameResourceKind::framebuffer) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Framebuffer requirement descriptor identity is inconsistent"
                );
            }
            if (descriptor.width == 0 || descriptor.height == 0 ||
                !std::isfinite(descriptor.scale) || descriptor.scale <= 0.0) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Invalid framebuffer descriptor dimensions or scale for '" +
                        descriptor.resource.id + "'"
                );
            }
            static_cast<void>(pixelFormat(descriptor.format));
            static_cast<void>(textureWrap(descriptor.wrapMode));
        }
        const auto& required = requirements.active;
        if (plan.output.kind != FrameResourceKind::framebuffer ||
            plan.output.id.empty() || !required.contains(plan.output.id)) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame plan output has no framebuffer descriptor"
            );
        }
        const FramebufferDescriptor& outputDescriptor =
            required.at(plan.output.id).descriptor;
        if (outputDescriptor.width != plan.width ||
            outputDescriptor.height != plan.height) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame plan output descriptor dimensions do not match the plan"
            );
        }
        const std::size_t nextByteCount = checkedRGBA8ByteCount(
            plan.width, plan.height
        );
        std::map<std::string, std::string> nextAliases;
        for (const auto& [id, requirement] : required) {
            static_cast<void>(requirement);
            nextAliases.emplace(id, id);
        }
        std::string nextOutputId = plan.output.id;

        // A cached color attachment may be upgraded in place when a later
        // visible plan first needs depth. Never downgrade it: replacing the
        // color texture would discard feedback pixels that authored effects
        // expect to survive visibility changes.
        const auto ensureRequiredDepth = [
            &session,
            this
        ](
            CachedFramebuffer& cached,
            const FramebufferAllocationRequirement& requirement
        ) {
            if (!sameDescriptor(
                    cached.requirement.descriptor,
                    requirement.descriptor
                ) ||
                !requirement.requiresDepthAttachment ||
                cached.requirement.requiresDepthAttachment) {
                return;
            }
            session.ensureDepthAttachment(cached.resource);
            clearNewDepthAttachment(session, cached.resource);
            cached.requirement.requiresDepthAttachment = true;
        };
        for (const auto& [id, requirement] : required) {
            if (auto current = framebuffers.find(id);
                current != framebuffers.end()) {
                ensureRequiredDepth(current->second, requirement);
            }
            if (auto inactive = inactiveFramebuffers.find(id);
                inactive != inactiveFramebuffers.end()) {
                ensureRequiredDepth(inactive->second, requirement);
            }
        }

        const auto activeMatches = [&required, this] {
            if (framebuffers.size() != required.size()) return false;
            for (const auto& [id, requirement] : required) {
                const auto current = framebuffers.find(id);
                if (current == framebuffers.end() ||
                    !framebufferRequirementSatisfies(
                        current->second.requirement, requirement
                    )) {
                    return false;
                }
            }
            return true;
        };
        const bool activeSetChanged = !activeMatches();

        // Allocate every staging node before creating GL objects. A map
        // allocation must never occur after FramebufferResource ownership has
        // moved because the resource intentionally has no RAII destructor.
        std::map<std::string, CachedFramebuffer> staged;
        std::map<std::string, FramebufferAllocationRequirement>
            replacementRequirements;
        std::set<std::string> reclaimedInactive;
        for (const auto& [id, requirement] : required) {
            const auto existing = framebuffers.find(id);
            if (existing != framebuffers.end() &&
                !framebufferRequirementSatisfies(
                    existing->second.requirement, requirement
                )) {
                replacementRequirements.emplace(id, requirement);
            }
            if (existing != framebuffers.end() &&
                framebufferRequirementSatisfies(
                    existing->second.requirement, requirement
                )) {
                continue;
            }
            const auto inactive = inactiveFramebuffers.find(id);
            if (inactive != inactiveFramebuffers.end() &&
                framebufferRequirementSatisfies(
                    inactive->second.requirement, requirement
                )) {
                reclaimedInactive.emplace(id);
                continue;
            }
            staged.emplace(
                id,
                CachedFramebuffer{requirement, FramebufferResource{}}
            );
        }

        // Reserve inactive-cache nodes before any GL ownership moves. Once a
        // framebuffer has been created it remains available for the wallpaper
        // lifetime, matching the previous executor's cross-frame persistence
        // semantics even across multiple distinct visibility transitions.
        using InactiveIterator = decltype(inactiveFramebuffers)::iterator;
        std::vector<InactiveIterator> insertedInactivePlaceholders;
        try {
            insertedInactivePlaceholders.reserve(framebuffers.size());
            if (activeSetChanged) {
                for (const auto& [id, current] : framebuffers) {
                    if (required.contains(id)) continue;
                    if (inactiveFramebuffers.contains(id)) {
                        throw Error(
                            ErrorCode::internalFailure,
                            "Framebuffer is simultaneously active and inactive '" +
                                id + "'"
                        );
                    }
                    const auto [inserted, didInsert] =
                        inactiveFramebuffers.emplace(
                            id,
                            CachedFramebuffer{
                                current.requirement, FramebufferResource{}
                            }
                        );
                    if (!didInsert) {
                        throw Error(
                            ErrorCode::internalFailure,
                            "Unable to reserve inactive framebuffer cache entry '" +
                                id + "'"
                        );
                    }
                    insertedInactivePlaceholders.push_back(inserted);
                }
            }
        } catch (...) {
            for (const InactiveIterator inserted :
                 insertedInactivePlaceholders) {
                inactiveFramebuffers.erase(inserted);
            }
            throw;
        }

        // Create and initialize replacements while the live cache remains
        // untouched. FramebufferResource is not RAII-owned, so every failure
        // path explicitly destroys both the current temporary and all
        // successfully staged GL objects.
        try {
            for (auto& [id, replacement] : staged) {
                static_cast<void>(id);
                const FramebufferAllocationRequirement& requirement =
                    replacement.requirement;
                const FramebufferDescriptor& descriptor = requirement.descriptor;
                FramebufferResource resource = session.createFramebuffer(
                    pixelFormat(descriptor.format),
                    descriptor.width,
                    descriptor.height,
                    textureWrap(descriptor.wrapMode),
                    requirement.requiresDepthAttachment
                );
                try {
                    clearNewFramebuffer(
                        session, resource, requirement.requiresDepthAttachment
                    );
                } catch (...) {
                    session.destroyFramebuffer(resource);
                    throw;
                }
                replacement.resource = std::move(resource);
            }
        } catch (...) {
            destroyCachedFramebuffers(session, staged);
            for (const InactiveIterator inserted :
                 insertedInactivePlaceholders) {
                inactiveFramebuffers.erase(inserted);
            }
            throw;
        }

        using CacheIterator = decltype(framebuffers)::iterator;
        std::size_t missingCount = 0;
        for (const auto& [id, requirement] : required) {
            static_cast<void>(requirement);
            if (!framebuffers.contains(id)) {
                ++missingCount;
            }
        }
        std::vector<CacheIterator> insertedPlaceholders;
        try {
            insertedPlaceholders.reserve(missingCount);
            for (const auto& [id, requirement] : required) {
                static_cast<void>(requirement);
                if (framebuffers.contains(id)) {
                    continue;
                }
                const auto [inserted, didInsert] = framebuffers.emplace(
                    id,
                    CachedFramebuffer{requirement, FramebufferResource{}}
                );
                if (!didInsert) {
                    throw Error(
                        ErrorCode::resourceValidation,
                        "Unable to reserve framebuffer cache entry '" + id + "'"
                    );
                }
                insertedPlaceholders.push_back(inserted);
            }
        } catch (...) {
            for (const CacheIterator inserted : insertedPlaceholders) {
                framebuffers.erase(inserted);
            }
            destroyCachedFramebuffers(session, staged);
            for (const InactiveIterator inserted :
                 insertedInactivePlaceholders) {
                inactiveFramebuffers.erase(inserted);
            }
            throw;
        }

        // All potentially throwing work is complete. Commit with only
        // noexcept descriptor swaps, GL deletion, resource moves, and map
        // erasure so a creation/initialization failure can never damage the
        // old arena. Inactive backing remains cached for the executor lifetime
        // so a descriptor that disappears for several plan generations still
        // resumes with its authored feedback contents intact.
        for (const auto& [id, requirement] : required) {
            auto replacement = staged.find(id);
            auto live = framebuffers.find(id);
            const bool wasReclaimed = reclaimedInactive.contains(id);
            if (replacement == staged.end() && !wasReclaimed) continue;

            const bool replacesLiveResource =
                !framebufferRequirementSatisfies(
                    live->second.requirement, requirement
                );
            if (wasReclaimed) {
                auto inactive = inactiveFramebuffers.find(id);
                if (inactive == inactiveFramebuffers.end()) {
                    std::terminate();
                }
                session.destroyFramebuffer(live->second.resource);
                live->second.requirement =
                    std::move(inactive->second.requirement);
                live->second.resource = std::move(inactive->second.resource);
                inactiveFramebuffers.erase(inactive);
            } else {
                if (replacesLiveResource) {
                    auto nextRequirement = replacementRequirements.find(id);
                    if (nextRequirement == replacementRequirements.end()) {
                        std::terminate();
                    }
                    live->second.requirement =
                        std::move(nextRequirement->second);
                }
                if (auto stale = inactiveFramebuffers.find(id);
                    stale != inactiveFramebuffers.end()) {
                    session.destroyFramebuffer(stale->second.resource);
                    inactiveFramebuffers.erase(stale);
                }
                session.destroyFramebuffer(live->second.resource);
                live->second.resource = std::move(replacement->second.resource);
            }
        }
        destroyCachedFramebuffers(session, staged);

        if (activeSetChanged) {
            for (auto cached = framebuffers.begin();
                 cached != framebuffers.end();) {
                if (required.contains(cached->first)) {
                    ++cached;
                    continue;
                }
                auto inactive = inactiveFramebuffers.find(cached->first);
                if (inactive == inactiveFramebuffers.end() ||
                    inactive->second.resource.framebuffer != 0) {
                    std::terminate();
                }
                inactive->second.resource = std::move(cached->second.resource);
                cached = framebuffers.erase(cached);
            }
        }

        // A quality-tier or drawable-projection size change replaces the
        // physical pixel grid. Inactive feedback from the previous grid can
        // never be reused without a descriptor mismatch, so retaining it only
        // keeps the old (often authored-resolution) VRAM alive. Descriptor
        // changes at a stable output size are evicted individually as well.
        for (auto cached = inactiveFramebuffers.begin();
             cached != inactiveFramebuffers.end();) {
            const auto descriptor = requirements.descriptors.find(
                cached->first
            );
            const bool descriptorChanged =
                descriptor != requirements.descriptors.end() &&
                !sameDescriptor(
                    cached->second.requirement.descriptor,
                    descriptor->second.descriptor
                );
            if (!outputSizeChanged && !descriptorChanged) {
                ++cached;
                continue;
            }
            session.destroyFramebuffer(cached->second.resource);
            cached = inactiveFramebuffers.erase(cached);
        }

        framebufferAliases.swap(nextAliases);
        outputId.swap(nextOutputId);
        width = plan.width;
        height = plan.height;
        byteCount = nextByteCount;
    }

    FramebufferResource& framebuffer(
        const FrameResourceRef& ref,
        const std::map<std::string, std::string>& aliases
    ) {
        if (ref.kind != FrameResourceKind::framebuffer) {
            throw Error(ErrorCode::resourceValidation, "Unknown framebuffer resource '" + ref.id + "'");
        }
        const auto alias = aliases.find(ref.id);
        if (alias == aliases.end()) {
            throw Error(
                ErrorCode::internalFailure,
                "Framebuffer alias arena has no logical resource '" +
                    ref.id + "'"
            );
        }
        const auto found = framebuffers.find(alias->second);
        if (found == framebuffers.end()) {
            throw Error(
                ErrorCode::internalFailure,
                "Framebuffer alias has no backing resource '" + ref.id + "'"
            );
        }
        return found->second.resource;
    }

    FramebufferResource& framebuffer(const FrameResourceRef& ref) {
        return framebuffer(ref, framebufferAliases);
    }

    AssetTextureResource& ensureAssetTexture(
        Device::Session& session,
        const FrameResourceRef& ref
    ) {
        if (ref.kind != FrameResourceKind::assetTexture) {
            throw Error(ErrorCode::resourceValidation, "Resource is not an asset texture");
        }
        auto found = assets.find(ref.id);
        if (found == assets.end()) {
            const std::string path = ref.logicalName.empty() ? ref.id : ref.logicalName;
            AssetTextureResource uploaded = session.uploadTexture(
                resolver().parseTexture(path), path
            );
            try {
                const auto [inserted, didInsert] = assets.try_emplace(ref.id);
                if (!didInsert) {
                    session.destroyTexture(uploaded);
                    found = inserted;
                } else {
                    inserted->second = std::move(uploaded);
                    found = inserted;
                }
            } catch (...) {
                session.destroyTexture(uploaded);
                throw;
            }
        }
        if (found->second.images.empty()) {
            throw Error(ErrorCode::resourceValidation, "Texture has no uploaded image: '" + ref.id + "'");
        }
        return found->second;
    }

    AssetTextureResource& assetTexture(
        Device::Session& session,
        const FrameResourceRef& ref,
        double timeSeconds = 0.0
    ) {
        AssetTextureResource& result = ensureAssetTexture(session, ref);
        if (result.video) {
            session.updateVideoTexture(result, timeSeconds);
        }
        return result;
    }

    HostTextureSlot& hostTextureSlot(const FrameResourceRef& ref) {
        if (ref.kind != FrameResourceKind::hostTexture) {
            throw Error(
                ErrorCode::internalFailure,
                "Resource is not a host texture"
            );
        }
        if (ref.id == "$mediaThumbnail") return mediaThumbnailCurrent;
        if (ref.id == "$mediaPreviousThumbnail") {
            return mediaThumbnailPrevious;
        }
        throw Error(
            ErrorCode::resourceValidation,
            "Unknown host texture resource '" + ref.id + "'"
        );
    }

    const HostTextureSlot& hostTextureSlot(
        const FrameResourceRef& ref
    ) const {
        if (ref.kind != FrameResourceKind::hostTexture) {
            throw Error(
                ErrorCode::internalFailure,
                "Resource is not a host texture"
            );
        }
        if (ref.id == "$mediaThumbnail") return mediaThumbnailCurrent;
        if (ref.id == "$mediaPreviousThumbnail") {
            return mediaThumbnailPrevious;
        }
        throw Error(
            ErrorCode::resourceValidation,
            "Unknown host texture resource '" + ref.id + "'"
        );
    }

    bool synchronizeHostTexture(
        Device::Session& session,
        HostTextureSlot& slot
    ) {
        if (!slot.image) {
            session.destroyTexture(slot.texture);
            slot.dirty = false;
            return false;
        }
        if (!slot.dirty && slot.texture != 0) return true;

        GLuint replacement = session.uploadRGBA8Texture(
            slot.image->width,
            slot.image->height,
            slot.image->pixels
        );
        session.destroyTexture(slot.texture);
        slot.texture = replacement;
        slot.dirty = false;
        return true;
    }

    [[nodiscard]] std::optional<FrameResourceRef> concreteTextureResource(
        const FrameResourceRef& resource
    ) const {
        switch (resource.kind) {
            case FrameResourceKind::assetTexture:
            case FrameResourceKind::framebuffer:
            case FrameResourceKind::hostTexture:
                return resource;
            case FrameResourceKind::userPropertyTexture:
                if (resource.resolvedAssetName.empty()) return std::nullopt;
                if (resource.resolvedAssetName == "$mediaThumbnail" ||
                    resource.resolvedAssetName ==
                        "$mediaPreviousThumbnail") {
                    return FrameResourceRef{
                        .kind = FrameResourceKind::hostTexture,
                        .id = resource.resolvedAssetName,
                        .logicalName = resource.resolvedAssetName,
                    };
                }
                return frameAssetTextureResource(
                    resource.resolvedAssetName
                );
        }
        std::terminate();
    }

    [[nodiscard]] bool textureReady(
        Device::Session& session,
        const FrameResourceRef& resource,
        const std::map<std::string, std::string>& aliases,
        double timeSeconds
    ) {
        switch (resource.kind) {
            case FrameResourceKind::framebuffer:
                static_cast<void>(framebuffer(resource, aliases));
                return true;
            case FrameResourceKind::assetTexture:
                // Asset textures are immutable for the lifetime of an
                // executor. Once uploaded, the GPU cache is the authoritative
                // ready state; consulting the resolver again would perform
                // needless filesystem/package work on every frame.
                if (assets.contains(resource.id)) return true;
                if (!resolver().contains(
                        resource.logicalName.empty()
                            ? resource.id
                            : resource.logicalName)) {
                    return false;
                }
                static_cast<void>(assetTexture(
                    session, resource, timeSeconds
                ));
                return true;
            case FrameResourceKind::hostTexture:
                // Metadata alone never makes an album cover sampleable. A
                // host resource becomes ready only after validated pixels
                // have reached this executor's texture store.
                return synchronizeHostTexture(
                    session, hostTextureSlot(resource)
                );
            case FrameResourceKind::userPropertyTexture:
                throw Error(
                    ErrorCode::internalFailure,
                    "A user-property texture reached GPU readiness without "
                    "being resolved to its selected provider"
                );
        }
        std::terminate();
    }

    [[nodiscard]] std::optional<FrameResourceRef> selectReadyTexture(
        Device::Session& session,
        const std::vector<FrameTextureCandidate>& candidates,
        const std::optional<FrameResourceRef>& previousInput,
        const FrameResourceRef& input,
        const std::map<std::string, std::string>& aliases,
        double timeSeconds
    ) {
        for (auto candidate = candidates.rbegin();
             candidate != candidates.rend(); ++candidate) {
            const auto concrete = concreteTextureResource(
                candidate->resource
            );
            if (concrete && textureReady(
                    session, *concrete, aliases, timeSeconds)) {
                return concrete;
            }
        }
        if (previousInput) {
            const auto concrete = concreteTextureResource(*previousInput);
            if (concrete && textureReady(
                    session, *concrete, aliases, timeSeconds)) {
                return concrete;
            }
        }
        const auto concreteInput = concreteTextureResource(input);
        if (concreteInput && textureReady(
                session, *concreteInput, aliases, timeSeconds)) {
            return concreteInput;
        }
        return std::nullopt;
    }

    GLuint texture(
        Device::Session& session,
        const FrameResourceRef& ref,
        std::size_t imageIndex = 0,
        double timeSeconds = 0.0
    ) {
        if (ref.kind == FrameResourceKind::framebuffer) {
            return framebuffer(ref).colorTexture;
        }
        if (ref.kind == FrameResourceKind::hostTexture) {
            HostTextureSlot& slot = hostTextureSlot(ref);
            if (!synchronizeHostTexture(session, slot) || slot.texture == 0) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Host texture is not ready: '" + ref.id + "'"
                );
            }
            return slot.texture;
        }
        auto& resource = assetTexture(session, ref, timeSeconds);
        if (imageIndex >= resource.images.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Texture image index is outside the uploaded resource: '" + ref.id + "'"
            );
        }
        return resource.images[imageIndex];
    }

    std::array<float, 4> textureResolution(
        const FrameResourceRef& ref,
        const std::map<std::string, std::string>& aliases
    ) const {
        if (ref.kind == FrameResourceKind::framebuffer) {
            const auto alias = aliases.find(ref.id);
            if (alias == aliases.end()) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Framebuffer alias arena has no logical resource '" +
                        ref.id + "'"
                );
            }
            const auto backing = framebuffers.find(alias->second);
            if (backing == framebuffers.end()) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Framebuffer alias has no backing resource '" +
                        ref.id + "'"
                );
            }
            const auto& resource = backing->second.resource;
            return {float(resource.width), float(resource.height), float(resource.width), float(resource.height)};
        }
        if (ref.kind == FrameResourceKind::hostTexture) {
            const HostTextureSlot& slot = hostTextureSlot(ref);
            if (!slot.image) return {};
            return {
                static_cast<float>(slot.image->width),
                static_cast<float>(slot.image->height),
                static_cast<float>(slot.image->width),
                static_cast<float>(slot.image->height),
            };
        }
        const auto found = assets.find(ref.id);
        return found == assets.end() ? std::array<float, 4>{} : found->second.resolution;
    }

    ProgramResource& program(
        Device::Session& session,
        const std::string& vertexShaderPath,
        const std::string& fragmentShaderPath,
        const ComboMap& combos,
        std::string_view description
    ) {
        std::ostringstream keyBuilder;
        keyBuilder << vertexShaderPath << '|' << fragmentShaderPath;
        for (const auto& [name, value] : combos) keyBuilder << '|' << name << '=' << value;
        const std::string key = keyBuilder.str();
        if (const auto found = programs.find(key); found != programs.end()) return found->second;

        if (vertexShaderPath.empty() || fragmentShaderPath.empty()) {
            throw Error(
                ErrorCode::invalidArgument,
                std::string(description) + " has no resolved shader paths"
            );
        }
        ShaderPreprocessOptions options;
        options.combos = combos;
        const ShaderPreprocessor preprocessor(resolver());
        const auto preprocessed = preprocessor.preprocessFiles(
            vertexShaderPath, fragmentShaderPath, options
        );
        const auto translated = ShaderCompiler::translate(
            preprocessed.vertex.source, preprocessed.fragment.source,
            preprocessed.vertex.name, preprocessed.fragment.name
        );
        std::vector<ShaderParameterMetadata> parameters = preprocessed.vertex.parameters;
        parameters.insert(
            parameters.end(), preprocessed.fragment.parameters.begin(),
            preprocessed.fragment.parameters.end()
        );
        const GLuint value = session.createProgram(
            translated.vertex, translated.fragment
        );
        ProgramResource candidate{
            .program = value,
            .parameters = std::move(parameters),
            .uniforms = {},
        };
        try {
            candidate.uniforms = activeUniforms(candidate.program);
            session.checkError(
                ErrorCode::programLink,
                "Inspecting active shader uniforms"
            );
            const auto [inserted, didInsert] = programs.try_emplace(key);
            if (!didInsert) {
                session.destroyProgram(candidate.program);
                return inserted->second;
            }
            inserted->second = std::move(candidate);
            candidate.program = 0;
            return inserted->second;
        } catch (...) {
            session.destroyProgram(candidate.program);
            throw;
        }
    }

    FrameResourceRef samplerDefaultResource(
        const FramePlan& plan,
        const FrameRenderPass& pass,
        std::string_view name
    ) const {
        const auto matches = [name](const FrameResourceRef& resource) {
            return resource.logicalName == name;
        };
        if (name == "_rt_FullFrameBuffer" ||
            name == "_rt_MipMappedFrameBuffer") {
            return plan.output;
        }
        if (name == "_alias_lightCookie") {
            for (const FramebufferDescriptor& descriptor : plan.framebuffers) {
                if (descriptor.resource.logicalName == "_rt_shadowAtlas") {
                    return {
                        .kind = FrameResourceKind::framebuffer,
                        .id = descriptor.resource.id,
                        .logicalName = "_alias_lightCookie",
                    };
                }
            }
            throw Error(
                ErrorCode::resourceValidation,
                "Sampler metadata default references unavailable runtime alias '_alias_lightCookie'"
            );
        }
        if (matches(pass.input)) {
            return pass.input;
        }
        if (pass.previousInput && matches(*pass.previousInput)) {
            return *pass.previousInput;
        }
        for (const auto& [slot, binding] : pass.textures) {
            static_cast<void>(slot);
            for (const FrameTextureCandidate& candidate :
                 binding.candidates) {
                if (matches(candidate.resource)) {
                    return candidate.resource;
                }
            }
        }
        if (matches(pass.destination)) {
            return pass.destination;
        }
        for (const FramebufferDescriptor& descriptor : plan.framebuffers) {
            if (matches(descriptor.resource)) {
                return descriptor.resource;
            }
        }
        if (name.starts_with("_rt_") || name.starts_with("_alias_")) {
            throw Error(
                ErrorCode::resourceValidation,
                "Sampler metadata default references unavailable runtime texture '" +
                    std::string(name) + "'"
            );
        }
        if (name == "$mediaThumbnail" ||
            name == "$mediaPreviousThumbnail") {
            return {
                .kind = FrameResourceKind::hostTexture,
                .id = std::string(name),
                .logicalName = std::string(name),
            };
        }
        return frameAssetTextureResource(name);
    }

    FrameResourceRef particleSamplerDefaultResource(
        const FramePlan& plan,
        const FrameParticleDescriptor& descriptor,
        std::string_view name
    ) const {
        if (name == "_rt_FullFrameBuffer" ||
            name == "_rt_MipMappedFrameBuffer") {
            return plan.output;
        }
        if (name == "_alias_lightCookie") {
            for (const FramebufferDescriptor& framebuffer : plan.framebuffers) {
                if (framebuffer.resource.logicalName == "_rt_shadowAtlas") {
                    return framebuffer.resource;
                }
            }
            throw Error(
                ErrorCode::resourceValidation,
                "Particle sampler metadata default references unavailable runtime alias '_alias_lightCookie'"
            );
        }
        if (descriptor.texture0.logicalName == name ||
            descriptor.texture0.id == name) {
            return descriptor.texture0;
        }
        for (const auto& [slot, binding] : descriptor.textures) {
            static_cast<void>(slot);
            for (const FrameTextureCandidate& candidate :
                 binding.candidates) {
                if (candidate.resource.logicalName == name ||
                    candidate.resource.id == name) {
                    return candidate.resource;
                }
            }
        }
        for (const FramebufferDescriptor& framebuffer : plan.framebuffers) {
            if (framebuffer.resource.logicalName == name ||
                framebuffer.resource.id == name) {
                return framebuffer.resource;
            }
        }
        if (name.starts_with("_rt_") || name.starts_with("_alias_")) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle sampler metadata default references unavailable runtime texture '" +
                    std::string(name) + "'"
            );
        }
        if (name == "$mediaThumbnail" ||
            name == "$mediaPreviousThumbnail") {
            return {
                .kind = FrameResourceKind::hostTexture,
                .id = std::string(name),
                .logicalName = std::string(name),
            };
        }
        return frameAssetTextureResource(name);
    }

    [[nodiscard]] static GLfloat checkedFloat(
        double value,
        std::string_view description
    ) {
        if (!std::isfinite(value) ||
            value > static_cast<double>(std::numeric_limits<GLfloat>::max()) ||
            value < -static_cast<double>(std::numeric_limits<GLfloat>::max())) {
            throw Error(
                ErrorCode::resourceValidation,
                std::string(description) +
                    " contains a non-finite or out-of-range value"
            );
        }
        return static_cast<GLfloat>(value);
    }

    static void validateMatrix(
        const Matrix& matrix,
        std::string_view description
    ) {
        for (const float value : matrix) {
            if (!std::isfinite(value)) {
                throw Error(
                    ErrorCode::resourceValidation,
                    std::string(description) + " contains a non-finite value"
                );
            }
        }
    }

    [[nodiscard]] static PreparedCommonUniforms prepareCommonUniforms(
        const ProgramResource& program,
        const FramePlan& logicalPlan,
        PhysicalRenderSize physicalOutput,
        const ResolvedFrameInputs& inputs,
        const Matrix& model,
        const Matrix& viewProjection,
        const Matrix& modelViewProjection,
        const CommonRenderableValues& renderable
    ) {
        static constexpr std::array<std::string_view, 6> audioUniforms{
            "g_AudioSpectrum16Left",
            "g_AudioSpectrum16Right",
            "g_AudioSpectrum32Left",
            "g_AudioSpectrum32Right",
            "g_AudioSpectrum64Left",
            "g_AudioSpectrum64Right",
        };
        const bool audioRequested = std::any_of(
            audioUniforms.begin(), audioUniforms.end(),
            [&program](std::string_view name) {
                return activeUniform(program, name) != nullptr;
            }
        );
        if (audioRequested && !inputs.audioSpectrum) {
            throw Error(
                ErrorCode::resourceValidation,
                "Active builtin audio uniform requires unavailable system "
                "audio spectrum input"
            );
        }
        if (physicalOutput.width == 0 || physicalOutput.height == 0) {
            throw Error(
                ErrorCode::internalFailure,
                "Physical framebuffer dimensions are unavailable for common shader uniforms"
            );
        }

        PreparedCommonUniforms result;
        result.textureReductionScale = prepareBuiltinUniform(
            program, "g_TextureReductionScale", GL_FLOAT
        );
        result.lightAmbientColor = prepareBuiltinUniform(
            program, "g_LightAmbientColor", GL_FLOAT_VEC3
        );
        result.lightSkylightColor = prepareBuiltinUniform(
            program, "g_LightSkylightColor", GL_FLOAT_VEC3
        );
        result.brightness = prepareBuiltinUniform(
            program, "g_Brightness", GL_FLOAT
        );
        result.userAlpha = prepareBuiltinUniform(
            program, "g_UserAlpha", GL_FLOAT
        );
        result.userAlphaProvidedByMaterial = std::any_of(
            program.parameters.begin(), program.parameters.end(),
            [](const ShaderParameterMetadata& parameter) {
                return parameter.name == "g_UserAlpha" &&
                    parameter.material.has_value();
            }
        );
        result.alpha = prepareBuiltinUniform(program, "g_Alpha", GL_FLOAT);
        result.color = prepareBuiltinUniform(
            program, "g_Color", GL_FLOAT_VEC3
        );
        result.color4 = prepareBuiltinUniform(
            program, "g_Color4", GL_FLOAT_VEC4
        );
        result.compositeColor = prepareBuiltinUniform(
            program, "g_CompositeColor", GL_FLOAT_VEC3
        );
        result.compositeColorProvidedByShader = std::any_of(
            program.parameters.begin(), program.parameters.end(),
            [](const ShaderParameterMetadata& parameter) {
                return parameter.name == "g_CompositeColor";
            }
        );
        result.time = prepareBuiltinUniform(program, "g_Time", GL_FLOAT);
        result.daytime = prepareBuiltinUniform(
            program, "g_Daytime", GL_FLOAT
        );
        result.modelViewProjection = prepareBuiltinUniform(
            program, "g_ModelViewProjectionMatrix", GL_FLOAT_MAT4
        );
        result.modelViewProjectionInverse = prepareBuiltinUniform(
            program, "g_ModelViewProjectionMatrixInverse", GL_FLOAT_MAT4
        );
        result.effectModelViewProjection = prepareBuiltinUniform(
            program, "g_EffectModelViewProjectionMatrix", GL_FLOAT_MAT4
        );
        result.model = prepareBuiltinUniform(
            program, "g_ModelMatrix", GL_FLOAT_MAT4
        );
        result.effectModel = prepareBuiltinUniform(
            program, "g_EffectModelMatrix", GL_FLOAT_MAT4
        );
        result.normalModel = prepareBuiltinUniform(
            program, "g_NormalModelMatrix", GL_FLOAT_MAT3
        );
        result.viewProjection = prepareBuiltinUniform(
            program, "g_ViewProjectionMatrix", GL_FLOAT_MAT4
        );
        result.pointer = prepareBuiltinUniform(
            program, "g_PointerPosition", GL_FLOAT_VEC2
        );
        result.pointerLast = prepareBuiltinUniform(
            program, "g_PointerPositionLast", GL_FLOAT_VEC2
        );
        result.effectTextureProjection = prepareBuiltinUniform(
            program, "g_EffectTextureProjectionMatrix", GL_FLOAT_MAT4
        );
        result.effectTextureProjectionInverse = prepareBuiltinUniform(
            program, "g_EffectTextureProjectionMatrixInverse", GL_FLOAT_MAT4
        );
        result.texelSize = prepareBuiltinUniform(
            program, "g_TexelSize", GL_FLOAT_VEC2
        );
        result.texelSizeHalf = prepareBuiltinUniform(
            program, "g_TexelSizeHalf", GL_FLOAT_VEC2
        );
        result.audioSpectrum16Left = prepareAudioSpectrumUniform(
            program, "g_AudioSpectrum16Left", 16
        );
        result.audioSpectrum16Right = prepareAudioSpectrumUniform(
            program, "g_AudioSpectrum16Right", 16
        );
        result.audioSpectrum32Left = prepareAudioSpectrumUniform(
            program, "g_AudioSpectrum32Left", 32
        );
        result.audioSpectrum32Right = prepareAudioSpectrumUniform(
            program, "g_AudioSpectrum32Right", 32
        );
        result.audioSpectrum64Left = prepareAudioSpectrumUniform(
            program, "g_AudioSpectrum64Left", 64
        );
        result.audioSpectrum64Right = prepareAudioSpectrumUniform(
            program, "g_AudioSpectrum64Right", 64
        );

        if (result.modelViewProjectionInverse >= 0) {
            result.modelViewProjectionInverseValue = inverse(
                modelViewProjection, "Model-view-projection matrix"
            );
        }
        result.ambientColorValue = {
            checkedFloat(logicalPlan.ambientColor.red, "Ambient light color"),
            checkedFloat(logicalPlan.ambientColor.green, "Ambient light color"),
            checkedFloat(logicalPlan.ambientColor.blue, "Ambient light color"),
        };
        result.skylightColorValue = {
            checkedFloat(logicalPlan.skylightColor.red, "Skylight color"),
            checkedFloat(logicalPlan.skylightColor.green, "Skylight color"),
            checkedFloat(logicalPlan.skylightColor.blue, "Skylight color"),
        };
        result.brightnessValue = checkedFloat(
            renderable.brightness, "Renderable brightness"
        );
        result.alphaValue = checkedFloat(
            renderable.alpha, "Renderable alpha"
        );
        result.color4Value = renderable.color;
        for (std::size_t index = 0; index < renderable.color.size(); ++index) {
            result.color4Value[index] = checkedFloat(
                renderable.color[index], "Renderable color"
            );
        }
        // Versioned image shaders consume opacity through g_Color4, while
        // legacy shaders consume the same layer value through g_UserAlpha.
        result.color4Value[3] = checkedFloat(
            renderable.color[3] * renderable.alpha,
            "Renderable color alpha"
        );
        std::copy_n(
            result.color4Value.begin(), 3, result.colorValue.begin()
        );
        result.timeValue = checkedFloat(inputs.timeSeconds, "Frame time");
        result.daytimeValue = checkedFloat(inputs.daytime, "Local daytime");
        result.pointerValue = {
            checkedFloat(inputs.effectPointerPosition.x, "Pointer position"),
            checkedFloat(inputs.effectPointerPosition.y, "Pointer position"),
        };
        result.pointerLastValue = {
            checkedFloat(
                inputs.effectPointerPositionLast.x,
                "Previous pointer position"
            ),
            checkedFloat(
                inputs.effectPointerPositionLast.y,
                "Previous pointer position"
            ),
        };
        result.texelSizeValue = {
            1.0F / static_cast<float>(physicalOutput.width),
            1.0F / static_cast<float>(physicalOutput.height),
        };
        result.texelSizeHalfValue = {
            result.texelSizeValue[0] * 0.5F,
            result.texelSizeValue[1] * 0.5F,
        };
        if (inputs.audioSpectrum) {
            result.audioSpectrumValue = *inputs.audioSpectrum;
        }
        validateMatrix(model, "Model matrix");
        validateMatrix(viewProjection, "View-projection matrix");
        validateMatrix(modelViewProjection, "Model-view-projection matrix");
        return result;
    }

    static void bindCommonUniforms(
        const PreparedCommonUniforms& uniforms,
        const Matrix& model,
        const Matrix& viewProjection,
        const Matrix& modelViewProjection
    ) {
        static constexpr std::array<float, 9> identity3{
            1.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 1.0F,
        };
        const Matrix identity4 = identityMatrix();
        if (uniforms.textureReductionScale >= 0) {
            glUniform1f(uniforms.textureReductionScale, 1.0F);
        }
        bindVector3(uniforms.lightAmbientColor, uniforms.ambientColorValue);
        bindVector3(uniforms.lightSkylightColor, uniforms.skylightColorValue);
        if (uniforms.brightness >= 0) {
            glUniform1f(uniforms.brightness, uniforms.brightnessValue);
        }
        if (uniforms.userAlpha >= 0 &&
            !uniforms.userAlphaProvidedByMaterial) {
            glUniform1f(uniforms.userAlpha, uniforms.alphaValue);
        }
        if (uniforms.alpha >= 0) {
            glUniform1f(uniforms.alpha, uniforms.alphaValue);
        }
        bindVector3(uniforms.color, uniforms.colorValue);
        bindVector4(uniforms.color4, uniforms.color4Value);
        if (!uniforms.compositeColorProvidedByShader) {
            bindVector3(uniforms.compositeColor, uniforms.colorValue);
        }
        if (uniforms.time >= 0) {
            glUniform1f(uniforms.time, uniforms.timeValue);
        }
        if (uniforms.daytime >= 0) {
            glUniform1f(uniforms.daytime, uniforms.daytimeValue);
        }
        bindMatrix(uniforms.modelViewProjection, modelViewProjection);
        bindMatrix(
            uniforms.modelViewProjectionInverse,
            uniforms.modelViewProjectionInverseValue
        );
        bindMatrix(uniforms.effectModelViewProjection, modelViewProjection);
        bindMatrix(uniforms.model, model);
        bindMatrix(uniforms.effectModel, model);
        bindMatrix3(uniforms.normalModel, identity3);
        bindMatrix(uniforms.viewProjection, viewProjection);
        bindVector2(uniforms.pointer, uniforms.pointerValue);
        bindVector2(uniforms.pointerLast, uniforms.pointerLastValue);
        bindMatrix(uniforms.effectTextureProjection, identity4);
        bindMatrix(uniforms.effectTextureProjectionInverse, identity4);
        bindVector2(uniforms.texelSize, uniforms.texelSizeValue);
        bindVector2(uniforms.texelSizeHalf, uniforms.texelSizeHalfValue);
        const auto bindAudio = [](
            const PreparedAudioSpectrumUniform& uniform,
            const float* values
        ) {
            if (uniform.location >= 0) {
                glUniform1fv(
                    uniform.location, uniform.activeElementCount, values
                );
            }
        };
        bindAudio(
            uniforms.audioSpectrum16Left,
            uniforms.audioSpectrumValue.spectrum16Left.data()
        );
        bindAudio(
            uniforms.audioSpectrum16Right,
            uniforms.audioSpectrumValue.spectrum16Right.data()
        );
        bindAudio(
            uniforms.audioSpectrum32Left,
            uniforms.audioSpectrumValue.spectrum32Left.data()
        );
        bindAudio(
            uniforms.audioSpectrum32Right,
            uniforms.audioSpectrumValue.spectrum32Right.data()
        );
        bindAudio(
            uniforms.audioSpectrum64Left,
            uniforms.audioSpectrumValue.spectrum64Left.data()
        );
        bindAudio(
            uniforms.audioSpectrum64Right,
            uniforms.audioSpectrumValue.spectrum64Right.data()
        );
    }

    [[nodiscard]] static std::optional<PreparedUniform> prepareRuntimeUniform(
        const ProgramResource& program,
        const std::string& name,
        const RuntimeValue& value
    ) {
        const ActiveUniform* uniform = activeUniform(program, name);
        if (uniform == nullptr) return std::nullopt;
        if (uniform->blockIndex >= 0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + name +
                    "' uses an unsupported uniform-block binding"
            );
        }
        if (uniform->isArray || uniform->size != 1) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + name + "' uses an unsupported array binding"
            );
        }
        if (uniform->location < 0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + name + "' has no bindable location"
            );
        }
        const GLenum type = uniform->type;
        PreparedUniform result{.location = uniform->location};
        if (type == GL_BOOL) {
            result.kind = PreparedUniform::Kind::integer;
            result.integer = value.boolean() ? 1 : 0;
            return result;
        }
        if (type == GL_INT) {
            const std::int64_t integer = value.integer();
            if (integer < std::numeric_limits<GLint>::min() ||
                integer > std::numeric_limits<GLint>::max()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Uniform '" + name + "' exceeds GLint range"
                );
            }
            result.kind = PreparedUniform::Kind::integer;
            result.integer = static_cast<GLint>(integer);
            return result;
        }
        const std::size_t expected = type == GL_FLOAT ? 1
            : type == GL_FLOAT_VEC2 ? 2
            : type == GL_FLOAT_VEC3 ? 3
            : type == GL_FLOAT_VEC4 ? 4 : 0;
        if (expected == 0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Uniform '" + name + "' has an unsupported metadata type"
            );
        }
        const auto components = numericComponents(value, expected, name);
        std::copy(
            components.begin(), components.end(), result.floating.begin()
        );
        result.kind = expected == 1 ? PreparedUniform::Kind::float1
            : expected == 2 ? PreparedUniform::Kind::float2
            : expected == 3 ? PreparedUniform::Kind::float3
                            : PreparedUniform::Kind::float4;
        return result;
    }

    static void bindPreparedUniform(const PreparedUniform& uniform) {
        switch (uniform.kind) {
            case PreparedUniform::Kind::integer:
                glUniform1i(uniform.location, uniform.integer);
                break;
            case PreparedUniform::Kind::float1:
                glUniform1fv(uniform.location, 1, uniform.floating.data());
                break;
            case PreparedUniform::Kind::float2:
                glUniform2fv(uniform.location, 1, uniform.floating.data());
                break;
            case PreparedUniform::Kind::float3:
                glUniform3fv(uniform.location, 1, uniform.floating.data());
                break;
            case PreparedUniform::Kind::float4:
                glUniform4fv(uniform.location, 1, uniform.floating.data());
                break;
        }
    }

    [[nodiscard]] GLuint preparedTexture(
        const PreparedTextureBinding& binding
    ) {
        if (binding.assetTexture != 0) return binding.assetTexture;
        return framebuffer(binding.resource).colorTexture;
    }

    void ensureGeometry(Device::Session& session) {
        if (vertexArray != 0 || vertexBuffer != 0) {
            if (vertexArray == 0 || vertexBuffer == 0) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Image OpenGL geometry is only partially initialized"
                );
            }
            return;
        }

        GLuint candidateVertexArray = 0;
        GLuint candidateVertexBuffer = 0;
        try {
            candidateVertexArray = session.createVertexArray();
            candidateVertexBuffer = session.createBuffer();
            glBindVertexArray(candidateVertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, candidateVertexBuffer);
            glBufferData(
                GL_ARRAY_BUFFER,
                sizeof(Vertex) * 6,
                nullptr,
                GL_DYNAMIC_DRAW
            );
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindVertexArray(0);
            session.checkError(
                ErrorCode::internalFailure,
                "initializing shared image geometry"
            );
            vertexArray = candidateVertexArray;
            vertexBuffer = candidateVertexBuffer;
        } catch (...) {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindVertexArray(0);
            session.destroyBuffer(candidateVertexBuffer);
            session.destroyVertexArray(candidateVertexArray);
            throw;
        }
    }

    void ensureParticleGeometry(Device::Session& session) {
        if (particleVertexArray != 0 || particleVertexBuffer != 0 ||
            particleElementBuffer != 0) {
            if (particleVertexArray == 0 || particleVertexBuffer == 0 ||
                particleElementBuffer == 0) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Particle OpenGL geometry is only partially initialized"
                );
            }
            return;
        }

        GLuint vertexArray = 0;
        GLuint vertexBuffer = 0;
        GLuint elementBuffer = 0;
        try {
            vertexArray = session.createVertexArray();
            vertexBuffer = session.createBuffer();
            elementBuffer = session.createBuffer();
            glBindVertexArray(vertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementBuffer);
            session.checkError(
                ErrorCode::internalFailure,
                "initializing shared particle geometry"
            );
            particleVertexArray = vertexArray;
            particleVertexBuffer = vertexBuffer;
            particleElementBuffer = elementBuffer;
        } catch (...) {
            session.destroyBuffer(elementBuffer);
            session.destroyBuffer(vertexBuffer);
            session.destroyVertexArray(vertexArray);
            throw;
        }
    }

    void releaseParticleGeometry(Device::Session& session) noexcept {
        session.destroyBuffer(particleElementBuffer);
        session.destroyBuffer(particleVertexBuffer);
        session.destroyVertexArray(particleVertexArray);
    }

    void ensurePuppetGeometry(Device::Session& session) {
        if (puppetVertexArray != 0 || puppetVertexBuffer != 0 ||
            puppetElementBuffer != 0) {
            if (puppetVertexArray == 0 || puppetVertexBuffer == 0 ||
                puppetElementBuffer == 0) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Puppet OpenGL geometry is only partially initialized"
                );
            }
            return;
        }

        GLuint vertexArray = 0;
        GLuint vertexBuffer = 0;
        GLuint elementBuffer = 0;
        try {
            vertexArray = session.createVertexArray();
            vertexBuffer = session.createBuffer();
            elementBuffer = session.createBuffer();
            glBindVertexArray(vertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementBuffer);
            session.checkError(
                ErrorCode::internalFailure,
                "initializing shared Puppet geometry"
            );
            puppetVertexArray = vertexArray;
            puppetVertexBuffer = vertexBuffer;
            puppetElementBuffer = elementBuffer;
        } catch (...) {
            session.destroyBuffer(elementBuffer);
            session.destroyBuffer(vertexBuffer);
            session.destroyVertexArray(vertexArray);
            throw;
        }
    }

    void releasePuppetGeometry(Device::Session& session) noexcept {
        session.destroyBuffer(puppetElementBuffer);
        session.destroyBuffer(puppetVertexBuffer);
        session.destroyVertexArray(puppetVertexArray);
    }

    static void configureImageAttributes(
        ImageAttributeState& current,
        GLint position,
        GLint texCoord,
        GLsizei stride
    ) {
        if (current.position == position && current.texCoord == texCoord) {
            return;
        }
        if (current.position >= 0) {
            glDisableVertexAttribArray(
                static_cast<GLuint>(current.position)
            );
        }
        if (current.texCoord >= 0 &&
            current.texCoord != current.position) {
            glDisableVertexAttribArray(
                static_cast<GLuint>(current.texCoord)
            );
        }
        if (position >= 0) {
            glEnableVertexAttribArray(static_cast<GLuint>(position));
            glVertexAttribPointer(
                static_cast<GLuint>(position),
                3, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offsetof(Vertex, position))
            );
        }
        if (texCoord >= 0) {
            glEnableVertexAttribArray(static_cast<GLuint>(texCoord));
            glVertexAttribPointer(
                static_cast<GLuint>(texCoord),
                2, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offsetof(Vertex, texCoord))
            );
        }
        current = {
            .position = position,
            .texCoord = texCoord,
        };
    }

    [[nodiscard]] PreparedDraw prepareDraw(
        Device::Session& session,
        const FramePlan& logicalPlan,
        const FramePlan& physicalPlan,
        const FrameRenderPass& pass,
        const ResolvedFrameInputs& inputs,
        const FrameVector2& frameParallax,
        const PreparedCamera& camera,
        const std::map<std::string, std::string>& aliases
    ) {
        if (pass.origin.imageIndex >= logicalPlan.images.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Render pass image index is invalid"
            );
        }
        const FrameImageDescriptor& image =
            logicalPlan.images[pass.origin.imageIndex];
        auto& destination = framebuffer(pass.destination, aliases);
        ComboMap effectiveCombos = pass.combos;
        if (image.source.kind == FrameResourceKind::assetTexture) {
            applyTexture0FormatCombo(
                effectiveCombos,
                assetTexture(session, image.source, inputs.timeSeconds).format
            );
        }
        ProgramResource& programResource = program(
            session,
            pass.vertexShaderPath,
            pass.fragmentShaderPath,
            effectiveCombos,
            "Frame render pass"
        );
        const GLuint activeProgram = programResource.program;
        PreparedDraw result{
            .pass = pass,
            .program = activeProgram,
        };
        const auto selectTexture0 = [&](const FrameResourceRef& resource) {
            AssetTextureResource& textureResource = assetTexture(
                session, resource, inputs.timeSeconds
            );
            if (image.textureAnimation &&
                image.textureAnimation->assetIdentity == resource.id) {
                return selectTextureAnimationFrame(
                    textureResource, image.textureAnimation->frame
                );
            }
            return selectTextureAnimation(
                textureResource, inputs.timeSeconds
            );
        };

        std::map<int, const ActiveUniform*> activeSamplers;
        for (const ActiveUniform& uniform : programResource.uniforms) {
            if (!isOpenGLSamplerType(uniform.type)) {
                continue;
            }
            if (uniform.type != GL_SAMPLER_2D) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active sampler '" + uniform.name +
                        "' uses an unsupported non-2D texture type"
                );
            }
            const std::optional<int> slot = textureSlot(uniform.name);
            if (!slot) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active sampler '" + uniform.name +
                        "' does not follow the g_TextureN binding contract"
                );
            }
            activeSamplers.insert_or_assign(*slot, &uniform);
        }

        std::set<int> textureSlots;
        for (const auto& [slot, binding] : pass.textures) {
            static_cast<void>(binding);
            if (slot < 0 || slot >= 32) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Texture slot is outside the supported range"
                );
            }
            textureSlots.emplace(slot);
        }
        for (const auto& [slot, uniform] : activeSamplers) {
            static_cast<void>(uniform);
            textureSlots.emplace(slot);
        }

        for (const int slot : textureSlots) {
            const std::string samplerName =
                "g_Texture" + std::to_string(slot);
            const auto active = activeSamplers.find(slot);
            std::vector<FrameTextureCandidate> candidates;
            if (active != activeSamplers.end()) {
                for (auto& [source, name] : samplerDefaultTextures(
                         programResource.parameters,
                         active->second->name)) {
                    if (name.empty()) continue;
                    candidates.push_back({
                        .source = source,
                        .resource = samplerDefaultResource(
                            physicalPlan, pass, name
                        ),
                    });
                }
            }
            const auto authored = pass.textures.find(slot);
            if (authored != pass.textures.end()) {
                candidates.insert(
                    candidates.end(),
                    authored->second.candidates.begin(),
                    authored->second.candidates.end()
                );
            }
            const bool hasProviderContract =
                authored != pass.textures.end() || !candidates.empty() ||
                slot == 0;
            if (!hasProviderContract) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active sampler '" + active->second->name +
                        "' requires texture slot " + std::to_string(slot) +
                        ", but the frame pass provides no texture or metadata default"
                );
            }

            const std::optional<FrameResourceRef> selected =
                selectReadyTexture(
                    session,
                    candidates,
                    pass.previousInput,
                    pass.input,
                    aliases,
                    inputs.timeSeconds
                );
            if (!selected) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Texture provider chain for slot " +
                        std::to_string(slot) +
                        " has no ready candidate, previous input, or input"
                );
            }
            const FrameResourceRef& resource = *selected;
            if (resource.kind == FrameResourceKind::framebuffer &&
                framebuffer(resource, aliases).framebuffer ==
                    destination.framebuffer) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active sampler '" + samplerName +
                        "' resolves to the render destination '" +
                        resource.logicalName + "'"
                );
            }
            std::size_t imageIndex = 0;
            GLuint assetTextureValue = 0;
            if (slot == 0 &&
                resource.kind == FrameResourceKind::assetTexture) {
                result.texture0Animation = selectTexture0(resource);
                imageIndex = result.texture0Animation.imageIndex;
            }
            if (resource.kind == FrameResourceKind::assetTexture ||
                resource.kind == FrameResourceKind::hostTexture) {
                assetTextureValue = texture(
                    session, resource, imageIndex, inputs.timeSeconds
                );
            } else {
                static_cast<void>(framebuffer(resource, aliases));
            }
            result.textures.push_back({
                .slot = slot,
                .resource = resource,
                .assetTexture = assetTextureValue,
                .resolution = textureResolution(resource, aliases),
                .samplerLocation = prepareBuiltinUniform(
                    programResource, samplerName, GL_SAMPLER_2D
                ),
                .resolutionLocation = prepareBuiltinUniform(
                    programResource, samplerName + "Resolution", GL_FLOAT_VEC4
                ),
            });
        }
        result.uniforms.texture0Translation = prepareBuiltinUniform(
            programResource, "g_Texture0Translation", GL_FLOAT_VEC2
        );
        result.uniforms.texture0Rotation = prepareBuiltinUniform(
            programResource, "g_Texture0Rotation", GL_FLOAT_VEC4
        );
        result.uniforms.alternateModel = prepareBuiltinUniform(
            programResource, "g_AltModelMatrix", GL_FLOAT_MAT4
        );
        result.uniforms.alternateViewProjection = prepareBuiltinUniform(
            programResource, "g_AltViewProjectionMatrix", GL_FLOAT_MAT4
        );
        const auto identity = identityMatrix();
        const float imageWidth = checkedFloat(image.size.x, "Image width");
        const float imageHeight = checkedFloat(image.size.y, "Image height");
        const Matrix localProjection = orthographic(
            0.0F, imageWidth, 0.0F, imageHeight, -1.0F, 1.0F
        );
        result.model = localProjection;
        result.viewProjection = identity;

        float left = -1.0F;
        float right = 1.0F;
        float bottom = -1.0F;
        float top = 1.0F;
        const bool puppetGeometry =
            pass.geometry == FrameGeometryKind::puppetMesh;
        const bool puppetSceneSpace = puppetGeometry &&
            pass.destination.kind == physicalPlan.output.kind &&
            pass.destination.id == physicalPlan.output.id;
        if (pass.geometry == FrameGeometryKind::imageLocal ||
            (puppetGeometry && !puppetSceneSpace)) {
            left = 0.0F;
            right = imageWidth;
            bottom = 0.0F;
            top = imageHeight;
            result.modelViewProjection = localProjection;
        } else if (pass.geometry == FrameGeometryKind::imageScene ||
                   pass.geometry == FrameGeometryKind::passthroughCapture ||
                   puppetSceneSpace) {
            const auto& transform = image.worldTransform;
            // Wallpaper Engine's authored Y grows upward from the scene's
            // bottom edge. Convert it once for the vertically flipped
            // offscreen scene; presentation restores the visible orientation.
            // Alignment and scale remain baked into geometry because shaders
            // may inspect each common matrix separately.
            const double scaledWidth = image.size.x * transform.scale.x;
            const double scaledHeight = image.size.y * transform.scale.y;
            double centerX =
                transform.origin.x -
                    static_cast<double>(logicalPlan.width) * 0.5;
            double centerY = centeredWallpaperY(
                transform.origin.y, logicalPlan.height
            );
            if (image.horizontalAlignment.find("left") != std::string::npos) {
                centerX += scaledWidth * 0.5;
            } else if (
                image.horizontalAlignment.find("right") != std::string::npos
            ) {
                centerX -= scaledWidth * 0.5;
            }
            if (image.horizontalAlignment.find("top") != std::string::npos) {
                centerY += scaledHeight * 0.5;
            } else if (
                image.horizontalAlignment.find("bottom") != std::string::npos
            ) {
                centerY -= scaledHeight * 0.5;
            }
            left = checkedFloat(
                centerX - scaledWidth * 0.5, "Image scene left edge"
            );
            right = checkedFloat(
                centerX + scaledWidth * 0.5, "Image scene right edge"
            );
            bottom = checkedFloat(
                centerY - scaledHeight * 0.5, "Image scene bottom edge"
            );
            top = checkedFloat(
                centerY + scaledHeight * 0.5, "Image scene top edge"
            );
            const float pivotX = checkedFloat(centerX, "Image rotation pivot");
            const float pivotY = checkedFloat(centerY, "Image rotation pivot");
            const float angle = checkedFloat(
                transform.angles.z, "Image Z angle"
            );
            Matrix screenTransform = multiply(
                translation(pivotX, pivotY, 0.0F),
                multiply(
                    rotationZ(-angle),
                    translation(-pivotX, -pivotY, 0.0F)
                )
            );
            if (logicalPlan.parallax.enabled) {
                const auto depth = numericComponents(
                    image.parallaxDepth.value,
                    2,
                    "Image parallax depth"
                );
                const FrameVector2 parallaxOffset = sceneParallaxOffset(
                    logicalPlan,
                    {
                        .x = static_cast<double>(depth[0]),
                        .y = static_cast<double>(depth[1]),
                    },
                    frameParallax,
                    "image"
                );
                // Linux post-multiplies this translation into the rotated
                // screen MVP. This order matters when the image is rotated.
                screenTransform = multiply(
                    screenTransform,
                    translation(
                        checkedFloat(parallaxOffset.x, "Image parallax X"),
                        checkedFloat(parallaxOffset.y, "Image parallax Y"),
                        0.0F
                    )
                );
            }
            if (!camera.orthographicViewProjection) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Scene orthographic camera was not prepared"
                );
            }
            if (puppetSceneSpace) {
                // MDLV positions are image-local offsets around the mesh
                // centre. Map those local coordinates through the authored
                // image transform before applying the scene camera.
                const Matrix localToScene = multiply(
                    translation(pivotX, pivotY, 0.0F),
                    multiply(
                        scaling(
                            checkedFloat(transform.scale.x, "Image scale X"),
                            checkedFloat(transform.scale.y, "Image scale Y"),
                            checkedFloat(transform.scale.z, "Image scale Z")
                        ),
                        translation(
                            -imageWidth * 0.5F,
                            -imageHeight * 0.5F,
                            0.0F
                        )
                    )
                );
                result.modelViewProjection = multiply(
                    *camera.orthographicViewProjection,
                    multiply(screenTransform, localToScene)
                );
            } else {
                result.modelViewProjection = multiply(
                    *camera.orthographicViewProjection,
                    screenTransform
                );
            }
        } else {
            result.modelViewProjection = identity;
        }
        validateMatrix(result.model, "Image model matrix");
        validateMatrix(result.viewProjection, "Image view-projection matrix");
        validateMatrix(
            result.modelViewProjection,
            "Image model-view-projection matrix"
        );
        const auto color = numericComponents(
            image.color.value, 4, "Renderable color"
        );
        const auto alpha = numericComponents(
            image.alpha.value, 1, "Renderable alpha"
        );
        const auto brightness = numericComponents(
            image.brightness.value, 1, "Renderable brightness"
        );
        result.commonUniforms = prepareCommonUniforms(
            programResource,
            logicalPlan,
            {
                .width = physicalPlan.width,
                .height = physicalPlan.height,
            },
            inputs,
            result.model,
            result.viewProjection,
            result.modelViewProjection,
            {
                .brightness = brightness[0],
                .alpha = alpha[0],
                .color = {color[0], color[1], color[2], color[3]},
            }
        );
        for (const auto& metadata : programResource.parameters) {
            if (!metadata.material || isSamplerParameter(metadata)) continue;
            if (const auto value = pass.constants.find(*metadata.material); value != pass.constants.end()) {
                if (auto uniform = prepareRuntimeUniform(
                        programResource, metadata.name, value->second.value
                    )) {
                    result.materialUniforms.push_back(*uniform);
                }
            } else if (metadata.defaultValue) {
                if (auto uniform = prepareRuntimeUniform(
                        programResource, metadata.name,
                        metadataDefault(*metadata.defaultValue)
                    )) {
                    result.materialUniforms.push_back(*uniform);
                }
            } else if (activeUniform(programResource, metadata.name) != nullptr) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active material uniform '" + metadata.name +
                        "' has neither a frame constant nor metadata default"
                );
            }
        }
        float uMax = 1.0F;
        float vMax = 1.0F;
        if (pass.textureCoordinates == FrameTexCoordKind::image) {
            const auto resolution = textureResolution(pass.input, aliases);
            if (resolution[0] > 0 && resolution[1] > 0) {
                uMax = resolution[2] / resolution[0];
                vMax = resolution[3] / resolution[1];
            }
        }
        if (puppetGeometry) {
            if (!image.puppetMesh) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Puppet render pass has no decoded mesh descriptor"
                );
            }
            const PuppetMesh& mesh = *image.puppetMesh;
            if (mesh.vertices.empty() || mesh.indices.empty() ||
                mesh.indices.size() > static_cast<std::size_t>(
                    std::numeric_limits<GLsizei>::max()
                )) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Puppet mesh has no drawable geometry or too many indices"
                );
            }
            std::vector<PuppetAnimationLayerInput> animationLayers;
            animationLayers.reserve(image.puppetAnimationLayers.size());
            for (const FramePuppetAnimationLayer& layer :
                 image.puppetAnimationLayers) {
                animationLayers.push_back({
                    .animationId = layer.animationId,
                    .timeSeconds = inputs.timeSeconds * layer.rate,
                    .blend = checkedFloat(layer.blend, "Puppet animation blend"),
                    .additive = layer.additive,
                });
            }
            const std::vector<std::array<float, 3>> puppetPositions =
                evaluatePuppetPositions(mesh, animationLayers);
            if (puppetPositions.size() != mesh.vertices.size()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Puppet animation produced a vertex-count mismatch"
                );
            }
            result.puppetVertices.reserve(mesh.vertices.size());
            for (std::size_t vertexIndex = 0;
                 vertexIndex < mesh.vertices.size();
                 ++vertexIndex) {
                const PuppetVertex& vertex = mesh.vertices[vertexIndex];
                const auto& position = puppetPositions[vertexIndex];
                const float localX = imageWidth * 0.5F + position[0];
                const float localY = imageHeight * 0.5F - position[1];
                if (!std::isfinite(localX) || !std::isfinite(localY) ||
                    !std::isfinite(position[2])) {
                    throw Error(
                        ErrorCode::resourceValidation,
                        "Puppet mesh position is non-finite after image-size conversion"
                    );
                }
                result.puppetVertices.push_back({
                    .position = {localX, localY, position[2]},
                    .texCoord = {vertex.texCoord[0], vertex.texCoord[1]},
                });
            }
            result.puppetIndices = mesh.indices;
            ensurePuppetGeometry(session);
        } else {
            result.vertices = {{
                {{left, bottom, 0}, {0, 0}},
                {{right, bottom, 0}, {uMax, 0}},
                {{right, top, 0}, {uMax, vMax}},
                {{left, bottom, 0}, {0, 0}},
                {{right, top, 0}, {uMax, vMax}},
                {{left, top, 0}, {0, vMax}},
            }};
            ensureGeometry(session);
        }
        result.positionLocation = glGetAttribLocation(
            activeProgram, "a_Position"
        );
        result.texCoordLocation = glGetAttribLocation(
            activeProgram, "a_TexCoord"
        );
        if (result.positionLocation >= 0 &&
            result.positionLocation == result.texCoordLocation) {
            throw Error(
                ErrorCode::resourceValidation,
                "Image position and texture-coordinate attributes share an OpenGL location"
            );
        }
        session.checkError(
            ErrorCode::draw,
            "preparing a frame render pass"
        );
        return result;
    }

    void draw(Device::Session& session, const PreparedDraw& prepared) {
        auto& destination = framebuffer(prepared.pass.destination);
        glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
        glViewport(
            0, 0, static_cast<GLsizei>(destination.width),
            static_cast<GLsizei>(destination.height)
        );
        configureState(prepared.pass);
        glUseProgram(prepared.program);

        for (const PreparedTextureBinding& binding : prepared.textures) {
            glActiveTexture(GL_TEXTURE0 + binding.slot);
            glBindTexture(GL_TEXTURE_2D, preparedTexture(binding));
            if (binding.samplerLocation >= 0) {
                glUniform1i(binding.samplerLocation, binding.slot);
            }
            if (binding.resolutionLocation >= 0) {
                glUniform4fv(
                    binding.resolutionLocation, 1,
                    binding.resolution.data()
                );
            }
        }
        if (prepared.uniforms.texture0Translation >= 0) {
            glUniform2fv(
                prepared.uniforms.texture0Translation, 1,
                prepared.texture0Animation.translation.data()
            );
        }
        if (prepared.uniforms.texture0Rotation >= 0) {
            glUniform4fv(
                prepared.uniforms.texture0Rotation, 1,
                prepared.texture0Animation.rotation.data()
            );
        }

        for (const PreparedUniform& uniform : prepared.materialUniforms) {
            bindPreparedUniform(uniform);
        }
        bindCommonUniforms(
            prepared.commonUniforms,
            prepared.model,
            prepared.viewProjection,
            prepared.modelViewProjection
        );
        bindMatrix(prepared.uniforms.alternateModel, prepared.model);
        bindMatrix(
            prepared.uniforms.alternateViewProjection,
            prepared.viewProjection
        );

        if (prepared.pass.geometry == FrameGeometryKind::puppetMesh) {
            if (prepared.puppetVertices.empty() ||
                prepared.puppetIndices.empty() ||
                prepared.puppetIndices.size() > static_cast<std::size_t>(
                    std::numeric_limits<GLsizei>::max()
                ) ||
                prepared.puppetVertices.size() >
                    std::numeric_limits<std::size_t>::max() / sizeof(Vertex) ||
                prepared.puppetIndices.size() >
                    std::numeric_limits<std::size_t>::max() /
                        sizeof(std::uint16_t) ||
                prepared.puppetVertices.size() * sizeof(Vertex) >
                    static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max()) ||
                prepared.puppetIndices.size() * sizeof(std::uint16_t) >
                    static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max())) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Puppet draw data exceeds OpenGL buffer limits"
                );
            }
            glBindVertexArray(puppetVertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, puppetVertexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, puppetElementBuffer);
            glBufferData(
                GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(
                    prepared.puppetVertices.size() * sizeof(Vertex)
                ),
                prepared.puppetVertices.data(),
                GL_DYNAMIC_DRAW
            );
            glBufferData(
                GL_ELEMENT_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(
                    prepared.puppetIndices.size() * sizeof(std::uint16_t)
                ),
                prepared.puppetIndices.data(),
                GL_STATIC_DRAW
            );
            configureImageAttributes(
                puppetAttributeState,
                prepared.positionLocation,
                prepared.texCoordLocation,
                static_cast<GLsizei>(sizeof(Vertex))
            );
            glDrawElements(
                GL_TRIANGLES,
                static_cast<GLsizei>(prepared.puppetIndices.size()),
                GL_UNSIGNED_SHORT,
                nullptr
            );
        } else {
            glBindVertexArray(vertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
            configureImageAttributes(
                imageAttributeState,
                prepared.positionLocation,
                prepared.texCoordLocation,
                static_cast<GLsizei>(sizeof(Vertex))
            );
            glDrawArrays(GL_TRIANGLES, prepared.firstVertex, 6);
        }
        session.checkError(ErrorCode::draw, "executing frame render pass");
    }

    [[nodiscard]] PreparedDraw prepareCopy(
        Device::Session& session,
        const FramePlan& logicalPlan,
        const FramePlan& physicalPlan,
        const FrameCopyCommand& command,
        const ResolvedFrameInputs& inputs,
        const FrameVector2& frameParallax,
        const PreparedCamera& camera,
        const std::map<std::string, std::string>& aliases
    ) {
        if (command.source.kind == FrameResourceKind::framebuffer &&
            framebuffer(command.source, aliases).framebuffer ==
                framebuffer(command.destination, aliases).framebuffer) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame copy source and destination resolve to the same backing resource"
            );
        }
        FrameRenderPass pass{
            .origin = command.origin,
            .shader = "commands/copy",
            .vertexShaderPath = "shaders/commands/copy.vert",
            .fragmentShaderPath = "shaders/commands/copy.frag",
            .blending = BlendingMode::normal,
            .culling = CullingMode::disabled,
            .depthTest = DepthMode::disabled,
            .depthWrite = DepthMode::disabled,
            .geometry = FrameGeometryKind::fullscreenLocal,
            .textureCoordinates = FrameTexCoordKind::full,
            .input = command.source,
            .destination = command.destination,
            .textures = {{
                0,
                FrameTextureBinding{
                    .candidates = {{
                        .source = FrameTextureCandidateSource::bind,
                        .resource = command.source,
                    }},
                },
            }},
            .writeAlpha = true,
        };
        return prepareDraw(
            session, logicalPlan, physicalPlan, pass, inputs, frameParallax,
            camera, aliases
        );
    }

    void prepareSwap(
        const FrameSwapCommand& command,
        std::map<std::string, std::string>& aliases
    ) const {
        if (command.source.kind != FrameResourceKind::framebuffer ||
            command.destination.kind != FrameResourceKind::framebuffer) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame swap requires framebuffer resources"
            );
        }
        auto source = aliases.find(command.source.id);
        auto destination = aliases.find(command.destination.id);
        if (source == aliases.end() || destination == aliases.end()) {
            throw Error(
                ErrorCode::internalFailure,
                "Frame swap cannot resolve its prepared framebuffer aliases"
            );
        }
        std::swap(source->second, destination->second);
    }

    void swap(const FrameSwapCommand& command) {
        if (command.source.kind != FrameResourceKind::framebuffer ||
            command.destination.kind != FrameResourceKind::framebuffer) {
            throw Error(ErrorCode::resourceValidation, "Frame swap requires framebuffer resources");
        }
        auto source = framebufferAliases.find(command.source.id);
        auto destination = framebufferAliases.find(command.destination.id);
        if (source == framebufferAliases.end() || destination == framebufferAliases.end()) {
            throw Error(
                ErrorCode::internalFailure,
                "Frame swap cannot resolve its execution framebuffer aliases"
            );
        }
        std::swap(source->second, destination->second);
    }

    [[nodiscard]] PreparedClear prepareClear(
        const FrameClearCommand& command,
        const std::map<std::string, std::string>& aliases
    ) {
        static_cast<void>(framebuffer(command.destination, aliases));
        return {
            .destination = command.destination,
            .color = {
                checkedFloat(command.color.red, "Clear color"),
                checkedFloat(command.color.green, "Clear color"),
                checkedFloat(command.color.blue, "Clear color"),
                checkedFloat(command.color.alpha, "Clear color"),
            },
        };
    }

    void clear(Device::Session& session, const PreparedClear& prepared) {
        auto& destination = framebuffer(prepared.destination);
        glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
        glViewport(
            0, 0, static_cast<GLsizei>(destination.width),
            static_cast<GLsizei>(destination.height)
        );
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(
            prepared.color[0], prepared.color[1],
            prepared.color[2], prepared.color[3]
        );
        glClear(GL_COLOR_BUFFER_BIT);
        session.checkError(ErrorCode::draw, "executing frame clear command");
    }

    [[nodiscard]] PreparedText prepareText(
        Device::Session& session,
        const FramePlan& logicalPlan,
        const FrameTextCommand& command,
        const PreparedCamera& camera,
        const std::map<std::string, std::string>& aliases
    ) {
        if (command.textIndex >= logicalPlan.texts.size()) {
            throw Error(ErrorCode::resourceValidation, "Frame text command index is invalid");
        }
        const auto& descriptor = logicalPlan.texts[command.textIndex];
        if (descriptor.objectId != command.objectId) {
            throw Error(ErrorCode::resourceValidation, "Frame text command object identity is inconsistent");
        }
        const FramebufferResource& destination = framebuffer(
            command.destination, aliases
        );
        const auto& transform = descriptor.worldTransform;
        const CachedTextRaster& cachedRaster = cachedTextRaster(descriptor);
        const text::RasterizedText& rasterized = cachedRaster.rasterized;
        const TextLayout layout = textLayout(descriptor, rasterized);

        const double effectiveAlpha = descriptor.color.alpha * descriptor.alpha;
        const std::array<double, 4> colorComponents{
            descriptor.color.red,
            descriptor.color.green,
            descriptor.color.blue,
            effectiveAlpha,
        };
        std::array<float, 4> color{};
        for (std::size_t index = 0; index < color.size(); ++index) {
            const double component = colorComponents[index];
            if (!std::isfinite(component)) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Text color and opacity components must be finite"
                );
            }
            color[index] = static_cast<float>(component);
        }

        const Matrix alignment = translation(
            checkedFloat(layout.alignmentX, "Text horizontal alignment"),
            checkedFloat(layout.alignmentY, "Text vertical alignment"),
            0.0F
        );
        Matrix modelViewProjection;
        if (command.localSpace) {
            const auto destinationAlias = aliases.find(command.destination.id);
            if (command.destination.kind != FrameResourceKind::framebuffer ||
                destinationAlias == aliases.end()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Text command destination has no logical framebuffer alias"
                );
            }
            const auto logicalDestination = std::find_if(
                logicalPlan.framebuffers.begin(),
                logicalPlan.framebuffers.end(),
                [&destinationAlias](const FramebufferDescriptor& framebuffer) {
                    return framebuffer.resource.kind ==
                            FrameResourceKind::framebuffer &&
                        framebuffer.resource.id == destinationAlias->second;
                }
            );
            if (logicalDestination == logicalPlan.framebuffers.end()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Text command destination has no logical framebuffer descriptor"
                );
            }
            // The text mesh is authored in the destination's logical local
            // space. The physical draw viewport scales that NDC result; using
            // the smaller physical FBO here would clip/crop text geometry.
            const float width = static_cast<float>(logicalDestination->width);
            const float height = static_cast<float>(logicalDestination->height);
            const Matrix localProjection = orthographic(
                0.0F, width, 0.0F, height, -1.0F, 1.0F
            );
            modelViewProjection = multiply(
                localProjection,
                translation(
                    checkedFloat(descriptor.padding.x, "Text padding X"),
                    checkedFloat(descriptor.padding.y, "Text padding Y"),
                    0.0F
                )
            );
        } else {
            const float originX = static_cast<float>(
                transform.origin.x -
                    static_cast<double>(logicalPlan.width) * 0.5
            );
            const float originY = static_cast<float>(
                centeredWallpaperY(transform.origin.y, logicalPlan.height)
            );
            const Matrix world = multiply(
                translation(originX, originY, float(transform.origin.z)),
                multiply(
                    rotationZ(-checkedFloat(transform.angles.z, "Text Z angle")),
                    multiply(
                        rotationY(checkedFloat(transform.angles.y, "Text Y angle")),
                        multiply(
                            rotationX(-checkedFloat(transform.angles.x, "Text X angle")),
                            scaling(
                                checkedFloat(transform.scale.x, "Text scale X"),
                                checkedFloat(transform.scale.y, "Text scale Y"),
                                checkedFloat(transform.scale.z, "Text scale Z")
                            )
                        )
                    )
                )
            );
            if (!camera.orthographicViewProjection) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Scene orthographic camera was not prepared"
                );
            }
            modelViewProjection = multiply(
                *camera.orthographicViewProjection,
                multiply(world, alignment)
            );
        }
        validateMatrix(modelViewProjection, "Text model-view-projection matrix");
        return {
            .destination = command.destination,
            .coverage = textRenderer.prepare(
                session, rasterized, cachedRaster.coverageKey
            ),
            .request = {
                .modelViewProjection = modelViewProjection,
                .color = color,
            },
        };
    }

    void drawText(
        Device::Session& session,
        const PreparedText& prepared
    ) {
        textRenderer.drawPrepared(
            session,
            framebuffer(prepared.destination),
            prepared.coverage,
            prepared.request
        );
    }

    [[nodiscard]] static float particleFloat(double value, std::string_view name) {
        if (!std::isfinite(value) ||
            value > static_cast<double>(std::numeric_limits<float>::max()) ||
            value < -static_cast<double>(std::numeric_limits<float>::max())) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle " + std::string(name) +
                    " contains a non-finite or out-of-range value"
            );
        }
        return static_cast<float>(value);
    }

    [[nodiscard]] static float particleRenderY(
        double simulationY,
        std::string_view name
    ) {
        // ParticleSimulation intentionally mirrors Linux's centered Y-up
        // simulation contract. Scene framebuffers now retain Wallpaper
        // Engine's Y-down orientation until presentation, so convert local
        // particle geometry exactly once at the simulation-to-render boundary.
        return particleFloat(-simulationY, name);
    }

    [[nodiscard]] static ParticleAtlasMetadata particleAtlasMetadata(
        const AssetTextureResource& texture
    ) {
        if (texture.images.empty()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rendering requires at least one uploaded texture image"
            );
        }
        const bool hasColumns = texture.spritesheetColumns > 0;
        const bool hasRows = texture.spritesheetRows > 0;
        const bool hasFrames = texture.spritesheetFrameCount > 0;
        if (!hasColumns && !hasRows && !hasFrames) return {};
        if (!hasColumns || !hasRows || !hasFrames ||
            !std::isfinite(texture.spritesheetDuration) ||
            texture.spritesheetDuration < 0.0F ||
            static_cast<std::uint64_t>(texture.spritesheetColumns) *
                    texture.spritesheetRows <
                texture.spritesheetFrameCount ||
            !std::isfinite(texture.resolution[0]) ||
            !std::isfinite(texture.resolution[1]) ||
            texture.resolution[0] <= 0.0F || texture.resolution[1] <= 0.0F) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle spritesheet metadata is incomplete or invalid"
            );
        }
        const float frameWidth = 1.0F / float(texture.spritesheetColumns);
        const float frameHeight = 1.0F / float(texture.spritesheetRows);
        const float frameAspect = texture.isAnimated() &&
                texture.resolution[2] > 0.0F &&
                texture.resolution[3] > 0.0F
            ? texture.resolution[3] / texture.resolution[2]
            : (texture.resolution[1] * frameHeight) /
                (texture.resolution[0] * frameWidth);
        if (!std::isfinite(frameAspect) || frameAspect <= 0.0F) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle spritesheet frame aspect ratio is invalid"
            );
        }
        return {
            .columns = texture.spritesheetColumns,
            .rows = texture.spritesheetRows,
            .frames = texture.spritesheetFrameCount,
            .duration = texture.spritesheetDuration,
            .frameAspect = frameAspect,
        };
    }

    [[nodiscard]] static double particleLifetimeAttribute(
        const particle::ParticleInstance& particle,
        const FrameParticleDescriptor& descriptor,
        const ParticleAtlasMetadata& atlas,
        double presentationOffsetSeconds
    ) {
        const double life = particle.lifetime > 0.0
            ? (particle.age + presentationOffsetSeconds) / particle.lifetime
            : 1.0;
        if (!std::isfinite(life) || life < 0.0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle normalized lifetime is invalid"
            );
        }
        if (!atlas.enabled()) return life;
        if (!std::isfinite(descriptor.sequenceMultiplier) ||
            descriptor.sequenceMultiplier <= 0.0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle sequence multiplier must be finite and greater than zero"
            );
        }

        const double frameCount = static_cast<double>(atlas.frames);
        if (descriptor.animationMode == "randomframe") {
            if (!std::isfinite(particle.randomFrameUnit) ||
                particle.randomFrameUnit < 0.0 ||
                particle.randomFrameUnit >= 1.0) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle random frame unit must be finite and in [0, 1)"
                );
            }
            const double frame = std::floor(
                particle.randomFrameUnit * frameCount
            );
            return (frame + 0.5) / frameCount;
        }
        if (descriptor.animationMode == "once") {
            const double frame = std::min(
                life * frameCount * descriptor.sequenceMultiplier,
                frameCount - 1.0
            );
            return frame / frameCount;
        }
        if (descriptor.animationMode != "sequence") {
            throw Error(
                ErrorCode::resourceValidation,
                "Unknown particle animation mode '" +
                    descriptor.animationMode + "'"
            );
        }
        // Wallpaper Engine stretches particle sprite-sheet sequences across
        // each particle's lifetime. The duration stored in TEX metadata is an
        // image-animation setting and is explicitly ignored for particles.
        // sequenceMultiplier controls how many sequence cycles fit into that
        // normalized lifetime.
        return std::fmod(
            life * descriptor.sequenceMultiplier,
            1.0
        );
    }

    [[nodiscard]] static ParticleDrawBatch particleBatch(
        const particle::ParticleSimulation& simulation,
        const FrameParticleDescriptor& descriptor,
        ParticleAtlasMetadata atlas
    ) {
        const auto& particles = simulation.particles();
        const double presentationOffset = simulation.accumulatorSeconds();
        if (particles.size() >
            std::numeric_limits<std::uint32_t>::max() / 4U) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle vertex index count exceeds the uint32 range"
            );
        }
        if (particles.size() >
                std::numeric_limits<std::size_t>::max() / 4U ||
            particles.size() >
                std::numeric_limits<std::size_t>::max() / 6U) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle geometry count overflows size_t"
            );
        }

        ParticleDrawBatch batch;
        batch.atlas = atlas;
        batch.vertices.reserve(particles.size() * 4U);
        batch.indices.reserve(particles.size() * 6U);
        for (const auto& particle : particles) {
            const std::array<std::array<float, 2>, 4> coordinates{{
                {{0.0F, 1.0F}},
                {{1.0F, 1.0F}},
                {{1.0F, 0.0F}},
                {{0.0F, 0.0F}},
            }};
            const auto position = std::array{
                particleFloat(
                    particle.position.x + particle.velocity.x * presentationOffset,
                    "position"
                ),
                particleRenderY(
                    particle.position.y + particle.velocity.y * presentationOffset,
                    "position"
                ),
                particleFloat(
                    particle.position.z + particle.velocity.z * presentationOffset,
                    "position"
                ),
            };
            const auto velocity = std::array{
                particleFloat(particle.velocity.x, "velocity"),
                particleRenderY(particle.velocity.y, "velocity"),
                particleFloat(particle.velocity.z, "velocity"),
            };
            const auto rotation = std::array{
                particleFloat(
                    particle.rotation.x +
                        particle.angularVelocity.x * presentationOffset,
                    "rotation"
                ),
                particleFloat(
                    particle.rotation.y +
                        particle.angularVelocity.y * presentationOffset,
                    "rotation"
                ),
                particleFloat(
                    particle.rotation.z +
                        particle.angularVelocity.z * presentationOffset,
                    "rotation"
                ),
            };
            const auto color = std::array{
                particleFloat(particle.color.x, "color"),
                particleFloat(particle.color.y, "color"),
                particleFloat(particle.color.z, "color"),
                particleFloat(particle.alpha, "alpha"),
            };
            const float size = particleFloat(particle.size, "size");
            const float lifetime = particleFloat(
                particleLifetimeAttribute(
                    particle, descriptor, atlas, presentationOffset
                ),
                "encoded lifetime"
            );
            const std::uint32_t base = static_cast<std::uint32_t>(
                batch.vertices.size()
            );
            for (const auto& coordinate : coordinates) {
                batch.vertices.push_back({
                    .position = {position[0], position[1], position[2]},
                    .texCoordRotationSize = {
                        coordinate[0], coordinate[1], rotation[2], size,
                    },
                    .color = {color[0], color[1], color[2], color[3]},
                    .velocityLifetime = {
                        velocity[0], velocity[1], velocity[2], lifetime,
                    },
                    .rotationXY = {rotation[0], rotation[1]},
                });
            }
            batch.indices.insert(
                batch.indices.end(),
                {base, base + 1U, base + 2U, base + 2U, base + 3U, base}
            );
        }
        return batch;
    }

    [[nodiscard]] static int positiveRendererInteger(
        double value,
        int fallback
    ) {
        if (!std::isfinite(value) || value <= 0.0) return fallback;
        const double rounded = std::floor(value);
        if (rounded > static_cast<double>(std::numeric_limits<int>::max())) {
            return std::numeric_limits<int>::max();
        }
        return std::max(1, static_cast<int>(rounded));
    }

    template <typename ParticleRange>
    [[nodiscard]] static ParticleDrawBatch particleRopePathBatch(
        const ParticleRange& particles,
        const FrameParticleDescriptor& descriptor,
        ParticleAtlasMetadata atlas,
        double presentationTimeSeconds,
        double presentationOffsetSeconds = 0.0
    ) {
        ParticleDrawBatch batch;
        batch.atlas = atlas;
        batch.rope = true;
        if (particles.size() < 2) return batch;

        // Linux clamps an authored zero/negative subdivision to one at render
        // time.  The parser's rope default is four, but that default is
        // already present in the descriptor; it must not be reused for an
        // explicitly invalid value here.
        const int subdivision = positiveRendererInteger(
            descriptor.renderer.subdivision, 1
        );
        const std::size_t segmentCount = particles.size() - 1;
        if (segmentCount > (std::numeric_limits<std::size_t>::max() - 1U) /
                static_cast<std::size_t>(subdivision)) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rope subdivision overflows geometry size"
            );
        }
        const std::size_t totalPoints =
            segmentCount * static_cast<std::size_t>(subdivision) + 1U;
        if (totalPoints >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) / 4U) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rope geometry exceeds the uint32 vertex-index range"
            );
        }
        if (totalPoints > std::numeric_limits<std::size_t>::max() / 4U) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rope geometry vertex count overflows size_t"
            );
        }

        struct Point final {
            particle::Vector3 position;
            double size = 0.0;
            std::array<double, 4> color{};
        };
        std::vector<Point> points(totalPoints);
        const auto catmullRom = [](particle::Vector3 p0,
                                   particle::Vector3 p1,
                                   particle::Vector3 p2,
                                   particle::Vector3 p3,
                                   double t) {
            const double t2 = t * t;
            const double t3 = t2 * t;
            return particle::Vector3{
                0.5 * ((2.0 * p1.x) + (-p0.x + p2.x) * t +
                       (2.0 * p0.x - 5.0 * p1.x + 4.0 * p2.x - p3.x) * t2 +
                       (-p0.x + 3.0 * p1.x - 3.0 * p2.x + p3.x) * t3),
                0.5 * ((2.0 * p1.y) + (-p0.y + p2.y) * t +
                       (2.0 * p0.y - 5.0 * p1.y + 4.0 * p2.y - p3.y) * t2 +
                       (-p0.y + 3.0 * p1.y - 3.0 * p2.y + p3.y) * t3),
                0.5 * ((2.0 * p1.z) + (-p0.z + p2.z) * t +
                       (2.0 * p0.z - 5.0 * p1.z + 4.0 * p2.z - p3.z) * t2 +
                       (-p0.z + 3.0 * p1.z - 3.0 * p2.z + p3.z) * t3),
            };
        };
        const auto presentedPoint = [presentationOffsetSeconds](const auto& p) {
            if constexpr (requires { p.velocity; }) {
                return Point{
                    .position = {
                        p.position.x + p.velocity.x * presentationOffsetSeconds,
                        p.position.y + p.velocity.y * presentationOffsetSeconds,
                        p.position.z + p.velocity.z * presentationOffsetSeconds,
                    },
                    .size = p.size,
                    .color = {p.color.x, p.color.y, p.color.z, p.alpha},
                };
            } else {
                return Point{
                    .position = p.position,
                    .size = p.size,
                    .color = {p.color.x, p.color.y, p.color.z, p.alpha},
                };
            }
        };
        for (std::size_t segment = 0; segment < segmentCount; ++segment) {
            const Point p1 = presentedPoint(particles[segment]);
            const Point p2 = presentedPoint(particles[segment + 1U]);
            const Point p0 = segment > 0
                ? presentedPoint(particles[segment - 1U]) : p1;
            const Point p3 = segment + 2U < particles.size()
                ? presentedPoint(particles[segment + 2U]) : p2;
            for (int step = 0; step < subdivision; ++step) {
                const double t = static_cast<double>(step) /
                    static_cast<double>(subdivision);
                const std::size_t index = segment * static_cast<std::size_t>(subdivision) +
                    static_cast<std::size_t>(step);
                points[index] = {
                    .position = catmullRom(p0.position, p1.position, p2.position, p3.position, t),
                    .size = p1.size + (p2.size - p1.size) * t,
                    .color = {
                        p1.color[0] + (p2.color[0] - p1.color[0]) * t,
                        p1.color[1] + (p2.color[1] - p1.color[1]) * t,
                        p1.color[2] + (p2.color[2] - p1.color[2]) * t,
                        p1.color[3] + (p2.color[3] - p1.color[3]) * t,
                    },
                };
            }
        }
        points.back() = presentedPoint(particles.back());

        const double uvScale = descriptor.renderer.uvScale > 0.0 &&
                std::isfinite(descriptor.renderer.uvScale)
            ? descriptor.renderer.uvScale : 1.0;
        const double trailLength =
            static_cast<double>(totalPoints - 1U) / uvScale + 1.0;
        const double usableLength = trailLength - 1.0;
        // CParticle records this from the lifetime initializer range, not
        // from the random values of the currently alive particles.  In
        // particular, a fixed range enables smoothing even before all
        // particles happen to share an identical sampled lifetime.
        bool uniformLifetimes = false;
        for (const auto& initializer : descriptor.configuration.initializers) {
            if (const auto* lifetime =
                    std::get_if<particle::LifetimeRandomInitializer>(&initializer)) {
                uniformLifetimes = lifetime->minimum == lifetime->maximum;
            }
        }
        const bool smooth = descriptor.renderer.uvSmoothing &&
            uniformLifetimes && !descriptor.renderer.uvScrolling;
        std::vector<double> arc;
        double arcTotal = 0.0;
        if (smooth) {
            arc.assign(totalPoints, 0.0);
            for (std::size_t index = 1; index < totalPoints; ++index) {
                const auto& a = points[index - 1].position;
                const auto& b = points[index].position;
                const double dx = b.x - a.x;
                const double dy = b.y - a.y;
                const double dz = b.z - a.z;
                arcTotal += std::sqrt(dx * dx + dy * dy + dz * dz);
                arc[index] = arcTotal;
            }
        }
        const double scrollOffset = descriptor.renderer.uvScrolling &&
                usableLength > 0.0
            ? std::fmod(presentationTimeSeconds, 10000.0) * usableLength
            : 0.0;

        batch.ropeVertices.reserve((totalPoints - 1U) * 4U);
        batch.indices.reserve((totalPoints - 1U) * 6U);
        for (std::size_t segment = 0; segment + 1U < totalPoints; ++segment) {
            const Point& start = points[segment];
            const Point& end = points[segment + 1U];
            const Point& previous = segment > 0 ? points[segment - 1U] : start;
            const Point& next = segment + 2U < totalPoints
                ? points[segment + 2U] : end;
            double trailPosition = static_cast<double>(segment);
            if (smooth && arcTotal > 0.0) {
                trailPosition = arc[segment] / arcTotal *
                    static_cast<double>(totalPoints - 1U);
            }
            trailPosition += scrollOffset;
            const std::uint32_t base = static_cast<std::uint32_t>(
                batch.ropeVertices.size()
            );
            const auto append = [&](float u, float v) {
                RopeParticleVertex vertex{};
                vertex.positionVec4[0] = particleFloat(start.position.x, "rope position");
                vertex.positionVec4[1] = particleRenderY(start.position.y, "rope position");
                vertex.positionVec4[2] = particleFloat(start.position.z, "rope position");
                vertex.positionVec4[3] = particleFloat(start.size, "rope size");
                vertex.texCoordVec4[0] = particleFloat(end.position.x, "rope position");
                vertex.texCoordVec4[1] = particleRenderY(end.position.y, "rope position");
                vertex.texCoordVec4[2] = particleFloat(end.position.z, "rope position");
                vertex.texCoordVec4[3] = particleFloat(trailLength, "rope length");
                vertex.texCoordVec4C1[0] = particleFloat(previous.position.x, "rope position");
                vertex.texCoordVec4C1[1] = particleRenderY(previous.position.y, "rope position");
                vertex.texCoordVec4C1[2] = particleFloat(previous.position.z, "rope position");
                vertex.texCoordVec4C1[3] = particleFloat(trailPosition, "rope trail position");
                vertex.texCoordVec4C2[0] = particleFloat(next.position.x, "rope position");
                vertex.texCoordVec4C2[1] = particleRenderY(next.position.y, "rope position");
                vertex.texCoordVec4C2[2] = particleFloat(next.position.z, "rope position");
                vertex.texCoordVec4C2[3] = particleFloat(end.size, "rope size");
                for (int channel = 0; channel < 4; ++channel) {
                    vertex.texCoordVec4C3[channel] = particleFloat(
                        end.color[static_cast<std::size_t>(channel)],
                        "rope color"
                    );
                    vertex.color[channel] = particleFloat(
                        start.color[static_cast<std::size_t>(channel)],
                        "rope color"
                    );
                }
                vertex.texCoordC4[0] = u;
                vertex.texCoordC4[1] = v;
                batch.ropeVertices.push_back(vertex);
            };
            append(0.0F, 0.0F);
            append(1.0F, 0.0F);
            append(1.0F, 1.0F);
            append(0.0F, 1.0F);
            batch.indices.insert(
                batch.indices.end(),
                {base, base + 1U, base + 2U, base + 2U, base + 3U, base}
            );
        }
        return batch;
    }

    [[nodiscard]] static RopePathPoint ropePathPoint(
        const particle::ParticleInstance& particle
    ) {
        return {
            .position = particle.position,
            .size = particle.size,
            .color = particle.color,
            .alpha = particle.alpha,
        };
    }

    [[nodiscard]] static RopePathPoint interpolateRopePathPoint(
        const RopePathPoint& start,
        const RopePathPoint& end,
        double amount
    ) {
        const auto interpolate = [amount](double a, double b) {
            return a + (b - a) * amount;
        };
        return {
            .position = {
                interpolate(start.position.x, end.position.x),
                interpolate(start.position.y, end.position.y),
                interpolate(start.position.z, end.position.z),
            },
            .size = interpolate(start.size, end.size),
            .color = {
                interpolate(start.color.x, end.color.x),
                interpolate(start.color.y, end.color.y),
                interpolate(start.color.z, end.color.z),
            },
            .alpha = interpolate(start.alpha, end.alpha),
        };
    }

    static void appendRopePathBatch(
        ParticleDrawBatch& destination,
        ParticleDrawBatch source
    ) {
        if (!source.rope || !source.vertices.empty()) {
            throw Error(
                ErrorCode::internalFailure,
                "Rope path builder returned an incompatible particle batch"
            );
        }
        if (source.ropeVertices.empty()) return;
        if (destination.ropeVertices.size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) -
                source.ropeVertices.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rope paths exceed the uint32 vertex-index range"
            );
        }
        const std::uint32_t vertexOffset = static_cast<std::uint32_t>(
            destination.ropeVertices.size()
        );
        if (destination.ropeVertices.size() >
                std::numeric_limits<std::size_t>::max() -
                    source.ropeVertices.size() ||
            destination.indices.size() >
                std::numeric_limits<std::size_t>::max() -
                    source.indices.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rope path geometry overflows size_t"
            );
        }
        for (const std::uint32_t index : source.indices) {
            if (index > std::numeric_limits<std::uint32_t>::max() -
                    vertexOffset) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle rope path index exceeds the uint32 range"
                );
            }
        }
        destination.ropeVertices.insert(
            destination.ropeVertices.end(),
            std::make_move_iterator(source.ropeVertices.begin()),
            std::make_move_iterator(source.ropeVertices.end())
        );
        destination.indices.reserve(
            destination.indices.size() + source.indices.size()
        );
        for (const std::uint32_t index : source.indices) {
            destination.indices.push_back(index + vertexOffset);
        }
    }

    [[nodiscard]] static ParticleDrawBatch particleRopeBatch(
        const particle::ParticleSimulation& simulation,
        const FrameParticleDescriptor& descriptor,
        ParticleAtlasMetadata atlas
    ) {
        return particleRopePathBatch(
            simulation.particles(), descriptor, atlas,
            simulation.simulationTimeSeconds() +
                simulation.accumulatorSeconds(),
            simulation.accumulatorSeconds()
        );
    }

    static void updateParticleRopeTrails(
        ParticleState& state,
        const FrameParticleDescriptor& descriptor
    ) {
        const double duration = descriptor.renderer.length;
        if (!std::isfinite(duration) || duration <= 0.0) {
            state.ropeTrails.clear();
            return;
        }
        const double currentTime = state.simulation.simulationTimeSeconds();
        const double tolerance = std::numeric_limits<double>::epsilon() *
            std::max(1.0, std::abs(currentTime)) * 64.0;
        const double cutoff = std::max(0.0, currentTime - duration);
        std::set<std::uint64_t> alive;

        for (const particle::ParticleInstance& particle :
             state.simulation.particles()) {
            alive.insert(particle.spawnId);
            RopeTrailHistory& history = state.ropeTrails[particle.spawnId];
            RopeTrailSample current{
                .timeSeconds = currentTime,
                .point = ropePathPoint(particle),
            };
            if (history.samples.empty()) {
                history.samples.push_back(std::move(current));
                continue;
            }

            if (currentTime + tolerance < history.samples.back().timeSeconds) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Particle rope trail time moved backwards"
                );
            }
            if (std::abs(
                    currentTime - history.samples.back().timeSeconds
                ) <= tolerance) {
                history.samples.back() = std::move(current);
            } else {
                // History acquisition follows the actual simulation/render
                // cadence. The authored segment count controls only the
                // resampled draw geometry below; using length / segments as
                // the acquisition period turns an otherwise 60 Hz trail into
                // visibly stepped geometry (for example 0.4 / 4 = 10 Hz).
                history.samples.push_back(std::move(current));
            }

            // Keep one point before the duration boundary so geometry can
            // interpolate an exact cutoff instead of shortening the trail.
            while (history.samples.size() > 2 &&
                   history.samples[1].timeSeconds <= cutoff + tolerance) {
                history.samples.erase(history.samples.begin());
            }
        }

        for (auto iterator = state.ropeTrails.begin();
             iterator != state.ropeTrails.end();) {
            if (!alive.contains(iterator->first)) {
                iterator = state.ropeTrails.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    [[nodiscard]] static std::vector<RopePathPoint> ropeTrailPath(
        const RopeTrailHistory& history,
        double currentTime,
        double duration,
        std::size_t maximumPointCount
    ) {
        if (history.samples.size() < 2 || maximumPointCount < 2) return {};
        const double cutoff = std::max(0.0, currentTime - duration);
        std::vector<RopeTrailSample> clipped;
        clipped.reserve(history.samples.size() + 1U);

        std::size_t first = 0;
        while (first + 1U < history.samples.size() &&
               history.samples[first + 1U].timeSeconds <= cutoff) {
            ++first;
        }
        if (first + 1U < history.samples.size() &&
            history.samples[first].timeSeconds < cutoff) {
            const RopeTrailSample& before = history.samples[first];
            const RopeTrailSample& after = history.samples[first + 1U];
            const double span = after.timeSeconds - before.timeSeconds;
            if (span > 0.0) {
                clipped.push_back({
                    .timeSeconds = cutoff,
                    .point = interpolateRopePathPoint(
                        before.point,
                        after.point,
                        (cutoff - before.timeSeconds) / span
                    ),
                });
                ++first;
            }
        }
        clipped.insert(
            clipped.end(),
            history.samples.begin() + static_cast<std::ptrdiff_t>(first),
            history.samples.end()
        );
        if (clipped.size() < 2) return {};

        if (clipped.size() > maximumPointCount) {
            std::vector<RopeTrailSample> resampled;
            resampled.reserve(maximumPointCount);
            const double startTime = clipped.front().timeSeconds;
            const double endTime = clipped.back().timeSeconds;
            std::size_t upper = 1;
            for (std::size_t index = 0; index < maximumPointCount; ++index) {
                const double amount = static_cast<double>(index) /
                    static_cast<double>(maximumPointCount - 1U);
                const double target = startTime +
                    (endTime - startTime) * amount;
                while (upper + 1U < clipped.size() &&
                       clipped[upper].timeSeconds < target) {
                    ++upper;
                }
                const RopeTrailSample& before = clipped[upper - 1U];
                const RopeTrailSample& after = clipped[upper];
                const double span = after.timeSeconds - before.timeSeconds;
                const double local = span > 0.0
                    ? std::clamp(
                          (target - before.timeSeconds) / span,
                          0.0,
                          1.0
                      )
                    : 0.0;
                resampled.push_back({
                    .timeSeconds = target,
                    .point = interpolateRopePathPoint(
                        before.point, after.point, local
                    ),
                });
            }
            clipped = std::move(resampled);
        }

        std::vector<RopePathPoint> result;
        result.reserve(clipped.size());
        for (const RopeTrailSample& sample : clipped) {
            result.push_back(sample.point);
        }
        return result;
    }

    [[nodiscard]] static ParticleDrawBatch particleRopeTrailBatch(
        const ParticleState& state,
        const FrameParticleDescriptor& descriptor,
        ParticleAtlasMetadata atlas
    ) {
        ParticleDrawBatch batch;
        batch.atlas = atlas;
        batch.rope = true;
        const double duration = descriptor.renderer.length;
        if (!std::isfinite(duration) || duration <= 0.0) return batch;

        const int segmentCount = std::max(
            2,
            positiveRendererInteger(descriptor.renderer.segments, 2)
        );
        const int subdivision = positiveRendererInteger(
            descriptor.renderer.subdivision, 1
        );
        const long double maximumSubsegments =
            static_cast<long double>(segmentCount) * subdivision;
        if (!std::isfinite(maximumSubsegments) ||
            maximumSubsegments >
                static_cast<long double>(std::numeric_limits<float>::max())) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rope trail segment count is too large"
            );
        }
        const double uvScale = descriptor.renderer.uvScale > 0.0 &&
                std::isfinite(descriptor.renderer.uvScale)
            ? descriptor.renderer.uvScale
            : 1.0;
        const long double maximumScaledSegments =
            maximumSubsegments / uvScale + 1.0L;
        if (!std::isfinite(maximumScaledSegments) ||
            maximumScaledSegments >
                static_cast<long double>(std::numeric_limits<float>::max())) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rope trail UV-scaled segment count is too large"
            );
        }
        batch.ropeSegmentMaxCountUnscaled = static_cast<float>(
            maximumSubsegments
        );
        batch.ropeSegmentMaxCount = static_cast<float>(
            maximumScaledSegments
        );
        const double sampleInterval = duration /
            static_cast<double>(segmentCount);
        const double presentationOffset =
            state.simulation.accumulatorSeconds();
        const double presentationTime =
            state.simulation.simulationTimeSeconds() + presentationOffset;
        batch.ropeSegmentTimeOffset = sampleInterval > 0.0
            ? static_cast<float>(
                  std::fmod(
                      presentationTime,
                      sampleInterval
                  ) / sampleInterval
              )
            : 0.0F;

        const std::size_t maximumPointCount =
            static_cast<std::size_t>(segmentCount) + 1U;
        bool tracedPath = false;
        for (const particle::ParticleInstance& particle :
             state.simulation.particles()) {
            const auto found = state.ropeTrails.find(particle.spawnId);
            if (found == state.ropeTrails.end()) continue;
            RopeTrailHistory presentedHistory = found->second;
            if (presentationOffset > 0.0) {
                presentedHistory.samples.push_back({
                    .timeSeconds = presentationTime,
                    .point = {
                        .position = {
                            particle.position.x +
                                particle.velocity.x * presentationOffset,
                            particle.position.y +
                                particle.velocity.y * presentationOffset,
                            particle.position.z +
                                particle.velocity.z * presentationOffset,
                        },
                        .size = particle.size,
                        .color = particle.color,
                        .alpha = particle.alpha,
                    },
                });
            }
            const std::vector<RopePathPoint> path = ropeTrailPath(
                presentedHistory,
                presentationTime,
                duration,
                maximumPointCount
            );
            if (path.size() < 2) continue;
            if (!tracedPath) {
                const RopePathPoint& tail = path.front();
                const RopePathPoint& head = path.back();
                frameTraceLog(
                    "rope.path object=%d spawn=%llu simulation=%.6f "
                    "presentation=%.6f accumulator=%.6f historySamples=%zu "
                    "pathPoints=%zu particle=(%.6f,%.6f,%.6f) "
                    "velocity=(%.6f,%.6f,%.6f) tail=(%.6f,%.6f,%.6f) "
                    "head=(%.6f,%.6f,%.6f)",
                    descriptor.objectId,
                    static_cast<unsigned long long>(particle.spawnId),
                    state.simulation.simulationTimeSeconds(),
                    presentationTime,
                    presentationOffset,
                    found->second.samples.size(),
                    path.size(),
                    particle.position.x,
                    particle.position.y,
                    particle.position.z,
                    particle.velocity.x,
                    particle.velocity.y,
                    particle.velocity.z,
                    tail.position.x,
                    tail.position.y,
                    tail.position.z,
                    head.position.x,
                    head.position.y,
                    head.position.z
                );
                tracedPath = true;
            }
            appendRopePathBatch(
                batch,
                particleRopePathBatch(
                    path,
                    descriptor,
                    atlas,
                    presentationTime
                )
            );
        }
        return batch;
    }

    [[nodiscard]] std::string particleAssetIdentity(
        const FrameParticleDescriptor& descriptor
    ) const {
        return frameGraph->graph()->model()->project().assetPath +
            "\x1f" + descriptor.definitionIdentity;
    }

    void initializeParticleStates(
        const FramePlan& plan,
        std::map<int, ParticleState>& working
    ) const {
        std::set<int> descriptorIds;
        for (const auto& descriptor : plan.particles) {
            if (!descriptorIds.emplace(descriptor.objectId).second) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame plan contains duplicate particle runtime object ids"
                );
            }
        }
        for (auto iterator = working.begin(); iterator != working.end();) {
            if (!descriptorIds.contains(iterator->first)) {
                iterator = working.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    [[nodiscard]] std::pair<ParticleState, ParticleDrawBatch>
    advanceParticleState(
        const FrameParticleDescriptor& descriptor,
        const ParticleAtlasMetadata& atlas,
        const ResolvedFrameInputs& inputs,
        const ParticleState* previous
    ) {
        const std::string identity = particleAssetIdentity(descriptor);
        const auto traceStarted = FrameTraceClock::now();
        const double simulationBefore = previous != nullptr &&
                previous->assetIdentity == identity
            ? previous->simulation.simulationTimeSeconds()
            : 0.0;
        const double accumulatorBefore = previous != nullptr &&
                previous->assetIdentity == identity
            ? previous->simulation.accumulatorSeconds()
            : 0.0;
        try {
            ParticleState state = previous != nullptr &&
                    previous->assetIdentity == identity
                ? *previous
                : ParticleState{
                      .assetIdentity = identity,
                      .simulation = particle::ParticleSimulation(
                          descriptor.configuration,
                          descriptor.objectId,
                          identity
                      ),
                  };
            state.simulation.advance(
                inputs.frameTimeSeconds,
                descriptor.configuration
            );
            ParticleDrawBatch batch;
            if (descriptor.renderer.kind ==
                FrameParticleRendererKind::ropeTrail) {
                updateParticleRopeTrails(state, descriptor);
                batch = particleRopeTrailBatch(state, descriptor, atlas);
            } else if (descriptor.renderer.kind ==
                       FrameParticleRendererKind::rope) {
                state.ropeTrails.clear();
                batch = particleRopeBatch(
                    state.simulation, descriptor, atlas
                );
            } else {
                state.ropeTrails.clear();
                batch = particleBatch(
                    state.simulation, descriptor, atlas
                );
            }
            std::size_t ropeSampleCount = 0;
            std::size_t maximumRopeSamples = 0;
            for (const auto& [spawnId, history] : state.ropeTrails) {
                static_cast<void>(spawnId);
                ropeSampleCount += history.samples.size();
                maximumRopeSamples = std::max(
                    maximumRopeSamples,
                    history.samples.size()
                );
            }
            frameTraceLog(
                "particle frame=%llu object=%d renderer=%d delta=%.6f "
                "simBefore=%.6f accBefore=%.6f simAfter=%.6f accAfter=%.6f "
                "alive=%zu histories=%zu samples=%zu maxSamples=%zu "
                "segments=%.3f subdivision=%.3f trailLength=%.6f "
                "vertices=%zu ropeVertices=%zu indices=%zu ms=%.3f",
                static_cast<unsigned long long>(traceFrameSequence),
                descriptor.objectId,
                static_cast<int>(descriptor.renderer.kind),
                inputs.frameTimeSeconds,
                simulationBefore,
                accumulatorBefore,
                state.simulation.simulationTimeSeconds(),
                state.simulation.accumulatorSeconds(),
                state.simulation.particles().size(),
                state.ropeTrails.size(),
                ropeSampleCount,
                maximumRopeSamples,
                descriptor.renderer.segments,
                descriptor.renderer.subdivision,
                descriptor.renderer.length,
                batch.vertices.size(),
                batch.ropeVertices.size(),
                batch.indices.size(),
                frameTraceMilliseconds(traceStarted)
            );
            return {std::move(state), std::move(batch)};
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::invalid_argument& error) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle object " + std::to_string(descriptor.objectId) +
                    " simulation failed: " + error.what()
            );
        } catch (const std::overflow_error& error) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle object " + std::to_string(descriptor.objectId) +
                    " simulation failed: " + error.what()
            );
        }
    }

    [[nodiscard]] ParticlePreparation prepareParticle(
        Device::Session& session,
        const FramePlan& logicalPlan,
        const FramePlan& physicalPlan,
        const FrameParticleCommand& command,
        std::size_t operationIndex,
        const ResolvedFrameInputs& inputs,
        const FrameVector2& frameParallax,
        const PreparedCamera& camera,
        const std::map<std::string, std::string>& aliases,
        const ParticleState* previousState,
        const ParticleDrawBatch* frozenBatch
    ) {
        if (command.particleIndex >= logicalPlan.particles.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame particle command index is invalid"
            );
        }
        const auto& descriptor = logicalPlan.particles[command.particleIndex];
        if (descriptor.objectId != command.objectId) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame particle command object identity is inconsistent"
            );
        }
        if (command.destination != physicalPlan.output) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle phase one requires the scene output as its destination"
            );
        }
        const bool ropeRenderer =
            descriptor.renderer.kind == FrameParticleRendererKind::rope ||
            descriptor.renderer.kind == FrameParticleRendererKind::ropeTrail;
        if (ropeRenderer && descriptor.shader != "genericropeparticle") {
            throw Error(
                ErrorCode::resourceValidation,
                "Rope particle renderer requires the genericropeparticle shader"
            );
        }
        if (descriptor.textures.empty() ||
            descriptor.texture0.kind != FrameResourceKind::assetTexture ||
            descriptor.texture0.id.empty()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle texture slot zero must reference a real asset texture"
            );
        }
        static_cast<void>(framebuffer(command.destination, aliases));

        AssetTextureResource& textureResource = assetTexture(
            session, descriptor.texture0, inputs.timeSeconds
        );
        const TextureAnimationSelection texture0Animation =
            selectTextureAnimation(textureResource, inputs.timeSeconds);
        const ParticleAtlasMetadata atlas = particleAtlasMetadata(
            textureResource
        );
        std::optional<ParticleDrawBatch> nextBatch;
        std::optional<ParticleState> nextState;
        const ParticleDrawBatch* batch = frozenBatch;
        if (batch == nullptr) {
            auto advanced = advanceParticleState(
                descriptor, atlas, inputs, previousState
            );
            nextState.emplace(std::move(advanced.first));
            nextBatch.emplace(std::move(advanced.second));
            batch = &*nextBatch;
        }

        ComboMap effectiveCombos = descriptor.combos;
        applyTexture0FormatCombo(effectiveCombos, textureResource.format);
        const auto requireCombo = [&](const char* name, int expected) {
            const auto authored = effectiveCombos.find(name);
            if (authored != effectiveCombos.end() &&
                authored->second != expected) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle shader combination '" + std::string(name) +
                        "' must be " + std::to_string(expected)
                );
            }
            effectiveCombos[name] = expected;
        };
        requireCombo("GS_ENABLED", 0);
        requireCombo("THICKFORMAT", 1);
        requireCombo(
            "TRAILRENDERER",
            descriptor.renderer.kind == FrameParticleRendererKind::spriteTrail ||
                descriptor.renderer.kind == FrameParticleRendererKind::ropeTrail
                ? 1 : 0
        );
        requireCombo(
            "TRAILFADEALPHA",
            descriptor.renderer.kind == FrameParticleRendererKind::ropeTrail &&
                    descriptor.renderer.fadeAlpha
                ? 1 : 0
        );
        requireCombo(
            "TRAILFADESIZE",
            descriptor.renderer.kind == FrameParticleRendererKind::ropeTrail &&
                    descriptor.renderer.fadeSize
                ? 1 : 0
        );
        effectiveCombos["SPRITESHEET"] = batch->atlas.enabled() ? 1 : 0;
        ProgramResource& programResource = program(
            session,
            descriptor.vertexShaderPath,
            descriptor.fragmentShaderPath,
            effectiveCombos,
            "Particle render pass"
        );
        const GLuint activeProgram = programResource.program;

        PreparedParticle prepared{
            .destination = command.destination,
            .blending = descriptor.blending,
            .culling = descriptor.culling,
            .depthTest = descriptor.depthTest,
            .depthWrite = descriptor.depthWrite,
            .program = activeProgram,
            .texture0Animation = texture0Animation,
            .rope = ropeRenderer,
            .refract = effectiveCombos.contains("REFRACT") &&
                effectiveCombos.at("REFRACT") != 0,
            .operationIndex = operationIndex,
        };
        const FrameResourceRef destination = command.destination;
        std::map<int, const ActiveUniform*> activeSamplers;
        for (const ActiveUniform& uniform : programResource.uniforms) {
            if (!isOpenGLSamplerType(uniform.type)) continue;
            if (uniform.type != GL_SAMPLER_2D) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active particle sampler '" + uniform.name +
                        "' uses an unsupported non-2D texture type"
                );
            }
            const std::optional<int> slot = textureSlot(uniform.name);
            if (!slot) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active particle sampler '" + uniform.name +
                        "' does not follow the g_TextureN binding contract"
                );
            }
            activeSamplers.insert_or_assign(*slot, &uniform);
        }

        std::set<int> textureSlots;
        for (const auto& [slot, binding] : descriptor.textures) {
            static_cast<void>(binding);
            if (slot < 0 || slot >= 32) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle texture slot is outside the supported range 0...31"
                );
            }
            textureSlots.emplace(slot);
        }
        for (const auto& [slot, uniform] : activeSamplers) {
            static_cast<void>(uniform);
            if (slot < 0 || slot >= 32) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active particle sampler slot is outside the supported range 0...31"
                );
            }
            textureSlots.emplace(slot);
        }

        for (const int slot : textureSlots) {
            const std::string samplerName =
                "g_Texture" + std::to_string(slot);
            const auto active = activeSamplers.find(slot);
            std::vector<FrameTextureCandidate> candidates;
            if (active != activeSamplers.end()) {
                for (auto& [source, name] : samplerDefaultTextures(
                         programResource.parameters,
                         active->second->name)) {
                    if (name.empty()) continue;
                    candidates.push_back({
                        .source = source,
                        .resource = particleSamplerDefaultResource(
                            physicalPlan, descriptor, name
                        ),
                    });
                }
            }
            const auto authored = descriptor.textures.find(slot);
            if (authored != descriptor.textures.end()) {
                candidates.insert(
                    candidates.end(),
                    authored->second.candidates.begin(),
                    authored->second.candidates.end()
                );
            }
            const bool hasProviderContract =
                authored != descriptor.textures.end() ||
                !candidates.empty() || slot == 0;
            if (!hasProviderContract) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active particle sampler '" + samplerName +
                        "' requires texture slot " +
                        std::to_string(slot) +
                        ", but the frame pass provides no texture or metadata default"
                );
            }
            const std::optional<FrameResourceRef> selected =
                selectReadyTexture(
                    session,
                    candidates,
                    std::nullopt,
                    descriptor.texture0,
                    aliases,
                    inputs.timeSeconds
                );
            if (!selected) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle texture provider chain for slot " +
                        std::to_string(slot) +
                        " has no ready candidate or primary input"
                );
            }
            const FrameResourceRef& resource = *selected;
            if (resource.id.empty()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle texture slot has an empty resource identity"
                );
            }
            GLuint assetTextureValue = 0;
            if (resource.kind == FrameResourceKind::assetTexture) {
                const std::size_t imageIndex = slot == 0
                    ? texture0Animation.imageIndex : 0;
                if (slot == 0 && resource.id == descriptor.texture0.id) {
                    if (imageIndex >= textureResource.images.size()) {
                        throw Error(
                            ErrorCode::resourceValidation,
                            "Particle texture animation selected an unavailable image"
                        );
                    }
                    assetTextureValue = textureResource.images[imageIndex];
                } else {
                    assetTextureValue = texture(
                        session, resource, imageIndex, inputs.timeSeconds
                    );
                }
            } else if (resource.kind == FrameResourceKind::hostTexture) {
                assetTextureValue = texture(
                    session, resource, 0, inputs.timeSeconds
                );
            } else if (resource.kind == FrameResourceKind::framebuffer) {
                const auto& source = framebuffer(resource, aliases);
                const auto& target = framebuffer(destination, aliases);
                if (source.framebuffer == target.framebuffer && !prepared.refract) {
                    throw Error(
                        ErrorCode::resourceValidation,
                        "Particle texture slot " + std::to_string(slot) +
                            " resolves to the render destination without REFRACT snapshot support"
                    );
                }
            } else {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle texture slots must reference asset textures or framebuffers"
                );
            }
            PreparedTextureBinding binding{
                .slot = slot,
                .resource = resource,
                .assetTexture = assetTextureValue,
                .resolution = textureResolution(resource, aliases),
                .samplerLocation = prepareBuiltinUniform(
                    programResource,
                    "g_Texture" + std::to_string(slot),
                    GL_SAMPLER_2D
                ),
                .resolutionLocation = prepareBuiltinUniform(
                    programResource,
                    "g_Texture" + std::to_string(slot) + "Resolution",
                    GL_FLOAT_VEC4
                ),
            };
            prepared.textures.push_back(std::move(binding));
        }
        prepared.attributeLocations.fill(-1);
        prepared.uniforms.texture0Translation = prepareBuiltinUniform(
            programResource, "g_Texture0Translation", GL_FLOAT_VEC2
        );
        prepared.uniforms.texture0Rotation = prepareBuiltinUniform(
            programResource, "g_Texture0Rotation", GL_FLOAT_VEC4
        );
        prepared.uniforms.modelInverse = prepareBuiltinUniform(
            programResource, "g_ModelMatrixInverse", GL_FLOAT_MAT4
        );
        prepared.uniforms.orientationUp = prepareBuiltinUniform(
            programResource, "g_OrientationUp", GL_FLOAT_VEC3
        );
        prepared.uniforms.orientationRight = prepareBuiltinUniform(
            programResource, "g_OrientationRight", GL_FLOAT_VEC3
        );
        prepared.uniforms.orientationForward = prepareBuiltinUniform(
            programResource, "g_OrientationForward", GL_FLOAT_VEC3
        );
        prepared.uniforms.viewUp = prepareBuiltinUniform(
            programResource, "g_ViewUp", GL_FLOAT_VEC3
        );
        prepared.uniforms.viewRight = prepareBuiltinUniform(
            programResource, "g_ViewRight", GL_FLOAT_VEC3
        );
        prepared.uniforms.eyePosition = prepareBuiltinUniform(
            programResource, "g_EyePosition", GL_FLOAT_VEC3
        );
        prepared.uniforms.renderVar0 = prepareBuiltinUniform(
            programResource, "g_RenderVar0", GL_FLOAT_VEC4
        );
        prepared.uniforms.renderVar1 = prepareBuiltinUniform(
            programResource, "g_RenderVar1", GL_FLOAT_VEC4
        );
        if (prepared.refract) {
            prepared.uniforms.refractAmount = prepareBuiltinUniform(
                programResource, "g_RefractAmount", GL_FLOAT
            );
        }
        const auto& transform = descriptor.worldTransform;
        const float originX = particleFloat(
            transform.origin.x -
                static_cast<double>(logicalPlan.width) * 0.5,
            "object origin"
        );
        const float originY = particleFloat(
            centeredWallpaperY(transform.origin.y, logicalPlan.height),
            "object origin"
        );
        const float originZ = particleFloat(transform.origin.z, "object origin");
        Matrix model = multiply(
            translation(originX, originY, originZ),
            multiply(
                rotationZ(-particleFloat(transform.angles.z, "object angle")),
                multiply(
                    rotationY(particleFloat(transform.angles.y, "object angle")),
                    multiply(
                        rotationX(-particleFloat(transform.angles.x, "object angle")),
                        scaling(
                            particleFloat(transform.scale.x, "object scale"),
                            particleFloat(transform.scale.y, "object scale"),
                            particleFloat(transform.scale.z, "object scale")
                        )
                    )
                )
            )
        );
        if (logicalPlan.parallax.enabled) {
            double depthX = descriptor.parallaxDepth.x;
            double depthY = descriptor.parallaxDepth.y;
            constexpr double minimumParticleDepth = 0.65;
            if (std::abs(depthX) < minimumParticleDepth) {
                depthX = depthX < 0.0
                    ? -minimumParticleDepth : minimumParticleDepth;
            }
            if (std::abs(depthY) < minimumParticleDepth) {
                depthY = depthY < 0.0
                    ? -minimumParticleDepth : minimumParticleDepth;
            }
            const FrameVector2 parallaxOffset = sceneParallaxOffset(
                logicalPlan,
                {.x = depthX, .y = depthY},
                frameParallax,
                "particle"
            );
            // Linux inserts particle parallax after the object origin and
            // before rotation and scale. Since translations commute, left
            // multiplying the completed model preserves that world-space order.
            model = multiply(
                translation(
                    static_cast<float>(parallaxOffset.x),
                    static_cast<float>(parallaxOffset.y),
                    0.0F
                ),
                model
            );
        }
        Matrix viewProjection;
        std::array<float, 3> eyePosition = camera.eyePosition;
        if (descriptor.perspective) {
            if (!camera.particlePerspective) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Particle perspective camera was not prepared"
                );
            }
            viewProjection = camera.particlePerspective->viewProjection;
            eyePosition = camera.particlePerspective->eyePosition;
        } else {
            if (!camera.orthographicViewProjection) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Scene orthographic camera was not prepared"
                );
            }
            viewProjection = *camera.orthographicViewProjection;
        }
        prepared.model = model;
        prepared.viewProjection = viewProjection;
        prepared.modelViewProjection = multiply(viewProjection, model);
        validateMatrix(prepared.model, "Particle model matrix");
        validateMatrix(
            prepared.viewProjection,
            "Particle view-projection matrix"
        );
        validateMatrix(
            prepared.modelViewProjection,
            "Particle model-view-projection matrix"
        );
        if (prepared.uniforms.modelInverse >= 0) {
            prepared.modelInverse = inverse(
                prepared.model, "Particle model matrix"
            );
        }
        prepared.eyePosition = eyePosition;
        if (batch->atlas.enabled()) {
            prepared.renderVar1 = {
                1.0F / float(batch->atlas.columns),
                1.0F / float(batch->atlas.rows),
                float(batch->atlas.frames),
                batch->atlas.frameAspect,
            };
        } else {
            const float textureWidth = textureResource.resolution[2];
            const float textureHeight = textureResource.resolution[3];
            if (!std::isfinite(textureWidth) || !std::isfinite(textureHeight) ||
                textureWidth <= 0.0F || textureHeight <= 0.0F) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle texture has an invalid real resolution"
                );
            }
            // Linux exposes the non-atlas texture's real height/width ratio
            // through g_RenderVar1.w for genericparticle shaders.
            prepared.renderVar1 = {
                0.0F, 0.0F, 0.0F, textureHeight / textureWidth,
            };
        }
        if (descriptor.renderer.kind ==
            FrameParticleRendererKind::ropeTrail) {
            // genericropeparticle assigns a different contract to g_RenderVar0
            // than genericparticle: x/w are history segment capacities and z
            // is progress toward the next history sample.
            prepared.renderVar0 = {
                batch->ropeSegmentMaxCountUnscaled,
                0.0F,
                batch->ropeSegmentTimeOffset,
                batch->ropeSegmentMaxCount,
            };
        } else if (descriptor.renderer.kind ==
                   FrameParticleRendererKind::spriteTrail) {
            prepared.renderVar0 = {
                particleFloat(descriptor.renderer.length, "renderer length"),
                particleFloat(
                    descriptor.renderer.maxLength,
                    "renderer maximum length"
                ),
                particleFloat(
                    descriptor.renderer.minLength,
                    "renderer minimum length"
                ),
                0.0F,
            };
        } else {
            prepared.renderVar0 = {};
        }
        static_cast<void>(particleFloat(inputs.timeSeconds, "frame time"));
        const auto& overrides = descriptor.configuration.overrides;
        prepared.commonUniforms = prepareCommonUniforms(
            programResource,
            logicalPlan,
            {
                .width = physicalPlan.width,
                .height = physicalPlan.height,
            },
            inputs,
            prepared.model,
            prepared.viewProjection,
            prepared.modelViewProjection,
            {
                .brightness = 1.0F,
                .alpha = particleFloat(
                    overrides.alpha, "instance alpha override"
                ),
                .color = {
                    particleFloat(overrides.color.x, "instance color override"),
                    particleFloat(overrides.color.y, "instance color override"),
                    particleFloat(overrides.color.z, "instance color override"),
                    1.0F,
                },
            }
        );
        for (const auto& metadata : programResource.parameters) {
            if (!metadata.material || isSamplerParameter(metadata)) continue;
            const auto authored = descriptor.constants.find(metadata.name);
            if (authored != descriptor.constants.end()) {
                if (auto uniform = prepareRuntimeUniform(
                        programResource, metadata.name, authored->second.value)) {
                    prepared.materialUniforms.push_back(std::move(*uniform));
                }
            } else if (metadata.defaultValue) {
                if (auto uniform = prepareRuntimeUniform(
                        programResource,
                        metadata.name,
                        metadataDefault(*metadata.defaultValue))) {
                    prepared.materialUniforms.push_back(std::move(*uniform));
                }
            } else if (activeUniform(
                           programResource, metadata.name) != nullptr) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Active particle material uniform '" + metadata.name +
                        "' has no metadata default"
                );
            }
        }

        const std::size_t vertexCount = batch->rope
            ? batch->ropeVertices.size() : batch->vertices.size();
        const std::size_t vertexStride = batch->rope
            ? sizeof(RopeParticleVertex) : sizeof(ParticleVertex);
        if (vertexCount >
                static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max()) /
                    vertexStride ||
            batch->indices.size() >
                static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max()) /
                    sizeof(std::uint32_t) ||
            batch->indices.size() >
                static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle geometry exceeds OpenGL buffer or draw-count limits"
            );
        }

        ensureParticleGeometry(session);
        const auto prepareAttribute = [&](std::size_t index,
                                          const char* name) {
            prepared.attributeLocations[index] = glGetAttribLocation(
                activeProgram, name
            );
        };
        if (ropeRenderer) {
            prepareAttribute(0, "a_PositionVec4");
            prepareAttribute(1, "a_TexCoordVec4");
            prepareAttribute(2, "a_TexCoordVec4C1");
            prepareAttribute(3, "a_TexCoordVec4C2");
            prepareAttribute(4, "a_TexCoordVec4C3");
            prepareAttribute(5, "a_TexCoordC4");
            prepareAttribute(6, "a_Color");
        } else {
            prepareAttribute(0, "a_Position");
            prepareAttribute(1, "a_TexCoordVec4");
            prepareAttribute(2, "a_Color");
            prepareAttribute(3, "a_TexCoordVec4C1");
            prepareAttribute(4, "a_TexCoordC2");
        }
        session.checkError(
            ErrorCode::draw,
            "preparing particle rendering"
        );
        return {
            .operation = std::move(prepared),
            .batch = std::move(nextBatch),
            .state = std::move(nextState),
        };
    }

    void ensureParticleRefractSnapshot(
        Device::Session& session,
        const FramebufferResource& destination
    ) {
        if (particleRefractSnapshot.framebuffer != 0 &&
            particleRefractSnapshot.width == destination.width &&
            particleRefractSnapshot.height == destination.height &&
            particleRefractSnapshot.format == PixelFormat::rgba8) {
            return;
        }
        FramebufferResource candidate = session.createFramebuffer(
            PixelFormat::rgba8,
            destination.width,
            destination.height,
            TextureWrap::clampToEdge,
            false
        );
        session.destroyFramebuffer(particleRefractSnapshot);
        particleRefractSnapshot = std::move(candidate);
    }

    void snapshotParticleRefractInput(
        Device::Session& session,
        const FramebufferResource& destination
    ) {
        ensureParticleRefractSnapshot(session, destination);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, destination.framebuffer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, particleRefractSnapshot.framebuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(
            0, 0,
            static_cast<GLint>(destination.width),
            static_cast<GLint>(destination.height),
            0, 0,
            static_cast<GLint>(particleRefractSnapshot.width),
            static_cast<GLint>(particleRefractSnapshot.height),
            GL_COLOR_BUFFER_BIT,
            GL_NEAREST
        );
        session.checkError(
            ErrorCode::draw,
            "snapshotting the particle REFRACT input framebuffer"
        );
    }

    void drawParticle(
        Device::Session& session,
        const PreparedParticle& prepared,
        const ParticleDrawBatch& batch
    ) {
        if ((!batch.rope && batch.vertices.empty()) ||
            (batch.rope && batch.ropeVertices.empty())) {
            glDisable(GL_DEPTH_CLAMP);
            return;
        }
        auto& destination = framebuffer(prepared.destination);
        bool needsRefractSnapshot = false;
        if (prepared.refract) {
            for (const PreparedTextureBinding& binding : prepared.textures) {
                if (binding.resource.kind != FrameResourceKind::framebuffer) {
                    continue;
                }
                if (framebuffer(binding.resource).framebuffer == destination.framebuffer) {
                    needsRefractSnapshot = true;
                    break;
                }
            }
        }
        if (needsRefractSnapshot) {
            snapshotParticleRefractInput(session, destination);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
        glViewport(
            0, 0,
            static_cast<GLsizei>(destination.width),
            static_cast<GLsizei>(destination.height)
        );
        configureState({
            .blending = prepared.blending,
            .culling = prepared.culling,
            .depthTest = prepared.depthTest,
            .depthWrite = prepared.depthWrite,
            .writeAlpha = true,
        });
        glEnable(GL_DEPTH_CLAMP);
        glUseProgram(prepared.program);
        for (const PreparedTextureBinding& binding : prepared.textures) {
            glActiveTexture(
                static_cast<GLenum>(GL_TEXTURE0 + binding.slot)
            );
            GLuint textureValue = binding.assetTexture;
            if (textureValue == 0 &&
                binding.resource.kind == FrameResourceKind::framebuffer) {
                const auto& source = framebuffer(binding.resource);
                textureValue = source.framebuffer == destination.framebuffer &&
                        needsRefractSnapshot
                    ? particleRefractSnapshot.colorTexture
                    : source.colorTexture;
            }
            if (textureValue == 0) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle texture binding resolved to OpenGL texture zero"
                );
            }
            glBindTexture(GL_TEXTURE_2D, textureValue);
            if (binding.samplerLocation >= 0) {
                glUniform1i(binding.samplerLocation, binding.slot);
            }
            bindVector4(binding.resolutionLocation, binding.resolution);
        }
        for (const PreparedUniform& uniform : prepared.materialUniforms) {
            bindPreparedUniform(uniform);
        }
        bindVector2(
            prepared.uniforms.texture0Translation,
            prepared.texture0Animation.translation
        );
        bindVector4(
            prepared.uniforms.texture0Rotation,
            prepared.texture0Animation.rotation
        );
        if (prepared.uniforms.refractAmount >= 0) {
            glUniform1f(prepared.uniforms.refractAmount, 0.05F);
        }
        bindCommonUniforms(
            prepared.commonUniforms,
            prepared.model,
            prepared.viewProjection,
            prepared.modelViewProjection
        );
        bindMatrix(prepared.uniforms.modelInverse, prepared.modelInverse);
        bindVector3(
            prepared.uniforms.orientationUp, {0.0F, 1.0F, 0.0F}
        );
        bindVector3(
            prepared.uniforms.orientationRight, {1.0F, 0.0F, 0.0F}
        );
        bindVector3(
            prepared.uniforms.orientationForward, {0.0F, 0.0F, 1.0F}
        );
        bindVector3(prepared.uniforms.viewUp, {0.0F, 1.0F, 0.0F});
        bindVector3(prepared.uniforms.viewRight, {1.0F, 0.0F, 0.0F});
        bindVector3(prepared.uniforms.eyePosition, prepared.eyePosition);
        bindVector4(
            prepared.uniforms.renderVar0, prepared.renderVar0
        );
        bindVector4(prepared.uniforms.renderVar1, prepared.renderVar1);
        glBindVertexArray(particleVertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, particleVertexBuffer);
        disableVertexAttributes();
        if (batch.rope) {
            glBufferData(
                GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(
                    batch.ropeVertices.size() * sizeof(RopeParticleVertex)
                ),
                batch.ropeVertices.data(),
                GL_DYNAMIC_DRAW
            );
        } else {
            glBufferData(
                GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(
                    batch.vertices.size() * sizeof(ParticleVertex)
                ),
                batch.vertices.data(),
                GL_DYNAMIC_DRAW
            );
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, particleElementBuffer);
        glBufferData(
            GL_ELEMENT_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(batch.indices.size() * sizeof(std::uint32_t)),
            batch.indices.data(),
            GL_DYNAMIC_DRAW
        );

        const auto bindAttribute = [&](GLint location, GLint components,
                                       std::size_t offset) {
            if (location < 0) return;
            glEnableVertexAttribArray(static_cast<GLuint>(location));
            glVertexAttribPointer(
                static_cast<GLuint>(location),
                components,
                GL_FLOAT,
                GL_FALSE,
                static_cast<GLsizei>(sizeof(ParticleVertex)),
                reinterpret_cast<const void*>(offset)
            );
        };
        if (batch.rope) {
            const GLsizei stride = static_cast<GLsizei>(sizeof(RopeParticleVertex));
            const auto bindRopeAttribute = [&](GLint location, GLint components,
                                               std::size_t offset) {
                if (location < 0) return;
                glEnableVertexAttribArray(static_cast<GLuint>(location));
                glVertexAttribPointer(
                    static_cast<GLuint>(location), components, GL_FLOAT,
                    GL_FALSE, stride, reinterpret_cast<const void*>(offset)
                );
            };
            bindRopeAttribute(
                prepared.attributeLocations[0], 4,
                offsetof(RopeParticleVertex, positionVec4)
            );
            bindRopeAttribute(
                prepared.attributeLocations[1], 4,
                offsetof(RopeParticleVertex, texCoordVec4)
            );
            bindRopeAttribute(
                prepared.attributeLocations[2], 4,
                offsetof(RopeParticleVertex, texCoordVec4C1)
            );
            bindRopeAttribute(
                prepared.attributeLocations[3], 4,
                offsetof(RopeParticleVertex, texCoordVec4C2)
            );
            bindRopeAttribute(
                prepared.attributeLocations[4], 4,
                offsetof(RopeParticleVertex, texCoordVec4C3)
            );
            bindRopeAttribute(
                prepared.attributeLocations[5], 2,
                offsetof(RopeParticleVertex, texCoordC4)
            );
            bindRopeAttribute(
                prepared.attributeLocations[6], 4,
                offsetof(RopeParticleVertex, color)
            );
        } else {
            const GLsizei stride = static_cast<GLsizei>(sizeof(ParticleVertex));
            bindAttribute(
                prepared.attributeLocations[0], 3,
                offsetof(ParticleVertex, position)
            );
            bindAttribute(
                prepared.attributeLocations[1], 4,
                offsetof(ParticleVertex, texCoordRotationSize)
            );
            bindAttribute(
                prepared.attributeLocations[2], 4,
                offsetof(ParticleVertex, color)
            );
            bindAttribute(
                prepared.attributeLocations[3], 4,
                offsetof(ParticleVertex, velocityLifetime)
            );
            bindAttribute(
                prepared.attributeLocations[4], 2,
                offsetof(ParticleVertex, rotationXY)
            );
        }
        glDrawElements(
            GL_TRIANGLES,
            static_cast<GLsizei>(batch.indices.size()),
            GL_UNSIGNED_INT,
            nullptr
        );
        glDisable(GL_DEPTH_CLAMP);
        session.checkError(ErrorCode::draw, "drawing particle sprites");
    }

    static void normalizeGLState() {
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_DEPTH_CLAMP);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_SAMPLE_COVERAGE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DITHER);
        glDisable(GL_MULTISAMPLE);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    static void disableVertexAttributes() {
        GLint count = 0;
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &count);
        for (GLint index = 0; index < count; ++index) {
            glDisableVertexAttribArray(static_cast<GLuint>(index));
        }
    }

    [[nodiscard]] ObjectOperationGroup operationGroup(
        const FramePlan& plan,
        const FrameOperation& operation,
        std::size_t operationIndex
    ) const {
        const auto imageIdentity = [&](const FramePassOrigin& origin) {
            if (origin.imageIndex >= plan.images.size()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " references an invalid image descriptor index"
                );
            }
            const FrameImageDescriptor& descriptor =
                plan.images[origin.imageIndex];
            if (descriptor.objectId != origin.objectId) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " has an inconsistent image object identity"
                );
            }
            return ObjectOperationGroup{
                .objectIndex = descriptor.objectIndex,
                .objectId = descriptor.objectId,
            };
        };

        if (const auto* pass = std::get_if<FrameRenderPass>(&operation)) {
            return imageIdentity(pass->origin);
        }
        if (const auto* command = std::get_if<FrameCopyCommand>(&operation)) {
            return imageIdentity(command->origin);
        }
        if (const auto* command = std::get_if<FrameSwapCommand>(&operation)) {
            return imageIdentity(command->origin);
        }
        if (const auto* command = std::get_if<FrameClearCommand>(&operation)) {
            return imageIdentity(command->origin);
        }
        if (const auto* command = std::get_if<FrameTextCommand>(&operation)) {
            if (command->textIndex >= plan.texts.size()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " references an invalid text descriptor index"
                );
            }
            const FrameTextDescriptor& descriptor =
                plan.texts[command->textIndex];
            if (descriptor.objectId != command->objectId) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " has an inconsistent text object identity"
                );
            }
            return {
                .objectIndex = descriptor.objectIndex,
                .objectId = descriptor.objectId,
            };
        }
        if (const auto* command =
                std::get_if<FrameParticleCommand>(&operation)) {
            if (command->particleIndex >= plan.particles.size()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " references an invalid particle descriptor index"
                );
            }
            const FrameParticleDescriptor& descriptor =
                plan.particles[command->particleIndex];
            if (descriptor.objectId != command->objectId) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame operation " + std::to_string(operationIndex) +
                        " has an inconsistent particle object identity"
                );
            }
            if (command->destination != plan.output) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame particle operation destination is inconsistent with the plan output"
                );
            }
            return {
                .objectIndex = descriptor.objectIndex,
                .objectId = descriptor.objectId,
            };
        }
        throw Error(
            ErrorCode::internalFailure,
            "Frame plan contains an unknown operation variant"
        );
    }

    [[nodiscard]] std::vector<ObjectOperationGroup> objectOperationGroups(
        const FramePlan& plan
    ) const {
        if (!plan.isExecutable()) {
            throw Error(ErrorCode::resourceValidation, planIssues(plan));
        }
        std::vector<ObjectOperationGroup> groups;
        std::set<int> closedObjectIds;
        std::set<int> scheduledParticleIds;
        for (std::size_t operationIndex = 0;
             operationIndex < plan.operations.size(); ++operationIndex) {
            ObjectOperationGroup identity = operationGroup(
                plan, plan.operations[operationIndex], operationIndex
            );
            if (groups.empty() ||
                groups.back().objectId != identity.objectId) {
                if (!groups.empty()) {
                    closedObjectIds.emplace(groups.back().objectId);
                }
                if (closedObjectIds.contains(identity.objectId)) {
                    throw Error(
                        ErrorCode::resourceValidation,
                        "Frame plan operations for runtime object id " +
                            std::to_string(identity.objectId) +
                            " are not contiguous"
                    );
                }
                groups.push_back(std::move(identity));
            } else if (groups.back().objectIndex != identity.objectIndex) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame plan reuses a runtime object id with inconsistent source templates"
                );
            }
            groups.back().operationIndexes.push_back(operationIndex);

            if (std::holds_alternative<FrameParticleCommand>(
                    plan.operations[operationIndex]) &&
                !scheduledParticleIds.emplace(groups.back().objectId).second) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Frame plan schedules a particle object more than once"
                );
            }
        }
        return groups;
    }

    [[nodiscard]] AssetTextureResource& requirementTexture(
        Device::Session& session,
        const FrameResourceRef& resource,
        std::string_view description
    ) {
        if (resource.kind != FrameResourceKind::assetTexture ||
            resource.id.empty()) {
            throw Error(
                ErrorCode::resourceValidation,
                std::string(description) +
                    " requires a concrete asset texture"
            );
        }
        // Resource-requirement discovery shares the executor's immutable GPU
        // asset cache with ordinary pass preparation. Parsing here directly
        // would decode the same texture once per frame before preflight.
        return ensureAssetTexture(session, resource);
    }

    template <typename Resolve>
    void collectSamplerDefaultFramebufferRequirements(
        const ProgramResource& programResource,
        Resolve&& resolve,
        std::vector<FrameResourceRef>& requirements,
        std::string_view description
    ) const {
        for (const ActiveUniform& uniform : programResource.uniforms) {
            if (!isOpenGLSamplerType(uniform.type)) continue;
            if (uniform.type != GL_SAMPLER_2D) {
                throw Error(
                    ErrorCode::resourceValidation,
                    std::string(description) + " sampler '" + uniform.name +
                        " uses an unsupported non-2D texture type"
                );
            }
            if (!textureSlot(uniform.name)) {
                throw Error(
                    ErrorCode::resourceValidation,
                    std::string(description) + " sampler '" + uniform.name +
                        " does not follow the g_TextureN binding contract"
                );
            }
            for (const auto& [source, name] : samplerDefaultTextures(
                     programResource.parameters, uniform.name
                 )) {
                static_cast<void>(source);
                if (name.empty()) continue;
                FrameResourceRef resource = resolve(name);
                if (resource.kind == FrameResourceKind::framebuffer) {
                    requirements.push_back(std::move(resource));
                }
            }
        }
    }

    void collectRenderDefaultFramebufferRequirements(
        Device::Session& session,
        const FramePlan& plan,
        const FrameRenderPass& pass,
        std::vector<FrameResourceRef>& requirements
    ) {
        if (pass.origin.imageIndex >= plan.images.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Render pass image index is invalid"
            );
        }
        const FrameImageDescriptor& image = plan.images[pass.origin.imageIndex];
        ComboMap combos = pass.combos;
        if (image.source.kind == FrameResourceKind::assetTexture) {
            applyTexture0FormatCombo(
                combos,
                requirementTexture(
                    session,
                    image.source,
                    "Frame render pass input"
                ).format
            );
        }
        ProgramResource& programResource = program(
            session,
            pass.vertexShaderPath,
            pass.fragmentShaderPath,
            combos,
            "Frame render pass"
        );
        collectSamplerDefaultFramebufferRequirements(
            programResource,
            [&plan, &pass, this](std::string_view name) {
                return samplerDefaultResource(plan, pass, name);
            },
            requirements,
            "Frame render pass"
        );
    }

    void collectParticleDefaultFramebufferRequirements(
        Device::Session& session,
        const FramePlan& plan,
        const FrameParticleCommand& command,
        std::vector<FrameResourceRef>& requirements
    ) {
        if (command.particleIndex >= plan.particles.size()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Frame particle command index is invalid"
            );
        }
        const FrameParticleDescriptor& descriptor =
            plan.particles[command.particleIndex];
        if (descriptor.textures.empty() ||
            descriptor.texture0.kind != FrameResourceKind::assetTexture ||
            descriptor.texture0.id.empty()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle texture slot zero must reference a real asset texture"
            );
        }
        const AssetTextureResource& texture = requirementTexture(
            session,
            descriptor.texture0,
            "Particle texture slot zero"
        );
        if (texture.images.empty()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Particle rendering requires at least one texture image"
            );
        }

        ComboMap combos = descriptor.combos;
        applyTexture0FormatCombo(combos, texture.format);
        const auto requireCombo = [&combos](const char* name, int expected) {
            const auto authored = combos.find(name);
            if (authored != combos.end() && authored->second != expected) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Particle shader combination '" + std::string(name) +
                        " must be " + std::to_string(expected)
                );
            }
            combos[name] = expected;
        };
        requireCombo("GS_ENABLED", 0);
        requireCombo("THICKFORMAT", 1);
        requireCombo(
            "TRAILRENDERER",
            descriptor.renderer.kind == FrameParticleRendererKind::spriteTrail ||
                    descriptor.renderer.kind == FrameParticleRendererKind::ropeTrail
                ? 1 : 0
        );
        const bool hasAtlas = texture.spritesheetColumns != 0 ||
            texture.spritesheetRows != 0 || texture.spritesheetFrameCount != 0;
        combos["SPRITESHEET"] = hasAtlas ? 1 : 0;
        ProgramResource& programResource = program(
            session,
            descriptor.vertexShaderPath,
            descriptor.fragmentShaderPath,
            combos,
            "Particle render pass"
        );
        collectSamplerDefaultFramebufferRequirements(
            programResource,
            [&plan, &descriptor, this](std::string_view name) {
                return particleSamplerDefaultResource(plan, descriptor, name);
            },
            requirements,
            "Particle render pass"
        );
    }

    [[nodiscard]] FramebufferPlanRequirements framebufferRequirements(
        Device::Session& session,
        const FramePlan& plan,
        const std::vector<ObjectOperationGroup>& groups
    ) {
        FramebufferPlanRequirements requirements =
            analyzeFramebufferPlanRequirements(plan);
        for (const ObjectOperationGroup& group : groups) {
            std::vector<FrameResourceRef> candidate;
            try {
                for (const std::size_t operationIndex : group.operationIndexes) {
                    const FrameOperation& operation =
                        plan.operations[operationIndex];
                    if (const auto* pass =
                            std::get_if<FrameRenderPass>(&operation)) {
                        collectRenderDefaultFramebufferRequirements(
                            session, plan, *pass, candidate
                        );
                    } else if (const auto* particle =
                                   std::get_if<FrameParticleCommand>(&operation)) {
                        collectParticleDefaultFramebufferRequirements(
                            session, plan, *particle, candidate
                        );
                    }
                }
                // Resolve every indirect reference before committing any of
                // this object group's requirements. If shader preparation
                // skips the object, no unreachable framebuffer is retained.
                for (const FrameResourceRef& resource : candidate) {
                    if (resource.kind != FrameResourceKind::framebuffer ||
                        resource.id.empty() ||
                        !requirements.descriptors.contains(resource.id)) {
                        throw Error(
                            ErrorCode::resourceValidation,
                            "Shader metadata references a framebuffer outside the frame plan '" +
                                resource.id + "'"
                        );
                    }
                }
                for (const FrameResourceRef& resource : candidate) {
                    requirements.requireFramebuffer(resource);
                }
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const Error& error) {
                if (isFrameFatalPreparationError(error)) throw;
                validateIsolatedPreparationState();
            } catch (const ShaderCompileError&) {
                validateIsolatedPreparationState();
            } catch (const FormatError&) {
                validateIsolatedPreparationState();
            } catch (const std::exception& error) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Unexpected framebuffer requirement preparation failure: " +
                        std::string(error.what())
                );
            }
        }
        return requirements;
    }

    [[nodiscard]] PreparedCamera prepareCamera(const FramePlan& plan) const {
        bool needsOrthographic = false;
        bool needsParticlePerspective = false;
        bool needsOrthographicEye = false;
        for (const FrameOperation& operation : plan.operations) {
            if (const auto* pass = std::get_if<FrameRenderPass>(&operation)) {
                needsOrthographic = needsOrthographic ||
                    pass->geometry == FrameGeometryKind::imageScene ||
                    pass->geometry == FrameGeometryKind::passthroughCapture ||
                    pass->geometry == FrameGeometryKind::puppetMesh;
            } else if (std::holds_alternative<FrameTextCommand>(operation)) {
                needsOrthographic = true;
            } else if (const auto* command =
                           std::get_if<FrameParticleCommand>(&operation)) {
                const FrameParticleDescriptor& descriptor =
                    plan.particles.at(command->particleIndex);
                if (descriptor.perspective) {
                    needsParticlePerspective = true;
                } else {
                    needsOrthographic = true;
                    needsOrthographicEye = true;
                }
            }
        }

        // FramePlan's camera descriptor captures the logical projection used
        // for script, particle, pointer, and composition evaluation. Physical
        // FBO scaling must not feed back into its world-space matrices.
        const float cameraWidth = static_cast<float>(
            plan.camera.orthogonalProjectionWidth
        );
        const float cameraHeight = static_cast<float>(
            plan.camera.orthogonalProjectionHeight
        );
        PreparedCamera result;
        if (needsOrthographic) {
            result.orthographicViewProjection.emplace(
                sceneOrthographicViewProjection(
                    plan.camera, cameraWidth, cameraHeight
                )
            );
            validateMatrix(
                *result.orthographicViewProjection,
                "Scene orthographic view-projection matrix"
            );
        }
        if (needsParticlePerspective) {
            result.particlePerspective.emplace(
                particlePerspectiveView(plan.camera, cameraWidth, cameraHeight)
            );
            validateMatrix(
                result.particlePerspective->viewProjection,
                "Particle perspective view-projection matrix"
            );
        }
        if (needsOrthographicEye) {
            result.eyePosition = {
                checkedFloat(plan.camera.eye.x, "Camera eye"),
                checkedFloat(plan.camera.eye.y, "Camera eye"),
                checkedFloat(plan.camera.eye.z, "Camera eye"),
            };
        }
        return result;
    }

    void prepareFrameArena(
        Device::Session& session,
        const FramePlan& plan,
        const std::vector<ObjectOperationGroup>& groups
    ) {
        normalizeGLState();
        session.checkError(
            ErrorCode::unsupportedContext,
            "normalizing the OpenGL context before frame preparation"
        );
        const FramebufferPlanRequirements requirements =
            framebufferRequirements(session, plan, groups);
        ensureFramebuffers(session, plan, requirements);
        static_cast<void>(framebuffer(plan.output));

        bool needsGeometry = false;
        bool needsPuppetGeometry = false;
        bool needsParticleGeometry = false;
        for (const FrameOperation& operation : plan.operations) {
            if (const auto* render = std::get_if<FrameRenderPass>(&operation)) {
                needsGeometry = true;
                needsPuppetGeometry = needsPuppetGeometry ||
                    render->geometry == FrameGeometryKind::puppetMesh;
            } else {
                needsGeometry = needsGeometry ||
                    std::holds_alternative<FrameCopyCommand>(operation);
            }
            needsParticleGeometry = needsParticleGeometry ||
                std::holds_alternative<FrameParticleCommand>(operation);
        }
        if (needsGeometry) ensureGeometry(session);
        if (needsPuppetGeometry) ensurePuppetGeometry(session);
        if (needsParticleGeometry) ensureParticleGeometry(session);
    }

    void beginFrameOutput(Device::Session& session, const FramePlan& plan) {
        std::array<GLfloat, 4> authoredClear{};
        if (plan.clearEnabled) {
            authoredClear = {
                checkedFloat(plan.clearColor.red, "Frame clear color"),
                checkedFloat(plan.clearColor.green, "Frame clear color"),
                checkedFloat(plan.clearColor.blue, "Frame clear color"),
                checkedFloat(plan.clearColor.alpha, "Frame clear color"),
            };
        }
        normalizeGLState();
        session.checkError(
            ErrorCode::unsupportedContext,
            "normalizing the OpenGL context before frame execution"
        );
        auto& output = framebuffer(plan.output);
        glBindFramebuffer(GL_FRAMEBUFFER, output.framebuffer);
        glViewport(0, 0, output.width, output.height);
        glClearColor(
            authoredClear[0], authoredClear[1],
            authoredClear[2], authoredClear[3]
        );
        glClearDepth(1.0);
        glClear(
            GL_COLOR_BUFFER_BIT |
            (output.depthRenderbuffer != 0 ? GL_DEPTH_BUFFER_BIT : 0)
        );
        session.checkError(ErrorCode::draw, "clearing the frame output");
    }

    [[nodiscard]] static bool isFrameFatalPreparationError(
        const Error& error
    ) noexcept {
        switch (error.code()) {
            case ErrorCode::contextCreation:
            case ErrorCode::unsupportedContext:
            case ErrorCode::framebufferCreation:
            case ErrorCode::readback:
            case ErrorCode::internalFailure:
                return true;
            case ErrorCode::invalidArgument:
            case ErrorCode::shaderCompilation:
            case ErrorCode::programLink:
            case ErrorCode::draw:
            case ErrorCode::textureDecode:
            case ErrorCode::textureUpload:
            case ErrorCode::resourceValidation:
                return false;
        }
        return true;
    }

    static void validateIsolatedPreparationState() {
        normalizeGLState();
        const GLenum residualError = glGetError();
        if (residualError != GL_NO_ERROR) {
            throw Error(
                ErrorCode::internalFailure,
                "Object preparation left residual OpenGL error " +
                    std::to_string(residualError)
            );
        }
    }

    [[nodiscard]] PreparedFrame preflightFrameObjects(
        Device::Session& session,
        const FramePlan& logicalPlan,
        const FramePlan& physicalPlan,
        const std::vector<ObjectOperationGroup>& groups,
        const ResolvedFrameInputs& inputs,
        const FrameVector2& frameParallax,
        const PreparedCamera& camera,
        const std::map<std::size_t, ParticleDrawBatch>* frozenParticleBatches =
            nullptr,
        const std::vector<FrameExecutionIssue>* frozenIssues = nullptr
    ) {
        PreparedFrame result;
        result.operations.resize(physicalPlan.operations.size());
        if (physicalPlan.operations.size() <=
            static_cast<std::size_t>(
                std::numeric_limits<GLint>::max()) / 6U) {
            result.imageVertices.reserve(physicalPlan.operations.size() * 6U);
        }
        result.frozenParticleBatches = frozenParticleBatches;
        if (frozenParticleBatches == nullptr) {
            result.particleStates = particles;
            initializeParticleStates(logicalPlan, result.particleStates);
        }
        std::map<std::string, std::string> aliases = framebufferAliases;

        for (const ObjectOperationGroup& group : groups) {
            if (frozenIssues != nullptr) {
                const auto frozenIssue = std::find_if(
                    frozenIssues->begin(), frozenIssues->end(),
                    [&](const FrameExecutionIssue& issue) {
                        return issue.severity ==
                                FramePlanIssueSeverity::skipObject &&
                            issue.objectId == group.objectId;
                    }
                );
                if (frozenIssue != frozenIssues->end()) {
                    result.issues.push_back(*frozenIssue);
                    continue;
                }
            }
            std::map<std::string, std::string> candidateAliases = aliases;
            std::vector<std::pair<std::size_t, PreparedOperation>>
                candidateOperations;
            candidateOperations.reserve(group.operationIndexes.size());
            std::optional<std::pair<std::size_t, ParticleDrawBatch>>
                candidateParticleBatch;
            std::optional<ParticleState> candidateParticleState;
            std::size_t failingOperation = group.operationIndexes.front();
            const auto recordSkippedObject = [&](std::string message) {
                validateIsolatedPreparationState();
                result.issues.push_back({
                    .severity = FramePlanIssueSeverity::skipObject,
                    .objectIndex = group.objectIndex,
                    .objectId = group.objectId,
                    .operationIndex = failingOperation,
                    .message = std::move(message),
                });
            };

            try {
                for (const std::size_t operationIndex :
                     group.operationIndexes) {
                    failingOperation = operationIndex;
                    const FrameOperation& operation =
                        physicalPlan.operations[operationIndex];
                    if (const auto* pass =
                            std::get_if<FrameRenderPass>(&operation)) {
                        candidateOperations.emplace_back(
                            operationIndex,
                            prepareDraw(
                                session, logicalPlan, physicalPlan, *pass,
                                inputs, frameParallax, camera, candidateAliases
                            )
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameCopyCommand>(&operation)) {
                        candidateOperations.emplace_back(
                            operationIndex,
                            prepareCopy(
                                session, logicalPlan, physicalPlan, *command, inputs,
                                frameParallax, camera, candidateAliases
                            )
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameSwapCommand>(&operation)) {
                        prepareSwap(*command, candidateAliases);
                        candidateOperations.emplace_back(
                            operationIndex, *command
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameClearCommand>(&operation)) {
                        candidateOperations.emplace_back(
                            operationIndex,
                            prepareClear(*command, candidateAliases)
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameTextCommand>(&operation)) {
                        candidateOperations.emplace_back(
                            operationIndex,
                            prepareText(
                                session, logicalPlan, *command, camera,
                                candidateAliases
                            )
                        );
                    } else if (const auto* command =
                                   std::get_if<FrameParticleCommand>(&operation)) {
                        const ParticleState* previousState = nullptr;
                        if (candidateParticleState) {
                            previousState = &*candidateParticleState;
                        } else if (frozenParticleBatches == nullptr) {
                            const auto previous = result.particleStates.find(
                                group.objectId
                            );
                            if (previous != result.particleStates.end()) {
                                previousState = &previous->second;
                            }
                        }
                        const ParticleDrawBatch* frozenBatch = nullptr;
                        if (frozenParticleBatches != nullptr) {
                            const auto found = frozenParticleBatches->find(
                                operationIndex
                            );
                            if (found == frozenParticleBatches->end()) {
                                throw Error(
                                    ErrorCode::internalFailure,
                                    "Replay particle operation has no frozen simulation batch"
                                );
                            }
                            frozenBatch = &found->second;
                        }
                        ParticlePreparation prepared = prepareParticle(
                            session, logicalPlan, physicalPlan, *command,
                            operationIndex, inputs, frameParallax, camera,
                            candidateAliases,
                            previousState, frozenBatch
                        );
                        candidateOperations.emplace_back(
                            operationIndex, std::move(prepared.operation)
                        );
                        if (prepared.batch) {
                            candidateParticleBatch.emplace(
                                operationIndex, std::move(*prepared.batch)
                            );
                        }
                        if (prepared.state) {
                            candidateParticleState.emplace(
                                std::move(*prepared.state)
                            );
                        }
                    } else {
                        throw Error(
                            ErrorCode::internalFailure,
                            "Frame plan contains an unknown operation variant"
                        );
                    }
                }
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const Error& error) {
                if (isFrameFatalPreparationError(error)) throw;
                recordSkippedObject(error.what());
                continue;
            } catch (const ShaderCompileError& error) {
                recordSkippedObject(error.what());
                continue;
            } catch (const FormatError& error) {
                recordSkippedObject(error.what());
                continue;
            } catch (const text::Error& error) {
                recordSkippedObject(error.what());
                continue;
            } catch (const std::exception& error) {
                throw Error(
                    ErrorCode::internalFailure,
                    "Unexpected object preparation failure: " +
                        std::string(error.what())
                );
            }

            aliases.swap(candidateAliases);
            for (auto& [operationIndex, prepared] : candidateOperations) {
                result.operations[operationIndex].emplace(
                    std::move(prepared)
                );
            }
            if (candidateParticleBatch) {
                const auto [inserted, didInsert] =
                    result.particleBatches.emplace(
                        candidateParticleBatch->first,
                        std::move(candidateParticleBatch->second)
                    );
                static_cast<void>(inserted);
                if (!didInsert) {
                    throw Error(
                        ErrorCode::internalFailure,
                        "Prepared particle batch operation index is duplicated"
                    );
                }
            }
            if (candidateParticleState) {
                result.particleStates.insert_or_assign(
                    group.objectId,
                    std::move(*candidateParticleState)
                );
            }
        }
        for (auto& operation : result.operations) {
            if (!operation) continue;
            auto* drawOperation = std::get_if<PreparedDraw>(&*operation);
            if (drawOperation == nullptr ||
                drawOperation->pass.geometry ==
                    FrameGeometryKind::puppetMesh) {
                continue;
            }
            if (result.imageVertices.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<GLint>::max()) -
                    drawOperation->vertices.size()) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Prepared image geometry exceeds OpenGL vertex-index range"
                );
            }
            drawOperation->firstVertex = static_cast<GLint>(
                result.imageVertices.size()
            );
            result.imageVertices.insert(
                result.imageVertices.end(),
                drawOperation->vertices.begin(),
                drawOperation->vertices.end()
            );
        }
        trimTextRasterCache();
        result.finalAliases = std::move(aliases);
        return result;
    }

    void executePreparedOperations(
        Device::Session& session,
        const PreparedFrame& prepared
    ) {
        const auto& particleBatches = prepared.frozenParticleBatches != nullptr
            ? *prepared.frozenParticleBatches
            : prepared.particleBatches;
        if (!prepared.imageVertices.empty()) {
            if (prepared.imageVertices.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<GLsizeiptr>::max()) /
                    sizeof(Vertex)) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Prepared image geometry exceeds OpenGL buffer range"
                );
            }
            ensureGeometry(session);
            glBindVertexArray(vertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
            glBufferData(
                GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(
                    prepared.imageVertices.size() * sizeof(Vertex)
                ),
                prepared.imageVertices.data(),
                GL_STREAM_DRAW
            );
            session.checkError(
                ErrorCode::draw,
                "uploading prepared frame image geometry"
            );
        }
        for (const auto& operation : prepared.operations) {
            if (!operation) continue;
            if (const auto* drawOperation =
                    std::get_if<PreparedDraw>(&*operation)) {
                draw(session, *drawOperation);
            } else if (const auto* swapOperation =
                           std::get_if<FrameSwapCommand>(&*operation)) {
                swap(*swapOperation);
            } else if (const auto* clearOperation =
                           std::get_if<PreparedClear>(&*operation)) {
                clear(session, *clearOperation);
            } else if (const auto* textOperation =
                           std::get_if<PreparedText>(&*operation)) {
                drawText(session, *textOperation);
            } else if (const auto* particleOperation =
                           std::get_if<PreparedParticle>(&*operation)) {
                const auto batch = particleBatches.find(
                    particleOperation->operationIndex
                );
                if (batch == particleBatches.end()) {
                    throw Error(
                        ErrorCode::internalFailure,
                        "Prepared particle operation has no simulation batch"
                    );
                }
                drawParticle(session, *particleOperation, batch->second);
            } else {
                throw Error(
                    ErrorCode::internalFailure,
                    "Prepared frame contains an unknown operation variant"
                );
            }
        }
        if (framebufferAliases != prepared.finalAliases) {
            throw Error(
                ErrorCode::internalFailure,
                "Prepared framebuffer alias simulation diverged during execution"
            );
        }
    }

    void clearCurrentOutput() {
        if (!device || outputId.empty()) return;
        auto session = device->activate();
        if (framebufferAliases.contains(outputId)) {
            normalizeGLState();
            auto& output = framebuffer({
                .kind = FrameResourceKind::framebuffer,
                .id = outputId,
            });
            glBindFramebuffer(GL_FRAMEBUFFER, output.framebuffer);
            glViewport(0, 0, output.width, output.height);
            glClearColor(0, 0, 0, 0);
            glClearDepth(1.0);
            glClear(
                GL_COLOR_BUFFER_BIT |
                (output.depthRenderbuffer != 0 ? GL_DEPTH_BUFFER_BIT : 0)
            );
            session.checkError(
                ErrorCode::draw,
                "clearing the previous frame after failure"
            );
        }
    }

    [[nodiscard]] FrameVector2 nextParallax(
        const FrameVector2& previous,
        const FramePlan& plan,
        const ResolvedFrameInputs& inputs
    ) const {
        // Target and delayed interpolation formulas are adapted from
        // Almamu/linux-wallpaperengine Render/Wallpapers/CScene.cpp at
        // b016d7d1fdcf4e5fd2f9c9fa420a8aaa07fee02d (GPL-3.0).
        if (!plan.parallax.enabled) {
            return {};
        }
        const FrameVector2 target = {
            (inputs.effectPointerPosition.x - 0.5) * plan.parallax.amount *
                plan.parallax.mouseInfluence,
            (inputs.effectPointerPosition.y - 0.5) * plan.parallax.amount *
                plan.parallax.mouseInfluence,
        };
        if (!std::isfinite(target.x) || !std::isfinite(target.y)) {
            throw Error(
                ErrorCode::resourceValidation,
                "Camera parallax target is non-finite"
            );
        }
        const double interpolation = std::clamp(
            plan.parallax.delay * inputs.frameTimeSeconds, 0.0, 1.0
        );
        FrameVector2 result = previous;
        result.x += (target.x - result.x) * interpolation;
        result.y += (target.y - result.y) * interpolation;
        if (!std::isfinite(result.x) || !std::isfinite(result.y)) {
            throw Error(
                ErrorCode::resourceValidation,
                "Camera parallax displacement is non-finite"
            );
        }
        return result;
    }

    [[nodiscard]] ResolvedFrameInputs resolveInputs(
        const FrameInputs& inputs,
        const std::optional<PresentationViewport>& presentation,
        std::optional<PresentationScaling> scaling
    ) const {
        const float daytime = localDaytime();
        ResolvedFrameInputs result{
            .timeSeconds = inputs.timeSeconds,
            .frameTimeSeconds = inputs.frameTimeSeconds,
            .isScreensaver = inputs.isScreensaver,
            .daytime = daytime,
            .timeOfDay = daytime,
            .audioSpectrum = inputs.audioSpectrum,
            .mediaSnapshot = inputs.mediaSnapshot,
        };
        const FrameVector2 drawablePointer = inputs.pointerPosition;
        result.pointerActive = inputs.pointerActive;
        result.pointerLeftDown = inputs.pointerLeftDown;
        if (!presentation) {
            result.pointerPosition = {
                std::clamp(drawablePointer.x, 0.0, 1.0),
                std::clamp(drawablePointer.y, 0.0, 1.0),
            };
        } else {
            if (!scaling) {
                throw Error(
                    ErrorCode::invalidArgument,
                    "Drawable pointer mapping requires a presentation scaling mode"
                );
            }
            // Pointer mapping belongs to the logical evaluated plan. `width`
            // and `height` describe the currently allocated physical output
            // after an explicit render-size override, so using them here
            // would change world coordinates on the following frame.
            const FrameProjectionSize logicalProjection =
                frameGraph->projectionSize(FrameProjectionSize{
                    .width = presentation->canvasWidth,
                    .height = presentation->canvasHeight,
                });
            const std::uint32_t sourceWidth = logicalProjection.width;
            const std::uint32_t sourceHeight = logicalProjection.height;
            if (sourceWidth == 0 || sourceHeight == 0) {
                throw Error(
                    ErrorCode::invalidArgument,
                    "Scene projection dimensions are unavailable for pointer mapping"
                );
            }
            const auto transform = makePresentationTransform(
                sourceWidth,
                sourceHeight,
                *presentation,
                *scaling
            );
            // Resolve the visible crop once in the host's canonical bottom-left
            // coordinates. Scripts, hit testing, and particle control points use
            // this value directly; Linux material effects and camera parallax use
            // a derived top-left Y below, matching CScene::getMousePosition().
            result.pointerPosition = transform.map(drawablePointer);
        }
        result.pointerPositionLast = hasPublishedPointer
            ? lastPublishedPointer
            : FrameVector2{};
        result.effectPointerPosition = linuxEffectPointer(
            result.pointerPosition
        );
        result.effectPointerPositionLast = hasPublishedPointer
            ? linuxEffectPointer(result.pointerPositionLast)
            : FrameVector2{};
        return result;
    }

    void appendCursorEvents(
        const FramePlan& previousPlan,
        const ResolvedFrameInputs& inputs,
        std::vector<script::ScriptCursorEvent>& events
    ) {
        const std::vector<CursorHit> hits = inputs.pointerActive
            ? hitTestInteractiveLayers(previousPlan, inputs.pointerPosition)
            : std::vector<CursorHit>{};
        const std::vector<CursorHit> previousTargets = cursorTargets;
        const auto makeEvent = [](
            script::ScriptCursorEventType type,
            const CursorHit& value
        ) {
            return script::ScriptCursorEvent{
                .type = type,
                .layerId = value.layerId,
                .worldX = value.worldX,
                .worldY = value.worldY,
                .worldZ = value.worldZ,
                .localX = value.localX,
                .localY = value.localY,
                .localZ = value.localZ,
                .hitBox = std::nullopt,
            };
        };
        const auto targetForLayer = [](
            const std::vector<CursorHit>& targets,
            int layerId
        ) -> const CursorHit* {
            const auto found = std::find_if(
                targets.begin(), targets.end(), [layerId](const CursorHit& target) {
                    return target.layerId == layerId;
                }
            );
            return found == targets.end() ? nullptr : &*found;
        };
        const auto currentProjectionFor = [&](int layerId)
            -> std::optional<CursorHit> {
            return projectCursorLayer(
                previousPlan, layerId, inputs.pointerPosition
            );
        };
        const auto eventTarget = [&](const CursorHit& fallback) {
            return currentProjectionFor(fallback.layerId).value_or(fallback);
        };
        for (const CursorHit& previous : previousTargets) {
            if (targetForLayer(hits, previous.layerId) == nullptr) {
                events.push_back(makeEvent(
                    script::ScriptCursorEventType::leave,
                    eventTarget(previous)
                ));
            }
        }
        for (const CursorHit& hit : hits) {
            if (targetForLayer(previousTargets, hit.layerId) == nullptr) {
                events.push_back(makeEvent(
                    script::ScriptCursorEventType::enter, hit
                ));
            }
        }

        const bool moved = hasCursorPosition &&
            inputs.pointerPosition != lastCursorPosition;
        const bool dragging = inputs.pointerLeftDown &&
            previousPointerLeftDown && !pressedCursorTargets.empty();
        if (dragging) {
            for (CursorHit& pressed : pressedCursorTargets) {
                const CursorHit dragTarget = currentProjectionFor(
                    pressed.layerId
                ).value_or(pressed);
                // Keep captured event coordinates current instead of freezing
                // world/local position at the original button-down point.
                pressed = dragTarget;
                if (!moved) continue;
                events.push_back(makeEvent(
                    script::ScriptCursorEventType::move, dragTarget
                ));
            }
        } else if (moved) {
            // Crossing layers is still a move over the new target. The order
            // is deterministic: leave, enter, then move.
            for (const CursorHit& hit : hits) {
                events.push_back(makeEvent(
                    script::ScriptCursorEventType::move, hit
                ));
            }
        }

        if (inputs.pointerLeftDown && !previousPointerLeftDown) {
            for (const CursorHit& hit : hits) {
                events.push_back(makeEvent(
                    script::ScriptCursorEventType::down, hit
                ));
            }
            pressedCursorTargets = hits;
        }

        if (!inputs.pointerLeftDown && previousPointerLeftDown) {
            if (!pressedCursorTargets.empty()) {
                // Button capture belongs to the pressed layer through release,
                // even outside its bounds. Each captured layer clicks only when
                // the release still hits that same layer.
                for (const CursorHit& pressed : pressedCursorTargets) {
                    const CursorHit releaseTarget = currentProjectionFor(
                        pressed.layerId
                    ).value_or(pressed);
                    events.push_back(makeEvent(
                        script::ScriptCursorEventType::up, releaseTarget
                    ));
                    if (const CursorHit* released = targetForLayer(
                            hits, pressed.layerId
                        )) {
                        events.push_back(makeEvent(
                            script::ScriptCursorEventType::click, *released
                        ));
                    }
                }
            } else {
                // A press that began outside every interactive layer can still
                // be released over one, matching the public cursorUp contract.
                for (const CursorHit& hit : hits) {
                    events.push_back(makeEvent(
                        script::ScriptCursorEventType::up, hit
                    ));
                }
            }
            pressedCursorTargets.clear();
        }

        cursorTargets = hits;
        previousPointerLeftDown = inputs.pointerLeftDown;
        hasCursorPosition = inputs.pointerActive;
        lastCursorPosition = inputs.pointerPosition;
    }

    [[nodiscard]] std::vector<script::ScriptCursorEvent> updateCursorEvents(
        const FramePlan* previousPlan,
        const ResolvedFrameInputs& inputs
    ) {
        // Cursor callbacks are evaluated against the last committed plan. Keep
        // queued button edges until that plan exists so a complete click cannot
        // disappear between two rendered frames.
        if (previousPlan == nullptr) return {};

        std::vector<script::ScriptCursorEvent> events;
        for (const PointerButtonState& state : pendingPointerTransitions) {
            ResolvedFrameInputs transitionInputs = inputs;
            transitionInputs.pointerActive = state.active;
            transitionInputs.pointerLeftDown = state.leftDown;
            appendCursorEvents(*previousPlan, transitionInputs, events);
        }
        pendingPointerTransitions.clear();
        // The sampled frame state also carries position and active-state changes
        // that did not produce a button transition.
        appendCursorEvents(*previousPlan, inputs, events);
        return events;
    }

    void invalidateFrame() noexcept {
        lastFrame.reset();
        cursorTargets.clear();
        pressedCursorTargets.clear();
        pendingPointerTransitions.clear();
        previousPointerLeftDown = false;
        hasCursorPosition = false;
        if (device) {
            try {
                auto session = device->activate();
                textRenderer.trimCache(session);
            } catch (const std::exception& cleanupError) {
                std::fprintf(
                    stderr,
                    "FramePlanExecutor failed to trim the text cache after frame failure: %s\n",
                    cleanupError.what()
                );
            } catch (...) {
                std::fprintf(
                    stderr,
                    "FramePlanExecutor failed to trim the text cache after frame failure with an unknown error\n"
                );
            }
        }
        try {
            clearCurrentOutput();
        } catch (const std::exception& cleanupError) {
            std::fprintf(
                stderr,
                "FramePlanExecutor failed to clear the previous output: %s\n",
                cleanupError.what()
            );
        } catch (...) {
            std::fprintf(
                stderr,
                "FramePlanExecutor failed to clear the previous output with an unknown error\n"
            );
        }
    }

    [[noreturn]] void failFrame(std::exception_ptr original) {
        invalidateFrame();
        std::rethrow_exception(original);
    }

    static constexpr std::size_t frameStageCount = 9;

    void recordFrameStats(
        double deltaSeconds,
        double renderMilliseconds,
        const std::array<double, frameStageCount>& stageMilliseconds,
        std::size_t operationCount
    ) {
        if (!frameStatsEnabled()) return;
        ++frameStats.frames;
        frameStats.deltaSum += deltaSeconds;
        frameStats.renderSum += renderMilliseconds;
        frameStats.deltaMinimum = std::min(
            frameStats.deltaMinimum, deltaSeconds
        );
        frameStats.deltaMaximum = std::max(
            frameStats.deltaMaximum, deltaSeconds
        );
        frameStats.renderMaximum = std::max(
            frameStats.renderMaximum, renderMilliseconds
        );
        if (deltaSeconds > 0.020) ++frameStats.deltaOver20Milliseconds;
        if (deltaSeconds > 0.025) ++frameStats.deltaOver25Milliseconds;
        if (deltaSeconds > 1.0 / 30.0) {
            ++frameStats.deltaOverOneThirtySecond;
        }
        if (renderMilliseconds > 16.6667) {
            ++frameStats.renderOverOneSixtySecond;
        }
        for (std::size_t index = 0; index < frameStageCount; ++index) {
            frameStats.stageSums[index] += stageMilliseconds[index];
        }
        std::size_t aliveParticleCount = 0;
        std::size_t ropeHistoryCount = 0;
        std::size_t ropeSampleCount = 0;
        for (const auto& [objectId, state] : particles) {
            static_cast<void>(objectId);
            aliveParticleCount += state.simulation.particles().size();
            ropeHistoryCount += state.ropeTrails.size();
            for (const auto& [spawnId, history] : state.ropeTrails) {
                static_cast<void>(spawnId);
                ropeSampleCount += history.samples.size();
            }
        }
        std::size_t vertexCount = 0;
        std::size_t ropeVertexCount = 0;
        std::size_t indexCount = 0;
        if (lastFrame) {
            for (const auto& [objectIndex, batch] :
                 lastFrame->particleBatches) {
                static_cast<void>(objectIndex);
                vertexCount += batch.vertices.size();
                ropeVertexCount += batch.ropeVertices.size();
                indexCount += batch.indices.size();
            }
        }
        frameStats.operationSum += operationCount;
        frameStats.aliveParticleSum += aliveParticleCount;
        frameStats.ropeHistorySum += ropeHistoryCount;
        frameStats.ropeSampleSum += ropeSampleCount;
        frameStats.vertexSum += vertexCount;
        frameStats.ropeVertexSum += ropeVertexCount;
        frameStats.indexSum += indexCount;
        if (frameStats.deltaSum < 2.0 || frameStats.frames == 0) return;

        const double frameCount = static_cast<double>(frameStats.frames);
        frameStatsLog(
            "frames=%llu deltaAvgMs=%.3f deltaMinMs=%.3f "
            "deltaMaxMs=%.3f deltaOver20=%llu deltaOver25=%llu "
            "deltaOver33=%llu renderAvgMs=%.3f renderMaxMs=%.3f "
            "renderOver16=%llu stagesMs={graph:%.3f arena:%.3f "
            "preflight:%.3f execute:%.3f} workAvg={ops:%.1f alive:%.1f "
            "histories:%.1f samples:%.1f vertices:%.1f ropeVertices:%.1f "
            "indices:%.1f}",
            static_cast<unsigned long long>(frameStats.frames),
            frameStats.deltaSum * 1000.0 /
                static_cast<double>(frameStats.frames),
            frameStats.deltaMinimum * 1000.0,
            frameStats.deltaMaximum * 1000.0,
            static_cast<unsigned long long>(
                frameStats.deltaOver20Milliseconds
            ),
            static_cast<unsigned long long>(
                frameStats.deltaOver25Milliseconds
            ),
            static_cast<unsigned long long>(
                frameStats.deltaOverOneThirtySecond
            ),
            frameStats.renderSum /
                static_cast<double>(frameStats.frames),
            frameStats.renderMaximum,
            static_cast<unsigned long long>(
                frameStats.renderOverOneSixtySecond
            ),
            frameStats.stageSums[2] / frameCount,
            frameStats.stageSums[4] / frameCount,
            frameStats.stageSums[5] / frameCount,
            frameStats.stageSums[7] / frameCount,
            static_cast<double>(frameStats.operationSum) / frameCount,
            static_cast<double>(frameStats.aliveParticleSum) / frameCount,
            static_cast<double>(frameStats.ropeHistorySum) / frameCount,
            static_cast<double>(frameStats.ropeSampleSum) / frameCount,
            static_cast<double>(frameStats.vertexSum) / frameCount,
            static_cast<double>(frameStats.ropeVertexSum) / frameCount,
            static_cast<double>(frameStats.indexSum) / frameCount
        );
        frameStats = {};
    }

    void render(
        const FrameInputs& inputs,
        std::optional<PresentationViewport> presentation = std::nullopt,
        std::optional<PresentationScaling> scaling = std::nullopt,
        std::optional<PhysicalRenderTarget> physicalTarget = std::nullopt
    ) {
        const auto traceStarted = FrameTraceClock::now();
        auto traceStageStarted = traceStarted;
        std::array<double, frameStageCount> stageMilliseconds{};
        std::size_t stageIndex = 0;
        const std::uint64_t currentTraceFrame = ++traceFrameSequence;
        const auto traceStage = [&](const char* stage) {
            const auto now = FrameTraceClock::now();
            frameTraceLog(
                "executor.stage frame=%llu stage=%s ms=%.3f",
                static_cast<unsigned long long>(currentTraceFrame),
                stage,
                frameTraceMilliseconds(traceStageStarted, now)
            );
            if (stageIndex < stageMilliseconds.size()) {
                stageMilliseconds[stageIndex++] =
                    frameTraceMilliseconds(traceStageStarted, now);
            }
            traceStageStarted = now;
        };
        frameTraceLog(
            "executor.begin frame=%llu runtime=%.6f delta=%.6f",
            static_cast<unsigned long long>(currentTraceFrame),
            inputs.timeSeconds,
            inputs.frameTimeSeconds
        );
        try {
            FrameInputs effectiveInputs = inputs;
            effectiveInputs.pointerActive = pointerActive;
            effectiveInputs.pointerLeftDown = pointerLeftDown;
            effectiveInputs.mediaSnapshot = mediaSnapshot;
            if (isScreensaver) {
                effectiveInputs.isScreensaver = isScreensaver;
            }
            if (!std::isfinite(effectiveInputs.pointerPosition.x) ||
                !std::isfinite(effectiveInputs.pointerPosition.y) ||
                !std::isfinite(effectiveInputs.timeSeconds) || effectiveInputs.timeSeconds < 0.0 ||
                !std::isfinite(effectiveInputs.frameTimeSeconds) || effectiveInputs.frameTimeSeconds < 0.0) {
                throw Error(
                    ErrorCode::invalidArgument,
                    "Frame inputs must be finite and time values must be non-negative"
                );
            }
            if (inputs.audioSpectrum) {
                const AudioSpectrumFrame& audio = *inputs.audioSpectrum;
                if (!audioSpectrumIsValid(audio)) {
                    throw Error(
                        ErrorCode::invalidArgument,
                        "Audio spectrum frame must contain only finite non-negative values"
                    );
                }
            }
            const std::optional<FrameProjectionSize> projectionFallback =
                presentation
                    ? std::optional<FrameProjectionSize>(FrameProjectionSize{
                          .width = presentation->canvasWidth,
                          .height = presentation->canvasHeight,
                      })
                    : std::nullopt;
            const ResolvedFrameInputs resolvedInputs = resolveInputs(
                effectiveInputs, presentation, scaling
            );
            traceStage("resolveInputs");
            std::vector<script::ScriptCursorEvent> cursorEvents =
                updateCursorEvents(
                    lastFrame ? &lastFrame->sourcePlan : nullptr,
                    resolvedInputs
                );
            traceStage("cursorEvents");
            EvaluatedFramePlan evaluated = frameGraph->evaluate(
                {
                    .runtimeSeconds = resolvedInputs.timeSeconds,
                    .frameTimeSeconds = resolvedInputs.frameTimeSeconds,
                    .isScreensaver = resolvedInputs.isScreensaver,
                    .timeOfDay = resolvedInputs.timeOfDay,
                    .audioSpectrum = resolvedInputs.audioSpectrum,
                    .pointerX = resolvedInputs.pointerPosition.x,
                    .pointerY = resolvedInputs.pointerPosition.y,
                    .pointerActive = resolvedInputs.pointerActive,
                    .pointerLeftDown = resolvedInputs.pointerLeftDown,
                    .cursorEvents = std::move(cursorEvents),
                    .mediaSnapshot = resolvedInputs.mediaSnapshot,
                    .soundRuntimeStates = soundRuntimeStates,
                },
                projectionFallback
            );
            traceStage("frameGraphEvaluate");
            resolveTextEffectGeometry(evaluated.plan);
            traceStage("textGeometry");
            const FramePlan& logicalPlan = evaluated.plan;
            std::optional<FramePlan> physicalPlan;
            const FramePlan* executionPlan = &logicalPlan;
            if (physicalTarget) {
                if (!scaling) {
                    throw Error(
                        ErrorCode::invalidArgument,
                        "Physical rendering requires a presentation scaling mode"
                    );
                }
                physicalPlan.emplace(withPhysicalRenderSize(
                    logicalPlan,
                    physicalRenderSize(logicalPlan, *physicalTarget, *scaling)
                ));
                executionPlan = &*physicalPlan;
            }
            const FramePlan& plan = *executionPlan;
            const std::vector<ObjectOperationGroup> groups =
                objectOperationGroups(plan);
            const PreparedCamera camera = prepareCamera(logicalPlan);
            const FrameVector2 workingParallax = nextParallax(
                parallaxDisplacement, logicalPlan, resolvedInputs
            );
            auto session = ensureDevice().activate();
            prepareFrameArena(session, plan, groups);
            traceStage("frameArena");
            PreparedFrame prepared = preflightFrameObjects(
                session,
                logicalPlan,
                plan,
                groups,
                resolvedInputs,
                workingParallax,
                camera
            );
            traceStage("preflight");
            beginFrameOutput(session, plan);
            traceStage("beginOutput");
            executePreparedOperations(session, prepared);
            traceStage("execute");
            textRenderer.trimCache(session);
            traceStage("trimCache");

            LastFrameState published{
                .sourcePlan = std::move(evaluated.plan),
                .evaluation = std::move(evaluated.evaluation),
                .inputs = resolvedInputs,
                .particleBatches = std::move(prepared.particleBatches),
                .issues = std::move(prepared.issues),
            };
            lastFrame.emplace(std::move(published));
            particles.swap(prepared.particleStates);
            parallaxDisplacement = workingParallax;
            lastPublishedPointer = resolvedInputs.pointerPosition;
            hasPublishedPointer = true;
            const double totalMilliseconds = frameTraceMilliseconds(
                traceStarted
            );
            frameTraceLog(
                "executor.end frame=%llu totalMs=%.3f operations=%zu "
                "particleObjects=%zu particleBatches=%zu",
                static_cast<unsigned long long>(currentTraceFrame),
                totalMilliseconds,
                plan.operations.size(),
                particles.size(),
                lastFrame->particleBatches.size()
            );
            recordFrameStats(
                resolvedInputs.frameTimeSeconds,
                totalMilliseconds,
                stageMilliseconds,
                plan.operations.size()
            );
        } catch (...) {
            frameTraceLog(
                "executor.fail frame=%llu totalMs=%.3f",
                static_cast<unsigned long long>(currentTraceFrame),
                frameTraceMilliseconds(traceStarted)
            );
            failFrame(std::current_exception());
        }
    }

    void replay(
        const PresentationViewport& presentation,
        std::optional<PresentationScaling> scaling = std::nullopt,
        std::optional<PhysicalRenderTarget> physicalTarget = std::nullopt
    ) {
        try {
            validatePresentationViewport(presentation);
            if (!lastFrame) {
                throw Error(
                    ErrorCode::invalidArgument,
                    "No successful evaluated scene frame is available to replay"
                );
            }
            const bool needsReprojection =
                frameGraph->requiresDrawableProjectionFallback() &&
                (lastFrame->sourcePlan.width != presentation.canvasWidth ||
                 lastFrame->sourcePlan.height != presentation.canvasHeight);
            FramePlan logicalReplayPlan = needsReprojection
                ? frameGraph->reproject(
                      lastFrame->evaluation,
                      FrameProjectionSize{
                          .width = presentation.canvasWidth,
                          .height = presentation.canvasHeight,
                      }
                  )
                : lastFrame->sourcePlan;
            if (needsReprojection) {
                resolveTextEffectGeometry(logicalReplayPlan);
            }
            std::optional<FramePlan> physicalReplayPlan;
            const FramePlan* executionPlan = &logicalReplayPlan;
            if (physicalTarget) {
                if (!scaling) {
                    throw Error(
                        ErrorCode::invalidArgument,
                        "Physical replay requires a presentation scaling mode"
                    );
                }
                physicalReplayPlan.emplace(withPhysicalRenderSize(
                    logicalReplayPlan,
                    physicalRenderSize(
                        logicalReplayPlan, *physicalTarget, *scaling
                    )
                ));
                executionPlan = &*physicalReplayPlan;
            }
            const FramePlan& replayPlan = *executionPlan;
            if (!needsReprojection &&
                width == replayPlan.width && height == replayPlan.height) {
                return;
            }
            const std::vector<ObjectOperationGroup> groups =
                objectOperationGroups(replayPlan);
            const PreparedCamera camera = prepareCamera(logicalReplayPlan);
            auto session = ensureDevice().activate();
            prepareFrameArena(session, replayPlan, groups);
            PreparedFrame prepared = preflightFrameObjects(
                session,
                logicalReplayPlan,
                replayPlan,
                groups,
                lastFrame->inputs,
                parallaxDisplacement,
                camera,
                &lastFrame->particleBatches,
                &lastFrame->issues
            );
            beginFrameOutput(session, replayPlan);
            executePreparedOperations(session, prepared);
            textRenderer.trimCache(session);
            if (needsReprojection) {
                // Reprojection changes only the frozen logical layout used by
                // future hit tests/replays. It neither evaluates scripts nor
                // advances particle simulation or frame time.
                lastFrame->sourcePlan = std::move(logicalReplayPlan);
            }
            lastFrame->issues = std::move(prepared.issues);
        } catch (...) {
            failFrame(std::current_exception());
        }
    }

    void present(
        const PresentationViewport& presentation,
        PresentationScaling scaling
    ) {
        const auto traceStarted = FrameTraceClock::now();
        if (borrowedContext == nullptr) {
            throw Error(
                ErrorCode::invalidArgument,
                "Presenting requires an executor created with a borrowed CGL context"
            );
        }
        validatePresentationViewport(presentation);
        if (outputId.empty() || !lastFrame.has_value()) {
            throw Error(ErrorCode::resourceValidation, "No successful scene frame is available to present");
        }

        auto session = ensureDevice().activate();
        auto& output = framebuffer({.kind = FrameResourceKind::framebuffer, .id = outputId});
        const auto transform = makePresentationTransform(
            output.width,
            output.height,
            presentation,
            scaling
        );
        const PresentationSlice slice = transform.slice();

        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_DEPTH_CLAMP);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_SAMPLE_COVERAGE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DITHER);
        glDisable(GL_MULTISAMPLE);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        glViewport(
            0,
            0,
            static_cast<GLsizei>(presentation.drawableWidth),
            static_cast<GLsizei>(presentation.drawableHeight)
        );
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (slice.hasContent) {
            presentationRenderer.draw(
                session,
                output,
                0,
                GL_BACK,
                presentation.drawableWidth,
                presentation.drawableHeight,
                slice,
                GL_LINEAR
            );
        }
        session.checkError(ErrorCode::draw, "presenting the scene frame to the drawable");
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        frameTraceLog(
            "executor.present frame=%llu drawable=%ux%u output=%ux%u ms=%.3f",
            static_cast<unsigned long long>(traceFrameSequence),
            presentation.drawableWidth,
            presentation.drawableHeight,
            output.width,
            output.height,
            frameTraceMilliseconds(traceStarted)
        );
    }

    std::shared_ptr<SceneFrameGraph> frameGraph;
    CGLContextObj borrowedContext = nullptr;
    std::unique_ptr<Device> device;
    std::map<std::string, CachedFramebuffer> framebuffers;
    std::map<std::string, CachedFramebuffer> inactiveFramebuffers;
    FramebufferResource particleRefractSnapshot;
    std::map<std::string, std::string> framebufferAliases;
    std::map<std::string, AssetTextureResource> assets;
    std::map<std::string, ProgramResource> programs;
    TextCoverageRenderer textRenderer;
    PresentationRenderer presentationRenderer;
    std::map<TextRasterKey, CachedTextRaster> textRasters;
    std::size_t textRasterBytes = 0;
    std::uint64_t textRasterUseSequence = 0;
    GLuint vertexArray = 0;
    GLuint vertexBuffer = 0;
    ImageAttributeState imageAttributeState;
    GLuint particleVertexArray = 0;
    GLuint particleVertexBuffer = 0;
    GLuint particleElementBuffer = 0;
    GLuint puppetVertexArray = 0;
    GLuint puppetVertexBuffer = 0;
    GLuint puppetElementBuffer = 0;
    ImageAttributeState puppetAttributeState;
    std::map<int, ParticleState> particles;
    std::string outputId;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t byteCount = 0;
    std::optional<LastFrameState> lastFrame;
    FrameVector2 parallaxDisplacement;
    FrameVector2 lastPublishedPointer;
    bool hasPublishedPointer = false;
    std::vector<CursorHit> cursorTargets;
    std::vector<CursorHit> pressedCursorTargets;
    struct PointerButtonState final {
        bool active = false;
        bool leftDown = false;
    };
    std::vector<PointerButtonState> pendingPointerTransitions;
    FrameVector2 lastCursorPosition;
    bool hasCursorPosition = false;
    bool previousPointerLeftDown = false;
    bool pointerActive = false;
    bool pointerLeftDown = false;
    std::optional<bool> isScreensaver;
    std::optional<script::ScriptMediaSnapshot> mediaSnapshot;
    std::optional<std::uint64_t> mediaThumbnailRevision;
    HostTextureSlot mediaThumbnailCurrent;
    HostTextureSlot mediaThumbnailPrevious;
    std::vector<script::ScriptSoundRuntimeSnapshot> soundRuntimeStates;
    std::uint64_t traceFrameSequence = 0;
    struct FrameStatsState final {
        std::uint64_t frames = 0;
        double deltaSum = 0.0;
        double deltaMinimum = std::numeric_limits<double>::infinity();
        double deltaMaximum = 0.0;
        double renderSum = 0.0;
        double renderMaximum = 0.0;
        std::uint64_t deltaOver20Milliseconds = 0;
        std::uint64_t deltaOver25Milliseconds = 0;
        std::uint64_t deltaOverOneThirtySecond = 0;
        std::uint64_t renderOverOneSixtySecond = 0;
        std::array<double, frameStageCount> stageSums{};
        std::uint64_t operationSum = 0;
        std::uint64_t aliveParticleSum = 0;
        std::uint64_t ropeHistorySum = 0;
        std::uint64_t ropeSampleSum = 0;
        std::uint64_t vertexSum = 0;
        std::uint64_t ropeVertexSum = 0;
        std::uint64_t indexSum = 0;
    } frameStats;

    [[nodiscard]] FramebufferResourceStats framebufferResourceStats() const noexcept {
        FramebufferResourceStats result{
            .framebufferCount = framebuffers.size(),
            .colorAttachmentCount = framebuffers.size(),
        };
        for (const auto& [id, framebuffer] : framebuffers) {
            static_cast<void>(id);
            if (framebuffer.resource.depthRenderbuffer != 0) {
                ++result.depthAttachmentCount;
            }
        }
        result.inactiveFramebufferCount = inactiveFramebuffers.size();
        result.inactiveColorAttachmentCount = inactiveFramebuffers.size();
        for (const auto& [id, framebuffer] : inactiveFramebuffers) {
            static_cast<void>(id);
            if (framebuffer.resource.depthRenderbuffer != 0) {
                ++result.inactiveDepthAttachmentCount;
            }
        }
        return result;
    }
};

FramePlanExecutor::FramePlanExecutor(std::shared_ptr<SceneFrameGraph> frameGraph)
    : impl_(std::make_unique<Impl>(std::move(frameGraph))) {}

FramePlanExecutor::FramePlanExecutor(
    std::shared_ptr<SceneFrameGraph> frameGraph,
    CGLContextObj borrowedContext
) : impl_(std::make_unique<Impl>(std::move(frameGraph), borrowedContext)) {}

FramePlanExecutor::~FramePlanExecutor() = default;
void FramePlanExecutor::render(const FrameInputs& inputs) { impl_->render(inputs); }
void FramePlanExecutor::render(
    const FrameInputs& inputs,
    std::uint32_t drawableWidth,
    std::uint32_t drawableHeight,
    PresentationScaling scaling
) {
    render(
        inputs,
        drawablePresentationViewport(drawableWidth, drawableHeight),
        scaling
    );
}
void FramePlanExecutor::render(
    const FrameInputs& inputs,
    const PresentationViewport& viewport,
    PresentationScaling scaling
) {
    impl_->render(inputs, viewport, scaling);
}
void FramePlanExecutor::render(
    const FrameInputs& inputs,
    const PresentationViewport& viewport,
    PresentationScaling scaling,
    PhysicalRenderTarget physicalTarget
) {
    impl_->render(inputs, viewport, scaling, physicalTarget);
}
void FramePlanExecutor::replay(
    std::uint32_t drawableWidth,
    std::uint32_t drawableHeight
) {
    replay(drawablePresentationViewport(drawableWidth, drawableHeight));
}
void FramePlanExecutor::replay(const PresentationViewport& viewport) {
    impl_->replay(viewport);
}
void FramePlanExecutor::replay(
    const PresentationViewport& viewport,
    PresentationScaling scaling,
    PhysicalRenderTarget physicalTarget
) {
    impl_->replay(viewport, scaling, physicalTarget);
}
void FramePlanExecutor::present(
    std::uint32_t drawableWidth,
    std::uint32_t drawableHeight,
    PresentationScaling scaling
) {
    present(
        drawablePresentationViewport(drawableWidth, drawableHeight), scaling
    );
}
void FramePlanExecutor::setPointerState(bool active, bool leftDown) {
    if (leftDown != impl_->pointerLeftDown) {
        impl_->pendingPointerTransitions.push_back({
            .active = active,
            .leftDown = leftDown,
        });
    }
    impl_->pointerActive = active;
    impl_->pointerLeftDown = leftDown;
}
void FramePlanExecutor::setScreensaverState(bool value) noexcept {
    impl_->isScreensaver = value;
}
void FramePlanExecutor::setMediaSnapshot(
    std::optional<script::ScriptMediaSnapshot> snapshot
) {
    impl_->mediaSnapshot = std::move(snapshot);
}
void FramePlanExecutor::setMediaThumbnail(MediaThumbnailRGBA8 thumbnail) {
    const std::size_t expected = Impl::checkedRGBA8ByteCount(
        thumbnail.width, thumbnail.height
    );
    if (thumbnail.width == 0 || thumbnail.height == 0 ||
        thumbnail.pixels.size() != expected) {
        throw Error(
            ErrorCode::invalidArgument,
            "Scene media thumbnail requires non-empty tightly packed RGBA8 pixels"
        );
    }
    if (expected > 256 * 1024 * 1024) {
        throw Error(
            ErrorCode::invalidArgument,
            "Scene media thumbnail exceeds the 256 MiB allocation limit"
        );
    }
    if (impl_->mediaThumbnailRevision &&
        thumbnail.revision < *impl_->mediaThumbnailRevision) {
        throw Error(
            ErrorCode::invalidArgument,
            "Scene media thumbnail revision cannot move backwards"
        );
    }
    if (impl_->mediaThumbnailRevision &&
        thumbnail.revision == *impl_->mediaThumbnailRevision) {
        if (impl_->mediaThumbnailCurrent.image &&
            *impl_->mediaThumbnailCurrent.image == thumbnail) {
            return;
        }
        throw Error(
            ErrorCode::invalidArgument,
            "Scene media thumbnail pixels are immutable within one revision"
        );
    }
    if (impl_->mediaThumbnailCurrent.image) {
        impl_->mediaThumbnailPrevious.image =
            impl_->mediaThumbnailCurrent.image;
        impl_->mediaThumbnailPrevious.dirty = true;
    }
    impl_->mediaThumbnailRevision = thumbnail.revision;
    impl_->mediaThumbnailCurrent.image = std::move(thumbnail);
    impl_->mediaThumbnailCurrent.dirty = true;
}
void FramePlanExecutor::clearMediaThumbnail(std::uint64_t revision) {
    if (impl_->mediaThumbnailRevision &&
        revision < *impl_->mediaThumbnailRevision) {
        throw Error(
            ErrorCode::invalidArgument,
            "Scene media thumbnail revision cannot move backwards"
        );
    }
    if (impl_->mediaThumbnailRevision &&
        revision == *impl_->mediaThumbnailRevision) {
        if (!impl_->mediaThumbnailCurrent.image) return;
        throw Error(
            ErrorCode::invalidArgument,
            "Scene media thumbnail presence is immutable within one revision"
        );
    }
    if (impl_->mediaThumbnailCurrent.image) {
        impl_->mediaThumbnailPrevious.image =
            impl_->mediaThumbnailCurrent.image;
        impl_->mediaThumbnailPrevious.dirty = true;
    }
    impl_->mediaThumbnailRevision = revision;
    impl_->mediaThumbnailCurrent.image.reset();
    impl_->mediaThumbnailCurrent.dirty = true;
}
void FramePlanExecutor::setSoundRuntimeStates(
    std::vector<script::ScriptSoundRuntimeSnapshot> states
) {
    impl_->soundRuntimeStates = std::move(states);
}
void FramePlanExecutor::present(
    const PresentationViewport& viewport,
    PresentationScaling scaling
) {
    impl_->present(viewport, scaling);
}
void FramePlanExecutor::invalidateFrame() noexcept { impl_->invalidateFrame(); }
std::uint32_t FramePlanExecutor::width() const noexcept { return impl_->width; }
std::uint32_t FramePlanExecutor::height() const noexcept { return impl_->height; }
std::size_t FramePlanExecutor::rgba8ByteCount() const noexcept {
    return impl_->byteCount;
}
FramebufferResourceStats FramePlanExecutor::framebufferResourceStats() const noexcept {
    return impl_->framebufferResourceStats();
}
std::optional<std::uint64_t> FramePlanExecutor::lastModelRevision() const noexcept {
    return impl_->lastFrame
        ? std::optional<std::uint64_t>(impl_->lastFrame->sourcePlan.modelRevision)
        : std::nullopt;
}
const std::vector<FrameSoundDescriptor>* FramePlanExecutor::lastSounds() const noexcept {
    return impl_->lastFrame ? &impl_->lastFrame->sourcePlan.sounds : nullptr;
}
const std::vector<FrameExecutionIssue>* FramePlanExecutor::lastIssues() const noexcept {
    return impl_->lastFrame ? &impl_->lastFrame->issues : nullptr;
}
void FramePlanExecutor::readRGBA8(std::span<std::uint8_t> output) {
    if (impl_->outputId.empty()) {
        throw Error(ErrorCode::readback, "No frame has been rendered");
    }
    auto session = impl_->ensureDevice().activate();
    session.readRGBA8(impl_->framebuffer({
        .kind = FrameResourceKind::framebuffer,
        .id = impl_->outputId,
    }), output, ReadbackSourceOrientation::wallpaperEngineTopLeft);
}

}  // namespace we::scene::gl
