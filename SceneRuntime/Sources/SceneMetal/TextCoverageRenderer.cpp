#include "TextCoverageRenderer.hpp"

#include <SceneShader/ShaderCompiler.hpp>

#include <cmath>
#include <list>
#include <unordered_map>

namespace we::scene::metal {
namespace {

constexpr std::string_view vertexShader = R"GLSL(#version 410 core
layout(location=0) in vec2 position; layout(location=1) in vec2 texCoord;
uniform mat4 modelViewProjection; out vec2 uv;
void main(){ gl_Position=modelViewProjection*vec4(position,0.0,1.0); uv=texCoord; }
)GLSL";
constexpr std::string_view fragmentShader = R"GLSL(#version 410 core
uniform sampler2D coverageTexture; uniform vec4 textColor; in vec2 uv; out vec4 fragmentColor;
void main(){ float c=texture(coverageTexture,uv).r; fragmentColor=vec4(textColor.rgb,textColor.a*c); }
)GLSL";

struct Vertex final {
    float x;
    float y;
    float u;
    float v;
};

constexpr std::array<Vertex, 6> coverageQuad{{
    {0, 0, 0, 0},
    {0, 1, 0, 1},
    {1, 1, 1, 1},
    {0, 0, 0, 0},
    {1, 1, 1, 1},
    {1, 0, 1, 0},
}};

struct KeyHash final {
    std::size_t operator()(const TextCoverageKey& key) const noexcept {
        return std::size_t(
            key.a ^ (key.b << 1) ^
            (std::uint64_t(key.width) << 32) ^ key.height
        );
    }
};

void validateCoverageLayout(const text::RasterizedText& value) {
    if (value.width == 0 || value.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Rasterized text coverage dimensions must be greater than zero"
        );
    }
    if (value.bytesPerRow < value.width ||
        std::size_t(value.bytesPerRow) * value.height !=
            value.coverage.size()) {
        throw Error(
            ErrorCode::invalidArgument,
            "Rasterized text coverage layout is invalid"
        );
    }
}

void validateDestination(const FramebufferResource& destination) {
    if (!destination || destination.width == 0 || destination.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Text rendering requires a valid Metal destination"
        );
    }
}

void validateDrawRequest(const TextDrawRequest& request) {
    for (const float component : request.color) {
        if (!std::isfinite(component)) {
            throw Error(
                ErrorCode::invalidArgument,
                "Text color components must be finite"
            );
        }
    }
    for (const float component : request.modelViewProjection) {
        if (!std::isfinite(component)) {
            throw Error(
                ErrorCode::invalidArgument,
                "Text transform must be finite"
            );
        }
    }
}

const TranslatedMetalShaderPair::UniformBinding& requireUniform(
    const std::vector<TranslatedMetalShaderPair::UniformBinding>& uniforms,
    std::string_view name,
    TranslatedMetalShaderPair::ValueType type
) {
    const auto found = std::find_if(
        uniforms.begin(), uniforms.end(),
        [name](const auto& value) { return value.name == name; }
    );
    if (found == uniforms.end() || found->type != type ||
        found->arrayLength != 1 || found->uniformBlock) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text Metal shader has an incompatible uniform '" +
                std::string(name) + "'"
        );
    }
    return *found;
}

const TranslatedMetalShaderPair::TextureBinding& requireTexture(
    const std::vector<TranslatedMetalShaderPair::TextureBinding>& textures,
    std::string_view name
) {
    const auto found = std::find_if(
        textures.begin(), textures.end(),
        [name](const auto& value) { return value.name == name; }
    );
    if (found == textures.end()) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text Metal shader is missing texture '" + std::string(name) + "'"
        );
    }
    return *found;
}

std::uint32_t requireAttribute(
    const TranslatedMetalShaderPair& shaders,
    std::string_view name,
    std::uint32_t componentCount
) {
    const auto found = std::find_if(
        shaders.vertexAttributes.begin(), shaders.vertexAttributes.end(),
        [name](const auto& value) { return value.name == name; }
    );
    if (found == shaders.vertexAttributes.end() ||
        found->componentCount != componentCount) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text Metal shader has an incompatible attribute '" +
                std::string(name) + "'"
        );
    }
    return found->location;
}

}  // namespace

struct TextCoverageRenderer::Impl final {
    struct Entry final {
        TextureResource texture;
        std::size_t bytes = 0;
        std::list<TextCoverageKey>::iterator lru;
    };

    static constexpr std::size_t maxEntries = 64;
    static constexpr std::size_t maxBytes = 32 * 1024 * 1024;

