#ifndef WE_SCENE_GL_VIDEO_DECODER_HPP
#define WE_SCENE_GL_VIDEO_DECODER_HPP

#include <cstddef>
#include <cstdint>

namespace we::scene::gl {

struct VideoFrameRGBA8 final {
    const std::uint8_t* bytes = nullptr;
    std::size_t byteCount = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] void* createVideoDecoder(
    const std::uint8_t* bytes,
    std::size_t byteCount,
    const char* source
);
void destroyVideoDecoder(void* decoder) noexcept;
[[nodiscard]] bool decodeVideoFrame(
    void* decoder,
    double timeSeconds,
    VideoFrameRGBA8& output
) noexcept;

}  // namespace we::scene::gl

#endif
