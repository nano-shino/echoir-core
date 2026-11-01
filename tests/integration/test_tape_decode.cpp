// Audio read back from the session tape's Opus pages must transcribe as well as the original: a
// stream run over the decoded pages scores within the max_rate of the fixture's stream case in
// tests/e2e/cases.json, on Whisper and on Qwen3-ASR. Each case skips when its model or fixture is
// missing.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "audio/resampler.h"
#include "audio/wav.h"
#include "doctest.h"
#include "json.hpp"
#include "stream/session.h"
#include "stream/tape.h"
#include "stream_support.h"
#include "test_support.h"
#include "text/score.h"

namespace et = echoir::test;

namespace {

constexpr std::int64_t kSpm = 16;

// The fixture as 16 kHz mono, the format the tape and the session work in.
std::vector<float> to_16k_mono(const echoir::audio::PcmBuffer& wav) {
    echoir::audio::Resampler resampler(wav.rate, wav.channels);
    std::vector<float> out;
    resampler.push(wav.samples, out);
    resampler.flush(out);
    return out;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

double case_max_rate(const std::string& id) {
    std::ifstream in(et::source_dir() / "tests" / "e2e" / "cases.json");
    REQUIRE(in);
    for (const auto& c : nlohmann::json::parse(in)) {
        if (c.at("id") == id) return c.at("max_rate").get<double>();
    }
    FAIL("no e2e case " << id);
    return 0.0;
}

// Streams 16 kHz mono audio through a session with the stream defaults of the echoir tool (the
// agreement policy, 10 s segments), in 500 ms pushes, and returns the FINAL text joined by spaces.
std::string transcribe(et::Engine& engine, const std::vector<float>& pcm) {
    std::vector<std::string> finals;
    echoir::SessionConfig config;
    config.policy = echoir::StreamPolicy::agreement;
    echoir::Session session(*engine.asr, *engine.vad, config,
                            [&finals](const echoir::SessionEvent& ev) {
                                if (ev.type == ECHOIR_EVENT_FINAL && !ev.text.empty()) {
                                    finals.push_back(ev.text);
                                }
                            });
    const std::size_t chunk = 500 * kSpm;
    for (std::size_t at = 0; at < pcm.size(); at += chunk) {
        const std::size_t n = std::min(chunk, pcm.size() - at);
        session.push(std::span<const float>(pcm.data() + at, n), 16000, 1);
        session.process();
    }
    session.flush();
    std::string joined;
    for (const auto& text : finals) {
        if (!joined.empty()) joined += ' ';
        joined += text;
    }
    return joined;
}

void check_paged_audio_scores(const et::BackendCase& backend, const std::string& case_id) {
    const auto fixture = et::fixture("pd_nasa_ep180");
    ECHOIR_REQUIRE_OR_SKIP(fixture, "pd_nasa_ep180.wav fixture");
    auto engine = et::load_engine(backend, /*use_gpu=*/true);
    ECHOIR_REQUIRE_OR_SKIP(engine, std::string(backend.model_id) + " and its VAD");
    const std::string ref =
        read_text(et::source_dir() / "tests" / "fixtures" / "pd_nasa_ep180.ref.txt");
    const double max_rate = case_max_rate(case_id);

    // The fixture, then 3 minutes of silence: the fixture is then older than the 2 minutes the
    // tape keeps in memory, so it is read back from a page file.
    const std::vector<float> original = to_16k_mono(echoir::audio::read_wav(*fixture));
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("echoir-tape-decode-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<float> paged;
    {
        auto tape = echoir::stream::Tape::create(folder);
        {
            auto writer = tape->attach("mic");
            writer->append(original);
            writer->append(std::vector<float>(static_cast<std::size_t>(3 * 60000 * kSpm), 0.0f));
        }
        REQUIRE(std::filesystem::exists(folder / "mic.00001.opus"));
        const auto end_ms = static_cast<std::int64_t>(original.size()) / kSpm;
        std::vector<std::int16_t> s16(tape->read_audio("mic", 0, end_ms, nullptr, 0));
        tape->read_audio("mic", 0, end_ms, s16.data(), s16.size());
        for (const std::int16_t s : s16) paged.push_back(static_cast<float>(s) / 32768.0f);
    }
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);

    const auto from_pages = echoir::text::error_rate(transcribe(*engine, paged), ref);
    const auto from_original = echoir::text::error_rate(transcribe(*engine, original), ref);
    const std::string model = backend.model_id;
    MESSAGE(model << ": rate " << from_pages.rate << " from the Opus pages, " << from_original.rate
                  << " from the original, max_rate " << max_rate);
    CHECK(from_pages.rate <= max_rate);
}

}  // namespace

TEST_CASE("tape: Opus-paged audio transcribes within the stream max_rate with whisper") {
    check_paged_audio_scores(et::kWhisper, "whisper_ep180_stream");
}

TEST_CASE("tape: Opus-paged audio transcribes within the stream max_rate with qwen") {
    check_paged_audio_scores(et::kQwen, "qwen_ep180_stream");
}
