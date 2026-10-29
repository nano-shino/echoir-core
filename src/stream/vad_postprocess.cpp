// Port of fireredvad/core/vad_postprocessor.py (Apache-2.0, FireRedTeam/FireRedVAD).
#include "stream/vad_postprocess.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace echoir::stream {
namespace {

// Frame timing in seconds, as upstream has it. The 10 ms step is a double, so
// seconds / 0.01 can land just below a whole frame (0.29 / 0.01 is 28.999...). The split step
// depends on that truncation, so these stay doubles.
constexpr double kFrameShiftS = 0.010;
constexpr double kFrameLengthS = 0.025;

enum class VadState { kSilence, kPossibleSpeech, kSpeech, kPossibleSilence };

// A speech span in seconds, rounded to 3 decimals.
struct Span {
    double start_s;
    double end_s;
};

// Python's round(x, 3): the exact binary value rounded to 3 decimals (ties to even), then the
// nearest double to that decimal. Going through text gives the same bits for every input. The
// text is read back as thousandths and divided by 1000, which IEEE rounds correctly, because
// Apple's libc++ has no floating-point from_chars before macOS 26. Apple's libc++ has no
// floating-point to_chars before iOS 16.3 either, so Apple formats with snprintf, which its libc
// rounds correctly; the decimal point there follows the C locale, so only digits are read.
double round3(double x) {
    // Beyond 2^53 thousandths the count is not exact; every double there is already a multiple
    // of 0.001 to within its own spacing.
    if (!(std::fabs(x) < 9.0e12)) {
        return x;
    }
    char buf[64];
#if defined(__APPLE__)
    const int n = std::snprintf(buf, sizeof(buf), "%.3f", x);
    if (n < 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
        throw std::runtime_error("round3: value does not fit in the buffer");
    }
    const char* const end = buf + n;
#else
    const auto formatted = std::to_chars(buf, buf + sizeof(buf), x, std::chars_format::fixed, 3);
    if (formatted.ec != std::errc{}) {
        throw std::runtime_error("round3: value does not fit in the buffer");
    }
    const char* const end = formatted.ptr;
#endif
    const char* p = buf;
    const bool negative = *p == '-';
    if (negative) {
        ++p;
    }
    long long thousandths = 0;
    for (; p != end; ++p) {
        if (*p >= '0' && *p <= '9') {
            thousandths = thousandths * 10 + (*p - '0');
        }
    }
    const double rounded = static_cast<double>(thousandths) / 1000.0;
    return negative ? -rounded : rounded;
}

// numpy's pairwise sum, which add.reduce uses for float64. Its blocking sets the rounding of the
// cumulative means at the start of a clip, so it is copied rather than summed left to right.
double pairwise_sum(const double* a, std::size_t n) {
    if (n < 8) {
        double res = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            res += a[i];
        }
        return res;
    }
    if (n <= 128) {
        double r[8];
        for (std::size_t j = 0; j < 8; ++j) {
            r[j] = a[j];
        }
        std::size_t i = 8;
        for (; i < n - (n % 8); i += 8) {
            for (std::size_t j = 0; j < 8; ++j) {
                r[j] += a[i + j];
            }
        }
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i) {
            res += a[i];
        }
        return res;
    }
    std::size_t half = n / 2;
    half -= half % 8;
    return pairwise_sum(a, half) + pairwise_sum(a + half, n - half);
}

// Trailing moving average over `window` frames. The first window - 1 frames average the frames
// so far. Each full window is a chain of fused multiply-adds, oldest frame first: that matches
// numpy's convolve bit for bit for windows up to 16 frames.
std::vector<double> smooth(std::span<const float> probs, int window) {
    const std::size_t n = probs.size();
    std::vector<double> p(probs.begin(), probs.end());
    if (window <= 1) {
        return p;
    }
    std::vector<double> out(n);
    const auto w = static_cast<std::size_t>(window);
    const double kernel = 1.0 / static_cast<double>(window);
    for (std::size_t i = 0; i < std::min(w - 1, n); ++i) {
        out[i] = pairwise_sum(p.data(), i + 1) / static_cast<double>(i + 1);
    }
    for (std::size_t i = w - 1; i < n; ++i) {
        double acc = 0.0;
        for (std::size_t j = i + 1 - w; j <= i; ++j) {
            acc = std::fma(p[j], kernel, acc);
        }
        out[i] = acc;
    }
    return out;
}

