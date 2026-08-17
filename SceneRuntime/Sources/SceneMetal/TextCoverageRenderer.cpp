#include "TextCoverageRenderer.hpp"

#include <SceneShader/ShaderCompiler.hpp>
#include <SceneShader/ShaderPreprocessor.hpp>

#include <cmath>
#include <algorithm>
#include <limits>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

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

constexpr float officialMsdfRange = 24.0F;
constexpr float maximumOutlineThickness = 5.1F;
constexpr float maximumFontEffectExtent = 6.0F;

struct Vertex final {
    float x;
    float y;
    float z;
    float u;
    float v;
};

constexpr std::array<Vertex, 6> coverageQuad{{
    {0, 0, 0, 0, 0},
    {0, 1, 0, 0, 1},
    {1, 1, 0, 1, 1},
    {0, 0, 0, 0, 0},
    {1, 1, 0, 1, 1},
    {1, 0, 0, 1, 0},
}};

struct KeyHash final {
    std::size_t operator()(const TextCoverageKey& key) const noexcept {
        return std::size_t(
            key.a ^ (key.b << 1) ^
            (std::uint64_t(key.width) << 32) ^ key.height ^
            (std::uint64_t(key.padding) << 17) ^ key.signedDistanceRange
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

struct SignedDistanceImage final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t padding = 0;
    std::uint32_t range = 0;
    std::vector<std::uint8_t> pixels;
};

TextFontStyle normalizedFontStyle(const TextFontStyle& source) {
    const std::array<float, 13> values{
        source.blurSize,
        source.outlineThickness,
        source.outlineColor[0],
        source.outlineColor[1],
        source.outlineColor[2],
        source.dropShadowSize,
        source.dropShadowColor[0],
        source.dropShadowColor[1],
        source.dropShadowColor[2],
        source.dropShadowOffset[0],
        source.dropShadowOffset[1],
        source.dropShadowOpacity,
        officialMsdfRange,
    };
    if (std::ranges::any_of(values, [](float value) {
            return !std::isfinite(value);
        }) || source.blurSize < 0.0F ||
        source.outlineThickness < 0.0F || source.dropShadowSize < 0.0F ||
        source.dropShadowOpacity < 0.0F) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text font effect values must be finite and non-negative"
        );
    }

    TextFontStyle result = source;
    // Match FUN_1401b3b60: the official material combo is selected from the
    // effective scalar values, not from the four serialized object flags.
    result.outline = result.outlineThickness >= 1.0F;
    result.blur = result.blurSize > 0.0F;
    result.dropShadow = result.dropShadowSize > 0.0F ||
        result.dropShadowOffset[0] * result.dropShadowOffset[0] +
            result.dropShadowOffset[1] * result.dropShadowOffset[1] >
                std::numeric_limits<float>::epsilon();
    // Keep the effective values in the RenderVar payload even when a combo is
    // disabled. The official writer selects the combo first, then clamps the
    // values; inactive fields are ignored by the shader but remain ABI data.
    result.blurSize = std::min(result.blurSize, maximumFontEffectExtent);
    result.outlineThickness = std::min(
        result.outlineThickness, maximumOutlineThickness
    );
    result.dropShadowSize = std::min(
        result.dropShadowSize, maximumFontEffectExtent
    );
    for (float& component : result.dropShadowOffset) {
        component = std::clamp(
            component,
            -maximumFontEffectExtent,
            maximumFontEffectExtent
        );
    }
    if (result.outlineThickness + result.blurSize >
        maximumOutlineThickness) {
        result.outlineThickness = std::max(
            maximumOutlineThickness - result.blurSize,
            0.0F
        );
    }
    return result;
}

