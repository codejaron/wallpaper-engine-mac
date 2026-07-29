#ifndef WE_SCENE_CORE_AUDIO_SPECTRUM_HPP
#define WE_SCENE_CORE_AUDIO_SPECTRUM_HPP

#include <algorithm>
#include <array>
#include <cmath>

namespace we::scene {

// One immutable host-provided Wallpaper Engine audio analysis frame. Every
// resolution covers low-to-high frequencies across the complete spectrum and
// preserves the left and right channels independently.
struct AudioSpectrumFrame final {
    std::array<float, 16> spectrum16Left{};
    std::array<float, 16> spectrum16Right{};
    std::array<float, 32> spectrum32Left{};
    std::array<float, 32> spectrum32Right{};
    std::array<float, 64> spectrum64Left{};
    std::array<float, 64> spectrum64Right{};
};

[[nodiscard]] inline bool audioSpectrumIsValid(
    const AudioSpectrumFrame& frame
) noexcept {
    const auto valid = [](const auto& values) {
        return std::all_of(
            values.begin(), values.end(),
            [](float value) { return std::isfinite(value) && value >= 0.0F; }
        );
    };
    return valid(frame.spectrum16Left) && valid(frame.spectrum16Right) &&
        valid(frame.spectrum32Left) && valid(frame.spectrum32Right) &&
        valid(frame.spectrum64Left) && valid(frame.spectrum64Right);
}

}  // namespace we::scene

#endif
