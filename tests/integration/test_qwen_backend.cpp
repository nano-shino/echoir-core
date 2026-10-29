// Qwen3-ASR through make_backend and QwenBackend. Cases that need the Qwen3-ASR 0.6B model, its
// mmproj, the FireRedVAD model or a fixture skip when the file is missing. The argument and
// missing-file cases need no model, so they always run.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "audio/resampler.h"
#include "audio/wav.h"
#include "backends/asr_backend.h"
#include "doctest.h"
#include "test_support.h"
#include "text/score.h"
#include "vad/vad.h"

#ifdef ECHOIR_HAS_QWEN

namespace {

namespace fs = std::filesystem;
namespace et = echoir::test;

// Engine params for a Qwen backend. The caller keeps the paths alive for the call.
echoir_engine_params qwen_params(const char* model_path, const char* mmproj_path, bool use_gpu) {
    echoir_engine_params params{};
    params.struct_size = sizeof(params);
    params.backend = ECHOIR_BACKEND_QWEN3_ASR;
    params.model_path = model_path;
    params.mmproj_path = mmproj_path;
    params.use_gpu = use_gpu ? 1 : 0;
    params.n_threads = 0;  // default
    return params;
}

// A file in the temp directory, removed when the object goes out of scope.
class TempFile {
public:
    TempFile(const std::string& name, const std::string& bytes)
        : path_(fs::temp_directory_path() / name) {
        std::ofstream out(path_, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            throw std::runtime_error("cannot write temp file " + path_.string());
        }
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    std::string path() const { return path_.string(); }

private:
    fs::path path_;
};

std::vector<float> load_16k_mono(const fs::path& path) {
    const echoir::audio::PcmBuffer wav = echoir::audio::read_wav(path);
    return echoir::audio::to_mono_16k(wav.samples, wav.rate, wav.channels);
}

// Throws when the file cannot be opened. An empty reference would otherwise score an empty
// hypothesis as a perfect match.
std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open reference " + path.string());
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// True when the phrase appears in the text after normalization, so case and punctuation do not
// matter.
bool contains_phrase(const std::string& text, const std::string& phrase) {
    return echoir::text::normalize_for_score(text).find(
               echoir::text::normalize_for_score(phrase)) != std::string::npos;
}

// Sample index of a millisecond offset, clamped to the buffer.
std::size_t sample_at(std::int64_t ms, std::size_t n_samples) {
    const auto samples = static_cast<std::size_t>(ms * 16);  // 16 samples per millisecond
    return std::min(samples, n_samples);
}

// The CPU backend shared by the cases below. Loading the 0.6B model takes seconds, and decodes do
// not depend on each other, so the cases share one instance. Callers check the files first.
echoir::IAsrBackend& shared_cpu_backend() {
    static const std::unique_ptr<echoir::IAsrBackend> backend = [] {
        const std::string model = et::model_file("qwen3-asr-0.6b-q8_0")->string();
        const std::string mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj")->string();
        return echoir::make_backend(qwen_params(model.c_str(), mmproj.c_str(), false));
    }();
    return *backend;
}

}  // namespace

TEST_CASE("make_backend needs an mmproj path for qwen3-asr") {
    // The mmproj check comes before the model is opened, so the model path need not exist.
    const std::string model = "unused-qwen.gguf";
    CHECK_THROWS_AS(echoir::make_backend(qwen_params(model.c_str(), nullptr, false)),
                    std::invalid_argument);
    CHECK_THROWS_AS(echoir::make_backend(qwen_params(model.c_str(), "", false)),
                    std::invalid_argument);
}

TEST_CASE("QwenBackend throws on a missing model file instead of aborting") {
    const std::string model = (et::source_dir() / "models" / "no-such-model.gguf").string();
    const std::string mmproj = (et::source_dir() / "models" / "no-such-mmproj.gguf").string();
    REQUIRE_THROWS_AS(echoir::make_backend(qwen_params(model.c_str(), mmproj.c_str(), false)),
                      std::runtime_error);
}

TEST_CASE("QwenBackend throws on a junk model file instead of aborting") {
    const TempFile junk("echoir_qwen_junk_model.gguf", std::string(4096, '\x5a'));
    const std::string path = junk.path();
    REQUIRE_THROWS_AS(echoir::make_backend(qwen_params(path.c_str(), path.c_str(), false)),
                      std::runtime_error);
}

TEST_CASE("QwenBackend throws on a missing mmproj file") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");

    const std::string model_path = model->string();
    const std::string mmproj = (et::source_dir() / "models" / "no-such-mmproj.gguf").string();
    REQUIRE_THROWS_AS(echoir::make_backend(qwen_params(model_path.c_str(), mmproj.c_str(), false)),
                      std::runtime_error);
}

TEST_CASE("QwenBackend decodes jfk.wav and accepts history") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    echoir::IAsrBackend& backend = shared_cpu_backend();
    CHECK(backend.name() == "qwen3-asr");

    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<std::string> no_history;
    const std::vector<std::string> with_history{"And so, my fellow Americans."};
    for (const std::vector<std::string>* history : {&no_history, &with_history}) {
        const echoir::DecodeResult result =
            backend.decode({std::span<const float>(pcm), *history, "auto"});
        INFO("history " << history->size() << " entries: " << result.text);
        CHECK(contains_phrase(result.text, "ask not what your country"));
    }
}

