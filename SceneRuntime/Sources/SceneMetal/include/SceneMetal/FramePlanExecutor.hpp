#ifndef WE_SCENE_METAL_FRAME_PLAN_EXECUTOR_HPP
#define WE_SCENE_METAL_FRAME_PLAN_EXECUTOR_HPP

#include <SceneCore/AudioSpectrum.hpp>
#include <SceneFrameGraph/SceneFrameGraph.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace we::scene::metal {

struct FrameInputs final {
    FrameVector2 pointerPosition;
    // Host input is sampled independently from the drawable pointer because
    // wallpaper windows intentionally ignore mouse events. `pointerActive`
    // is false when the cursor is on another display/window.
    bool pointerActive = false;
    bool pointerLeftDown = false;
    double timeSeconds = 0.0;
    double frameTimeSeconds = 0.0;
    // Explicit host mode; omitted means the embedding host has not connected
    // the Wallpaper Engine desktop/screensaver mode yet.
    std::optional<bool> isScreensaver;
    // Local-day fraction used by SceneScript's engine.timeOfDay. The executor
    // supplies this from the host clock for every evaluated frame.
    double timeOfDay = 0.0;
    std::optional<AudioSpectrumFrame> audioSpectrum;
    // Optional host media state is copied into the evaluated SceneScript
    // frame. An absent snapshot is an explicit unavailable integration state.
    std::optional<script::ScriptMediaSnapshot> mediaSnapshot;
};

struct MediaThumbnailRGBA8 final {
    std::uint64_t revision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Tightly packed, top-row-first RGBA8 pixels owned by the executor.
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] friend bool operator==(
        const MediaThumbnailRGBA8&,
        const MediaThumbnailRGBA8&
    ) = default;
};

enum class PresentationScaling { stretch, aspectFit, aspectFill, automatic };

// The requested backing-pixel ceiling for an explicitly downscaled Scene
// render. Balanced additionally applies the runtime's 1080p performance
// ceiling; High is represented by no target so the author-resolution path
// remains byte-for-byte unchanged.
enum class PhysicalRenderQuality { balanced, powerSaving };

struct PhysicalRenderTarget final {
    std::uint32_t backingWidth = 0;
    std::uint32_t backingHeight = 0;
    PhysicalRenderQuality quality = PhysicalRenderQuality::balanced;
};

struct PhysicalRenderSize final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] friend bool operator==(
        const PhysicalRenderSize&,
        const PhysicalRenderSize&
    ) = default;
};

// Chooses a fixed physical output size from a logical author plan and a
// backing-pixel target. This is pure policy: it never observes frame time.
[[nodiscard]] PhysicalRenderSize physicalRenderSize(
    const FramePlan& logicalPlan,
    const PhysicalRenderTarget& target,
    PresentationScaling scaling
);

// Copies a logical plan for GL execution at physicalSize. World-space data,
// camera projection, script/particle state, and every operation remain
// logical; only framebuffer dimensions and the plan's output dimensions move.
[[nodiscard]] FramePlan withPhysicalRenderSize(
    const FramePlan& logicalPlan,
    PhysicalRenderSize physicalSize
);

struct PresentationRect final {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] friend bool operator==(
        const PresentationRect& lhs,
        const PresentationRect& rhs
    ) = default;
};

// One display's location inside a bottom-left-origin virtual desktop canvas.
// Drawable dimensions may differ from viewport dimensions (for example on a
// Retina display), but describe the same visible rectangle.
struct PresentationViewport final {
    std::uint32_t canvasWidth = 0;
    std::uint32_t canvasHeight = 0;
    std::uint32_t viewportX = 0;
    std::uint32_t viewportY = 0;
    std::uint32_t viewportWidth = 0;
    std::uint32_t viewportHeight = 0;
    std::uint32_t drawableWidth = 0;
    std::uint32_t drawableHeight = 0;
};

struct PresentationSlice final {
    bool hasContent = false;
    // Source rectangle in the rendered Scene output.
    PresentationRect source;
    // Destination rectangle in the current display's local drawable.
    PresentationRect destination;
};

// Pure scene-to-canvas transform shared by pointer mapping and presentation.
// Construct through makePresentationTransform so dimensions and bounds are
// validated before map() or slice() is used.
struct PresentationTransform final {
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    PresentationViewport viewport;
    PresentationRect source;
    PresentationRect canvasDestination;

    [[nodiscard]] FrameVector2 map(FrameVector2 drawablePoint) const;
    [[nodiscard]] PresentationSlice slice() const;
};

