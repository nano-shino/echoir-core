// Streaming session on real models. The session's invariants must hold over a whole run, and the
// forced cut must fire on a long run of speech. Each case skips when its model or fixture is
// missing.
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "audio/wav.h"
#include "doctest.h"
#include "stream/session.h"
#include "stream_support.h"
#include "test_support.h"
#include "vad/vad.h"

namespace ea = echoir::audio;
namespace et = echoir::test;

namespace {

// A forced cut is searched in the 3 s before the cap, so a cut segment is at least 17 s long.
constexpr std::int64_t kMinCutSegmentMs = 17000;

// The whole span as one speech range. The session then keeps its open segment running until the
// forced cut, so every cut is a cap cut, not a pause that a real VAD would have split at.
class FullSpanVad final : public echoir::vad::IVad {
public:
    std::vector<echoir::vad::Range> detect(std::span<const float> pcm16k_mono) override {
        if (pcm16k_mono.size() < 16) return {};
        return {echoir::vad::Range{0, static_cast<std::int64_t>(pcm16k_mono.size()) / 16}};
    }
    std::string name() const override { return "full-span"; }
};

}  // namespace

TEST_CASE("stream session keeps its invariants on sentences_en with whisper") {
    const auto fixture = et::fixture("sentences_en");
    ECHOIR_REQUIRE_OR_SKIP(fixture, "sentences_en.wav fixture");
    auto engine = et::load_engine(et::kWhisper);
    ECHOIR_REQUIRE_OR_SKIP(engine, "whisper-tiny-en or silero-v6.2.0 model");
    const ea::PcmBuffer wav = ea::read_wav(*fixture);
    et::check_invariants(et::stream_all(*engine->asr, *engine->vad, wav), et::duration_ms_of(wav));
}

TEST_CASE("stream session keeps its invariants on sentences_en with qwen") {
    const auto fixture = et::fixture("sentences_en");
    ECHOIR_REQUIRE_OR_SKIP(fixture, "sentences_en.wav fixture");
    auto engine = et::load_engine(et::kQwen);
    ECHOIR_REQUIRE_OR_SKIP(engine, "qwen3-asr-0.6b-q8_0 or firered-vad-v1 model");
    const ea::PcmBuffer wav = ea::read_wav(*fixture);
    et::check_invariants(et::stream_all(*engine->asr, *engine->vad, wav), et::duration_ms_of(wav));
}

// runon_en is continuous speech, so the open segment reaches the cap, and the session must cut it.
// The cut is the quietest block in the last 3 s before the cap, so each cut segment lasts between
// 17 s and 20 s. Only the final flushed segment may be shorter.
static void check_forced_cuts(const et::BackendCase& backend) {
    const auto fixture = et::fixture("runon_en");
    ECHOIR_REQUIRE_OR_SKIP(fixture, "runon_en.wav fixture");
    // The VAD is replaced below, so only the backend is needed here.
    auto asr = et::load_asr(backend);
    ECHOIR_REQUIRE_OR_SKIP(asr, "speech model for the streaming golden test");
    const ea::PcmBuffer wav = ea::read_wav(*fixture);
    FullSpanVad span_vad;
    const auto events = et::stream_all(**asr, span_vad, wav);
    et::check_invariants(events, et::duration_ms_of(wav));

    const auto finals = et::finals_of(events);
    REQUIRE(finals.size() >= 2);
    for (std::size_t i = 0; i + 1 < finals.size(); ++i) {
        const std::int64_t length = finals[i].end_ms - finals[i].start_ms;
        INFO("final " << i << " lasts " << length << " ms");
        CHECK(length >= kMinCutSegmentMs);
        CHECK(length <= et::kMaxSegmentMs);
    }
}

TEST_CASE("stream session forces a cut on runon_en with whisper") {
    check_forced_cuts(et::kWhisper);
}

TEST_CASE("stream session forces a cut on runon_en with qwen") { check_forced_cuts(et::kQwen); }