TEST_CASE("QwenBackend decodes jfk.wav with a context") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<std::string> history{"And so, my fellow Americans."};
    const echoir::DecodeResult result = shared_cpu_backend().decode(
        {std::span<const float>(pcm), history, "auto", "Vocabulary: Kennedy, Americans."});
    INFO("context decode: " << result.text);
    CHECK(contains_phrase(result.text, "ask not what your country"));
}

TEST_CASE("QwenBackend rejects a context over 512 tokens") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");

    // Each repeat is at least one token, so 1000 of them are far over the 512-token limit.
    std::string context;
    for (int i = 0; i < 1000; ++i) {
        context += "Kennedy ";
    }
    const std::vector<float> silence(16000, 0.0f);
    const std::vector<std::string> no_history;
    CHECK_THROWS_AS(
        shared_cpu_backend().decode({std::span<const float>(silence), no_history, "auto", context}),
        std::invalid_argument);
}

TEST_CASE("QwenBackend gives the same text for jfk.wav after another window") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto jfk = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(jfk, "jfk.wav fixture");
    const auto zh = et::fixture("zh_short");
    ECHOIR_REQUIRE_OR_SKIP(zh, "zh_short fixture");

    echoir::IAsrBackend& backend = shared_cpu_backend();
    const std::vector<float> jfk_pcm = load_16k_mono(*jfk);
    const std::vector<float> zh_pcm = load_16k_mono(*zh);
    const std::vector<std::string> no_history;

    // The second jfk decode follows a different window, so it only matches the first one if the
    // KV cache from the Chinese decode was cleared.
    const std::string first =
        backend.decode({std::span<const float>(jfk_pcm), no_history, "auto"}).text;
    backend.decode({std::span<const float>(zh_pcm), no_history, "auto"});
    const std::string second =
        backend.decode({std::span<const float>(jfk_pcm), no_history, "auto"}).text;
    INFO("first: " << first << "\nsecond: " << second);
    CHECK(contains_phrase(first, "ask not what your country"));
    CHECK(contains_phrase(second, "ask not what your country"));
    CHECK(first == second);
}

TEST_CASE("QwenBackend with a forced language returns text without a language header") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<std::string> no_history;
    const std::string text =
        shared_cpu_backend().decode({std::span<const float>(pcm), no_history, "en"}).text;
    INFO("forced en: " << text);
    CHECK(contains_phrase(text, "ask not what your country"));
    CHECK(text.find("<asr_text>") == std::string::npos);
    CHECK(lower(text).find("language") == std::string::npos);

    // Forcing French changes the output, so the prefill is really in effect. The English phrase
    // would appear anyway if the language were ignored.
    const std::string french =
        shared_cpu_backend().decode({std::span<const float>(pcm), no_history, "fr"}).text;
    INFO("forced fr: " << french);
    CHECK(!french.empty());
    CHECK(!contains_phrase(french, "ask not what your country"));
    CHECK(french.find("<asr_text>") == std::string::npos);
}

TEST_CASE("QwenBackend rejects a language that is not a supported ISO code") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");

    // The language is checked before the silence test, so one second of silence is enough.
    const std::vector<float> silence(16000, 0.0f);
    const std::vector<std::string> no_history;
    for (const char* language : {"xx", "EN", "english", "klingon"}) {
        INFO("language " << language);
        CHECK_THROWS_AS(
            shared_cpu_backend().decode({std::span<const float>(silence), no_history, language}),
            std::invalid_argument);
    }
}

TEST_CASE("QwenBackend rejects a window longer than 30 s") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");

    const std::vector<float> long_window(30 * 16000 + 1, 0.1f);
    const std::vector<std::string> no_history;
    CHECK_THROWS_AS(
        shared_cpu_backend().decode({std::span<const float>(long_window), no_history, "auto"}),
        std::invalid_argument);
}

TEST_CASE("QwenBackend returns empty text for silence") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto silence = et::fixture("silence_30s");
    ECHOIR_REQUIRE_OR_SKIP(silence, "silence_30s fixture");

    const std::vector<float> pcm = load_16k_mono(*silence);
    const std::vector<std::string> no_history;
    const std::span<const float> first_5s(pcm.data(), std::min<std::size_t>(pcm.size(), 5 * 16000));
    CHECK(shared_cpu_backend().decode({first_5s, no_history, "auto"}).text.empty());
    CHECK(shared_cpu_backend().decode({std::span<const float>(), no_history, "auto"}).text.empty());
}

TEST_CASE("QwenBackend transcribes quiet speech") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    // jfk.wav at these gains has an RMS of about -51 dBFS and -71 dBFS, and it must still decode.
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<std::string> no_history;
    for (const float gain : {0.02f, 0.002f}) {
        std::vector<float> quiet(pcm.size());
        std::transform(pcm.begin(), pcm.end(), quiet.begin(), [gain](float x) { return x * gain; });
        const std::string text =
            shared_cpu_backend().decode({std::span<const float>(quiet), no_history, "auto"}).text;
        INFO("gain " << gain << ": " << text);
        CHECK(contains_phrase(text, "ask not what your country"));
    }
}

