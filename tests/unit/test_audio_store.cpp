// The audio a session keeps: read-back of the stored samples, eviction, the level envelope, reset,
// and the input mute. Runs on the scripted backend and VAD from tests/support/live_fakes.h.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "../support/live_fakes.h"
#include "audio/resampler.h"
#include "doctest.h"
#include "errors.h"
#include "stream/audio_store.h"
#include "stream/session.h"

namespace {

namespace ts = echoir::test_support;
using echoir::SessionConfig;
using echoir::SessionEvent;
using echoir::stream::AudioStore;

constexpr std::int64_t kAll = std::numeric_limits<std::int64_t>::max();

// 16-bit values that fill the range, as floats the way 16-bit audio enters a session.
std::vector<std::int16_t> s16_ramp(std::size_t samples) {
    std::vector<std::int16_t> out(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(i * 7919 % 65536) - 32768);
    }
    return out;
}

std::vector<float> to_float(const std::vector<std::int16_t>& s16) {
    std::vector<float> out(s16.size());
    for (std::size_t i = 0; i < s16.size(); ++i) {
        out[i] = static_cast<float>(s16[i]) / 32768.0f;
    }
    return out;
}

std::vector<float> sine(std::size_t frames, double hz, double rate, double amplitude,
                        int channels) {
    constexpr double kTwoPi = 6.283185307179586;
    std::vector<float> out(frames * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v =
            static_cast<float>(amplitude * std::sin(kTwoPi * hz * static_cast<double>(i) / rate));
        for (int c = 0; c < channels; ++c) {
            out[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = v;
        }
    }
    return out;
}

std::vector<std::int16_t> read_all(const AudioStore& store, std::int64_t start_ms = 0,
                                   std::int64_t end_ms = kAll) {
    std::vector<std::int16_t> out(store.read_audio(start_ms, end_ms, nullptr, 0));
    CHECK(store.read_audio(start_ms, end_ms, out.data(), out.size()) == out.size());
    return out;
}

// A pulled session with the scripted backend and the energy VAD, with its events collected.
struct Rig {
    explicit Rig(std::int64_t retain_ms) {
        SessionConfig config;
        config.retain_audio_ms = retain_ms;
        session = std::make_unique<echoir::Session>(
            backend, vad, config, [this](const SessionEvent& ev) { log.add(ev); });
    }
    ts::LiveBackend backend;
    ts::LiveVad vad;
    ts::EventLog log;
    std::unique_ptr<echoir::Session> session;  // last, so it is destroyed before the fakes
};

}  // namespace

TEST_CASE("audio store: 16 kHz mono audio reads back sample for sample") {
    Rig rig(2000);
    const std::vector<std::int16_t> pushed = s16_ramp(16000);
    const std::vector<float> pcm = to_float(pushed);
    // In pieces, so the store sees several appends.
    for (std::size_t at = 0; at < pcm.size(); at += 1000) {
        const std::size_t n = std::min<std::size_t>(1000, pcm.size() - at);
        rig.session->push(std::span<const float>(pcm.data() + at, n), 16000, 1);
    }
    CHECK(read_all(rig.session->audio()) == pushed);
    // A sub-range starts at its own time on the timeline, and the end is clamped to the newest.
    const std::vector<std::int16_t> tail = read_all(rig.session->audio(), 250, 5000);
    REQUIRE(tail.size() == 16000 - 250 * 16);
    CHECK(std::equal(tail.begin(), tail.end(), pushed.begin() + 250 * 16));
    CHECK(rig.session->audio().read_audio(1000, 2000, nullptr, 0) == 0);
}

TEST_CASE("audio store: 48 kHz stereo is stored as the resampler's 16 kHz mono output") {
    Rig rig(2000);
    const std::vector<float> pcm = sine(48000, 440.0, 48000.0, 0.5, 2);
    for (std::size_t at = 0; at < pcm.size(); at += 960) {
        rig.session->push(std::span<const float>(pcm.data() + at, 960), 48000, 2);
    }
    echoir::audio::Resampler reference(48000, 2);
    std::vector<float> expected;
    reference.push(pcm, expected);

    const std::vector<std::int16_t> stored = read_all(rig.session->audio());
    REQUIRE(stored.size() == expected.size());
    CHECK(stored.size() > 15900);
    double sum = 0.0;
    for (std::size_t i = 0; i < stored.size(); ++i) {
        CHECK(std::abs(static_cast<double>(stored[i]) - expected[i] * 32768.0) <= 1.0);
        const double v = stored[i] / 32768.0;
        sum += v * v;
    }
    // The 440 Hz tone passes the filter: its RMS is the amplitude over sqrt(2).
    CHECK(std::sqrt(sum / static_cast<double>(stored.size())) ==
          doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.03));
}

