// The algorithm follows csukuangfj/kaldi-native-fbank (Apache-2.0); the FFT and code are ours.
#include "audio/fbank.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace echoir::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;

constexpr std::size_t kFrameLen = 400;         // 25 ms at 16 kHz
constexpr std::size_t kFrameShift = 160;       // 10 ms
constexpr std::size_t kFftLen = 512;           // the frame, zero-padded to a power of two
constexpr std::size_t kFftBins = kFftLen / 2;  // the Nyquist bin is left out
constexpr float kPreemphasis = 0.97f;
constexpr float kLowHz = 20.0f;
constexpr float kHighHz = 8000.0f;            // Nyquist
constexpr float kBinHz = 16000.0f / kFftLen;  // width of one FFT bin, 31.25 Hz
// The model is trained on int16 samples, so the input is scaled to that range before the
// front end. The scale is a power of two, so the scaling is exact.
constexpr float kSampleScale = 32768.0f;

struct Complex {
    float re;
    float im;
};

// One triangular mel filter. Its nonzero weights cover a run of FFT bins.
struct MelBin {
    std::size_t first = 0;       // first FFT bin with a weight
    std::vector<float> weights;  // weights for FFT bins first, first + 1, ...
};

struct Tables {
    std::array<float, kFrameLen> window{};  // Povey window
    std::array<Complex, kFftLen / 2> twiddle{};
    std::array<MelBin, kFbankBins> banks;
};

float mel_scale(float hz) { return 1127.0f * std::log(1.0f + hz / 700.0f); }

Tables build_tables() {
    Tables t;

    // Povey window: a Hann window raised to 0.85. Computed in double, stored as float.
    const double a = 2.0 * kPi / static_cast<double>(kFrameLen - 1);
    for (std::size_t j = 0; j < kFrameLen; ++j) {
        t.window[j] =
            static_cast<float>(std::pow(0.5 - 0.5 * std::cos(a * static_cast<double>(j)), 0.85));
    }

    for (std::size_t k = 0; k < kFftLen / 2; ++k) {
        const double angle = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(kFftLen);
        t.twiddle[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }

    // Bin edges are equally spaced in mel. Each triangle spans two steps, and its centre is one
    // step above its left edge.
    const float mel_low = mel_scale(kLowHz);
    const float mel_high = mel_scale(kHighHz);
    const float step = (mel_high - mel_low) / static_cast<float>(kFbankBins + 1);
    for (std::size_t b = 0; b < kFbankBins; ++b) {
        const float left = mel_low + static_cast<float>(b) * step;
        const float center = left + step;
        const float right = left + 2.0f * step;
        MelBin& bin = t.banks[b];
        for (std::size_t k = 0; k < kFftBins; ++k) {
            const float m = mel_scale(kBinHz * static_cast<float>(k));
            float w = 0.0f;
            if (m > left && m <= center) {
                w = (m - left) / (center - left);
            } else if (m > center && m < right) {
                w = (right - m) / (right - center);
            } else {
                continue;  // the weights of one triangle are contiguous in k
            }
            if (bin.weights.empty()) bin.first = k;
            bin.weights.push_back(w);
        }
    }
    return t;
}

const Tables& tables() {
    static const Tables t = build_tables();
    return t;
}

// In-place iterative radix-2 FFT of kFftLen points.
void fft_in_place(Complex* x, const Tables& t) {
    constexpr std::size_t n = kFftLen;
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t half = len >> 1;
        const std::size_t stride = n / len;
        for (std::size_t start = 0; start < n; start += len) {
            for (std::size_t k = 0; k < half; ++k) {
                const Complex w = t.twiddle[k * stride];
                const Complex odd = x[start + k + half];
                const Complex prod{odd.re * w.re - odd.im * w.im, odd.re * w.im + odd.im * w.re};
                const Complex even = x[start + k];
                x[start + k + half] = {even.re - prod.re, even.im - prod.im};
                x[start + k] = {even.re + prod.re, even.im + prod.im};
            }
        }
    }
}

}  // namespace

std::vector<float> kaldi_fbank(std::span<const float> pcm16k_mono) {
    if (pcm16k_mono.size() < kFrameLen) return {};
    const Tables& t = tables();
    const std::size_t frames = 1 + (pcm16k_mono.size() - kFrameLen) / kFrameShift;
    std::vector<float> out(frames * kFbankBins);

    std::array<float, kFrameLen> frame{};
    std::array<Complex, kFftLen> fft{};
    std::array<float, kFftBins> power{};
    const float eps = std::numeric_limits<float>::epsilon();

    for (std::size_t f = 0; f < frames; ++f) {
        const float* src = pcm16k_mono.data() + f * kFrameShift;
        for (std::size_t i = 0; i < kFrameLen; ++i) frame[i] = src[i] * kSampleScale;

        // DC removal first, then preemphasis from the end, so each sample still sees the
        // previous sample before it was changed.
        float mean = 0.0f;
        for (float v : frame) mean += v;
        mean /= static_cast<float>(kFrameLen);
        for (float& v : frame) v -= mean;
        for (std::size_t i = kFrameLen - 1; i > 0; --i) frame[i] -= kPreemphasis * frame[i - 1];
        frame[0] -= kPreemphasis * frame[0];

        for (std::size_t i = 0; i < kFrameLen; ++i) fft[i] = {frame[i] * t.window[i], 0.0f};
        for (std::size_t i = kFrameLen; i < kFftLen; ++i) fft[i] = {0.0f, 0.0f};
        fft_in_place(fft.data(), t);
        for (std::size_t k = 0; k < kFftBins; ++k) {
            power[k] = fft[k].re * fft[k].re + fft[k].im * fft[k].im;
        }

        float* row = out.data() + f * kFbankBins;
        for (std::size_t b = 0; b < kFbankBins; ++b) {
            const MelBin& bin = t.banks[b];
            float sum = 0.0f;
            for (std::size_t j = 0; j < bin.weights.size(); ++j) {
                sum += bin.weights[j] * power[bin.first + j];
            }
            // The floor keeps silent bins finite.
            row[b] = std::log(std::max(sum, eps));
        }
    }
    return out;
}

void apply_cmvn(std::span<float> feats, std::span<const float> mean, std::span<const float> istd) {
    if (mean.size() != kFbankBins || istd.size() != kFbankBins || feats.size() % kFbankBins != 0) {
        throw std::invalid_argument("apply_cmvn: needs 80 means and 80 istds and whole frames");
    }
    for (std::size_t i = 0; i < feats.size(); ++i) {
        const std::size_t b = i % kFbankBins;
        // Double keeps the subtraction and scaling exact enough that one final rounding remains.
        feats[i] = static_cast<float>((static_cast<double>(feats[i]) - mean[b]) * istd[b]);
    }
}

}  // namespace echoir::audio