[[nodiscard]] PresentationViewport drawablePresentationViewport(
    std::uint32_t drawableWidth,
    std::uint32_t drawableHeight
);
void validatePresentationViewport(const PresentationViewport& viewport);
[[nodiscard]] PresentationTransform makePresentationTransform(
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    const PresentationViewport& viewport,
    PresentationScaling scaling
);

struct FrameExecutionIssue final {
    FramePlanIssueSeverity severity = FramePlanIssueSeverity::skipObject;
    std::size_t objectIndex = 0;
    int objectId = 0;
    std::size_t operationIndex = 0;
    std::string message;
};

// Physical framebuffer backing owned by the executor. Statistics separate the
// latest executable arena from persistent inactive backing; the total physical
// allocation for each attachment kind is their sum.
struct FramebufferResourceStats final {
    // Active is the current executable plan's physical baseline.
    std::size_t framebufferCount = 0;
    std::size_t colorAttachmentCount = 0;
    std::size_t depthAttachmentCount = 0;
    // Previously active descriptors remain cached so authored feedback pixels
    // survive any number of visibility transitions during this executor's
    // lifetime.
    std::size_t inactiveFramebufferCount = 0;
    std::size_t inactiveColorAttachmentCount = 0;
    std::size_t inactiveDepthAttachmentCount = 0;
};

// Executes coherent SceneFrameGraph snapshots in one private GL resource
// domain. No plan state is reconstructed by the Objective-C/C boundary.
class FramePlanExecutor final {
public:
    explicit FramePlanExecutor(std::shared_ptr<SceneFrameGraph> frameGraph);
    FramePlanExecutor(
        std::shared_ptr<SceneFrameGraph> frameGraph,
        void* borrowedMetalDevice
    );
    ~FramePlanExecutor();

    FramePlanExecutor(const FramePlanExecutor&) = delete;
    FramePlanExecutor& operator=(const FramePlanExecutor&) = delete;
    FramePlanExecutor(FramePlanExecutor&&) = delete;
    FramePlanExecutor& operator=(FramePlanExecutor&&) = delete;

    void render(const FrameInputs& inputs);
    void render(
        const FrameInputs& inputs,
        std::uint32_t drawableWidth,
        std::uint32_t drawableHeight,
        PresentationScaling scaling
    );
    void render(
        const FrameInputs& inputs,
        const PresentationViewport& viewport,
        PresentationScaling scaling
    );
    void render(
        const FrameInputs& inputs,
        const PresentationViewport& viewport,
        PresentationScaling scaling,
        PhysicalRenderTarget physicalTarget
    );
    void replay(std::uint32_t drawableWidth, std::uint32_t drawableHeight);
    void replay(const PresentationViewport& viewport);
    void replay(
        const PresentationViewport& viewport,
        PresentationScaling scaling,
        PhysicalRenderTarget physicalTarget
    );
    void present(
        void* metalDrawable,
        std::uint32_t drawableWidth,
        std::uint32_t drawableHeight,
        PresentationScaling scaling
    );
    void present(
        void* metalDrawable,
        const PresentationViewport& viewport,
        PresentationScaling scaling
    );
    // Updates the sampled desktop pointer state and preserves every left-button
    // edge until SceneScript cursor events can be evaluated.
    void setPointerState(bool active, bool leftDown);
    void setScreensaverState(bool isScreensaver) noexcept;
    void setMediaSnapshot(
        std::optional<script::ScriptMediaSnapshot> snapshot
    );
    void setMediaThumbnail(MediaThumbnailRGBA8 thumbnail);
    void clearMediaThumbnail(std::uint64_t revision);
    void setSoundRuntimeStates(
        std::vector<script::ScriptSoundRuntimeSnapshot> states
    );
    // Clears the published frame after host-side validation rejects a render
    // attempt before execution can begin.
    void invalidateFrame() noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::size_t rgba8ByteCount() const noexcept;
    [[nodiscard]] FramebufferResourceStats framebufferResourceStats() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> lastModelRevision() const noexcept;
    [[nodiscard]] const std::vector<FrameSoundDescriptor>* lastSounds() const noexcept;
    [[nodiscard]] const std::vector<FrameExecutionIssue>* lastIssues() const noexcept;

    // Returns tightly packed RGBA8 rows with a top-left origin.
    void readRGBA8(std::span<std::uint8_t> output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace we::scene::metal

#endif