    std::shared_ptr<Program> program;
    BufferResource vertexBuffer;
    std::uint32_t positionLocation = 0;
    std::uint32_t texCoordLocation = 1;
    std::uint32_t coverageTextureIndex = 0;
    std::uint32_t coverageSamplerIndex = 0;
    std::uint32_t textColorBufferIndex = 0;
    std::uint32_t modelViewProjectionBufferIndex = 0;
    std::unordered_map<TextCoverageKey, Entry, KeyHash> textures;
    std::list<TextCoverageKey> recency;
    std::size_t cachedBytes = 0;
    std::uint64_t generation = 1;

    void invalidatePreparedHandles() noexcept {
        ++generation;
        if (generation == 0) ++generation;
    }

    void ensurePipeline(Device::Session& session) {
        if (program) return;
        const TranslatedMetalShaderPair translated =
            ShaderCompiler::translateToMetal(
                vertexShader,
                fragmentShader,
                "text-coverage.vert",
                "text-coverage.frag"
            );
        const auto& matrix = requireUniform(
            translated.vertexUniforms,
            "modelViewProjection",
            TranslatedMetalShaderPair::ValueType::float4x4
        );
        const auto& color = requireUniform(
            translated.fragmentUniforms,
            "textColor",
            TranslatedMetalShaderPair::ValueType::float4
        );
        const auto& texture = requireTexture(
            translated.fragmentTextures,
            "coverageTexture"
        );
        positionLocation = requireAttribute(translated, "position", 2);
        texCoordLocation = requireAttribute(translated, "texCoord", 2);
        modelViewProjectionBufferIndex = matrix.bufferIndex;
        textColorBufferIndex = color.bufferIndex;
        coverageTextureIndex = texture.textureIndex;
        coverageSamplerIndex = texture.samplerIndex;
        program = session.createProgram(translated);
        session.uploadBuffer(
            vertexBuffer,
            std::as_bytes(std::span(coverageQuad))
        );
    }
};

TextCoverageRenderer::TextCoverageRenderer()
    : impl_(std::make_unique<Impl>()) {}

TextCoverageRenderer::~TextCoverageRenderer() = default;

PreparedTextCoverage TextCoverageRenderer::prepare(
    Device::Session& session,
    const text::RasterizedText& value
) {
    return prepare(session, value, keyFor(value));
}

TextCoverageKey TextCoverageRenderer::keyFor(
    const text::RasterizedText& value
) {
    validateCoverageLayout(value);
    TextCoverageKey key{.width = value.width, .height = value.height};
    for (const auto byte : value.coverage) {
        key.a = (key.a ^ byte) * 1099511628211ULL;
        key.b = (key.b + byte + 0x9e3779b97f4a7c15ULL) *
            0xbf58476d1ce4e5b9ULL;
    }
    return key;
}

PreparedTextCoverage TextCoverageRenderer::prepare(
    Device::Session& session,
    const text::RasterizedText& value,
    const TextCoverageKey& key
) {
    validateCoverageLayout(value);
    if (key.width != value.width || key.height != value.height) {
        throw Error(
            ErrorCode::invalidArgument,
            "Text coverage key dimensions do not match the raster"
        );
    }
    impl_->ensurePipeline(session);

    auto found = impl_->textures.find(key);
    if (found != impl_->textures.end()) {
        impl_->recency.splice(
            impl_->recency.begin(), impl_->recency, found->second.lru
        );
        found->second.lru = impl_->recency.begin();
        return {
            .texture = &found->second.texture,
            .width = value.width,
            .height = value.height,
            .owner = impl_.get(),
            .generation = impl_->generation,
        };
    }

    const std::size_t bytes = value.coverage.size();
    TextureResource texture = session.uploadCoverageTexture(
        value.width,
        value.height,
        value.bytesPerRow,
        value.coverage
    );
    impl_->recency.push_front(key);
    try {
        const auto [inserted, didInsert] = impl_->textures.emplace(
            key,
            Impl::Entry{
                .texture = std::move(texture),
                .bytes = bytes,
                .lru = impl_->recency.begin(),
            }
        );
        if (!didInsert) {
            impl_->recency.pop_front();
            impl_->recency.splice(
                impl_->recency.begin(),
                impl_->recency,
                inserted->second.lru
            );
            inserted->second.lru = impl_->recency.begin();
            return {
                .texture = &inserted->second.texture,
                .width = value.width,
                .height = value.height,
                .owner = impl_.get(),
                .generation = impl_->generation,
            };
        }
        impl_->cachedBytes += bytes;
        return {
            .texture = &inserted->second.texture,
            .width = value.width,
            .height = value.height,
            .owner = impl_.get(),
            .generation = impl_->generation,
        };
    } catch (...) {
        impl_->recency.pop_front();
        throw;
    }
}

