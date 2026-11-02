// Two sessions on one loaded model, each on its own thread: they take turns on the decoder and the
// VAD, and each gets the transcript it gets alone. Skips when the model or the fixture is missing.
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/wav.h"
#include "doctest.h"
#include "stream/engine_gate.h"
#include "stream/session.h"
#include "stream_support.h"
#include "test_support.h"

namespace ea = echoir::audio;
namespace et = echoir::test;

namespace {

std::vector<std::string> final_texts(const std::vector<echoir::SessionEvent>& events) {
    std::vector<std::string> out;
    for (const echoir::SessionEvent& ev : et::finals_of(events)) {
        out.push_back(ev.text);
    }
    return out;
}

}  // namespace

TEST_CASE("shared engine: two whisper sessions on one model each transcribe jfk as alone") {
    const auto fixture = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(fixture, "jfk.wav fixture");
    auto engine = et::load_engine(et::kWhisper);
    ECHOIR_REQUIRE_OR_SKIP(engine, "whisper-tiny-en and silero-v6.2.0 models");
    const ea::PcmBuffer wav = ea::read_wav(*fixture);

    const std::vector<std::string> alone =
        final_texts(et::stream_all(*engine->asr, *engine->vad, wav));
    REQUIRE(!alone.empty());

    auto gate = std::make_shared<echoir::stream::EngineGate>();
    std::vector<echoir::SessionEvent> events_b;
    std::thread other(
        [&] { events_b = et::stream_all(*engine->asr, *engine->vad, wav, {}, gate); });
    const std::vector<echoir::SessionEvent> events_a =
        et::stream_all(*engine->asr, *engine->vad, wav, {}, gate);
    other.join();

    const std::int64_t duration = et::duration_ms_of(wav);
    et::check_invariants(events_a, duration);
    et::check_invariants(events_b, duration);
    CHECK(final_texts(events_a) == alone);
    CHECK(final_texts(events_b) == alone);
    std::string joined;
    for (const std::string& text : final_texts(events_a)) joined += text;
    CHECK(joined.find("ask not") != std::string::npos);
}