std::uint32_t checkedEffectExtent(const TextFontStyle& style) {
    const double shadowExtent = style.dropShadowSize +
        std::abs(style.dropShadowOffset[0]) +
        std::abs(style.dropShadowOffset[1]);
    const double extent = std::max({
        0.0,
        static_cast<double>(style.blurSize),
        static_cast<double>(style.outlineThickness),
        shadowExtent,
    });
    if (!std::isfinite(extent) || extent > 4096.0) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text font effect extent must be finite and no greater than 4096"
        );
    }
    return static_cast<std::uint32_t>(std::ceil(extent)) + 2U;
}

SignedDistanceImage makeSignedDistanceImage(
    const text::RasterizedText& value,
    const TextFontStyle& style
) {
    validateCoverageLayout(value);
    const std::uint32_t padding = checkedEffectExtent(style);
    const std::uint32_t range = static_cast<std::uint32_t>(officialMsdfRange);
    if (value.width > std::numeric_limits<std::uint32_t>::max() - padding * 2U ||
        value.height > std::numeric_limits<std::uint32_t>::max() - padding * 2U) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text font effect image dimensions overflow 32-bit limits"
        );
    }
    const std::uint32_t width = value.width + padding * 2U;
    const std::uint32_t height = value.height + padding * 2U;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    if (height != 0 && pixelCount / height != width) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text font effect image dimensions overflow host limits"
        );
    }
    constexpr float diagonal = 1.4142135623730951F;
    const float infinity = std::numeric_limits<float>::max() / 4.0F;
    std::vector<float> inside(pixelCount, infinity);
    std::vector<float> outside(pixelCount, infinity);
    const auto index = [width](std::uint32_t x, std::uint32_t y) {
        return static_cast<std::size_t>(y) * width + x;
    };
    for (std::uint32_t y = 0; y < value.height; ++y) {
        for (std::uint32_t x = 0; x < value.width; ++x) {
            const std::uint8_t coverage = value.coverage[
                static_cast<std::size_t>(y) * value.bytesPerRow + x
            ];
            const std::size_t target = index(x + padding, y + padding);
            if (coverage >= 128U) {
                inside[target] = 0.0F;
            } else {
                outside[target] = 0.0F;
            }
        }
    }
    const auto relax = [width, height, diagonal](std::vector<float>& distance) {
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t current = static_cast<std::size_t>(y) * width + x;
                float best = distance[current];
                if (x > 0) best = std::min(best, distance[current - 1] + 1.0F);
                if (y > 0) best = std::min(best, distance[current - width] + 1.0F);
                if (x > 0 && y > 0) {
                    best = std::min(best, distance[current - width - 1] + diagonal);
                }
                if (x + 1 < width && y > 0) {
                    best = std::min(best, distance[current - width + 1] + diagonal);
                }
                distance[current] = best;
            }
        }
        for (std::uint32_t y = height; y-- > 0;) {
            for (std::uint32_t x = width; x-- > 0;) {
                const std::size_t current = static_cast<std::size_t>(y) * width + x;
                float best = distance[current];
                if (x + 1 < width) best = std::min(best, distance[current + 1] + 1.0F);
                if (y + 1 < height) best = std::min(best, distance[current + width] + 1.0F);
                if (x + 1 < width && y + 1 < height) {
                    best = std::min(best, distance[current + width + 1] + diagonal);
                }
                if (x > 0 && y + 1 < height) {
                    best = std::min(best, distance[current + width - 1] + diagonal);
                }
                distance[current] = best;
            }
        }
    };
    relax(inside);
    relax(outside);
    std::vector<std::uint8_t> pixels(pixelCount * 4U, 0U);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = index(x, y);
            const bool isInside = inside[at] == 0.0F;
            const float distance = std::min(
                isInside ? outside[at] : inside[at],
                static_cast<float>(range)
            );
            const float encoded = std::clamp(
                0.5F + (isInside ? distance : -distance) /
                    static_cast<float>(range),
                0.0F,
                1.0F
            );
            const auto byte = static_cast<std::uint8_t>(std::lround(
                encoded * 255.0F
            ));
            pixels[at * 4U + 0U] = byte;
            pixels[at * 4U + 1U] = byte;
            pixels[at * 4U + 2U] = byte;
            pixels[at * 4U + 3U] = 255U;
        }
    }
    return {
        .width = width,
        .height = height,
        .padding = padding,
        .range = range,
        .pixels = std::move(pixels),
    };
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

