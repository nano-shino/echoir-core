// WhisperBackend and make_backend. Cases that need whisper-tiny-en skip when the model is missing.
// The bad-file, bad-header and bad-param cases need no model, so they always run.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>  // mkfifo
#endif

#include "audio/resampler.h"
#include "audio/wav.h"
#include "backends/asr_backend.h"
#include "doctest.h"
#include "errors.h"
#include "test_support.h"

namespace {

namespace fs = std::filesystem;

// Engine params for a CPU whisper backend. The caller keeps model_path alive for the call.
echoir_engine_params whisper_params(const char* model_path) {
    echoir_engine_params params{};
    params.struct_size = sizeof(params);
    params.backend = ECHOIR_BACKEND_WHISPER;
    params.model_path = model_path;
    params.use_gpu = 0;
    params.n_threads = 0;  // default
    return params;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Positions of the header fields after the magic, in whisper's file order.
enum HeaderField : std::size_t {
    kNVocab,
    kNAudioCtx,
    kNAudioState,
    kNAudioHead,
    kNAudioLayer,
    kNTextCtx,
    kNTextState,
    kNTextHead,
    kNTextLayer,
    kNMels,
    kFtype,
    kHeaderFieldCount,
};

using HeaderFields = std::array<std::int32_t, kHeaderFieldCount>;

// tiny.en's values. A case changes one or two fields, so the check it hits stays clear.
constexpr HeaderFields kTinyEnHeader = {51864, 1500, 384, 6, 4, 448, 384, 6, 4, 80, 1};

// tiny.en's header with one field changed.
HeaderFields with_field(HeaderField field, std::int32_t value) {
    HeaderFields fields = kTinyEnHeader;
    fields[field] = value;
    return fields;
}

// The 48 bytes whisper reads first: the ggml magic, then the fields as little-endian int32.
std::string header_bytes(const HeaderFields& fields) {
    std::string bytes = "lmgg";  // GGML_FILE_MAGIC, little endian
    for (const std::int32_t value : fields) {
        const auto bits = static_cast<std::uint32_t>(value);
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<char>((bits >> shift) & 0xffu));
        }
    }
    return bytes;
}

// A file in the temp dir with the given bytes, removed when the object goes out of scope.
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

// Writes a header-only file and checks that make_backend rejects it with an error that names
// `field`. The header check runs before whisper reads the file, so no model is needed.
void expect_header_rejected(const HeaderFields& fields, const std::string& field) {
    INFO("header field " << field);
    const TempFile file("echoir_bad_header_model.bin", header_bytes(fields));
    const std::string path = file.path();
    bool threw = false;
    try {
        echoir::make_backend(whisper_params(path.c_str()));
    } catch (const std::runtime_error& error) {
        threw = true;
        INFO("error: " << error.what());
        CHECK(std::string(error.what()).find(field) != std::string::npos);
    }
    CHECK(threw);
}

}  // namespace

TEST_CASE("make_backend rejects a null model path") {
    echoir_engine_params params{};
    params.struct_size = sizeof(params);
    params.backend = ECHOIR_BACKEND_WHISPER;
    params.model_path = nullptr;
    CHECK_THROWS_AS(echoir::make_backend(params), std::invalid_argument);
}

TEST_CASE("WhisperBackend throws on a missing model file instead of aborting") {
    const std::string path = (echoir::test::source_dir() / "models" / "no-such-model.bin").string();
    REQUIRE_THROWS_AS(echoir::make_backend(whisper_params(path.c_str())), echoir::ModelLoadError);
}

#ifndef _WIN32
// Opening a named pipe with no writer blocks, so the path must be rejected before it is opened.
TEST_CASE("WhisperBackend rejects a named pipe instead of blocking") {
    const fs::path pipe = fs::temp_directory_path() / "echoir_test_whisper_fifo.bin";
    std::error_code ec;
    fs::remove(pipe, ec);
    REQUIRE(mkfifo(pipe.c_str(), 0600) == 0);
    const std::string path = pipe.string();
    CHECK_THROWS_AS(echoir::make_backend(whisper_params(path.c_str())), echoir::ModelLoadError);
    fs::remove(pipe, ec);
}
#endif