// Speech starts after min_speech frames of speech and ends after min_silence frames of silence.
// Speech that qualifies is marked back to where it started.
std::vector<std::uint8_t> apply_state_machine(const std::vector<std::uint8_t>& binary,
                                              int min_speech, int min_silence) {
    if (min_speech <= 0 && min_silence <= 0) {
        return binary;
    }
    const auto n = static_cast<std::ptrdiff_t>(binary.size());
    std::vector<std::uint8_t> decisions(binary.size(), 0);
    VadState state = VadState::kSilence;
    std::ptrdiff_t speech_start = -1;
    std::ptrdiff_t silence_start = -1;
    for (std::ptrdiff_t t = 0; t < n; ++t) {
        const bool is_speech = binary[static_cast<std::size_t>(t)] != 0;
        switch (state) {
            case VadState::kSilence:
                if (is_speech) {
                    state = VadState::kPossibleSpeech;
                    speech_start = t;
                }
                break;
            case VadState::kPossibleSpeech:
                if (is_speech) {
                    if (t - speech_start >= min_speech) {
                        state = VadState::kSpeech;
                        for (std::ptrdiff_t k = speech_start; k < t; ++k) {
                            decisions[static_cast<std::size_t>(k)] = 1;
                        }
                    }
                } else {
                    state = VadState::kSilence;
                    speech_start = -1;
                }
                break;
            case VadState::kSpeech:
                if (!is_speech) {
                    state = VadState::kPossibleSilence;
                    silence_start = t;
                }
                break;
            case VadState::kPossibleSilence:
                if (!is_speech) {
                    if (t - silence_start >= min_silence) {
                        state = VadState::kSilence;
                        speech_start = -1;
                    }
                } else {
                    state = VadState::kSpeech;
                    silence_start = -1;
                }
                break;
        }
        decisions[static_cast<std::size_t>(t)] =
            (state == VadState::kSpeech || state == VadState::kPossibleSilence) ? 1 : 0;
    }
    return decisions;
}

// A rising edge is moved back over the smoothing window, which delayed it.
std::vector<std::uint8_t> fix_window_start(const std::vector<std::uint8_t>& decisions, int window) {
    auto out = decisions;
    const auto n = static_cast<std::ptrdiff_t>(decisions.size());
    for (std::ptrdiff_t t = 1; t < n; ++t) {
        const auto i = static_cast<std::size_t>(t);
        if (decisions[i - 1] == 0 && decisions[i] == 1) {
            const std::ptrdiff_t start = std::max<std::ptrdiff_t>(0, t - window);
            for (std::ptrdiff_t k = start; k < t; ++k) {
                out[static_cast<std::size_t>(k)] = 1;
            }
        }
    }
    return out;
}

// Silence gaps shorter than merge frames, between speech, become speech.
std::vector<std::uint8_t> merge_short_silence(const std::vector<std::uint8_t>& decisions,
                                              int merge) {
    if (merge <= 0) {
        return decisions;
    }
    auto out = decisions;
    const auto n = static_cast<std::ptrdiff_t>(decisions.size());
    std::ptrdiff_t silence_start = -1;
    for (std::ptrdiff_t t = 0; t < n; ++t) {
        const auto i = static_cast<std::size_t>(t);
        if (t > 0 && decisions[i - 1] == 1 && decisions[i] == 0 && silence_start < 0) {
            silence_start = t;
        } else if (t > 0 && decisions[i - 1] == 0 && decisions[i] == 1 && silence_start >= 0) {
            const std::ptrdiff_t silence_frame = t - silence_start;
            if (silence_frame < merge) {
                for (std::ptrdiff_t k = silence_start; k < t; ++k) {
                    out[static_cast<std::size_t>(k)] = 1;
                }
            }
            silence_start = -1;
        }
    }
    return out;
}

