// Keeps a live capture's timeline on the wall clock.
//
// A session's timestamps count the audio it was given. A loopback device sends nothing while no
// sound plays (WASAPI), and a lost default device sends nothing until one comes back, so without
// help the timeline would stop while real time goes on. GapFill says how much silence to push so
// the audio pushed since the start covers the time that passed.
#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace echoir::capture {

class GapFill {
public:
    using Clock = std::chrono::steady_clock;

    // A gap is filled once the device has sent nothing for idle. A shorter stall is jitter, and
    // the audio it holds up still arrives.
    explicit GapFill(Clock::duration idle) : idle_(idle) {}

    // The capture starts at now.
    void start(Clock::time_point now) {
        start_ = now;
        last_audio_ = now;
        covered_seconds_ = 0.0;
    }

    // frames of device audio at rate were pushed at now.
    void on_audio(std::size_t frames, std::uint32_t rate, Clock::time_point now) {
        covered_seconds_ += static_cast<double>(frames) / rate;
        last_audio_ = now;
    }

    // frames of silence at rate were pushed.
    void on_silence(std::size_t frames, std::uint32_t rate) {
        covered_seconds_ += static_cast<double>(frames) / rate;
    }

    // The frames of silence at rate that bring the pushed audio up to now. Zero while the device
    // keeps sending, and when the pushed audio is already ahead of the clock.
    std::size_t silence_frames(std::uint32_t rate, Clock::time_point now) const {
        if (rate == 0 || now - last_audio_ < idle_) {
            return 0;
        }
        const double behind =
            std::chrono::duration<double>(now - start_).count() - covered_seconds_;
        if (behind <= 0.0) {
            return 0;
        }
        return static_cast<std::size_t>(std::floor(behind * rate));
    }

private:
    Clock::duration idle_;
    Clock::time_point start_{};
    Clock::time_point last_audio_{};
    double covered_seconds_ = 0.0;
};

}  // namespace echoir::capture
