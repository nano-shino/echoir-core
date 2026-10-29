// FireRedVad as an IVad: ranges for jfk and silence, empty and very short spans, the factory, the
// post-processor settings, and the probabilities of a span run in several parts against one call
// over the whole span.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "audio/fbank.h"
#include "audio/resampler.h"
#include "audio/wav.h"
#include "doctest.h"
#include "errors.h"
#include "stream/vad_postprocess.h"
#include "test_support.h"
#include "vad/firered_model.h"
#include "vad/firered_vad.h"
#include "vad/vad.h"

namespace ea = echoir::audio;
namespace ev = echoir::vad;
namespace es = echoir::stream;
namespace et = echoir::test;

namespace {

// A FireRedVAD model in the registry, with the name its detector reports and the jfk.wav ranges
// that upstream FireRedVAD gives with its default settings (jfk.vad.ranges.json and
// jfk.stream.ranges.json in tests/fixtures/firered/).
struct Variant {
    const char* id;
    const char* name;
    bool stream;
    std::array<ev::Range, 4> jfk;
};

const std::array<Variant, 2> kVariants = {{
    {"firered-vad-v1",
     "firered",
     false,
     {{{270, 2420}, {3210, 4580}, {5330, 7810}, {8110, 10680}}}},
    {"firered-stream-vad-v1",
     "firered-stream",
     true,
     {{{280, 2500}, {3250, 4570}, {5380, 7830}, {8150, 11000}}}},
}};

// Runs body once per variant. The body is a function of its own, so a missing model skips only
// that variant.
template <typename Body>
void for_each_variant(Body body) {
    for (const Variant& v : kVariants) {
        INFO(v.id);
        body(v);
    }
}

std::vector<float> load_16k_mono(const std::filesystem::path& path) {
    const ea::PcmBuffer wav = ea::read_wav(path);
    return ea::to_mono_16k(wav.samples, wav.rate, wav.channels);
}

std::int64_t total_ms(const std::vector<ev::Range>& ranges) {
    std::int64_t sum = 0;
    for (const ev::Range& r : ranges) sum += r.end_ms - r.start_ms;
    return sum;
}

bool same_ranges(const std::vector<ev::Range>& a, const std::vector<ev::Range>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].start_ms != b[i].start_ms || a[i].end_ms != b[i].end_ms) return false;
    }
    return true;
}

// Largest absolute difference. Different lengths count as infinitely different.
float max_abs_diff(std::span<const float> a, std::span<const float> b) {
    if (a.size() != b.size()) {
        return std::numeric_limits<float>::infinity();
    }
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

// The clip repeated `times` times, end to end.
std::vector<float> repeated(std::span<const float> clip, std::size_t times) {
    std::vector<float> out;
    out.reserve(clip.size() * times);
    for (std::size_t i = 0; i < times; ++i) {
        out.insert(out.end(), clip.begin(), clip.end());
    }
    return out;
}

}  // namespace

TEST_CASE("firered VAD finds the speech in jfk.wav") {
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);
    for_each_variant([&](const Variant& v) {
        const auto model = et::model_file(v.id, "vad");
        ECHOIR_REQUIRE_OR_SKIP(model, std::string(v.id) + " (vad) model");

        ev::FireRedVad vad(*model);
        CHECK(vad.name() == v.name);
        CHECK(vad.params().min_speech_frame ==
              ev::FireRedVad::default_params(v.stream).min_speech_frame);
        const std::vector<ev::Range> ranges = vad.detect(pcm);
        REQUIRE(!ranges.empty());
        for (const ev::Range& r : ranges) {
            CHECK(r.start_ms >= 0);
            CHECK(r.end_ms <= 11000);
            CHECK(r.end_ms > r.start_ms);
        }
        for (std::size_t i = 1; i < ranges.size(); ++i) {
            CHECK(ranges[i - 1].end_ms <= ranges[i].start_ms);
        }
        CHECK(total_ms(ranges) > 7000);
        // Exact to the 10 ms frame against upstream, which merges no gaps. The defaults merge
        // them, so the upstream settings are the defaults with merge_silence_frame 0.
        es::VadPostprocessParams upstream = ev::FireRedVad::default_params(v.stream);
        upstream.merge_silence_frame = 0;
        ev::FireRedVad upstream_vad(*model, upstream);
        CHECK(same_ranges(upstream_vad.detect(pcm),
                          std::vector<ev::Range>(v.jfk.begin(), v.jfk.end())));
    });
}

