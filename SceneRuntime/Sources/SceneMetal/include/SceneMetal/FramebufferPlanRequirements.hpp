#ifndef WE_SCENE_METAL_FRAMEBUFFER_PLAN_REQUIREMENTS_HPP
#define WE_SCENE_METAL_FRAMEBUFFER_PLAN_REQUIREMENTS_HPP

#include <SceneFrameGraph/SceneFrameGraph.hpp>
#include <SceneMetal/SceneMetal.hpp>

#include <map>
#include <string>

namespace we::scene::metal {

// The physical backing required for one logical framebuffer in an executable
// frame. This deliberately includes attachment policy: a logical
// FramebufferDescriptor alone is not enough to decide whether an existing GL
// allocation can be reused.
struct FramebufferAllocationRequirement final {
    FramebufferDescriptor descriptor;
    bool requiresDepthAttachment = false;
};

// A pure render-graph liveness/attachment analysis. It validates every
// framebuffer reference in the complete plan before selecting only the
// resources that execution can reach. Shader metadata defaults are appended
// by FramePlanExecutor after the matching program has been resolved.
struct FramebufferPlanRequirements final {
    // Retained so shader metadata discovered after static plan analysis can
    // promote an indirect framebuffer read without reopening validation.
    std::map<std::string, FramebufferAllocationRequirement> descriptors;
    std::map<std::string, FramebufferAllocationRequirement> active;

    void requireFramebuffer(
        const FrameResourceRef& resource,
        bool requiresDepthAttachment = false
    );
};

[[nodiscard]] FramebufferPlanRequirements analyzeFramebufferPlanRequirements(
    const FramePlan& plan
);

}  // namespace we::scene::metal

#endif
