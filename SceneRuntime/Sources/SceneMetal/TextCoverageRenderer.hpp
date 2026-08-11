#ifndef WE_SCENE_METAL_TEXT_COVERAGE_RENDERER_HPP
#define WE_SCENE_METAL_TEXT_COVERAGE_RENDERER_HPP

#include "SceneMetalDevice.hpp"

#include <SceneText/SceneText.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace we::scene::metal {

struct TextDrawRequest final {
    // Column-major transform from the local coverage quad into clip space.
    // Scene camera, world transform, alignment and padding remain caller-owned.
    std::array<float, 16> modelViewProjection{};
    std::array<float, 4> color{1, 1, 1, 1};
};

struct PreparedTextCoverage final {
    const TextureResource* texture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const void* owner = nullptr;
    std::uint64_t generation = 0;
};

struct TextCoverageKey final {
    std::uint64_t a = 1469598103934665603ULL;
    std::uint64_t b = 1099511628211ULL;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const TextCoverageKey&) const = default;
};

class TextCoverageRenderer final {
public:
    TextCoverageRenderer();
    ~TextCoverageRenderer();
    TextCoverageRenderer(const TextCoverageRenderer&) = delete;
    TextCoverageRenderer& operator=(const TextCoverageRenderer&) = delete;

    // Handles remain valid until trimCache() evicts an entry or release() is
    // called. draw() may trim the cache, so callers preparing multiple texts
    // must draw all prepared handles before calling draw() or trimCache().
    [[nodiscard]] PreparedTextCoverage prepare(
        Device::Session& session,
        const text::RasterizedText& text
    );
    [[nodiscard]] PreparedTextCoverage prepare(
        Device::Session& session,
        const text::RasterizedText& text,
        const TextCoverageKey& key
    );
    [[nodiscard]] static TextCoverageKey keyFor(
        const text::RasterizedText& text
    );
    void drawPrepared(
        Device::Session& session,
        const FramebufferResource& destination,
        const PreparedTextCoverage& text,
        const TextDrawRequest& request
    );
    void draw(
        Device::Session& session,
        const FramebufferResource& destination,
        const text::RasterizedText& text,
        const TextDrawRequest& request
    );
    void trimCache(Device::Session& session);
    void release(Device::Session& session) noexcept;
    [[nodiscard]] std::size_t cachedTextureCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace we::scene::metal

#endif
