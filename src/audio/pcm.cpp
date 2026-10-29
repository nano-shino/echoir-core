#include "audio/pcm.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace echoir::audio {

std::vector<float> s16_to_f32(std::span<const std::int16_t> src) {
    std::vector<float> dst(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        dst[i] = static_cast<float>(src[i]) * (1.0f / 32768.0f);
    }
    return dst;
}

std::vector<std::int16_t> f32_to_s16(std::span<const float> src) {
    std::vector<std::int16_t> dst(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        float x = std::isnan(src[i]) ? 0.0f : std::clamp(src[i], -1.0f, 1.0f);
        // Upper clamp: +1.0 scales to 32768, which does not fit in int16.
        float scaled = std::min(x * 32768.0f, 32767.0f);
        dst[i] = static_cast<std::int16_t>(std::lrint(scaled));
    }
    return dst;
}

}  // namespace echoir::audio
