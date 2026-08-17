#include "SceneMetalPresentation.hpp"

#include <SceneShader/ShaderCompiler.hpp>

#include <array>
#include <string_view>

namespace we::scene::metal {
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
    textureCoordinate = mix(sourceRectangle.xy, sourceRectangle.zw, corner);
}
)glsl";

constexpr std::string_view presentationFragmentShader = R"glsl(
#version 410 core

uniform sampler2D sourceTexture;

in vec2 textureCoordinate;
out vec4 fragmentColor;

void main() {
    // Intermediate Scene alpha is not the alpha contract of the desktop
    // surface. The final wallpaper drawable is opaque on both platforms.
    fragmentColor = vec4(texture(sourceTexture, textureCoordinate).rgb, 1.0);
}
)glsl";

void validatePresentationDraw(
    const FramebufferResource& source,
    const FramebufferResource& destination,
    const PresentationSlice& slice
) {
    if (!slice.hasContent) return;
    if (!source || source.width == 0 || source.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation requires a valid source framebuffer texture"
        );
    }
    if (!destination || destination.width == 0 || destination.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation requires a valid destination framebuffer texture"
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
    if (slice.destination.x > destination.width ||
        slice.destination.width > destination.width - slice.destination.x ||
        slice.destination.y > destination.height ||
        slice.destination.height > destination.height - slice.destination.y) {
        throw Error(
            ErrorCode::invalidArgument,
            "Presentation destination slice exceeds the drawable"
        );
    }
}

std::uint32_t uniformIndex(
    const TranslatedMetalShaderPair& shaders,
    std::string_view name
) {
    for (const auto& binding : shaders.vertexUniforms) {
        if (binding.name == name) return binding.bufferIndex;
    }
    throw Error(
        ErrorCode::programLink,
        "Presentation shader is missing vertex uniform " + std::string(name)
    );
}

const TranslatedMetalShaderPair::TextureBinding& textureBinding(
    const TranslatedMetalShaderPair& shaders,
    std::string_view name
) {
    for (const auto& binding : shaders.fragmentTextures) {
        if (binding.name == name) return binding;
    }
    throw Error(
        ErrorCode::programLink,
        "Presentation shader is missing fragment texture " + std::string(name)
    );
}

}  // namespace

void PresentationRenderer::initialize(Device::Session& session) {
    if (program_) return;
    const auto shaders = ShaderCompiler::translateToMetal(
        presentationVertexShader,
        presentationFragmentShader,
        "presentation vertex",
        "presentation fragment"
    );
    sourceRectangleBufferIndex_ = uniformIndex(shaders, "sourceRectangle");
    destinationRectangleBufferIndex_ = uniformIndex(
        shaders, "destinationRectangle"
    );
    const auto& sourceTexture = textureBinding(shaders, "sourceTexture");
    sourceTextureIndex_ = sourceTexture.textureIndex;
    sourceSamplerIndex_ = sourceTexture.samplerIndex;
    program_ = session.createProgram(shaders);
}

void PresentationRenderer::draw(
    Device::Session& session,
    const FramebufferResource& source,
    FramebufferResource& destination,
    const PresentationSlice& slice,
    TextureFilter filter
) {
    validatePresentationDraw(source, destination, slice);
    if (!slice.hasContent) return;
    initialize(session);

    const float inverseSourceWidth = 1.0F / source.width;
    const float inverseSourceHeight = 1.0F / source.height;
    const std::array<float, 4> sourceRectangle{
        slice.source.x * inverseSourceWidth,
        (source.height - slice.source.y - slice.source.height) *
            inverseSourceHeight,
        (slice.source.x + slice.source.width) * inverseSourceWidth,
        (source.height - slice.source.y) * inverseSourceHeight,
    };
    const float inverseDestinationWidth = 1.0F / destination.width;
    const float inverseDestinationHeight = 1.0F / destination.height;
    const std::array<float, 4> destinationRectangle{
        slice.destination.x * inverseDestinationWidth,
        (destination.height - slice.destination.y -
         slice.destination.height) * inverseDestinationHeight,
        slice.destination.width * inverseDestinationWidth,
        slice.destination.height * inverseDestinationHeight,
    };

    DrawRequest request;
    request.program = program_;
    request.destination = &destination;
    request.uniforms = {
        UniformBytesBinding{
            .vertexBufferIndex = sourceRectangleBufferIndex_,
            .bytes = sourceRectangle.data(),
            .byteCount = sizeof(sourceRectangle),
        },
        UniformBytesBinding{
            .vertexBufferIndex = destinationRectangleBufferIndex_,
            .bytes = destinationRectangle.data(),
            .byteCount = sizeof(destinationRectangle),
        },
    };
    request.textures = {
        TextureStageBinding{
            .texture = &source.colorTexture,
            .filterOverride = filter,
            .fragmentTextureIndex = sourceTextureIndex_,
            .fragmentSamplerIndex = sourceSamplerIndex_,
        },
    };
    session.draw(request, 0, 6);
}

void PresentationRenderer::release(Device::Session& session) noexcept {
    session.destroyProgram(program_);
    sourceTextureIndex_ = 0;
    sourceSamplerIndex_ = 0;
    sourceRectangleBufferIndex_ = 0;
    destinationRectangleBufferIndex_ = 0;
}

}  // namespace we::scene::metal
