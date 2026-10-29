#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "audio/fbank.h"
#include "audio/wav.h"
#include "doctest.h"

namespace ea = echoir::audio;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBins = ea::kFbankBins;
// Golden fbank files hold the first 300 frames of upstream's output (fewer for a short clip);
// golden feat files hold every frame, because the model's look-ahead needs them all.
constexpr std::size_t kGoldenRows = 300;
constexpr double kParityTolerance = 2e-3;
constexpr double kPi = 3.14159265358979323846;

fs::path fixtures_dir() { return fs::path(ECHOIR_SOURCE_DIR) / "tests" / "fixtures"; }
fs::path firered_dir() { return fixtures_dir() / "firered"; }

// Golden files are generated outside the build (tools/dev/firered_reference.py). When one is
// missing, the test prints SKIP and passes, so the suite runs without them.
bool present_or_skip(std::initializer_list<fs::path> paths) {
    for (const fs::path& p : paths) {
        if (fs::exists(p)) continue;
        MESSAGE("SKIP: missing " << p.string());
        std::fprintf(stderr, "SKIP: missing %s\n", p.string().c_str());
        return false;
    }
    return true;
}

// Reads little-endian float32 values, whatever the host byte order.
std::vector<float> read_f32le(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
    if (bytes.size() % 4 != 0) throw std::runtime_error("not float32 data: " + path.string());
    std::vector<float> out(bytes.size() / 4);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint32_t bits = static_cast<std::uint32_t>(bytes[4 * i]) |
                                   static_cast<std::uint32_t>(bytes[4 * i + 1]) << 8 |
                                   static_cast<std::uint32_t>(bytes[4 * i + 2]) << 16 |
                                   static_cast<std::uint32_t>(bytes[4 * i + 3]) << 24;
        out[i] = std::bit_cast<float>(bits);
    }
    return out;
}

// A 16 kHz mono clip as f32 in [-1, 1].
std::vector<float> read_mono_16k(const fs::path& path) {
    const ea::PcmBuffer wav = ea::read_wav(path);
    if (wav.rate != 16000 || wav.channels != 1) {
        throw std::runtime_error("expected 16 kHz mono: " + path.string());
    }
    return wav.samples;
}

std::size_t frame_count(std::size_t samples) {
    const std::vector<float> pcm(samples, 0.1f);
    const std::vector<float> out = ea::kaldi_fbank(pcm);
    CHECK(out.size() % kBins == 0);
    return out.size() / kBins;
}

// Compares the leading rows of `got`, a whole clip, with a golden file value by value. The golden
// file must hold at least min(300, frames) rows. Keeps NaN visible.
void expect_matches_golden(const std::vector<float>& got, const std::vector<float>& golden) {
    REQUIRE(golden.size() % kBins == 0);
    const std::size_t rows = golden.size() / kBins;
    REQUIRE(rows >= std::min(kGoldenRows, got.size() / kBins));
    REQUIRE(got.size() >= golden.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < golden.size(); ++i) {
        const double d = std::abs(static_cast<double>(got[i]) - static_cast<double>(golden[i]));
        if (std::isnan(d) || d > worst) worst = d;
    }
    CHECK(worst <= kParityTolerance);
}

}  // namespace

TEST_CASE("kaldi fbank: frames are 0 below 400 samples, then 1 + (n - 400) / 160") {
    CHECK(frame_count(0) == 0);
    CHECK(frame_count(399) == 0);
    CHECK(frame_count(400) == 1);
    CHECK(frame_count(560) == 2);
    CHECK(frame_count(16000) == 1 + (16000 - 400) / 160);
}

TEST_CASE("kaldi fbank: silence gives log(FLT_EPSILON) in every bin") {
    const std::vector<float> silence(1000, 0.0f);
    const std::vector<float> out = ea::kaldi_fbank(silence);
    REQUIRE(out.size() == 4 * kBins);
    const float floor_value = std::log(std::numeric_limits<float>::epsilon());
    bool all_floor = true;
    for (float v : out) all_floor = all_floor && std::abs(v - floor_value) < 1e-6f;
    CHECK(all_floor);
}