TEST_CASE("firered VAD finds no speech in silence_30s") {
    const auto wav = et::fixture("silence_30s");
    ECHOIR_REQUIRE_OR_SKIP(wav, "silence_30s fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);
    for_each_variant([&](const Variant& v) {
        const auto model = et::model_file(v.id, "vad");
        ECHOIR_REQUIRE_OR_SKIP(model, std::string(v.id) + " (vad) model");

        ev::FireRedVad vad(*model);
        CHECK(vad.detect(pcm).empty());
    });
}

TEST_CASE("firered VAD gives no frames or ranges for an empty or very short span") {
    for_each_variant([&](const Variant& v) {
        const auto model = et::model_file(v.id, "vad");
        ECHOIR_REQUIRE_OR_SKIP(model, std::string(v.id) + " (vad) model");

        ev::FireRedVad vad(*model);
        CHECK(vad.detect(std::span<const float>{}).empty());
        CHECK(vad.speech_probs(std::span<const float>{}).empty());

        // A 25 ms window needs 400 samples, so 399 samples hold no frame at all.
        const std::vector<float> short_span(399, 0.0f);
        CHECK(vad.speech_probs(short_span).empty());
        CHECK(vad.detect(short_span).empty());

        // Frame i starts at sample 160 i, so 400 samples give one frame, 559 still one, and 560
        // give a second.
        const std::vector<float> one_window(400, 0.0f);
        const std::vector<float> just_under_two(559, 0.0f);
        const std::vector<float> two_windows(560, 0.0f);
        CHECK(vad.speech_probs(one_window).size() == 1);
        CHECK(vad.speech_probs(just_under_two).size() == 1);
        CHECK(vad.speech_probs(two_windows).size() == 2);
    });
}

TEST_CASE("make_vad returns the FireRedVad detector for each FireRed GGUF") {
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);
    for_each_variant([&](const Variant& v) {
        const auto model = et::model_file(v.id, "vad");
        ECHOIR_REQUIRE_OR_SKIP(model, std::string(v.id) + " (vad) model");

        const std::unique_ptr<ev::IVad> vad = ev::make_vad(*model);
        REQUIRE(vad != nullptr);
        CHECK(vad->name() == v.name);
        CHECK(vad->detect(std::span<const float>{}).empty());

        // The factory gives the same ranges as constructing the class directly.
        ev::FireRedVad direct(*model);
        CHECK(same_ranges(vad->detect(pcm), direct.detect(pcm)));
    });
}

TEST_CASE("firered VAD probabilities over several parts match one model call") {
    // Six copies of jfk are 66 s, or 6598 frames: three parts.
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> long_pcm = repeated(load_16k_mono(*wav), 6);
    const std::size_t frames = 1 + (long_pcm.size() - 400) / 160;
    REQUIRE(frames > 2 * ev::FireRedVad::kPartFrames);

    for_each_variant([&](const Variant& v) {
        const auto model_path = et::model_file(v.id, "vad");
        ECHOIR_REQUIRE_OR_SKIP(model_path, std::string(v.id) + " (vad) model");

        // Reference: the whole span in one call through the model.
        ev::FireRedModel model(*model_path);
        std::vector<float> feats = ea::kaldi_fbank(long_pcm);
        ea::apply_cmvn(feats, model.cmvn_mean(), model.cmvn_istd());
        const std::vector<float> expected = model.probs(feats, frames);

        ev::FireRedVad vad(*model_path);
        const std::vector<float> got = vad.speech_probs(long_pcm);
        REQUIRE(got.size() == expected.size());
        CHECK(max_abs_diff(got, expected) <= 1e-5f);

        const std::vector<ev::Range> ranges = vad.detect(long_pcm);
        REQUIRE(!ranges.empty());
        for (std::size_t i = 1; i < ranges.size(); ++i) {
            CHECK(ranges[i - 1].end_ms <= ranges[i].start_ms);
        }
        CHECK(ranges.back().end_ms <= 66000);
    });
}