const TranslatedMetalShaderPair::UniformBinding* findUniform(
    const std::vector<TranslatedMetalShaderPair::UniformBinding>& uniforms,
    std::string_view name,
    TranslatedMetalShaderPair::ValueType type
) {
    const auto found = std::find_if(
        uniforms.begin(), uniforms.end(),
        [name](const auto& value) { return value.name == name; }
    );
    if (found == uniforms.end()) return nullptr;
    if (found->type != type || found->arrayLength != 1 ||
        found->uniformBlock) {
        throw Error(
            ErrorCode::resourceValidation,
            "Text Metal shader has an incompatible uniform '" +
                std::string(name) + "'"
        );
    }
    return &*found;
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
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t padding = 0;
        std::uint32_t signedDistanceRange = 0;
        std::list<TextCoverageKey>::iterator lru;
    };

    static constexpr std::size_t maxEntries = 64;
    static constexpr std::size_t maxBytes = 32 * 1024 * 1024;

    struct Pipeline final {
        std::shared_ptr<Program> program;
        std::uint32_t positionLocation = 0;
        std::uint32_t texCoordLocation = 1;
        std::uint32_t textureIndex = 0;
        std::uint32_t samplerIndex = 0;
        std::uint32_t colorBufferIndex = 0;
        std::uint32_t modelViewProjectionBufferIndex = 0;
        std::optional<std::uint32_t> textureResolutionBufferIndex;
        std::array<std::optional<std::uint32_t>, 4> renderVarBufferIndexes;
        bool positionFloat3 = false;
    };

    const AssetResolver* resolver = nullptr;
    BufferResource vertexBuffer;
    std::map<std::uint8_t, Pipeline> pipelines;
    std::unordered_map<TextCoverageKey, Entry, KeyHash> textures;
    std::list<TextCoverageKey> recency;
    std::size_t cachedBytes = 0;
    std::uint64_t generation = 1;

    void invalidatePreparedHandles() noexcept {
        ++generation;
        if (generation == 0) ++generation;
    }

    explicit Impl(const AssetResolver* assetResolver = nullptr)
        : resolver(assetResolver) {}

    [[nodiscard]] static std::uint8_t pipelineKey(
        const TextFontStyle& style
    ) noexcept {
        const bool outline = style.outline;
        const bool blur = style.blur;
        const bool dropShadow = style.dropShadow;
        const bool signedDistance = style.msdf || outline || blur || dropShadow;
        return static_cast<std::uint8_t>(
            (signedDistance ? 1U : 0U) |
            (blur ? 2U : 0U) |
            (outline ? 4U : 0U) |
            (dropShadow ? 8U : 0U)
        );
    }

    Pipeline& ensurePipeline(
        Device::Session& session,
        const TextFontStyle& style
    ) {
        if (resolver == nullptr && style.requiresSignedDistance()) {
            throw Error(
                ErrorCode::resourceValidation,
                "Signed-distance text rendering requires the official font assets"
            );
        }
        const bool officialPipeline = resolver != nullptr &&
            style.requiresSignedDistance();
        const std::uint8_t key = !officialPipeline
            ? 0U : pipelineKey(style);
        if (auto found = pipelines.find(key); found != pipelines.end()) {
            return found->second;
        }
        TranslatedMetalShaderPair translated;
        if (!officialPipeline) {
            translated = ShaderCompiler::translateToMetal(
                vertexShader,
                fragmentShader,
                "text-coverage.vert",
                "text-coverage.frag"
            );
        } else {
            const std::uint8_t effective = pipelineKey(style);
            ShaderPreprocessOptions options;
            options.combos = {
                {"MSDF", (effective & 1U) != 0U ? 1 : 0},
                {"BLUR_ENABLED", (effective & 2U) != 0U ? 1 : 0},
                {"OUTLINE_ENABLED", (effective & 4U) != 0U ? 1 : 0},
                {"DROP_SHADOW_ENABLED", (effective & 8U) != 0U ? 1 : 0},
                {"COLORFONT", 0},
            };
            const ShaderPreprocessor preprocessor(*resolver);
            const PreprocessedShaderPair preprocessed =
                preprocessor.preprocessFiles(
                    "shaders/font.vert",
                    "shaders/font.frag",
                    options
                );
            translated = ShaderCompiler::translateToMetal(
                preprocessed.vertex.source,
                preprocessed.fragment.source,
                preprocessed.vertex.name,
                preprocessed.fragment.name
            );
        }
        const std::string_view matrixName = !officialPipeline
            ? "modelViewProjection"
            : "g_ModelViewProjectionMatrix";
        const std::string_view colorName = !officialPipeline
            ? "textColor" : "g_Color4";
        const std::string_view textureName = !officialPipeline
            ? "coverageTexture" : "g_Texture0";
        const std::string_view positionName = !officialPipeline
            ? "position" : "a_Position";
        const std::string_view texCoordName = !officialPipeline
            ? "texCoord" : "a_TexCoord";
        const auto& matrix = requireUniform(
            translated.vertexUniforms,
            matrixName,
            TranslatedMetalShaderPair::ValueType::float4x4
        );
        const auto& color = requireUniform(
            translated.fragmentUniforms,
            colorName,
            TranslatedMetalShaderPair::ValueType::float4
        );
        const auto& texture = requireTexture(
            translated.fragmentTextures,
            textureName
        );
        Pipeline pipeline{
            .program = session.createProgram(translated),
            .positionLocation = requireAttribute(
                translated, positionName, !officialPipeline ? 2U : 3U
            ),
            .texCoordLocation = requireAttribute(
                translated, texCoordName, 2
            ),
            .textureIndex = texture.textureIndex,
            .samplerIndex = texture.samplerIndex,
            .colorBufferIndex = color.bufferIndex,
            .modelViewProjectionBufferIndex = matrix.bufferIndex,
            .positionFloat3 = officialPipeline,
        };
        if (officialPipeline) {
            if (const auto* resolution = findUniform(
                    translated.fragmentUniforms,
                    "g_Texture0Resolution",
                    TranslatedMetalShaderPair::ValueType::float4
                )) {
                pipeline.textureResolutionBufferIndex =
                    resolution->bufferIndex;
            }
            for (std::size_t index = 0;
                 index < pipeline.renderVarBufferIndexes.size(); ++index) {
                if (const auto* renderVar = findUniform(
                        translated.fragmentUniforms,
                        "g_RenderVar" + std::to_string(index),
                        TranslatedMetalShaderPair::ValueType::float4
                    )) {
                    pipeline.renderVarBufferIndexes[index] =
                        renderVar->bufferIndex;
                }
            }
        }
        if (!vertexBuffer.buffer) {
            session.uploadBuffer(
                vertexBuffer,
                std::as_bytes(std::span(coverageQuad))
            );
        }
        return pipelines.emplace(key, std::move(pipeline)).first->second;
    }
};

