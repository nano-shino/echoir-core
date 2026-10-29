// The decoder prefix: continuing a decode from an earlier one on Qwen3-ASR, and refusing a prefix
// on Whisper. Cases that need a model or a fixture skip when the file is missing. The argument
// cases run on silent input, so they need the Qwen model but no audio decode.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/resampler.h"
#include "audio/wav.h"
#include "backends/asr_backend.h"
#include "doctest.h"
#include "test_support.h"
#include "text/score.h"

namespace {

namespace fs = std::filesystem;
namespace et = echoir::test;

// The media marker of libmtmd. A prefix must not contain it.
constexpr const char* kMediaMarker = "<__media__>";

echoir_engine_params engine_params(echoir_backend backend, const char* model_path,
                                   const char* mmproj_path) {
    echoir_engine_params params{};
    params.struct_size = sizeof(params);
    params.backend = backend;
    params.model_path = model_path;
    params.mmproj_path = mmproj_path;
    params.use_gpu = 0;
    params.n_threads = 0;  // default
    return params;
}

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

// The text without its last n words, normalized so case and punctuation do not matter.
std::string without_last_words(const std::string& text, std::size_t n) {
    std::istringstream in(echoir::text::normalize_for_score(text));
    std::vector<std::string> words;
    for (std::string word; in >> word;) {
        words.push_back(word);
    }
    std::string kept;
    for (std::size_t i = 0; i + n < words.size(); ++i) {
        if (!kept.empty()) {
            kept += ' ';
        }
        kept += words[i];
    }
    return kept;
}

// The CPU Qwen backend shared by the cases below. Callers check the files first.
echoir::IAsrBackend& shared_qwen_backend() {
    static const std::unique_ptr<echoir::IAsrBackend> backend = [] {
        const std::string model = et::model_file("qwen3-asr-0.6b-q8_0")->string();
        const std::string mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj")->string();
        const echoir_engine_params params =
            engine_params(ECHOIR_BACKEND_QWEN3_ASR, model.c_str(), mmproj.c_str());
        return echoir::make_backend(params);
    }();
    return *backend;
}

}  // namespace

TEST_CASE("QwenBackend supports a prefix") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");

    CHECK(shared_qwen_backend().supports_prefix());
}

TEST_CASE("QwenBackend continues an earlier decode from its prefix") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    fs::path ref_path = *wav;
    ref_path.replace_extension(".ref.txt");
    const std::string ref = read_text(ref_path);

    echoir::IAsrBackend& backend = shared_qwen_backend();
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::span<const float> first_60(pcm.data(), pcm.size() * 60 / 100);
    const std::vector<std::string> no_history;

    for (const char* language : {"auto", "en"}) {
        INFO("language " << language);
        const echoir::DecodeResult first = backend.decode({first_60, no_history, language});
        const echoir::DecodeResult second = backend.decode(
            {std::span<const float>(pcm), no_history, language, {}, first.continuation, 5});
        INFO("first: " << first.text << "\nsecond: " << second.text);

        // The rollback removes the last few words of the first text, so the second text keeps
        // everything before them.
        CHECK(!first.continuation.empty());
        CHECK(echoir::text::normalize_for_score(second.text)
                  .rfind(without_last_words(first.text, 5), 0) == 0);
        CHECK(second.text.find("<asr_text>") == std::string::npos);
        CHECK(lower(second.text).find("language") == std::string::npos);

        // The offline jfk case in tests/e2e/cases.json allows a rate of 0.00.
        const echoir::text::ErrorRate score = echoir::text::error_rate(second.text, ref);
        INFO("WER " << score.rate << " for: " << second.text);
        CHECK(score.rate <= 0.0);
    }
}