TEST_CASE("kaldi fbank: a 1 kHz tone peaks in the bin whose centre is nearest 1 kHz") {
    std::vector<float> tone(16000);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = static_cast<float>(
            0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / 16000.0));
    }
    const std::vector<float> out = ea::kaldi_fbank(tone);
    REQUIRE(out.size() >= kBins);
    const float* row = out.data() + (out.size() / kBins / 2) * kBins;
    std::size_t best = 0;
    for (std::size_t b = 1; b < kBins; ++b) {
        if (row[b] > row[best]) best = b;
    }

    // Mel edges as the filterbank spaces them: centre b sits at low + (b + 1) * step.
    const double mel_low = 1127.0 * std::log(1.0 + 20.0 / 700.0);
    const double mel_high = 1127.0 * std::log(1.0 + 8000.0 / 700.0);
    const double step = (mel_high - mel_low) / static_cast<double>(kBins + 1);
    const double centre = mel_low + static_cast<double>(best + 1) * step;
    const double tone_mel = 1127.0 * std::log(1.0 + 1000.0 / 700.0);
    // The nearest centre is within half a step of the tone. Any other centre is farther away.
    CHECK(std::abs(centre - tone_mel) <= step / 2.0);
}

TEST_CASE("apply_cmvn: subtracts the mean and scales by the inverse std, per bin") {
    std::vector<float> feats(2 * kBins, 10.0f);
    std::vector<float> mean(kBins);
    std::vector<float> istd(kBins);
    for (std::size_t b = 0; b < kBins; ++b) {
        mean[b] = static_cast<float>(b);
        istd[b] = 0.5f * static_cast<float>(b + 1);
    }
    ea::apply_cmvn(feats, mean, istd);
    bool ok = true;
    for (std::size_t i = 0; i < feats.size(); ++i) {
        const std::size_t b = i % kBins;
        const float want = (10.0f - static_cast<float>(b)) * 0.5f * static_cast<float>(b + 1);
        ok = ok && std::abs(feats[i] - want) < 1e-5f;
    }
    CHECK(ok);
}

TEST_CASE("apply_cmvn: rejects mismatched sizes") {
    std::vector<float> feats(kBins);
    const std::vector<float> ok_stats(kBins, 1.0f);
    const std::vector<float> short_stats(kBins - 1, 1.0f);
    CHECK_THROWS_AS(ea::apply_cmvn(feats, short_stats, ok_stats), std::invalid_argument);
    CHECK_THROWS_AS(ea::apply_cmvn(feats, ok_stats, short_stats), std::invalid_argument);
    std::vector<float> partial(kBins + 1, 0.0f);
    CHECK_THROWS_AS(ea::apply_cmvn(partial, ok_stats, ok_stats), std::invalid_argument);
}

TEST_CASE("kaldi fbank parity with the FireRedVAD reference: jfk.wav") {
    const fs::path golden = firered_dir() / "jfk.fbank.f32";
    const fs::path wav = fixtures_dir() / "jfk.wav";
    if (!present_or_skip({golden, wav})) return;

    const std::vector<float> got = ea::kaldi_fbank(read_mono_16k(wav));
    expect_matches_golden(got, read_f32le(golden));
}

TEST_CASE("kaldi fbank parity with the FireRedVAD reference: tone_noise.wav") {
    const fs::path golden = firered_dir() / "tone_noise.fbank.f32";
    const fs::path wav = firered_dir() / "tone_noise.wav";
    if (!present_or_skip({golden, wav})) return;

    const std::vector<float> got = ea::kaldi_fbank(read_mono_16k(wav));
    expect_matches_golden(got, read_f32le(golden));
}

TEST_CASE("kaldi fbank + CMVN parity with the FireRedVAD reference: jfk.wav") {
    const fs::path golden = firered_dir() / "jfk.feat.f32";
    const fs::path cmvn = firered_dir() / "cmvn.f32";
    const fs::path wav = fixtures_dir() / "jfk.wav";
    if (!present_or_skip({golden, cmvn, wav})) return;

    const std::vector<float> stats = read_f32le(cmvn);
    REQUIRE(stats.size() == 2 * kBins);
    std::vector<float> got = ea::kaldi_fbank(read_mono_16k(wav));
    ea::apply_cmvn(got, std::span<const float>(stats.data(), kBins),
                   std::span<const float>(stats.data() + kBins, kBins));
    expect_matches_golden(got, read_f32le(golden));
}

TEST_CASE("kaldi fbank + CMVN parity with the FireRedVAD reference: tone_noise.wav") {
    const fs::path golden = firered_dir() / "tone_noise.feat.f32";
    const fs::path cmvn = firered_dir() / "cmvn.f32";
    const fs::path wav = firered_dir() / "tone_noise.wav";
    if (!present_or_skip({golden, cmvn, wav})) return;

    const std::vector<float> stats = read_f32le(cmvn);
    REQUIRE(stats.size() == 2 * kBins);
    std::vector<float> got = ea::kaldi_fbank(read_mono_16k(wav));
    ea::apply_cmvn(got, std::span<const float>(stats.data(), kBins),
                   std::span<const float>(stats.data() + kBins, kBins));
    expect_matches_golden(got, read_f32le(golden));
}
