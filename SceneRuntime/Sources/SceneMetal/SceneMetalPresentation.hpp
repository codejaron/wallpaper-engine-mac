#ifndef WE_SCENE_METAL_PRESENTATION_HPP
#define WE_SCENE_METAL_PRESENTATION_HPP

#include "SceneMetalDevice.hpp"

#include <SceneMetal/FramePlanExecutor.hpp>

namespace we::scene::metal {

// Draws a logical presentation slice from Wallpaper Engine's top-left scene
// texture into a Metal drawable or another framebuffer target.
class PresentationRenderer final {
public:
    void draw(
        Device::Session& session,
        const FramebufferResource& source,
        FramebufferResource& destination,
        const PresentationSlice& slice,
        TextureFilter filter
    );
    void release(Device::Session& session) noexcept;

private:
    void initialize(Device::Session& session);

    std::shared_ptr<Program> program_;
    std::uint32_t sourceTextureIndex_ = 0;
    std::uint32_t sourceSamplerIndex_ = 0;
    std::uint32_t sourceRectangleBufferIndex_ = 0;
    std::uint32_t destinationRectangleBufferIndex_ = 0;
};

}  // namespace we::scene::metal

#endif
