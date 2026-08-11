#ifndef WE_SCENE_METAL_VIDEO_DECODER_HPP
#define WE_SCENE_METAL_VIDEO_DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

namespace we::scene::metal {

enum class VideoFramePixelFormat {
    rgba8,
    bgra8,
};

struct VideoFrame final {
    // Keeps the CVPixelBuffer (or the rare straight-alpha conversion buffer)
    // alive until the Metal upload has consumed bytes.
    std::shared_ptr<const void> storage;
    const std::uint8_t* bytes = nullptr;
    std::size_t byteCount = 0;
    std::size_t bytesPerRow = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t serial = 0;
    VideoFramePixelFormat pixelFormat = VideoFramePixelFormat::rgba8;
};

[[nodiscard]] void* createVideoDecoder(
    const std::uint8_t* bytes,
    std::size_t byteCount,
    const char* source
);
void destroyVideoDecoder(void* decoder) noexcept;
// Requests are coalesced by the decoder's worker. Requesting never waits for
// AVFoundation; copyLatestVideoFrame returns the newest completely decoded
// frame and keeps its storage alive in VideoFrame.
[[nodiscard]] bool requestVideoFrame(
    void* decoder,
    double timeSeconds
) noexcept;
[[nodiscard]] bool copyLatestVideoFrame(
    void* decoder,
    VideoFrame& output
) noexcept;

}  // namespace we::scene::metal

#endif