TEST_CASE("WhisperBackend throws on junk and foreign files instead of aborting") {
    // Junk with no magic.
    const TempFile junk("echoir_junk_model.bin", std::string(4096, '\x5a'));
    const std::string junk_path = junk.path();
    REQUIRE_THROWS_AS(echoir::make_backend(whisper_params(junk_path.c_str())), std::runtime_error);

    // The ggml magic ("lmgg" on disk, little endian) and an all-zero header. The magic passes,
    // so the field checks have to reject it, because whisper's loader does not check the fields.
    const std::string magic_only = std::string("lmgg") + std::string(44, '\0');
    const TempFile zeroed("echoir_zeroed_model.bin", magic_only);
    const std::string zeroed_path = zeroed.path();
    REQUIRE_THROWS_AS(echoir::make_backend(whisper_params(zeroed_path.c_str())),
                      std::runtime_error);

    // Empty file.
    const TempFile empty("echoir_empty_model.bin", "");
    const std::string empty_path = empty.path();
    REQUIRE_THROWS_AS(echoir::make_backend(whisper_params(empty_path.c_str())), std::runtime_error);
}

TEST_CASE("WhisperBackend rejects header sizes that crash or exhaust whisper") {
    // A vocabulary above 51866 crashes whisper's loader, and a vocabulary below the published
    // range is not a Whisper model. Huge layer counts exhaust memory.
    expect_header_rejected(with_field(kNVocab, 51900), "n_vocab");
    expect_header_rejected(with_field(kNVocab, 60000), "n_vocab");
    expect_header_rejected(with_field(kNVocab, 51867), "n_vocab");
    expect_header_rejected(with_field(kNVocab, 51863), "n_vocab");
    expect_header_rejected(with_field(kNAudioLayer, 0x7fffffff), "n_audio_layer");
    expect_header_rejected(with_field(kNTextLayer, 0x7fffffff), "n_text_layer");

    // One head per state, so the state size is the only bad field. Huge states abort in ggml.
    HeaderFields huge_state = with_field(kNAudioState, 0x7fffffff);
    huge_state[kNTextState] = 0x7fffffff;
    huge_state[kNAudioHead] = 1;
    huge_state[kNTextHead] = 1;
    expect_header_rejected(huge_state, "n_audio_state");
}

TEST_CASE("WhisperBackend rejects ftype values that ggml has no tensor type for") {
    // ftype 4 is a legacy LLaMA format that this ggml does not map, and -1 is UNKNOWN. Either one
    // aborts inside ggml if it reaches the loader.
    expect_header_rejected(with_field(kFtype, 4), "ftype");
    expect_header_rejected(with_field(kFtype, -1), "ftype");
}

TEST_CASE("WhisperBackend decodes jfk.wav with whisper-tiny-en") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");
    const auto wav = echoir::test::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    const echoir::audio::PcmBuffer buffer = echoir::audio::read_wav(*wav);
    const std::vector<float> mono =
        echoir::audio::to_mono_16k(buffer.samples, buffer.rate, buffer.channels);

    const std::string model_path = model->string();
    const auto backend = echoir::make_backend(whisper_params(model_path.c_str()));
    CHECK(backend->name() == "whisper");

    // "auto" on an English-only model maps to "en", and "en" passes through. An empty history
    // gives no prompt, and a previous final gives the prompt path. Every combination must decode.
    const std::vector<std::string> no_history;
    const std::vector<std::string> with_history{"And so, my fellow Americans."};
    for (const std::vector<std::string>* history : {&no_history, &with_history}) {
        for (const char* language : {"auto", "en"}) {
            const std::string label = language;
            const echoir::DecodeResult result =
                backend->decode({std::span<const float>(mono), *history, label});
            INFO("language " << label << ", " << history->size()
                             << " history entries: " << result.text);
            CHECK(lower(result.text).find("ask not what your country") != std::string::npos);
        }
    }
}

