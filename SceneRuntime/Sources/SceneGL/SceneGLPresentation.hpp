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

}  // namespace we::scene::gl

#endif