TEST_CASE("QwenBackend returns empty text for a window shorter than 100 ms") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");

    // A single sample aborts inside libmtmd, so it must never reach the encoder. The windows are
    // loud, so only their length makes them skip the decode.
    const std::vector<std::string> no_history;
    const std::vector<float> one_sample(1, 0.5f);
    const std::vector<float> just_short(1599, 0.5f);
    CHECK(shared_cpu_backend()
              .decode({std::span<const float>(one_sample), no_history, "auto"})
              .text.empty());
    CHECK(shared_cpu_backend()
              .decode({std::span<const float>(just_short), no_history, "auto"})
              .text.empty());
}

TEST_CASE("QwenBackend rejects NaN and infinite samples") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");

    // One bad sample in an otherwise loud second. Without the check the model writes text up to
    // the generation cap.
    const std::vector<std::string> no_history;
    for (const float bad :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        std::vector<float> window(16000, 0.1f);
        window[8000] = bad;
        INFO("bad sample " << bad);
        CHECK_THROWS_AS(
            shared_cpu_backend().decode({std::span<const float>(window), no_history, "auto"}),
            std::invalid_argument);
    }
}

TEST_CASE("QwenBackend decodes zh_short with a character error rate of at most 0.10") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("zh_short");
    ECHOIR_REQUIRE_OR_SKIP(wav, "zh_short fixture");

    fs::path ref_path = *wav;
    ref_path.replace_extension(".ref.txt");
    const std::string ref = read_text(ref_path);
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<std::string> no_history;
    const std::string hyp =
        shared_cpu_backend().decode({std::span<const float>(pcm), no_history, "auto"}).text;

    const echoir::text::ErrorRate score = echoir::text::error_rate(hyp, ref);
    INFO("hyp: " << hyp << "\nref: " << ref << "\nCER " << score.rate);
    CHECK(!hyp.empty());
    CHECK(score.rate <= 0.10);
}

TEST_CASE("QwenBackend transcribes sentences_en by VAD ranges with a WER of at most 0.10") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto vad_model = et::model_file("firered-vad-v1", "vad");
    ECHOIR_REQUIRE_OR_SKIP(vad_model, "firered-vad-v1 (vad) model");
    const auto wav = et::fixture("sentences_en");
    ECHOIR_REQUIRE_OR_SKIP(wav, "sentences_en fixture");

    fs::path ref_path = *wav;
    ref_path.replace_extension(".ref.txt");
    const std::string ref = read_text(ref_path);
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::unique_ptr<echoir::vad::IVad> vad = echoir::vad::make_vad(*vad_model);
    const std::vector<echoir::vad::Range> ranges = vad->detect(pcm);
    REQUIRE(!ranges.empty());

    // Each range is decoded alone, with the last two texts as history, as the streaming layer
    // passes finals.
    echoir::IAsrBackend& backend = shared_cpu_backend();
    std::vector<std::string> history;
    std::string hyp;
    for (const echoir::vad::Range& range : ranges) {
        const std::size_t begin = sample_at(range.start_ms, pcm.size());
        const std::size_t end = sample_at(range.end_ms, pcm.size());
        if (end <= begin) {
            continue;
        }
        const std::span<const float> slice(pcm.data() + begin, end - begin);
        const std::string text = backend.decode({slice, history, "auto"}).text;
        if (!hyp.empty()) {
            hyp += ' ';
        }
        hyp += text;
        history.push_back(text);
        if (history.size() > 2) {
            history.erase(history.begin());
        }
    }

    const echoir::text::ErrorRate score = echoir::text::error_rate(hyp, ref);
    INFO("hyp: " << hyp << "\nCER/WER " << score.rate << " (" << score.subs << " subs, "
                 << score.dels << " dels, " << score.ins << " ins over " << score.ref_len << ")");
    CHECK(!hyp.empty());
    CHECK(score.rate <= 0.10);
}

TEST_CASE("QwenBackend with use_gpu decodes jfk.wav") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    const std::string model_path = model->string();
    const std::string mmproj_path = mmproj->string();
    const auto backend =
        echoir::make_backend(qwen_params(model_path.c_str(), mmproj_path.c_str(), true));
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<std::string> no_history;
    const std::string text =
        backend->decode({std::span<const float>(pcm), no_history, "auto"}).text;
    INFO("gpu: " << text);
    CHECK(contains_phrase(text, "ask not what your country"));
}

#else

TEST_CASE("make_backend reports qwen3-asr as not built without ECHOIR_WITH_QWEN") {
    echoir_engine_params params{};
    params.struct_size = sizeof(params);
    params.backend = ECHOIR_BACKEND_QWEN3_ASR;
    const std::string model = "unused-qwen.gguf";
    params.model_path = model.c_str();
    CHECK_THROWS_AS(echoir::make_backend(params), std::runtime_error);
}

#endif  // ECHOIR_HAS_QWEN