TEST_CASE("QwenBackend with a rollback past the whole prefix decodes as without one") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    echoir::IAsrBackend& backend = shared_qwen_backend();
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::span<const float> first_60(pcm.data(), pcm.size() * 60 / 100);
    const std::vector<std::string> no_history;

    for (const char* language : {"auto", "en"}) {
        INFO("language " << language);
        const echoir::DecodeResult plain =
            backend.decode({std::span<const float>(pcm), no_history, language});
        const echoir::DecodeResult first = backend.decode({first_60, no_history, language});
        // 1000 tokens is more than the whole prefix, so nothing of it survives the rollback.
        const echoir::DecodeResult rolled = backend.decode(
            {std::span<const float>(pcm), no_history, language, {}, first.continuation, 1000});
        INFO("plain: " << plain.text << "\nrolled: " << rolled.text);
        CHECK(rolled.text == plain.text);
        CHECK(rolled.continuation == plain.continuation);
    }
}

// The transcript tag is one token in the prompt, so a rollback into it removes it whole. A broken
// tag left at the end of the prompt would derail the decode.
TEST_CASE("QwenBackend rolls back over the transcript tag as one token") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    echoir::IAsrBackend& backend = shared_qwen_backend();
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<std::string> no_history;
    const echoir::DecodeResult plain =
        backend.decode({std::span<const float>(pcm), no_history, "auto"});
    // "And" is one token and the tag another, so a rollback of 2 ends the prefix right before the
    // tag, and the model writes the tag again.
    const std::string prefix = "language English<asr_text>And";
    const echoir::DecodeResult rolled =
        backend.decode({std::span<const float>(pcm), no_history, "auto", {}, prefix, 2});
    INFO("plain: " << plain.continuation << "\nrolled: " << rolled.continuation);
    CHECK(rolled.continuation.rfind("language English<asr_text>", 0) == 0);
    CHECK(rolled.continuation.find("<asr_t", 1) == rolled.continuation.find("<asr_text>"));
    CHECK(rolled.text == plain.text);
}

TEST_CASE("QwenBackend rejects a prefix with the media marker or a negative rollback") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");

    // Argument checks run before the silence check, so silent input is enough.
    const std::vector<float> silence(16000, 0.0f);
    const std::vector<std::string> no_history;
    const std::string marked = std::string("Hello ") + kMediaMarker;
    CHECK_THROWS_AS(shared_qwen_backend().decode(
                        {std::span<const float>(silence), no_history, "auto", {}, marked, 0}),
                    std::invalid_argument);
    CHECK_THROWS_AS(shared_qwen_backend().decode(
                        {std::span<const float>(silence), no_history, "auto", {}, "Hello", -1}),
                    std::invalid_argument);
}

TEST_CASE("QwenBackend rejects a prefix that does not fit the context") {
    const auto model = et::model_file("qwen3-asr-0.6b-q8_0");
    ECHOIR_REQUIRE_OR_SKIP(model, "qwen3-asr-0.6b-q8_0 model (models/Qwen3-ASR-0.6B-Q8_0.gguf)");
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    // About 3000 tokens, which with the 11 s window and its generation cap is over the 2048-token
    // context. The size check runs after the silence check, so the window must not be silent.
    std::string prefix;
    for (int i = 0; i < 3000; ++i) {
        prefix += "word ";
    }
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<std::string> no_history;
    CHECK_THROWS_AS(shared_qwen_backend().decode(
                        {std::span<const float>(pcm), no_history, "auto", {}, prefix, 0}),
                    std::invalid_argument);
}

TEST_CASE("WhisperBackend has no prefix support and rejects a prefix") {
    const auto model = et::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    const std::string path = model->string();
    const echoir_engine_params params =
        engine_params(ECHOIR_BACKEND_WHISPER, path.c_str(), nullptr);
    const std::unique_ptr<echoir::IAsrBackend> backend = echoir::make_backend(params);
    CHECK(!backend->supports_prefix());

    // Without a prefix the text is the continuation, since nothing is stripped.
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<std::string> no_history;
    const echoir::DecodeResult result =
        backend->decode({std::span<const float>(pcm), no_history, "auto"});
    CHECK(result.continuation == result.text);
    CHECK(!result.text.empty());

    CHECK_THROWS_AS(
        backend->decode(
            {std::span<const float>(pcm), no_history, "auto", {}, "And so my fellow Americans", 0}),
        std::invalid_argument);
}
