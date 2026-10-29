// Kaldi-style log-mel filterbank features and CMVN, as kaldi-native-fbank computes them with
// 16 kHz input, a 25 ms window, a 10 ms shift and 80 mel bins. This is the front end of
// FireRedVAD. The algorithm follows csukuangfj/kaldi-native-fbank (Apache-2.0); the code is ours.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace echoir::audio {

// Mel bins per frame.
inline constexpr std::size_t kFbankBins = 80;

// Input is 16 kHz mono f32 in [-1, 1]. Frame i covers samples [160 i, 160 i + 400), so a clip
// of n samples has 0 frames when n < 400 and 1 + (n - 400) / 160 frames otherwise.
// Returns frames x kFbankBins log mel energies (natural log, each energy floored at FLT_EPSILON
// before the log), row-major, one row per frame. The caller owns the result. Thread-safe: the
// filter tables are built on first use.
std::vector<float> kaldi_fbank(std::span<const float> pcm16k_mono);

// Kaldi CMVN, in place: x = (x - mean) * istd for each bin. feats holds whole frames of
// kFbankBins values, and mean and istd hold one value per bin. Throws std::invalid_argument when
// a size does not match. It touches only its arguments, so calls on different buffers may overlap.
void apply_cmvn(std::span<float> feats, std::span<const float> mean, std::span<const float> istd);

}  // namespace echoir::audio