// Speech grows by `extend` frames on each side. This is numpy's convolve(..., 'same') with a
// window of 2 * extend + 1 ones, which returns max(n, window) values. When the input is shorter
// than the window, the output is the window's length and the alignment shifts by (n - 1) / 2.
std::vector<std::uint8_t> extend_speech(const std::vector<std::uint8_t>& decisions, int extend) {
    if (extend <= 0) {
        return decisions;
    }
    const auto n = static_cast<std::ptrdiff_t>(decisions.size());
    const std::ptrdiff_t window = 2 * static_cast<std::ptrdiff_t>(extend) + 1;
    const std::ptrdiff_t length = std::max(n, window);
    const std::ptrdiff_t shift = (std::min(n, window) - 1) / 2;
    // prefix[k] is the number of speech decisions before index k.
    std::vector<std::ptrdiff_t> prefix(static_cast<std::size_t>(n) + 1, 0);
    for (std::ptrdiff_t k = 0; k < n; ++k) {
        prefix[static_cast<std::size_t>(k) + 1] =
            prefix[static_cast<std::size_t>(k)] + decisions[static_cast<std::size_t>(k)];
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(length), 0);
    for (std::ptrdiff_t i = 0; i < length; ++i) {
        const std::ptrdiff_t lo = std::max<std::ptrdiff_t>(0, i + shift - (window - 1));
        const std::ptrdiff_t hi = std::min<std::ptrdiff_t>(n - 1, i + shift);
        if (lo <= hi &&
            prefix[static_cast<std::size_t>(hi) + 1] - prefix[static_cast<std::size_t>(lo)] > 0) {
            out[static_cast<std::size_t>(i)] = 1;
        }
    }
    return out;
}

// Speech spans of a 0/1 decision sequence, in seconds, rounded to 3 decimals. A span still open
// at the end ends at frames * 10 ms + 25 ms. With wav_dur, every end is capped at wav_dur, so the
// output stays inside the clip.
std::vector<Span> decision_to_segment(const std::vector<std::uint8_t>& decisions,
                                      std::optional<double> wav_dur) {
    std::vector<Span> segments;
    const auto n = static_cast<std::ptrdiff_t>(decisions.size());
    std::ptrdiff_t speech_start = -1;
    for (std::ptrdiff_t t = 0; t < n; ++t) {
        const std::uint8_t decision = decisions[static_cast<std::size_t>(t)];
        if (decision == 1 && speech_start < 0) {
            speech_start = t;
        } else if (decision == 0 && speech_start >= 0) {
            double end = static_cast<double>(t) * kFrameShiftS;
            if (wav_dur) {
                end = std::min(end, *wav_dur);
            }
            segments.push_back(
                {round3(static_cast<double>(speech_start) * kFrameShiftS), round3(end)});
            speech_start = -1;
        }
    }
    if (speech_start >= 0) {
        // Separate statements: the product is rounded before the add, as in upstream.
        const double frames_s = static_cast<double>(n) * kFrameShiftS;
        double end = frames_s + kFrameLengthS;
        if (wav_dur) {
            end = std::min(end, *wav_dur);
        }
        segments.push_back({round3(static_cast<double>(speech_start) * kFrameShiftS), round3(end)});
    }
    return segments;
}

// Split points inside one over-long span: the quietest frame of [start + max/2, start + max),
// then the search restarts after it.
std::vector<std::ptrdiff_t> find_split_points(std::span<const float> probs,
                                              std::ptrdiff_t max_speech) {
    std::vector<std::ptrdiff_t> points;
    const auto length = static_cast<std::ptrdiff_t>(probs.size());
    std::ptrdiff_t start = 0;
    while (start < length) {
        if (length - start <= max_speech) {
            break;
        }
        const std::ptrdiff_t window_start = start + max_speech / 2;
        const std::ptrdiff_t window_end = start + max_speech;
        // Strict < keeps the first minimum, as numpy's argmin does.
        std::ptrdiff_t min_index = window_start;
        for (std::ptrdiff_t k = window_start + 1; k < window_end; ++k) {
            if (probs[static_cast<std::size_t>(k)] < probs[static_cast<std::size_t>(min_index)]) {
                min_index = k;
            }
        }
        points.push_back(min_index);
        start = min_index + 1;
    }
    return points;
}