TEST_CASE("audio store: evicted audio is refused") {
    AudioStore store(1000);
    const std::vector<float> pcm = to_float(s16_ramp(3 * 16000));
    store.append(pcm);
    CHECK_THROWS_AS(store.read_audio(0, 100, nullptr, 0), echoir::StateError);
    CHECK_THROWS_AS(store.read_audio(1999, 2500, nullptr, 0), echoir::StateError);
    CHECK(store.read_audio(2000, 3000, nullptr, 0) == 16000);
    std::vector<std::int16_t> small(10);
    CHECK_THROWS_AS(store.read_audio(2000, 3000, small.data(), small.size()),
                    std::invalid_argument);
    CHECK_THROWS_AS(store.read_audio(-1, 10, nullptr, 0), std::invalid_argument);
    CHECK_THROWS_AS(store.read_audio(20, 10, nullptr, 0), std::invalid_argument);

    AudioStore none(0);
    none.append(pcm);
    CHECK_THROWS_AS(none.read_audio(0, 10, nullptr, 0), echoir::StateError);
    // The envelope is kept with no audio.
    CHECK(none.read_peaks(0, kAll, 10, nullptr, 0) == 300);
}

TEST_CASE("audio store: peaks of a sine, per 10 ms and per bucket") {
    AudioStore store(0);
    // 1 kHz at 16 kHz is 16 samples per period, so every 10 ms cell holds whole periods. 1005 ms:
    // the last 5 ms are not a whole cell yet.
    const std::vector<float> pcm = sine(16080, 1000.0, 16000.0, 0.5, 1);
    store.append(pcm);
    REQUIRE(store.read_peaks(0, kAll, 10, nullptr, 0) == 100);

    std::vector<float> cells(200);
    REQUIRE(store.read_peaks(0, kAll, 10, cells.data(), 100) == 100);
    for (std::size_t c = 0; c < 100; ++c) {
        CHECK(cells[c * 2] == doctest::Approx(-0.5).epsilon(0.01));
        CHECK(cells[c * 2 + 1] == doctest::Approx(0.5).epsilon(0.01));
    }
    // 50 ms buckets over [100, 345): 25 cells, in 5 buckets.
    std::vector<float> buckets(10);
    REQUIRE(store.read_peaks(100, 345, 50, nullptr, 0) == 5);
    REQUIRE(store.read_peaks(100, 345, 50, buckets.data(), 5) == 5);
    CHECK(buckets[9] == doctest::Approx(0.5).epsilon(0.01));
    CHECK_THROWS_AS(store.read_peaks(100, 345, 50, buckets.data(), 4), std::invalid_argument);
    CHECK_THROWS_AS(store.read_peaks(0, 100, 15, nullptr, 0), std::invalid_argument);
    CHECK_THROWS_AS(store.read_peaks(0, 100, 0, nullptr, 0), std::invalid_argument);
    CHECK(store.read_peaks(2000, 3000, 10, nullptr, 0) == 0);

    // Silence after the tone: the newest bucket, the input level, drops to 0.
    store.append(std::vector<float>(800, 0.0f));
    const std::size_t n = store.read_peaks(0, kAll, 10, nullptr, 0);
    const auto newest = static_cast<std::int64_t>(n) * 10;
    std::vector<float> level(2);
    REQUIRE(store.read_peaks(newest - 10, newest, 10, level.data(), 1) == 1);
    CHECK(level[0] == 0.0f);
    CHECK(level[1] == 0.0f);
}

TEST_CASE("audio store: reset empties the audio and the envelope") {
    Rig rig(2000);
    std::size_t phase = 0;
    rig.session->push(ts::tone(16000, true, phase), 16000, 1);
    REQUIRE(rig.session->audio().read_audio(0, kAll, nullptr, 0) == 16000);
    rig.session->reset();
    CHECK(rig.session->audio().read_audio(0, kAll, nullptr, 0) == 0);
    CHECK(rig.session->audio().read_peaks(0, kAll, 10, nullptr, 0) == 0);

    // The timeline restarts at 0: new audio is read back from 0.
    const std::vector<std::int16_t> pushed = s16_ramp(1600);
    rig.session->push(to_float(pushed), 16000, 1);
    CHECK(read_all(rig.session->audio()) == pushed);
}

TEST_CASE("audio store: a muted stretch is silence that keeps the timeline and ends the segment") {
    Rig rig(5000);
    std::size_t phase = 0;
    rig.session->push(ts::tone(1500 * 16, true, phase), 16000, 1);
    rig.session->process();
    REQUIRE(rig.log.count(ECHOIR_EVENT_PARTIAL) == 1);
    REQUIRE(rig.log.count(ECHOIR_EVENT_FINAL) == 0);

    rig.session->set_input_muted(true);
    rig.session->push(ts::tone(1000 * 16, true, phase), 16000, 1);
    rig.session->process();
    rig.session->set_input_muted(false);
    rig.session->push(ts::tone(500 * 16, true, phase), 16000, 1);

    // The ongoing segment ended where the mute began.
    const auto entries = rig.log.snapshot();
    const auto final_it = std::find_if(entries.begin(), entries.end(), [](const auto& e) {
        return e.event.type == ECHOIR_EVENT_FINAL;
    });
    REQUIRE(final_it != entries.end());
    CHECK(final_it->event.start_ms == 0);
    CHECK(final_it->event.end_ms == 1500);
    CHECK(final_it->event.text == "w1500");

    const std::vector<std::int16_t> stored = read_all(rig.session->audio());
    REQUIRE(stored.size() == 3000 * 16);
    CHECK(std::all_of(stored.begin() + 1500 * 16, stored.begin() + 2500 * 16,
                      [](std::int16_t s) { return s == 0; }));
    CHECK(std::any_of(stored.begin() + 2500 * 16, stored.end(),
                      [](std::int16_t s) { return s != 0; }));
}
