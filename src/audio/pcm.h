// Sample-format conversion for the core's f32 PCM.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace echoir::audio {

// int16 to float32 in [-1, 1). The scale is a power of two, so the conversion is exact and
// f32_to_s16 undoes it bit for bit.
std::vector<float> s16_to_f32(std::span<const std::int16_t> src);

// float32 to int16: clamps to [-1, 1], scales by 32768 and rounds to nearest. NaN becomes 0.
std::vector<std::int16_t> f32_to_s16(std::span<const float> src);

}  // namespace echoir::audio
