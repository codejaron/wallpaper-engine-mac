#include "TextCoverageRenderer.hpp"

#include <cmath>
#include <list>
#include <unordered_map>

namespace we::scene::gl {
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

struct Key final {
    std::uint64_t a = 1469598103934665603ULL;
    std::uint64_t b = 1099511628211ULL;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const Key&) const = default;
};

struct KeyHash final {
    std::size_t operator()(const Key& key) const noexcept {
        return std::size_t(
            key.a ^ (key.b << 1) ^
            (std::uint64_t(key.width) << 32) ^ key.height
        );
    }
};

Key keyFor(const text::RasterizedText& value) {
    Key key{.width = value.width, .height = value.height};
    for (const auto byte : value.coverage) {
        key.a = (key.a ^ byte) * 1099511628211ULL;
        key.b = (key.b + byte + 0x9e3779b97f4a7c15ULL) *
            0xbf58476d1ce4e5b9ULL;
    }
    return key;
}

void validateCoverageLayout(const text::RasterizedText& value) {
    if (value.width == 0 || value.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Rasterized text coverage dimensions must be greater than zero"
        );
    }
    if (value.bytesPerRow < value.width ||
        std::size_t(value.bytesPerRow) * value.height != value.coverage.size()) {
        throw Error(
            ErrorCode::invalidArgument,
            "Rasterized text coverage layout is invalid"
        );
    }
}

void validateDestination(const FramebufferResource& destination) {
    if (destination.framebuffer == 0 || destination.width == 0 ||
        destination.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Text rendering requires a valid destination framebuffer"
        );
    }
}