TEST_CASE("firered VAD default post-processor settings are the upstream README values, merged") {
    const es::VadPostprocessParams full = ev::FireRedVad::default_params(false);
    CHECK(full.smooth_window_size == 5);
    CHECK(full.prob_threshold == 0.4);
    CHECK(full.min_speech_frame == 20);
    CHECK(full.max_speech_frame == 2000);
    CHECK(full.min_silence_frame == 20);
    // The one setting that differs from the upstream README, which uses 0.
    CHECK(full.merge_silence_frame == 50);
    CHECK(full.extend_speech_frame == 0);

    const es::VadPostprocessParams stream = ev::FireRedVad::default_params(true);
    CHECK(stream.min_speech_frame == 8);
    CHECK(stream.smooth_window_size == full.smooth_window_size);
    CHECK(stream.max_speech_frame == full.max_speech_frame);
}

TEST_CASE("firered streaming settings do not split long speech, and offline ones still do") {
    // 25 s of speech with one silent frame at 15 s. Offline, the span is split at that frame.
    std::vector<float> probs(2500, 0.9f);
    probs[1500] = 0.0f;
    // Smoothing is off, so the silent frame is the quietest one.
    es::VadPostprocessParams offline_params = ev::FireRedVad::default_params(false);
    offline_params.smooth_window_size = 1;
    es::VadPostprocessParams streaming_params =
        ev::FireRedVad::default_params(false, ev::VadUse::streaming);
    streaming_params.smooth_window_size = 1;
    const es::VadPostprocessor offline(offline_params);
    const es::VadPostprocessor streaming(streaming_params);

    const std::vector<ev::Range> split = offline.ranges(probs, 25.0);
    REQUIRE(split.size() == 2);
    CHECK(split[0].end_ms == 15000);
    CHECK(split[1].start_ms == 15010);

    const std::vector<ev::Range> whole = streaming.ranges(probs, 25.0);
    REQUIRE(whole.size() == 1);
    CHECK(whole[0].start_ms == 0);
    CHECK(whole[0].end_ms == 25000);
}

TEST_CASE("firered VAD streaming use reaches the detector through make_vad") {
    const auto model = et::model_file("firered-vad-v1", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "firered-vad-v1 (vad) model");

    const ev::FireRedVad offline(*model);
    const ev::FireRedVad streaming(*model, 2, {}, ev::VadUse::streaming);
    CHECK(offline.params().max_speech_frame == 2000);
    CHECK(streaming.params().max_speech_frame == std::numeric_limits<int>::max());

    const std::unique_ptr<ev::IVad> via_factory =
        ev::make_vad(*model, 2, {}, ev::VadUse::streaming);
    const auto* firered = dynamic_cast<const ev::FireRedVad*>(via_factory.get());
    REQUIRE(firered != nullptr);
    CHECK(firered->params().max_speech_frame == std::numeric_limits<int>::max());
}

TEST_CASE("firered VAD takes post-processor settings from its caller") {
    const auto model = et::model_file("firered-vad-v1", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "firered-vad-v1 (vad) model");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);

    ev::FireRedVad base(*model);
    es::VadPostprocessParams wider = ev::FireRedVad::default_params(false);
    wider.extend_speech_frame = 50;
    ev::FireRedVad extended(*model, wider);
    CHECK(extended.name() == "firered");
    CHECK(extended.params().extend_speech_frame == 50);
    // jfk has silence around its speech, so extending each range by 0.5 s adds time.
    CHECK(total_ms(extended.detect(pcm)) > total_ms(base.detect(pcm)));
}

TEST_CASE("firered VAD throws ModelLoadError for a missing model and rejects zero threads") {
    const std::filesystem::path missing = "/nonexistent/echoir/firered-missing.gguf";
    // Braces for the one-argument form: `T(x);` inside the macro would declare a variable.
    CHECK_THROWS_AS(ev::FireRedVad{missing}, echoir::ModelLoadError);
    CHECK_THROWS_AS(ev::FireRedVad(missing, 0), std::invalid_argument);
    CHECK_FALSE(std::is_copy_constructible_v<ev::FireRedVad>);
    CHECK(std::is_nothrow_move_constructible_v<ev::FireRedVad>);
}

