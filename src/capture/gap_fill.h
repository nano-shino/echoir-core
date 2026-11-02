// Keeps a live capture's timeline on the wall clock, across short gaps.
//
// A session's timestamps count the audio it was given. A loopback device sends nothing while no
// sound plays (WASAPI), and a lost default device sends nothing until one comes back, so without
// help the timeline would stop while real time goes on. GapFill says how much silence to push so
// the audio pushed since the start covers the time that passed.
//
// Only the first max_fill of a gap is filled. That is enough silence for the VAD to end the open
// segment. The rest of a longer gap, such as a quiet loopback for minutes or a system sleep, is
// left off the timeline: filling it would push its whole length through the VAD, the decoder and
// the stored audio, and after a sleep all at once. The audio after such a gap starts after a seam.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace echoir::capture {

class GapFill {
public:
    using Clock = std::chrono::steady_clock;

    // A gap is filled once the device has sent nothing for idle, and for at most max_fill of it. A
    // shorter stall is jitter, and the audio it holds up still arrives.
    GapFill(Clock::duration idle, Clock::duration max_fill) : idle_(idle), max_fill_(max_fill) {}

    // The capture starts at now.
    void start(Clock::time_point now) {
        start_ = now;
        last_audio_ = now;
        covered_seconds_ = 0.0;
        skipped_seconds_ = 0.0;
    }

    // Device audio is about to be pushed at now, and the first of it was captured queued_seconds
    // before now. Push the gap's silence (silence_frames) first. Returns true when the audio ends a
    // gap longer than max_fill: the time past max_fill is then left off the timeline, and the audio
    // starts after a seam.
    bool resume(Clock::time_point now, double queued_seconds) {
        const double gap = seconds(now - last_audio_) - std::max(0.0, queued_seconds);
        const double max_fill = seconds(max_fill_);
        if (gap <= max_fill) {
            return false;
        }
        skipped_seconds_ += gap - max_fill;
        return true;
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

    // The frames of silence at rate that bring the pushed audio up to now, or up to max_fill into
    // the gap when that is earlier. Zero while the device keeps sending, and when the pushed audio
    // is already ahead of the clock.
    std::size_t silence_frames(std::uint32_t rate, Clock::time_point now) const {
        if (rate == 0 || now - last_audio_ < idle_) {
            return 0;
        }
        const Clock::time_point until = std::min(now, last_audio_ + max_fill_);
        const double behind = seconds(until - start_) - skipped_seconds_ - covered_seconds_;
        if (behind <= 0.0) {
            return 0;
        }
        return static_cast<std::size_t>(std::floor(behind * rate));
    }

private:
    static double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

    Clock::duration idle_;
    Clock::duration max_fill_;
    Clock::time_point start_{};
    Clock::time_point last_audio_{};
    double covered_seconds_ = 0.0;
    // The time of long gaps that was left off the timeline.
    double skipped_seconds_ = 0.0;
};

}  // namespace echoir::capture
