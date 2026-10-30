#include <chrono>

#include "capture/gap_fill.h"
#include "doctest.h"

using echoir::capture::GapFill;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kRate = 48000;

}  // namespace

TEST_CASE("GapFill: no silence while the device keeps sending") {
    GapFill gap(200ms);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    for (int i = 1; i <= 50; ++i) {
        const auto now = t0 + i * 20ms;
        gap.on_audio(kRate / 50, kRate, now);
        CHECK(gap.silence_frames(kRate, now) == 0);
    }
}

TEST_CASE("GapFill: a stall shorter than idle is not filled") {
    GapFill gap(200ms);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(kRate / 10, kRate, t0 + 100ms);
    CHECK(gap.silence_frames(kRate, t0 + 299ms) == 0);
}

TEST_CASE("GapFill: a silent device is filled up to the clock") {
    GapFill gap(200ms);
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
    GapFill gap(200ms);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    CHECK(gap.silence_frames(kRate, t0 + 100ms) == 0);
    CHECK(gap.silence_frames(kRate, t0 + 500ms) == kRate / 2);
}

TEST_CASE("GapFill: audio ahead of the clock adds no silence") {
    GapFill gap(200ms);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(2 * kRate, kRate, t0 + 1s);
    CHECK(gap.silence_frames(kRate, t0 + 1500ms) == 0);
}

TEST_CASE("GapFill: a new rate after a reopen keeps the covered time") {
    GapFill gap(200ms);
    const auto t0 = GapFill::Clock::now();
    gap.start(t0);
    gap.on_audio(kRate, kRate, t0 + 1s);
    CHECK(gap.silence_frames(44100, t0 + 2s) == 44100);
}
