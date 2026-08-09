#ifndef WE_SCENE_GL_PRESENTATION_HPP
#define WE_SCENE_GL_PRESENTATION_HPP

#include "SceneGLDevice.hpp"

#include <SceneGL/FramePlanExecutor.hpp>

namespace we::scene::gl {

// Draws a logical presentation slice from Wallpaper Engine's top-left scene
// texture into an ordinary OpenGL drawable. A textured draw keeps the scene
// and presentation work in one driver command stream. Apple's OpenGL-to-Metal
// bridge otherwise turns glBlitFramebuffer into a synchronous command-buffer
// submission on every frame.
class PresentationRenderer final {
public:
    void draw(
        Device::Session& session,
        const FramebufferResource& source,
        GLuint destinationFramebuffer,
        GLenum destinationBuffer,
        std::uint32_t destinationWidth,
        std::uint32_t destinationHeight,
        const PresentationSlice& slice,
        GLenum filter
    );
    void release(Device::Session& session) noexcept;

private:
    void initialize(Device::Session& session);

    GLuint program_ = 0;
    GLuint vertexArray_ = 0;
    GLint sourceTextureLocation_ = -1;
    GLint sourceRectangleLocation_ = -1;
    GLint destinationRectangleLocation_ = -1;
};

}  // namespace we::scene::gl

#endif
