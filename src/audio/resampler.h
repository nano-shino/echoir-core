// Streaming conversion of any rate and channel count to the core's format: mono, 16 kHz, f32.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace echoir::audio {

// Keeps resampler state across push() calls, so chunk boundaries add no clicks and no gaps.
// Channels are averaged to mono first, then the linear resampler (miniaudio, low-pass on)
// converts the rate. Input already at 16 kHz is copied through unchanged.
class Resampler {
public:
    // Throws std::invalid_argument for a zero rate or channel count.
    Resampler(std::uint32_t in_rate, std::uint32_t in_channels);
    ~Resampler();
    Resampler(Resampler&&) noexcept;
    Resampler& operator=(Resampler&&) noexcept;
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    // Appends the 16 kHz mono samples for `interleaved` (a whole number of frames) to `out`.
    void push(std::span<const float> interleaved, std::vector<float>& out);

    // Appends the tail that the filter still holds: its ringing after the last input, down to
    // one 16-bit step. Call once at end of stream. Pushing more audio after this needs reset()
    // first, or the flushed tail lands inside the stream.
    void flush(std::vector<float>& out);

    // Drops all state, as when a new session starts.
    void reset();

private:
    struct State;
    static void process_mono(State& s, std::span<const float> mono, std::vector<float>& out);

    std::unique_ptr<State> state_;
};

// One-shot conversion of a complete buffer. Same result as one push() plus flush().
std::vector<float> to_mono_16k(std::span<const float> interleaved, std::uint32_t rate,
                               std::uint32_t channels);

}  // namespace echoir::audio
