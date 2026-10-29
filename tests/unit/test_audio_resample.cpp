#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "audio/resampler.h"
#include "doctest.h"

namespace ea = echoir::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Interleaved sine, the same value in every channel.
std::vector<float> sine(std::uint32_t rate, std::uint32_t channels, double freq_hz, double seconds,
                        float amplitude) {
    const auto frames = static_cast<std::size_t>(rate * seconds);
    std::vector<float> out(frames * channels);
    const double step = 2.0 * kPi * freq_hz / rate;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(amplitude * std::sin(step * static_cast<double>(f)));
        for (std::uint32_t c = 0; c < channels; ++c) out[f * channels + c] = v;
    }
    return out;
}

// Sign changes in the signal. A sine at f Hz for T seconds gives about 2 f T.
std::size_t zero_crossings(const std::vector<float>& x) {
    std::size_t n = 0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("to_mono_16k: 48 kHz stereo sine keeps its frequency within 1%") {
    const std::vector<float> in = sine(48000, 2, 1000.0, 2.0, 0.5f);
    const std::vector<float> out = ea::to_mono_16k(in, 48000, 2);

    // Output is 2 s at 16 kHz plus the filter's ringing after the last sample (8 frames here).
    CHECK(out.size() >= 32000);
    CHECK(out.size() <= 32000 + 16);

    const double expected = 2.0 * 1000.0 * 2.0;  // 4000 crossings
    const auto crossings = static_cast<double>(zero_crossings(out));
    CHECK(std::abs(crossings - expected) <= expected * 0.01);

    // The filter should not gain or lose much amplitude in the passband.
    float peak = 0.0f;
    for (std::size_t i = 1000; i + 1000 < out.size(); ++i) peak = std::max(peak, std::abs(out[i]));
    CHECK(peak > 0.45f);
    CHECK(peak < 0.55f);
}

TEST_CASE("to_mono_16k: 8 kHz mono sine upsamples with its frequency intact") {
    const std::vector<float> in = sine(8000, 1, 500.0, 2.0, 0.5f);
    const std::vector<float> out = ea::to_mono_16k(in, 8000, 1);

    // Upsampling 2x doubles the ringing in output frames (about 20 here).
    CHECK(out.size() >= 32000);
    CHECK(out.size() <= 32000 + 32);
    const double expected = 2.0 * 500.0 * 2.0;  // 2000 crossings
    const auto crossings = static_cast<double>(zero_crossings(out));
    CHECK(std::abs(crossings - expected) <= expected * 0.01);
}

TEST_CASE("to_mono_16k: 16 kHz mono input passes through unchanged") {
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> in(4000);
    for (float& s : in) s = dist(rng);

    const std::vector<float> out = ea::to_mono_16k(in, 16000, 1);
    bool same = out == in;
    CHECK(same);
}

TEST_CASE("to_mono_16k: stereo channels are averaged") {
    // Left is 0.8, right is 0.2, so the mono value should be 0.5 at 16 kHz.
    std::vector<float> in;
    for (int i = 0; i < 1600; ++i) {
        in.push_back(0.8f);
        in.push_back(0.2f);
    }
    const std::vector<float> out = ea::to_mono_16k(in, 16000, 2);
    REQUIRE(out.size() == 1600);
    CHECK(out[800] == doctest::Approx(0.5f));
}

TEST_CASE("Resampler: pushing in chunks gives the same output as one push") {
    // State carried across push() calls is what makes chunked streaming seamless.
    const std::vector<float> in = sine(48000, 2, 440.0, 1.0, 0.7f);

    const std::vector<float> whole = ea::to_mono_16k(in, 48000, 2);

    ea::Resampler r(48000, 2);
    std::vector<float> chunked;
    const std::size_t chunk_frames[] = {1, 7, 480, 1000, 3, 2048};
    std::size_t at = 0;
    std::size_t k = 0;
    while (at < in.size()) {
        const std::size_t frames = chunk_frames[k % 6];
        const std::size_t take = std::min(frames * 2, in.size() - at);
        r.push(std::span<const float>(in).subspan(at, take), chunked);
        at += take;
        ++k;
    }
    r.flush(chunked);

    REQUIRE(chunked.size() == whole.size());
    bool same = chunked == whole;
    CHECK(same);
}

TEST_CASE("Resampler: reset() makes the next stream independent of the last one") {
    const std::vector<float> in = sine(48000, 2, 300.0, 0.5, 0.6f);
    const std::vector<float> fresh = ea::to_mono_16k(in, 48000, 2);

    ea::Resampler r(48000, 2);
    std::vector<float> discard;
    r.push(in, discard);
    r.flush(discard);
    r.reset();

    std::vector<float> again;
    r.push(in, again);
    r.flush(again);
    REQUIRE(again.size() == fresh.size());
    bool same = again == fresh;
    CHECK(same);
}

TEST_CASE("Resampler: rejects zero rate, zero channels, and a partial frame") {
    CHECK_THROWS_AS(ea::Resampler(0, 1), std::invalid_argument);
    CHECK_THROWS_AS(ea::Resampler(48000, 0), std::invalid_argument);

    ea::Resampler r(48000, 2);
    std::vector<float> out;
    const float odd[] = {0.1f, 0.2f, 0.3f};
    CHECK_THROWS_AS(r.push(odd, out), std::invalid_argument);
}

TEST_CASE("Resampler: moving keeps the stream state") {
    const std::vector<float> in = sine(48000, 1, 200.0, 0.5, 0.5f);
    const std::vector<float> whole = ea::to_mono_16k(in, 48000, 1);

    ea::Resampler a(48000, 1);
    std::vector<float> out;
    const std::size_t half = in.size() / 2;
    a.push(std::span<const float>(in).first(half), out);
    ea::Resampler b(std::move(a));
    b.push(std::span<const float>(in).subspan(half), out);
    b.flush(out);

    REQUIRE(out.size() == whole.size());
    bool same = out == whole;
    CHECK(same);
}

TEST_CASE("Resampler: flush keeps the ringing after the last input sample") {
    // The reference is the same stream followed by a second of silence, and it is never flushed.
    // Flush must return the same first samples, and drop nothing at or above one 16-bit step.
    const float lsb = 1.0f / 32768.0f;
    const std::uint32_t rates[] = {8000, 22050, 44100, 48000, 96000};
    for (std::uint32_t rate : rates) {
        const std::size_t n = rate / 10;
        std::vector<float> impulse(n, 0.0f);
        impulse[n - 1] = 1.0f;
        std::mt19937 rng(rate);
        std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
        std::vector<float> burst(n);
        for (float& s : burst) s = dist(rng);

        for (const std::vector<float>* in : {&impulse, &burst}) {
            std::vector<float> padded = *in;
            padded.resize(n + rate, 0.0f);
            ea::Resampler reference(rate, 1);
            std::vector<float> truth;
            reference.push(padded, truth);

            const std::vector<float> got = ea::to_mono_16k(*in, rate, 1);
            REQUIRE(got.size() <= truth.size());
            bool prefix = std::equal(got.begin(), got.end(), truth.begin());
            CHECK(prefix);
            float missed = 0.0f;
            for (std::size_t k = got.size(); k < truth.size(); ++k) {
                missed = std::max(missed, std::abs(truth[k]));
            }
            CHECK(missed < lsb);
        }
    }
}

TEST_CASE("Resampler: flush after silence adds no samples") {
    ea::Resampler r(48000, 1);
    std::vector<float> out;
    const std::vector<float> silence(4800, 0.0f);
    r.push(silence, out);
    const std::size_t pushed = out.size();
    r.flush(out);
    CHECK(out.size() == pushed);
}
