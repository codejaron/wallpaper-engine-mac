#ifndef WE_SCENE_GL_FRAME_PLAN_EXECUTOR_HPP
#define WE_SCENE_GL_FRAME_PLAN_EXECUTOR_HPP

#include <SceneFrameGraph/SceneFrameGraph.hpp>

#include <OpenGL/OpenGL.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace we::scene::gl {

struct FrameInputs final {
    FrameVector2 pointerPosition;
    double timeSeconds = 0.0;
    double frameTimeSeconds = 0.0;
};

enum class PresentationScaling { stretch, aspectFit, aspectFill };

struct FrameExecutionIssue final {
    FramePlanIssueSeverity severity = FramePlanIssueSeverity::skipObject;
    std::size_t objectIndex = 0;
    int objectId = 0;
    std::size_t operationIndex = 0;
    std::string message;
};

// Executes coherent SceneFrameGraph snapshots in one private GL resource
// domain. No plan state is reconstructed by the Objective-C/C boundary.
class FramePlanExecutor final {
public:
    explicit FramePlanExecutor(std::shared_ptr<SceneFrameGraph> frameGraph);
    FramePlanExecutor(
        std::shared_ptr<SceneFrameGraph> frameGraph,
        CGLContextObj borrowedContext
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
    void replay(std::uint32_t drawableWidth, std::uint32_t drawableHeight);
    void present(
        std::uint32_t drawableWidth,
        std::uint32_t drawableHeight,
        PresentationScaling scaling
    );
    // Clears the published frame after host-side validation rejects a render
    // attempt before execution can begin.
    void invalidateFrame() noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::size_t rgba8ByteCount() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> lastModelRevision() const noexcept;
    [[nodiscard]] const std::vector<FrameSoundDescriptor>* lastSounds() const noexcept;
    [[nodiscard]] const std::vector<FrameExecutionIssue>* lastIssues() const noexcept;

    // Returns tightly packed RGBA8 rows with a top-left origin.
    void readRGBA8(std::span<std::uint8_t> output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace we::scene::gl

#endif