TEST_CASE("firered VAD overrides through make_vad change its post-processor settings") {
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);
    for_each_variant([&](const Variant& v) {
        const auto model = et::model_file(v.id, "vad");
        ECHOIR_REQUIRE_OR_SKIP(model, std::string(v.id) + " (vad) model");

        ev::VadOverrides wider;
        wider.add("extend_speech_frame=50");
        const std::unique_ptr<ev::IVad> base = ev::make_vad(*model);
        const std::unique_ptr<ev::IVad> extended = ev::make_vad(*model, 2, wider);
        CHECK(total_ms(extended->detect(pcm)) > total_ms(base->detect(pcm)));

        // Only the named setting changes. The variant's own minimum speech stays.
        ev::FireRedVad direct(*model, 2, wider);
        CHECK(direct.params().extend_speech_frame == 50);
        CHECK(direct.params().min_speech_frame ==
              ev::FireRedVad::default_params(v.stream).min_speech_frame);
        CHECK(direct.params().prob_threshold ==
              ev::FireRedVad::default_params(v.stream).prob_threshold);
    });
}

TEST_CASE("each firered VAD override key sets its own field and changes the jfk ranges") {
    const auto model = et::model_file("firered-vad-v1", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "firered-vad-v1 (vad) model");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<ev::Range> base = ev::FireRedVad(*model).detect(pcm);

    struct Case {
        const char* setting;
        double (*field)(const es::VadPostprocessParams&);
        double value;
    };
    const Case cases[] = {
        {"smooth_window_size=1", [](const auto& p) { return double(p.smooth_window_size); }, 1},
        {"prob_threshold=0.9", [](const auto& p) { return p.prob_threshold; }, 0.9},
        {"threshold=0.9", [](const auto& p) { return p.prob_threshold; }, 0.9},
        {"min_speech_frame=240", [](const auto& p) { return double(p.min_speech_frame); }, 240},
        {"max_speech_frame=200", [](const auto& p) { return double(p.max_speech_frame); }, 200},
        {"min_silence_frame=150", [](const auto& p) { return double(p.min_silence_frame); }, 150},
        {"merge_silence_frame=0", [](const auto& p) { return double(p.merge_silence_frame); }, 0},
        {"extend_speech_frame=50", [](const auto& p) { return double(p.extend_speech_frame); }, 50},
    };
    for (const Case& c : cases) {
        INFO(c.setting);
        ev::VadOverrides overrides;
        overrides.add(c.setting);
        ev::FireRedVad vad(*model, 2, overrides);
        CHECK(c.field(vad.params()) == c.value);
        CHECK_FALSE(same_ranges(vad.detect(pcm), base));
    }
}

TEST_CASE("firered VAD threshold is a short name for prob_threshold") {
    const auto model = et::model_file("firered-vad-v1", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "firered-vad-v1 (vad) model");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);

    ev::VadOverrides short_name;
    short_name.add("threshold=0.9");
    ev::VadOverrides long_name;
    long_name.add("prob_threshold=0.9");
    ev::FireRedVad by_short(*model, 2, short_name);
    ev::FireRedVad by_long(*model, 2, long_name);
    CHECK(by_short.params().prob_threshold == 0.9);
    CHECK(same_ranges(by_short.detect(pcm), by_long.detect(pcm)));

    // Giving both names is ambiguous, so it is an error.
    ev::VadOverrides both;
    both.add("threshold=0.5");
    both.add("prob_threshold=0.5");
    CHECK_THROWS_AS(ev::FireRedVad(*model, 2, both), std::invalid_argument);
}

TEST_CASE("firered VAD rejects a Silero key, an unknown key and a bad value") {
    const auto model = et::model_file("firered-vad-v1", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "firered-vad-v1 (vad) model");
    for (const char* assignment : {"min_silence_ms=10", "no_such_setting=1", "prob_threshold=2",
                                   "min_speech_frame=-1", "max_speech_frame=0"}) {
        INFO(assignment);
        ev::VadOverrides overrides;
        overrides.add(assignment);
        CHECK_THROWS_AS(ev::FireRedVad(*model, 2, overrides), std::invalid_argument);
        CHECK_THROWS_AS(ev::make_vad(*model, 2, overrides), std::invalid_argument);
    }
}
