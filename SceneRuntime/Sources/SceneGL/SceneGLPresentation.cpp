#include "SceneGLPresentation.hpp"

#include <cmath>
#include <string_view>

namespace we::scene::gl {

namespace {

constexpr std::string_view edgeClampedVertexShader = R"GLSL(
#version 410 core

uniform vec4 sourceUV;
out vec2 textureCoordinate;

const vec2 positions[6] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);

void main() {
    vec2 position = positions[gl_VertexID];
    vec2 unitPosition = position * 0.5 + 0.5;
    textureCoordinate = mix(sourceUV.xy, sourceUV.zw, unitPosition);
    gl_Position = vec4(position, 0.0, 1.0);
}
)GLSL";

constexpr std::string_view edgeClampedFragmentShader = R"GLSL(
#version 410 core

uniform sampler2D sourceTexture;
in vec2 textureCoordinate;
out vec4 fragmentColor;

void main() {
    fragmentColor = texture(sourceTexture, textureCoordinate);
}
)GLSL";

bool finiteSlice(const EdgeClampedPresentationSlice& slice) noexcept {
    return std::isfinite(slice.sourceLeft) &&
        std::isfinite(slice.sourceBottom) &&
        std::isfinite(slice.sourceRight) &&
        std::isfinite(slice.sourceTop);
}

}  // namespace

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

void EdgeClampedPresentationRenderer::ensurePipeline(
    Device::Session& session
) {
    if (program_ != 0) return;

    GLuint candidateProgram = 0;
    GLuint candidateVertexArray = 0;
    try {
        candidateProgram = session.createProgram(
            edgeClampedVertexShader,
            edgeClampedFragmentShader
        );
        candidateVertexArray = session.createVertexArray();
        const GLint candidateSourceTextureLocation = glGetUniformLocation(
            candidateProgram, "sourceTexture"
        );
        const GLint candidateSourceUVLocation = glGetUniformLocation(
            candidateProgram, "sourceUV"
        );
        if (candidateSourceTextureLocation < 0 || candidateSourceUVLocation < 0) {
            throw Error(
                ErrorCode::resourceValidation,
                "Edge-clamped presentation shader is missing a required uniform"
            );
        }
        session.checkError(
            ErrorCode::draw,
            "creating the edge-clamped presentation pipeline"
        );
        program_ = candidateProgram;
        vertexArray_ = candidateVertexArray;
        sourceTextureLocation_ = candidateSourceTextureLocation;
        sourceUVLocation_ = candidateSourceUVLocation;
    } catch (...) {
        session.destroyVertexArray(candidateVertexArray);
        session.destroyProgram(candidateProgram);
        throw;
    }
}

void EdgeClampedPresentationRenderer::present(
    Device::Session& session,
    const FramebufferResource& source,
    GLuint destinationFramebuffer,
    GLenum destinationBuffer,
    const EdgeClampedPresentationSlice& slice,
    GLenum filter
) {
    if (source.framebuffer == 0 || source.colorTexture == 0 ||
        source.width == 0 || source.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Edge-clamped presentation requires a valid source framebuffer"
        );
    }
    if (slice.destination.width == 0 || slice.destination.height == 0 ||
        !finiteSlice(slice)) {
        throw Error(
            ErrorCode::invalidArgument,
            "Edge-clamped presentation requires a finite non-empty slice"
        );
    }
    if (filter != GL_NEAREST && filter != GL_LINEAR) {
        throw Error(
            ErrorCode::invalidArgument,
            "Edge-clamped presentation requires a nearest or linear OpenGL filter"
        );
    }

    ensurePipeline(session);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destinationFramebuffer);
    glDrawBuffer(destinationBuffer);
    glViewport(
        static_cast<GLint>(slice.destination.x),
        static_cast<GLint>(slice.destination.y),
        static_cast<GLsizei>(slice.destination.width),
        static_cast<GLsizei>(slice.destination.height)
    );
    glUseProgram(program_);
    glBindVertexArray(vertexArray_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source.colorTexture);

    GLint previousWrapS = GL_REPEAT;
    GLint previousWrapT = GL_REPEAT;
    GLint previousMinFilter = GL_NEAREST;
    GLint previousMagFilter = GL_NEAREST;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &previousWrapS);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &previousWrapT);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &previousMinFilter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &previousMagFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    // Scene output stores its logical top at GL v=0. Convert the logical
    // bottom-left slice into texture coordinates at this final boundary.
    glUniform1i(sourceTextureLocation_, 0);
    glUniform4f(
        sourceUVLocation_,
        static_cast<GLfloat>(slice.sourceLeft),
        static_cast<GLfloat>(1.0 - slice.sourceBottom),
        static_cast<GLfloat>(slice.sourceRight),
        static_cast<GLfloat>(1.0 - slice.sourceTop)
    );
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, previousWrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, previousWrapT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, previousMinFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, previousMagFilter);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

void EdgeClampedPresentationRenderer::release(
    Device::Session& session
) noexcept {
    session.destroyVertexArray(vertexArray_);
    session.destroyProgram(program_);
    sourceTextureLocation_ = -1;
    sourceUVLocation_ = -1;
}

}  // namespace we::scene::gl