void validateDrawRequest(const TextDrawRequest& request) {
    for (const float component : request.color) {
        if (!std::isfinite(component) || component < 0.0F || component > 1.0F) {
            throw Error(
                ErrorCode::invalidArgument,
                "Text color components must be finite values in [0, 1]"
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

}  // namespace

struct TextCoverageRenderer::Impl final {
    struct Entry final {
        GLuint texture = 0;
        std::size_t bytes = 0;
        std::list<Key>::iterator lru;
    };

    static constexpr std::size_t maxEntries = 64;
    static constexpr std::size_t maxBytes = 32 * 1024 * 1024;

    GLuint program = 0;
    GLuint vertexArray = 0;
    GLuint vertexBuffer = 0;
    GLint coverageTextureLocation = -1;
    GLint textColorLocation = -1;
    GLint modelViewProjectionLocation = -1;
    std::unordered_map<Key, Entry, KeyHash> textures;
    std::list<Key> recency;
    std::size_t cachedBytes = 0;
    std::uint64_t generation = 1;

    void invalidatePreparedHandles() noexcept {
        ++generation;
        if (generation == 0) ++generation;
    }

    void ensurePipeline(Device::Session& session) {
        if (program != 0) return;

        GLuint candidateProgram = 0;
        GLuint candidateVertexArray = 0;
        GLuint candidateVertexBuffer = 0;
        try {
            candidateProgram = session.createProgram(vertexShader, fragmentShader);
            candidateVertexArray = session.createVertexArray();
            candidateVertexBuffer = session.createBuffer();
            glBindVertexArray(candidateVertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, candidateVertexBuffer);
            glBufferData(
                GL_ARRAY_BUFFER,
                sizeof(Vertex) * 6,
                nullptr,
                GL_DYNAMIC_DRAW
            );
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(
                0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr
            );
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(
                1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                reinterpret_cast<void*>(sizeof(float) * 2)
            );
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindVertexArray(0);

            const GLint candidateCoverageTextureLocation =
                glGetUniformLocation(candidateProgram, "coverageTexture");
            const GLint candidateTextColorLocation =
                glGetUniformLocation(candidateProgram, "textColor");
            const GLint candidateModelViewProjectionLocation =
                glGetUniformLocation(candidateProgram, "modelViewProjection");
            if (candidateCoverageTextureLocation < 0 ||
                candidateTextColorLocation < 0 ||
                candidateModelViewProjectionLocation < 0) {
                throw Error(
                    ErrorCode::resourceValidation,
                    "Text coverage shader does not expose its required uniforms"
                );
            }
            session.checkError(
                ErrorCode::draw,
                "creating the text quad pipeline"
            );

            program = candidateProgram;
            vertexArray = candidateVertexArray;
            vertexBuffer = candidateVertexBuffer;
            coverageTextureLocation = candidateCoverageTextureLocation;
            textColorLocation = candidateTextColorLocation;
            modelViewProjectionLocation =
                candidateModelViewProjectionLocation;
        } catch (...) {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindVertexArray(0);
            session.destroyBuffer(candidateVertexBuffer);
            session.destroyVertexArray(candidateVertexArray);
            session.destroyProgram(candidateProgram);
            throw;
        }
    }
};

TextCoverageRenderer::TextCoverageRenderer()
    : impl_(std::make_unique<Impl>()) {}

TextCoverageRenderer::~TextCoverageRenderer() = default;

PreparedTextCoverage TextCoverageRenderer::prepare(
    Device::Session& session,
    const text::RasterizedText& value
) {
    validateCoverageLayout(value);

    impl_->ensurePipeline(session);

    const Key key = keyFor(value);
    auto found = impl_->textures.find(key);
    if (found != impl_->textures.end()) {
        impl_->recency.splice(
            impl_->recency.begin(), impl_->recency, found->second.lru
        );
        found->second.lru = impl_->recency.begin();
        return {
            .texture = found->second.texture,
            .width = value.width,
            .height = value.height,
            .owner = impl_.get(),
            .generation = impl_->generation,
        };
    }

    const std::size_t bytes = value.coverage.size();
    GLuint texture = session.uploadCoverageTexture(
        value.width,
        value.height,
        value.bytesPerRow,
        value.coverage
    );
    try {
        impl_->recency.push_front(key);
        try {
            const auto [inserted, didInsert] = impl_->textures.emplace(
                key,
                Impl::Entry{
                    .texture = texture,
                    .bytes = bytes,
                    .lru = impl_->recency.begin(),
                }
            );
            if (!didInsert) {
                impl_->recency.pop_front();
                session.destroyTexture(texture);
                impl_->recency.splice(
                    impl_->recency.begin(),
                    impl_->recency,
                    inserted->second.lru
                );
                inserted->second.lru = impl_->recency.begin();
                return {
                    .texture = inserted->second.texture,
                    .width = value.width,
                    .height = value.height,
                    .owner = impl_.get(),
                    .generation = impl_->generation,
                };
            }
            impl_->cachedBytes += bytes;
        } catch (...) {
            impl_->recency.pop_front();
            throw;
        }
    } catch (...) {
        session.destroyTexture(texture);
        throw;
    }

    return {
        .texture = texture,
        .width = value.width,
        .height = value.height,
        .owner = impl_.get(),
        .generation = impl_->generation,
    };
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
    if (text.texture == 0 || text.width == 0 || text.height == 0) {
        throw Error(
            ErrorCode::invalidArgument,
            "Prepared text coverage is incomplete"
        );
    }
    if (impl_->program == 0 || impl_->vertexArray == 0 ||
        impl_->vertexBuffer == 0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text coverage pipeline has not been prepared"
        );
    }
    validateDrawRequest(request);

    const float width = float(text.width);
    const float height = float(text.height);
    // CoreGraphics coverage is top-down, matching every other Wallpaper Engine
    // source texture. Keep v=0 on the local bottom edge; presentation performs
    // the one global vertical correction for the completed scene.
    const Vertex vertices[] = {
        {0, 0, 0, 0},
        {0, height, 0, 1},
        {width, height, 1, 1},
        {0, 0, 0, 0},
        {width, height, 1, 1},
        {width, 0, 1, 0},
    };
    glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
    glViewport(0, 0, destination.width, destination.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(
        GL_SRC_ALPHA,
        GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE,
        GL_ONE_MINUS_SRC_ALPHA
    );
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(impl_->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, text.texture);
    glUniform1i(impl_->coverageTextureLocation, 0);
    glUniform4fv(impl_->textColorLocation, 1, request.color.data());
    glUniformMatrix4fv(
        impl_->modelViewProjectionLocation,
        1,
        GL_FALSE,
        request.modelViewProjection.data()
    );
    glBindVertexArray(impl_->vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, impl_->vertexBuffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    session.checkError(ErrorCode::draw, "drawing text coverage");
}

void TextCoverageRenderer::draw(
    Device::Session& session,
    const FramebufferResource& destination,
    const text::RasterizedText& value,
    const TextDrawRequest& request
) {
    // Preserve draw()'s validation order and avoid populating the cache for a
    // request that cannot be drawn. Direct preflight callers validate their
    // draw operation independently before calling prepare().
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
        const Key oldest = impl_->recency.back();
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
    session.destroyVertexArray(impl_->vertexArray);
    session.destroyProgram(impl_->program);
    impl_->coverageTextureLocation = -1;
    impl_->textColorLocation = -1;
    impl_->modelViewProjectionLocation = -1;
}

std::size_t TextCoverageRenderer::cachedTextureCount() const noexcept {
    return impl_->textures.size();
}

}  // namespace we::scene::gl