TextCoverageRenderer::TextCoverageRenderer()
    : impl_(std::make_unique<Impl>()) {}

TextCoverageRenderer::TextCoverageRenderer(const AssetResolver& resolver)
    : impl_(std::make_unique<Impl>(&resolver)) {}

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
    return prepare(session, value, key, {});
}

PreparedTextCoverage TextCoverageRenderer::prepare(
    Device::Session& session,
    const text::RasterizedText& value,
    const TextCoverageKey& key,
    const TextFontStyle& style
) {
    validateCoverageLayout(value);
    if (key.width != value.width || key.height != value.height) {
        throw Error(
            ErrorCode::invalidArgument,
            "Text coverage key dimensions do not match the raster"
        );
    }
    const TextFontStyle effectiveStyle = normalizedFontStyle(style);
    impl_->ensurePipeline(session, effectiveStyle);

    TextCoverageKey effectiveKey = key;
    std::optional<SignedDistanceImage> signedDistance;
    if (effectiveStyle.requiresSignedDistance()) {
        signedDistance = makeSignedDistanceImage(value, effectiveStyle);
        effectiveKey.padding = signedDistance->padding;
        effectiveKey.signedDistanceRange = signedDistance->range;
    } else {
        effectiveKey.padding = 0;
        effectiveKey.signedDistanceRange = 0;
    }

    auto found = impl_->textures.find(effectiveKey);
    if (found != impl_->textures.end()) {
        impl_->recency.splice(
            impl_->recency.begin(), impl_->recency, found->second.lru
        );
        found->second.lru = impl_->recency.begin();
        return {
            .texture = &found->second.texture,
            .width = found->second.width,
            .height = found->second.height,
            .contentWidth = value.width,
            .contentHeight = value.height,
            .owner = impl_.get(),
            .generation = impl_->generation,
            .padding = found->second.padding,
            .signedDistanceRange = static_cast<float>(
                found->second.signedDistanceRange
            ),
            .signedDistance = found->second.signedDistanceRange != 0,
        };
    }

    const std::uint32_t textureWidth = signedDistance
        ? signedDistance->width : value.width;
    const std::uint32_t textureHeight = signedDistance
        ? signedDistance->height : value.height;
    const std::size_t bytes = signedDistance
        ? signedDistance->pixels.size() : value.coverage.size();
    TextureResource texture = signedDistance
        ? session.uploadRGBA8Texture(
              textureWidth, textureHeight, signedDistance->pixels
          )
        : session.uploadCoverageTexture(
              value.width,
              value.height,
              value.bytesPerRow,
              value.coverage
          );
    impl_->recency.push_front(effectiveKey);
    try {
        const auto [inserted, didInsert] = impl_->textures.emplace(
            effectiveKey,
            Impl::Entry{
                .texture = std::move(texture),
                .bytes = bytes,
                .width = textureWidth,
                .height = textureHeight,
                .padding = signedDistance ? signedDistance->padding : 0U,
                .signedDistanceRange = signedDistance
                    ? signedDistance->range : 0U,
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
                .width = inserted->second.width,
                .height = inserted->second.height,
                .contentWidth = value.width,
                .contentHeight = value.height,
                .owner = impl_.get(),
                .generation = impl_->generation,
                .padding = inserted->second.padding,
                .signedDistanceRange = static_cast<float>(
                    inserted->second.signedDistanceRange
                ),
                .signedDistance =
                    inserted->second.signedDistanceRange != 0,
            };
        }
        impl_->cachedBytes += bytes;
        return {
            .texture = &inserted->second.texture,
            .width = inserted->second.width,
            .height = inserted->second.height,
            .contentWidth = value.width,
            .contentHeight = value.height,
            .owner = impl_.get(),
            .generation = impl_->generation,
            .padding = inserted->second.padding,
            .signedDistanceRange = static_cast<float>(
                inserted->second.signedDistanceRange
            ),
            .signedDistance = inserted->second.signedDistanceRange != 0,
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
        text.height == 0 || text.contentWidth == 0 ||
        text.contentHeight == 0 || !impl_->vertexBuffer.buffer) {
        throw Error(
            ErrorCode::invalidArgument,
            "Prepared text coverage is incomplete"
        );
    }
    validateDrawRequest(request);
    const TextFontStyle effectiveStyle = normalizedFontStyle(
        request.fontStyle
    );
    if (text.signedDistance != effectiveStyle.requiresSignedDistance()) {
        throw Error(
            ErrorCode::invalidArgument,
            "Prepared text coverage does not match the requested font style"
        );
    }
    Impl::Pipeline& pipeline = impl_->ensurePipeline(session, effectiveStyle);
    if (!pipeline.program) {
        throw Error(
            ErrorCode::internalFailure,
            "Prepared text coverage has no Metal font pipeline"
        );
    }

    const float width = float(text.width);
    const float height = float(text.height);
    std::array<float, 16> scaledModelViewProjection =
        request.modelViewProjection;
    for (std::size_t row = 0; row < 4; ++row) {
        scaledModelViewProjection[row] *= width;
        scaledModelViewProjection[4 + row] *= height;
        scaledModelViewProjection[12 + row] +=
            request.modelViewProjection[row] * -float(text.padding) +
            request.modelViewProjection[4 + row] * -float(text.padding);
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
    const std::array<float, 4> textureResolution{
        width, height, width, height,
    };
    // This backend builds the atlas at final pixel size, so its runtime atlas
    // scale is 1. The remaining fields match the official font pass writer.
    const std::array<std::array<float, 4>, 4> renderVariables{{
        {
            text.signedDistanceRange,
            effectiveStyle.outlineThickness,
            effectiveStyle.blurSize,
            effectiveStyle.dropShadowSize,
        },
        {
            effectiveStyle.outlineColor[0],
            effectiveStyle.outlineColor[1],
            effectiveStyle.outlineColor[2],
            effectiveStyle.dropShadowOffset[0],
        },
        {
            effectiveStyle.dropShadowColor[0],
            effectiveStyle.dropShadowColor[1],
            effectiveStyle.dropShadowColor[2],
            effectiveStyle.dropShadowOffset[1],
        },
        {effectiveStyle.dropShadowOpacity, 0.0F, 0.0F, 0.0F},
    }};
    DrawRequest draw{
        .program = pipeline.program,
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
                    .location = pipeline.positionLocation,
                    .format = pipeline.positionFloat3
                        ? VertexFormat::float3 : VertexFormat::float2,
                    .offset = offsetof(Vertex, x),
                },
                {
                    .location = pipeline.texCoordLocation,
                    .format = VertexFormat::float2,
                    .offset = offsetof(Vertex, u),
                },
            },
        },
        .vertexBuffer = &impl_->vertexBuffer,
        .uniforms = {
            {
                .vertexBufferIndex =
                    pipeline.modelViewProjectionBufferIndex,
                .bytes = scaledModelViewProjection.data(),
                .byteCount = sizeof(scaledModelViewProjection),
            },
            {
                .fragmentBufferIndex = pipeline.colorBufferIndex,
                .bytes = request.color.data(),
                .byteCount = sizeof(request.color),
            },
        },
        .textures = {
            {
                .texture = text.texture,
                .fragmentTextureIndex = pipeline.textureIndex,
                .fragmentSamplerIndex = pipeline.samplerIndex,
            },
        },
    };
    if (pipeline.textureResolutionBufferIndex) {
        draw.uniforms.push_back({
            .fragmentBufferIndex = *pipeline.textureResolutionBufferIndex,
            .bytes = textureResolution.data(),
            .byteCount = sizeof(textureResolution),
        });
    }
    for (std::size_t index = 0;
         index < pipeline.renderVarBufferIndexes.size(); ++index) {
        if (!pipeline.renderVarBufferIndexes[index]) continue;
        draw.uniforms.push_back({
            .fragmentBufferIndex = *pipeline.renderVarBufferIndexes[index],
            .bytes = renderVariables[index].data(),
            .byteCount = sizeof(renderVariables[index]),
        });
    }
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
    const PreparedTextCoverage prepared = prepare(
        session, value, keyFor(value), request.fontStyle
    );
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
    for (auto& [key, pipeline] : impl_->pipelines) {
        static_cast<void>(key);
        session.destroyProgram(pipeline.program);
    }
    impl_->pipelines.clear();
}

std::size_t TextCoverageRenderer::cachedTextureCount() const noexcept {
    return impl_->textures.size();
}

}  // namespace we::scene::metal