void TextCoverageRenderer::drawPrepared(
    Device::Session& session,
    const FramebufferResource& destination,
    const PreparedTextCoverage& text,
    const TextDrawRequest& request
) {
    validateDestination(destination);
    if (text.owner != impl_.get() || text.generation != impl_->generation) {
        throw Error(
            ErrorCode::invalidArgument,
            "Prepared text coverage handle is stale or belongs to another renderer"
        );
    }
    if (text.texture == nullptr || !*text.texture || text.width == 0 ||
        text.height == 0 || !impl_->program || !impl_->vertexBuffer.buffer) {
        throw Error(
            ErrorCode::invalidArgument,
            "Prepared text coverage is incomplete"
        );
    }
    validateDrawRequest(request);

    const float width = float(text.width);
    const float height = float(text.height);
    std::array<float, 16> scaledModelViewProjection =
        request.modelViewProjection;
    for (std::size_t row = 0; row < 4; ++row) {
        scaledModelViewProjection[row] *= width;
        scaledModelViewProjection[4 + row] *= height;
        if (!std::isfinite(scaledModelViewProjection[row]) ||
            !std::isfinite(scaledModelViewProjection[4 + row])) {
            throw Error(
                ErrorCode::invalidArgument,
                "Scaled text transform must be finite"
            );
        }
    }

    auto& mutableDestination =
        const_cast<FramebufferResource&>(destination);
    DrawRequest draw{
        .program = impl_->program,
        .destination = &mutableDestination,
        .state = {
            .blending = BlendMode::alpha,
            .writeAlpha = true,
            .alphaSourceOne = true,
        },
        .vertexLayout = {
            .stride = sizeof(Vertex),
            .attributes = {
                {
                    .location = impl_->positionLocation,
                    .format = VertexFormat::float2,
                    .offset = offsetof(Vertex, x),
                },
                {
                    .location = impl_->texCoordLocation,
                    .format = VertexFormat::float2,
                    .offset = offsetof(Vertex, u),
                },
            },
        },
        .vertexBuffer = &impl_->vertexBuffer,
        .uniforms = {
            {
                .vertexBufferIndex =
                    impl_->modelViewProjectionBufferIndex,
                .bytes = scaledModelViewProjection.data(),
                .byteCount = sizeof(scaledModelViewProjection),
            },
            {
                .fragmentBufferIndex = impl_->textColorBufferIndex,
                .bytes = request.color.data(),
                .byteCount = sizeof(request.color),
            },
        },
        .textures = {
            {
                .texture = text.texture,
                .fragmentTextureIndex = impl_->coverageTextureIndex,
                .fragmentSamplerIndex = impl_->coverageSamplerIndex,
            },
        },
    };
    session.draw(draw, 0, 6);
}

void TextCoverageRenderer::draw(
    Device::Session& session,
    const FramebufferResource& destination,
    const text::RasterizedText& value,
    const TextDrawRequest& request
) {
    validateDestination(destination);
    validateCoverageLayout(value);
    validateDrawRequest(request);
    const PreparedTextCoverage prepared = prepare(session, value);
    try {
        drawPrepared(session, destination, prepared, request);
    } catch (...) {
        trimCache(session);
        throw;
    }
    trimCache(session);
}

void TextCoverageRenderer::trimCache(Device::Session& session) {
    bool evicted = false;
    while (impl_->textures.size() > Impl::maxEntries ||
           impl_->cachedBytes > Impl::maxBytes) {
        if (impl_->recency.empty()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Text coverage cache metadata is inconsistent"
            );
        }
        const TextCoverageKey oldest = impl_->recency.back();
        const auto victim = impl_->textures.find(oldest);
        if (victim == impl_->textures.end() ||
            victim->second.bytes > impl_->cachedBytes) {
            throw Error(
                ErrorCode::resourceValidation,
                "Text coverage cache metadata is inconsistent"
            );
        }
        session.destroyTexture(victim->second.texture);
        impl_->cachedBytes -= victim->second.bytes;
        impl_->recency.pop_back();
        impl_->textures.erase(victim);
        evicted = true;
    }
    if (evicted) impl_->invalidatePreparedHandles();
}

void TextCoverageRenderer::release(Device::Session& session) noexcept {
    for (auto& [key, entry] : impl_->textures) {
        static_cast<void>(key);
        session.destroyTexture(entry.texture);
    }
    impl_->textures.clear();
    impl_->recency.clear();
    impl_->cachedBytes = 0;
    impl_->invalidatePreparedHandles();
    session.destroyBuffer(impl_->vertexBuffer);
    session.destroyProgram(impl_->program);
}

std::size_t TextCoverageRenderer::cachedTextureCount() const noexcept {
    return impl_->textures.size();
}

}  // namespace we::scene::metal
