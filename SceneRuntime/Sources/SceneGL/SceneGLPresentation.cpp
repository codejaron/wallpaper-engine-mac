#include "SceneGLPresentation.hpp"

namespace we::scene::gl {

void blitWallpaperEngineOutput(
    const FramebufferResource& source,
    GLuint destinationFramebuffer,
    GLenum destinationBuffer,
    const PresentationSlice& slice,
    GLenum filter
) {
    if (!slice.hasContent) return;
    if (source.framebuffer == 0 || source.width == 0 || source.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation requires a valid source framebuffer"
        );
    }
    if (slice.source.x > source.width ||
        slice.source.width > source.width - slice.source.x ||
        slice.source.y > source.height ||
        slice.source.height > source.height - slice.source.y) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation source slice exceeds the scene framebuffer"
        );
    }
    if (filter != GL_NEAREST && filter != GL_LINEAR) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation requires a nearest or linear OpenGL filter"
        );
    }

    // PresentationSlice uses logical bottom-left scene coordinates. The
    // internal framebuffer stores Wallpaper Engine's logical top at GL y=0,
    // so map the selected interval around the full source height and reverse
    // its endpoints. Reversing only the destination would pick the wrong half
    // of a vertically spanned wallpaper.
    const GLint sourceLogicalBottom = static_cast<GLint>(
        source.height - slice.source.y
    );
    const GLint sourceLogicalTop = static_cast<GLint>(
        source.height - slice.source.y - slice.source.height
    );

    glBindFramebuffer(GL_READ_FRAMEBUFFER, source.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destinationFramebuffer);
    glDrawBuffer(destinationBuffer);
    glBlitFramebuffer(
        static_cast<GLint>(slice.source.x),
        sourceLogicalBottom,
        static_cast<GLint>(slice.source.x + slice.source.width),
        sourceLogicalTop,
        static_cast<GLint>(slice.destination.x),
        static_cast<GLint>(slice.destination.y),
        static_cast<GLint>(
            slice.destination.x + slice.destination.width
        ),
        static_cast<GLint>(
            slice.destination.y + slice.destination.height
        ),
        GL_COLOR_BUFFER_BIT,
        filter
    );
}

}  // namespace we::scene::gl