TEST_CASE("WhisperBackend decodes jfk.wav with a context, and trims history to fit it") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");
    const auto wav = echoir::test::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    const echoir::audio::PcmBuffer buffer = echoir::audio::read_wav(*wav);
    const std::vector<float> mono =
        echoir::audio::to_mono_16k(buffer.samples, buffer.rate, buffer.channels);
    const std::string model_path = model->string();
    const auto backend = echoir::make_backend(whisper_params(model_path.c_str()));

    const std::string context = "Vocabulary: Kennedy, Americans.";
    const std::vector<std::string> no_history;
    const echoir::DecodeResult result =
        backend->decode({std::span<const float>(mono), no_history, "auto", context});
    INFO("context decode: " << result.text);
    CHECK(lower(result.text).find("ask not what your country") != std::string::npos);

    // Forty finals are far over the prompt limit. The oldest go first, so the decode still runs.
    std::vector<std::string> long_history;
    for (int i = 0; i < 40; ++i) {
        long_history.push_back("And so, my fellow Americans, ask not what your country can do.");
    }
    CHECK_NOTHROW(backend->decode({std::span<const float>(mono), long_history, "auto", context}));
}

TEST_CASE("WhisperBackend rejects a context over the prompt limit") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");

    const std::string model_path = model->string();
    const auto backend = echoir::make_backend(whisper_params(model_path.c_str()));
    // Each word is at least one token, so 300 of them are over the 223-token prompt limit.
    std::string context;
    for (int i = 0; i < 300; ++i) {
        context += "Kennedy ";
    }
    const std::vector<float> silence(16000, 0.0f);
    const std::vector<std::string> no_history;
    CHECK_THROWS_AS(backend->decode({std::span<const float>(silence), no_history, "auto", context}),
                    std::invalid_argument);
}

TEST_CASE("WhisperBackend rejects a language the model does not have") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");

    const std::string model_path = model->string();
    const auto backend = echoir::make_backend(whisper_params(model_path.c_str()));
    // One second of silence. The language is checked before whisper runs, so nothing is decoded.
    const std::vector<float> silence(16000, 0.0f);
    const std::vector<std::string> history;
    // tiny.en knows only English, so a code for another language and an unknown code both fail.
    for (const char* language : {"fr", "xx", "EN"}) {
        INFO("language " << language);
        CHECK_THROWS_AS(backend->decode({std::span<const float>(silence), history, language}),
                        std::invalid_argument);
    }
}

TEST_CASE("WhisperBackend returns empty text for empty pcm") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");

    const std::string model_path = model->string();
    const auto backend = echoir::make_backend(whisper_params(model_path.c_str()));
    const std::vector<std::string> history;
    const echoir::DecodeResult result =
        backend->decode({std::span<const float>(), history, "auto"});
    CHECK(result.text.empty());
}

TEST_CASE("WhisperBackend rejects a truncated real model") {
    const auto model = echoir::test::model_file("whisper-tiny-en");
    ECHOIR_REQUIRE_OR_SKIP(model, "whisper-tiny-en model (models/ggml-tiny.en.bin)");

    // The header is intact, so only the loader can notice the missing weights. Loading must throw,
    // not return a model without them. At 300 KB the loader logs a WARN and still returns a
    // context (the LoadWatch path). At 4 MiB it fails outright (the null context path).
    for (const std::size_t size : {std::size_t{300'000}, std::size_t{4} << 20}) {
        std::ifstream in(*model, std::ios::binary);
        std::string bytes(size, '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(in.gcount()));
        const TempFile truncated("echoir_truncated_model.bin", bytes);
        const std::string path = truncated.path();
        INFO("truncated to " << size << " bytes");
        REQUIRE_THROWS_AS(echoir::make_backend(whisper_params(path.c_str())), std::runtime_error);
    }
}
