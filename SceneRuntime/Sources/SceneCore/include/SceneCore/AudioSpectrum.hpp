#ifndef WE_SCENE_CORE_AUDIO_SPECTRUM_HPP
#define WE_SCENE_CORE_AUDIO_SPECTRUM_HPP

#include <algorithm>
#include <array>
#include <cmath>

namespace we::scene {

// One immutable host-provided Wallpaper Engine audio analysis frame. The
// Linux runtime publishes the same mono spectrum to both channel names, but
// keeping the channels distinct preserves the authored shader contract and
// allows a native host to provide stereo analysis later.
struct AudioSpectrumFrame final {
    std::array<float, 16> spectrum16Left{};
    std::array<float, 16> spectrum16Right{};
    std::array<float, 32> spectrum32Left{};
    std::array<float, 32> spectrum32Right{};
    std::array<float, 64> spectrum64Left{};
    std::array<float, 64> spectrum64Right{};
};

[[nodiscard]] inline bool audioSpectrumIsFinite(
    const AudioSpectrumFrame& frame
) noexcept {
    const auto finite = [](const auto& values) {
        return std::all_of(
            values.begin(), values.end(),
            [](float value) { return std::isfinite(value); }
        );
    };
    return finite(frame.spectrum16Left) && finite(frame.spectrum16Right) &&
        finite(frame.spectrum32Left) && finite(frame.spectrum32Right) &&
        finite(frame.spectrum64Left) && finite(frame.spectrum64Right);
}

}  // namespace we::scene

#endif