// Clears one frame per split point in every span longer than max_speech frames. The span bounds
// are truncated from seconds, as upstream does, and the split is searched in the raw
// probabilities.
std::vector<std::uint8_t> split_long_speech(const std::vector<std::uint8_t>& decisions,
                                            std::span<const float> probs,
                                            std::ptrdiff_t max_speech) {
    auto out = decisions;
    const auto n_probs = static_cast<std::ptrdiff_t>(probs.size());
    for (const Span& seg : decision_to_segment(decisions, std::nullopt)) {
        const auto start_frame = static_cast<std::ptrdiff_t>(seg.start_s / kFrameShiftS);
        const auto end_frame = static_cast<std::ptrdiff_t>(seg.end_s / kFrameShiftS);
        if (end_frame - start_frame <= max_speech) {
            continue;
        }
        // Python slicing: an out-of-range span is cut to the probabilities that exist.
        const std::ptrdiff_t from = std::min(start_frame, n_probs);
        const std::ptrdiff_t to = std::clamp(end_frame, from, n_probs);
        for (const std::ptrdiff_t point : find_split_points(
                 probs.subspan(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)),
                 max_speech)) {
            out[static_cast<std::size_t>(from + point)] = 0;
        }
    }
    return out;
}

}  // namespace

VadPostprocessor::VadPostprocessor(const VadPostprocessParams& params) : params_(params) {
    if (params_.max_speech_frame < 1) {
        throw std::invalid_argument("VadPostprocessor: max_speech_frame must be at least 1");
    }
    params_.smooth_window_size = std::max(1, params_.smooth_window_size);
}

std::vector<std::uint8_t> VadPostprocessor::process(std::span<const float> probs) const {
    if (probs.empty()) {
        return {};
    }
    const std::vector<double> smoothed = smooth(probs, params_.smooth_window_size);
    std::vector<std::uint8_t> binary(smoothed.size());
    for (std::size_t i = 0; i < smoothed.size(); ++i) {
        binary[i] = smoothed[i] >= params_.prob_threshold ? 1 : 0;
    }
    std::vector<std::uint8_t> decisions =
        apply_state_machine(binary, params_.min_speech_frame, params_.min_silence_frame);
    decisions = fix_window_start(decisions, params_.smooth_window_size);
    decisions = merge_short_silence(decisions, params_.merge_silence_frame);
    decisions = extend_speech(decisions, params_.extend_speech_frame);
    return split_long_speech(decisions, probs, params_.max_speech_frame);
}

std::vector<vad::Range> VadPostprocessor::ranges(std::span<const float> probs,
                                                 double duration_s) const {
    if (!std::isfinite(duration_s) || duration_s < 0.0) {
        throw std::invalid_argument("VadPostprocessor::ranges: duration_s must be finite and >= 0");
    }
    // Rounding to ms can land just past the clip, and a capped end can fall before its start.
    const auto duration_ms = static_cast<std::int64_t>(std::floor(duration_s * 1000.0));
    std::vector<vad::Range> out;
    for (const Span& seg : decision_to_segment(process(probs), duration_s)) {
        const auto start_ms = static_cast<std::int64_t>(std::llround(seg.start_s * 1000.0));
        const auto end_ms =
            std::min(static_cast<std::int64_t>(std::llround(seg.end_s * 1000.0)), duration_ms);
        if (end_ms > start_ms) {
            out.push_back({start_ms, end_ms});
        }
    }
    return out;
}

}  // namespace echoir::stream
