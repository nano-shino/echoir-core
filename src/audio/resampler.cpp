#include "audio/resampler.h"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "audio/miniaudio_config.h"

namespace echoir::audio {
namespace {

constexpr std::uint32_t kCoreRate = 16000;

}  // namespace

struct Resampler::State {
    std::uint32_t in_rate = 0;
    std::uint32_t channels = 0;
    bool passthrough = false;  // input is already 16 kHz, so no filter runs
    bool ready = false;        // ma_resampler_init succeeded, so uninit is owed
    ma_resampler resampler{};
    std::vector<float> mono;  // scratch for the channel average
};

// Runs mono f32 through the resampler and appends the output. The resampler keeps its
// filter state between calls, so the caller can split the input at any frame.
void Resampler::process_mono(State& s, std::span<const float> mono, std::vector<float>& out) {
    if (s.passthrough) {
        out.insert(out.end(), mono.begin(), mono.end());
        return;
    }
    ma_uint64 remaining = mono.size();
    const float* in = mono.data();
    while (remaining > 0) {
        // Ask for one frame more than the expected count, so a short call is not a stall.
        ma_uint64 expected = 0;
        if (ma_resampler_get_expected_output_frame_count(&s.resampler, remaining, &expected) !=
            MA_SUCCESS) {
            throw std::runtime_error("resampler: cannot size output");
        }
        const std::size_t base = out.size();
        const ma_uint64 capacity = expected + 1;
        out.resize(base + static_cast<std::size_t>(capacity));

        ma_uint64 frames_in = remaining;
        ma_uint64 frames_out = capacity;
        ma_result result = ma_resampler_process_pcm_frames(&s.resampler, in, &frames_in,
                                                           out.data() + base, &frames_out);
        out.resize(base + static_cast<std::size_t>(frames_out));
        if (result != MA_SUCCESS) {
            throw std::runtime_error(std::string("resampler failed: ") +
                                     ma_result_description(result));
        }
        if (frames_in == 0 && frames_out == 0) {
            throw std::runtime_error("resampler made no progress");
        }
        in += frames_in;
        remaining -= frames_in;
    }
}

Resampler::Resampler(std::uint32_t in_rate, std::uint32_t in_channels)
    : state_(std::make_unique<State>()) {
    if (in_rate == 0 || in_channels == 0) {
        throw std::invalid_argument("Resampler: rate and channels must be non-zero");
    }
    state_->in_rate = in_rate;
    state_->channels = in_channels;
    state_->passthrough = in_rate == kCoreRate;
    if (!state_->passthrough) {
        ma_resampler_config config = ma_resampler_config_init(ma_format_f32, 1, in_rate, kCoreRate,
                                                              ma_resample_algorithm_linear);
        ma_result result = ma_resampler_init(&config, nullptr, &state_->resampler);
        if (result != MA_SUCCESS) {
            throw std::runtime_error(std::string("resampler init failed: ") +
                                     ma_result_description(result));
        }
        state_->ready = true;
    }
}

Resampler::~Resampler() {
    if (state_ && state_->ready) ma_resampler_uninit(&state_->resampler, nullptr);
}

// The State lives on the heap, so miniaudio's internal self-pointers stay valid across a move.
Resampler::Resampler(Resampler&&) noexcept = default;

Resampler& Resampler::operator=(Resampler&& other) noexcept {
    if (this != &other) {
        // Release what this object owns before taking over the other one's state.
        if (state_ && state_->ready) ma_resampler_uninit(&state_->resampler, nullptr);
        state_ = std::move(other.state_);
    }
    return *this;
}

void Resampler::push(std::span<const float> interleaved, std::vector<float>& out) {
    State& s = *state_;
    if (interleaved.size() % s.channels != 0) {
        throw std::invalid_argument("Resampler::push: input ends in a partial frame");
    }
    const std::size_t frames = interleaved.size() / s.channels;
    if (s.channels == 1) {
        process_mono(s, interleaved, out);
        return;
    }

    s.mono.resize(frames);
    const float inv = 1.0f / static_cast<float>(s.channels);
    for (std::size_t f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (std::uint32_t c = 0; c < s.channels; ++c) {
            sum += interleaved[f * s.channels + c];
        }
        s.mono[f] = sum * inv;
    }
    process_mono(s, s.mono, out);
}

void Resampler::flush(std::vector<float>& out) {
    State& s = *state_;
    if (s.passthrough) return;
    // The filter rings for a few milliseconds after its last input (about 10 frames at 48 kHz,
    // about 25 at 8 kHz). That ringing is real audio, so feed zeros and keep their output until
    // it has stayed below one 16-bit step for a while. Then drop the trailing quiet frames, so a
    // silent input adds nothing.
    constexpr float kLsb = 1.0f / 32768.0f;
    constexpr std::size_t kQuietRun = 64;  // quiet frames in a row that end the tail
    const std::vector<float> zero(1, 0.0f);
    std::size_t quiet = 0;
    // At most one second of zeros, so a filter that never settles still ends.
    for (std::uint32_t fed = 0; fed < s.in_rate && quiet < kQuietRun; ++fed) {
        const std::size_t before = out.size();
        process_mono(s, zero, out);
        for (std::size_t k = before; k < out.size(); ++k) {
            quiet = std::fabs(out[k]) < kLsb ? quiet + 1 : 0;
        }
    }
    // quiet counts only the frames this call appended, so this never cuts earlier output.
    out.resize(out.size() - quiet);
}

void Resampler::reset() {
    State& s = *state_;
    if (s.ready) ma_resampler_reset(&s.resampler);
}

std::vector<float> to_mono_16k(std::span<const float> interleaved, std::uint32_t rate,
                               std::uint32_t channels) {
    Resampler resampler(rate, channels);
    std::vector<float> out;
    const std::uint64_t frames = interleaved.size() / channels;
    out.reserve(static_cast<std::size_t>(frames * kCoreRate / rate + 16));
    resampler.push(interleaved, out);
    resampler.flush(out);
    return out;
}

}  // namespace echoir::audio
