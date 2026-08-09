#include "SceneGLPresentation.hpp"

#include <array>
#include <string_view>

namespace we::scene::gl {
namespace {

constexpr std::string_view presentationVertexShader = R"glsl(
#version 410 core

uniform vec4 sourceRectangle;
uniform vec4 destinationRectangle;

out vec2 textureCoordinate;

const vec2 corners[6] = vec2[6](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(0.0, 1.0),
    vec2(1.0, 0.0),
    vec2(1.0, 1.0)
);

void main() {
    vec2 corner = corners[gl_VertexID];
    vec2 normalizedPosition = destinationRectangle.xy +
        corner * destinationRectangle.zw;
    gl_Position = vec4(normalizedPosition * 2.0 - 1.0, 0.0, 1.0);
    textureCoordinate = mix(
        sourceRectangle.xy,
        sourceRectangle.zw,
        corner
    );
}
)glsl";

constexpr std::string_view presentationFragmentShader = R"glsl(
#version 410 core

uniform sampler2D sourceTexture;

in vec2 textureCoordinate;
out vec4 fragmentColor;

void main() {
    fragmentColor = texture(sourceTexture, textureCoordinate);
}
)glsl";

void validatePresentationDraw(
    const FramebufferResource& source,
    std::uint32_t destinationWidth,
    std::uint32_t destinationHeight,
    const PresentationSlice& slice,
    GLenum filter
) {
    if (!slice.hasContent) return;
    if (source.framebuffer == 0 || source.colorTexture == 0 ||
        source.width == 0 || source.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation requires a valid source framebuffer texture"
        );
    }
    if (destinationWidth == 0 || destinationHeight == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation requires non-zero destination dimensions"
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
    if (slice.destination.x > destinationWidth ||
        slice.destination.width >
            destinationWidth - slice.destination.x ||
        slice.destination.y > destinationHeight ||
        slice.destination.height >
            destinationHeight - slice.destination.y) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation destination slice exceeds the drawable"
        );
    }
    if (filter != GL_NEAREST && filter != GL_LINEAR) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation requires a nearest or linear OpenGL filter"
        );
    }
}

}  // namespace

void PresentationRenderer::initialize(Device::Session& session) {
    if (program_ != 0 && vertexArray_ != 0) return;
    release(session);
    try {
        program_ = session.createProgram(
            presentationVertexShader,
            presentationFragmentShader
        );
        vertexArray_ = session.createVertexArray();
        sourceTextureLocation_ = glGetUniformLocation(
            program_, "sourceTexture"
        );
        sourceRectangleLocation_ = glGetUniformLocation(
            program_, "sourceRectangle"
        );
        destinationRectangleLocation_ = glGetUniformLocation(
            program_, "destinationRectangle"
        );
        if (sourceTextureLocation_ < 0 || sourceRectangleLocation_ < 0 ||
            destinationRectangleLocation_ < 0) {
            throw Error(
                ErrorCode::programLink,
                "Presentation shader is missing a required uniform"
            );
        }
        session.checkError(
            ErrorCode::programLink,
            "Preparing the presentation shader"
        );
    } catch (...) {
        release(session);
        throw;
    }
}

void PresentationRenderer::draw(
    Device::Session& session,
    const FramebufferResource& source,
    GLuint destinationFramebuffer,
    GLenum destinationBuffer,
    std::uint32_t destinationWidth,
    std::uint32_t destinationHeight,
    const PresentationSlice& slice,
    GLenum filter
) {
    validatePresentationDraw(
        source,
        destinationWidth,
        destinationHeight,
        slice,
        filter
    );
    if (!slice.hasContent) return;
    initialize(session);

    const float inverseSourceWidth = 1.0F /
        static_cast<float>(source.width);
    const float inverseSourceHeight = 1.0F /
        static_cast<float>(source.height);
    const std::array<float, 4> sourceRectangle{
        static_cast<float>(slice.source.x) * inverseSourceWidth,
        static_cast<float>(source.height - slice.source.y) *
            inverseSourceHeight,
        static_cast<float>(slice.source.x + slice.source.width) *
            inverseSourceWidth,
        static_cast<float>(
            source.height - slice.source.y - slice.source.height
        ) * inverseSourceHeight,
    };
    const float inverseDestinationWidth = 1.0F /
        static_cast<float>(destinationWidth);
    const float inverseDestinationHeight = 1.0F /
        static_cast<float>(destinationHeight);
    const std::array<float, 4> destinationRectangle{
        static_cast<float>(slice.destination.x) * inverseDestinationWidth,
        static_cast<float>(slice.destination.y) * inverseDestinationHeight,
        static_cast<float>(slice.destination.width) * inverseDestinationWidth,
        static_cast<float>(slice.destination.height) * inverseDestinationHeight,
    };

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destinationFramebuffer);
    glDrawBuffer(destinationBuffer);
    glViewport(
        0,
        0,
        static_cast<GLsizei>(destinationWidth),
        static_cast<GLsizei>(destinationHeight)
    );
    glUseProgram(program_);
    glBindVertexArray(vertexArray_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source.colorTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glUniform1i(sourceTextureLocation_, 0);
    glUniform4fv(sourceRectangleLocation_, 1, sourceRectangle.data());
    glUniform4fv(
        destinationRectangleLocation_,
        1,
        destinationRectangle.data()
    );
    glDrawArrays(GL_TRIANGLES, 0, 6);
    session.checkError(
        ErrorCode::draw,
        "drawing the scene frame to the presentation target"
    );
}

void PresentationRenderer::release(Device::Session& session) noexcept {
    session.destroyVertexArray(vertexArray_);
    session.destroyProgram(program_);
    sourceTextureLocation_ = -1;
    sourceRectangleLocation_ = -1;
    destinationRectangleLocation_ = -1;
}

}  // namespace we::scene::gl
