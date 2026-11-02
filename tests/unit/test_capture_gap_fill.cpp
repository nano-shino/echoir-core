#include <chrono>

#include "capture/gap_fill.h"
#include "doctest.h"

using echoir::capture::GapFill;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kRate = 48000;

// Frame counts that differ only by the rounding of a fraction of a second.
bool near(std::size_t a, std::size_t b) { return a + 1 >= b && b + 1 >= a; }

}  // namespace

TEST_CASE("GapFill: no silence while the device keeps sending") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    for (int i = 1; i <= 50; ++i) {
        const auto now = t0 + i * 20ms;
        gap.on_audio(kRate / 50, kRate, now);
        CHECK(gap.silence_frames(kRate, now) == 0);
    }
}

TEST_CASE("GapFill: a stall shorter than idle is not filled") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(kRate / 10, kRate, t0 + 100ms);
    CHECK(gap.silence_frames(kRate, t0 + 299ms) == 0);
}

TEST_CASE("GapFill: a silent device is filled up to the clock") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(kRate, kRate, t0 + 1s);

    const auto later = t0 + 3s;
    const std::size_t missing = gap.silence_frames(kRate, later);
    CHECK(missing == 2 * kRate);
    gap.on_silence(missing, kRate);
    CHECK(gap.silence_frames(kRate, later) == 0);

    // The gap keeps being filled while the device stays silent.
    CHECK(gap.silence_frames(kRate, later + 20ms) == kRate / 50);
}

TEST_CASE("GapFill: a device that never sends is filled from the start") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    CHECK(gap.silence_frames(kRate, t0 + 100ms) == 0);
    CHECK(gap.silence_frames(kRate, t0 + 500ms) == kRate / 2);
}

TEST_CASE("GapFill: audio ahead of the clock adds no silence") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(2 * kRate, kRate, t0 + 1s);
    CHECK(gap.silence_frames(kRate, t0 + 1500ms) == 0);
}

TEST_CASE("GapFill: a new rate after a reopen keeps the covered time") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(kRate, kRate, t0 + 1s);
    CHECK(gap.silence_frames(44100, t0 + 2s) == 44100);
}

TEST_CASE("GapFill: a long gap is filled only up to max_fill, and its end is a seam") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(kRate, kRate, t0 + 1s);

    // The device stays quiet for a minute: the fill stops after 5 s of it.
    std::size_t filled = 0;
    for (auto now = t0 + 1s + 20ms; now <= t0 + 61s; now += 20ms) {
        const std::size_t n = gap.silence_frames(kRate, now);
        gap.on_silence(n, kRate);
        filled += n;
    }
    CHECK(near(filled, 5 * kRate));
    CHECK(gap.silence_frames(kRate, t0 + 61s) == 0);

    // The audio after it is a seam. The 55 s past the fill are off the timeline, so the next short
    // gap is filled from where the audio ends, not from the wall clock.
    CHECK(gap.resume(t0 + 61s, 0.0));
    gap.on_audio(kRate / 50, kRate, t0 + 61s);
    CHECK(gap.silence_frames(kRate, t0 + 61s + 20ms) == 0);
    CHECK(near(gap.silence_frames(kRate, t0 + 62s), kRate - kRate / 50));
    CHECK_FALSE(gap.resume(t0 + 62s, 0.0));
}

TEST_CASE("GapFill: a short gap is not a seam") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(kRate, kRate, t0 + 1s);
    CHECK(gap.silence_frames(kRate, t0 + 5s) == 4 * kRate);
    gap.on_silence(4 * kRate, kRate);
    CHECK_FALSE(gap.resume(t0 + 5s, 0.0));
}

TEST_CASE("GapFill: after a stall of hours, at most max_fill is pushed at once") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(kRate, kRate, t0 + 1s);
    // Nothing runs for three hours (a system sleep). On wake, half a second of audio is queued.
    const auto wake = t0 + 3h;
    CHECK(gap.silence_frames(kRate, wake - 500ms) == 5 * kRate);
    gap.on_silence(5 * kRate, kRate);
    CHECK(gap.resume(wake, 0.5));
    gap.on_audio(kRate / 2, kRate, wake);
    // The timeline is back on the clock after the queued audio: no debt, no surplus.
    CHECK(near(gap.silence_frames(kRate, wake + 300ms), 3 * kRate / 10));
}

TEST_CASE("GapFill: audio queued during a gap counts toward it") {
    GapFill gap(200ms, 5s);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(kRate, kRate, t0 + 1s);
    // 6 s since the last audio, but the queued audio began 1.5 s ago: a 4.5 s gap.
    CHECK_FALSE(gap.resume(t0 + 7s, 1.5));
}
