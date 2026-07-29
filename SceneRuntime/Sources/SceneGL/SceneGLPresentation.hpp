#ifndef WE_SCENE_GL_PRESENTATION_HPP
#define WE_SCENE_GL_PRESENTATION_HPP

#include "SceneGLDevice.hpp"

#include <SceneGL/FramePlanExecutor.hpp>

namespace we::scene::gl {

// Copies a logical presentation slice from Wallpaper Engine's top-left scene
// framebuffer into an ordinary OpenGL drawable. Source Y is both remapped and
// reversed here so vertically sliced displays select the correct scene region.
void blitWallpaperEngineOutput(
    const FramebufferResource& source,
    GLuint destinationFramebuffer,
    GLenum destinationBuffer,
    const PresentationSlice& slice,
    GLenum filter
);

// Samples a Wallpaper Engine framebuffer through UVs that may extend beyond
// the source bounds. OpenGL edge clamping reproduces the engine's default
// orientation-aware presentation without cropping preserved authored content.
class EdgeClampedPresentationRenderer final {
public:
    void present(
        Device::Session& session,
        const FramebufferResource& source,
        GLuint destinationFramebuffer,
        GLenum destinationBuffer,
        const EdgeClampedPresentationSlice& slice,
        GLenum filter
    );
    void release(Device::Session& session) noexcept;

private:
    void ensurePipeline(Device::Session& session);

    GLuint program_ = 0;
    GLuint vertexArray_ = 0;
    GLint sourceTextureLocation_ = -1;
    GLint sourceUVLocation_ = -1;
};

}  // namespace we::scene::gl

#endif
