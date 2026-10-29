#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>  // mkfifo
#endif

#include "audio/resampler.h"
#include "audio/wav.h"
#include "doctest.h"
#include "errors.h"
#include "test_support.h"
#include "vad/silero_vad.h"
#include "vad/vad.h"

namespace ea = echoir::audio;
namespace ev = echoir::vad;
namespace et = echoir::test;

namespace {

// Reads a fixture WAV as the core's format: 16 kHz mono f32.
std::vector<float> load_16k_mono(const std::filesystem::path& path) {
    const ea::PcmBuffer wav = ea::read_wav(path);
    return ea::to_mono_16k(wav.samples, wav.rate, wav.channels);
}

std::int64_t total_ms(const std::vector<ev::Range>& ranges) {
    std::int64_t sum = 0;
    for (const ev::Range& r : ranges) sum += r.end_ms - r.start_ms;
    return sum;
}

// A file in the temp directory, removed when the test ends, also when it fails.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& name)
        : path(std::filesystem::temp_directory_path() / name) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Little-endian, as the model file stores its fields.
void put_u32(std::vector<char>& bytes, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xffu));
    }
}

// Overwrites the int32 at `offset`.
void put_i32_at(std::vector<char>& bytes, std::size_t offset, std::int32_t value) {
    const auto u = static_cast<std::uint32_t>(value);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<char>((u >> (8 * i)) & 0xffu);
    }
}

// The header fields of a VAD model, in the order whisper.cpp reads them. The defaults are the
// values of the v6.2.0 model. A test changes one field at a time.
struct Header {
    std::int32_t name_len = 10;  // the name that follows is "silero-16k"
    std::int32_t n_window = 512;
    std::int32_t n_layers = 4;
    // in, out and kernel size of each encoder layer
    std::array<std::int32_t, 12> encoder = {129, 128, 3, 128, 64, 3, 64, 64, 3, 64, 128, 3};
    std::int32_t lstm_input = 128;
    std::int32_t lstm_hidden = 128;
    std::int32_t final_conv_in = 128;
    std::int32_t final_conv_out = 1;
};

// The bytes of a model file that stops after the header, with no tensor data.
std::vector<char> header_only_file(const Header& h) {
    std::vector<char> bytes;
    put_u32(bytes, 0x67676d6cu);  // ggml magic
    put_u32(bytes, static_cast<std::uint32_t>(h.name_len));
    const std::string name = "silero-16k";
    bytes.insert(bytes.end(), name.begin(), name.end());
    put_u32(bytes, 6);  // version 6.2.0
    put_u32(bytes, 2);
    put_u32(bytes, 0);
    put_u32(bytes, static_cast<std::uint32_t>(h.n_window));
    put_u32(bytes, 64);  // n_context
    put_u32(bytes, static_cast<std::uint32_t>(h.n_layers));
    for (std::int32_t v : h.encoder) put_u32(bytes, static_cast<std::uint32_t>(v));
    put_u32(bytes, static_cast<std::uint32_t>(h.lstm_input));
    put_u32(bytes, static_cast<std::uint32_t>(h.lstm_hidden));
    put_u32(bytes, static_cast<std::uint32_t>(h.final_conv_in));
    put_u32(bytes, static_cast<std::uint32_t>(h.final_conv_out));
    return bytes;
}

}  // namespace

TEST_CASE("silero VAD finds the speech in jfk.wav") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    const std::vector<float> pcm = load_16k_mono(*wav);
    ev::SileroVad vad(*model);
    const std::vector<ev::Range> ranges = vad.detect(pcm);

    REQUIRE(!ranges.empty());
    for (const ev::Range& r : ranges) {
        CHECK(r.start_ms >= 0);
        CHECK(r.end_ms <= 11000);
        CHECK(r.end_ms > r.start_ms);
    }
    CHECK(total_ms(ranges) > 7000);

    // Each call frees its own results, so a second call gives the same ranges.
    const std::vector<ev::Range> again = vad.detect(pcm);
    REQUIRE(again.size() == ranges.size());
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        CHECK(again[i].start_ms == ranges[i].start_ms);
        CHECK(again[i].end_ms == ranges[i].end_ms);
    }
}

TEST_CASE("silero VAD finds no speech in silence_30s") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    const auto wav = et::fixture("silence_30s");
    ECHOIR_REQUIRE_OR_SKIP(wav, "silence_30s fixture");

    ev::SileroVad vad(*model);
    CHECK(vad.detect(load_16k_mono(*wav)).empty());
}

TEST_CASE("silero VAD returns no ranges for an empty span") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");

    ev::SileroVad vad(*model);
    CHECK(vad.detect(std::span<const float>{}).empty());
}

TEST_CASE("a missing VAD model throws instead of aborting") {
    const std::filesystem::path missing = "/nonexistent/echoir/ggml-silero-missing.bin";
    // Parentheses, not braces: a comma inside braces would split the doctest macro arguments.
    CHECK_THROWS_AS(ev::SileroVad{missing}, echoir::ModelLoadError);
    CHECK_THROWS_AS(ev::SileroVad(missing, 2), echoir::ModelLoadError);
}

#ifndef _WIN32
// Opening a named pipe with no writer blocks, so the path must be rejected before it is opened.
TEST_CASE("a named pipe is rejected as a VAD model instead of blocking") {
    TempFile pipe("echoir_test_vad_fifo.bin");
    std::error_code ec;
    std::filesystem::remove(pipe.path, ec);
    REQUIRE(mkfifo(pipe.path.c_str(), 0600) == 0);
    CHECK_THROWS_AS(ev::SileroVad{pipe.path}, echoir::ModelLoadError);
}
#endif

TEST_CASE("a file that is not a VAD model throws") {
    TempFile junk("echoir_test_not_a_vad_model.bin");
    {
        std::ofstream out(junk.path, std::ios::binary);
        REQUIRE(out.is_open());
        out << "this is not a ggml model file";
    }
    REQUIRE(std::filesystem::is_regular_file(junk.path));
    CHECK_THROWS_AS(ev::SileroVad{junk.path}, std::runtime_error);
}

TEST_CASE("bad VAD headers throw instead of aborting") {
    // These need no model: the header check runs before whisper.cpp reads the file.
    struct Case {
        const char* what;
        Header header;
    };
    Header zero_name;
    zero_name.name_len = 0;
    Header negative_name;
    negative_name.name_len = -1;
    Header zero_window;
    zero_window.n_window = 0;
    Header big_window;
    big_window.n_window = 1024;
    Header five_layers;
    five_layers.n_layers = 5;
    Header zero_encoder;
    zero_encoder.encoder = {};
    Header negative_encoder_in;
    negative_encoder_in.encoder[0] = -1;
    Header lstm_mismatch;
    lstm_mismatch.lstm_input = 64;
    Header zero_hidden;
    zero_hidden.lstm_hidden = 0;
    Header huge_hidden;
    huge_hidden.lstm_hidden = 2147483647;
    Header zero_final_in;
    zero_final_in.final_conv_in = 0;

    const std::vector<Case> cases = {
        {"name length 0", zero_name},
        {"name length -1", negative_name},
        {"n_window 0", zero_window},
        {"n_window 1024", big_window},
        {"5 encoder layers", five_layers},
        {"all encoder sizes 0", zero_encoder},
        {"encoder in size -1", negative_encoder_in},
        {"lstm_input differs from the last encoder layer", lstm_mismatch},
        {"lstm_hidden 0", zero_hidden},
        {"lstm_hidden 2147483647", huge_hidden},
        {"final_conv_in 0", zero_final_in},
    };
    for (const Case& c : cases) {
        INFO(c.what);
        TempFile file("echoir_test_bad_vad_header.bin");
        write_bytes(file.path, header_only_file(c.header));
        CHECK_THROWS_AS(ev::SileroVad{file.path}, std::runtime_error);
    }
}

TEST_CASE("a VAD file that ends inside its header throws") {
    TempFile file("echoir_test_short_vad_header.bin");
    write_bytes(file.path, header_only_file(Header{}));
    std::filesystem::resize_file(file.path, 60);
    CHECK_THROWS_AS(ev::SileroVad{file.path}, std::runtime_error);
}

TEST_CASE("a VAD file with a valid header and no tensor data throws") {
    // whisper.cpp would load this with no weights and only a warning.
    TempFile file("echoir_test_vad_header_only.bin");
    write_bytes(file.path, header_only_file(Header{}));
    CHECK_THROWS_AS(ev::SileroVad{file.path}, std::runtime_error);
}

TEST_CASE("a copy of the VAD model loads, and bad copies throw") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    const std::vector<char> original = read_bytes(*model);

    // The unedited copy must load, so the failures below come from the edits.
    TempFile valid_copy("echoir_test_vad_copy.bin");
    write_bytes(valid_copy.path, original);
    CHECK_NOTHROW(ev::SileroVad{valid_copy.path});

    // Byte offsets in the v6.2.0 header, whose model name is 10 bytes: name length at 4,
    // n_window at 30, and the first tensor's name length at 110 (after the 106-byte header and
    // its dimension count).
    struct Patch {
        const char* what;
        std::size_t offset;
        std::int32_t value;
    };
    const std::vector<Patch> patches = {
        {"n_window 0", 30, 0},
        {"n_window 1024", 30, 1024},
        {"name length -1", 4, -1},
        {"first tensor name length -1", 110, -1},
    };
    for (const Patch& p : patches) {
        INFO(p.what);
        std::vector<char> bytes = original;
        put_i32_at(bytes, p.offset, p.value);
        TempFile file("echoir_test_patched_vad.bin");
        write_bytes(file.path, bytes);
        CHECK_THROWS_AS(ev::SileroVad{file.path}, std::runtime_error);
    }
}

TEST_CASE("a truncated copy of the VAD model throws") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    const std::vector<char> original = read_bytes(*model);

    // Cutting 4 bytes makes the last tensor's read short, which whisper.cpp ignores, so the file
    // would load. Cutting 40000 bytes removes whole tensors.
    for (std::size_t cut : {std::size_t{4}, std::size_t{40000}}) {
        INFO("cut " << cut << " bytes");
        TempFile file("echoir_test_truncated_vad.bin");
        write_bytes(
            file.path,
            std::vector<char>(original.begin(), original.end() - static_cast<std::ptrdiff_t>(cut)));
        CHECK_THROWS_AS(ev::SileroVad{file.path}, std::runtime_error);
    }
}

TEST_CASE("silero VAD rejects zero threads and is move-only") {
    const std::filesystem::path missing = "/nonexistent/echoir/ggml-silero-missing.bin";
    CHECK_THROWS_AS(ev::SileroVad(missing, 0), std::invalid_argument);
    CHECK_FALSE(std::is_copy_constructible_v<ev::SileroVad>);
    CHECK(std::is_nothrow_move_constructible_v<ev::SileroVad>);
}

namespace {

// What() of the ModelLoadError that load throws, or "" when it throws nothing or another type.
template <typename Load>
std::string model_load_error(Load&& load) {
    try {
        load();
    } catch (const echoir::ModelLoadError& e) {
        return e.what();
    } catch (...) {
        // Another exception type makes the caller's check fail.
    }
    return {};
}

}  // namespace

TEST_CASE("make_vad returns the Silero detector for a ggml VAD model") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");

    const std::unique_ptr<ev::IVad> vad = ev::make_vad(*model);
    REQUIRE(vad != nullptr);
    CHECK(vad->name() == "silero");
    CHECK(vad->detect(std::span<const float>{}).empty());

    // The factory gives the same detector as constructing SileroVad directly.
    const std::vector<float> pcm = load_16k_mono(*wav);
    const std::vector<ev::Range> ranges = vad->detect(pcm);
    ev::SileroVad direct(*model);
    const std::vector<ev::Range> expected = direct.detect(pcm);
    REQUIRE(ranges.size() == expected.size());
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        CHECK(ranges[i].start_ms == expected[i].start_ms);
        CHECK(ranges[i].end_ms == expected[i].end_ms);
    }
    CHECK(total_ms(ranges) > 7000);
}

TEST_CASE("make_vad throws ModelLoadError for a missing file") {
    const std::filesystem::path missing = "/nonexistent/echoir/ggml-silero-missing.bin";
    CHECK_THROWS_AS(ev::make_vad(missing), echoir::ModelLoadError);
}

#ifndef _WIN32
TEST_CASE("make_vad rejects a named pipe instead of blocking") {
    TempFile pipe("echoir_test_make_vad_fifo.bin");
    std::error_code ec;
    std::filesystem::remove(pipe.path, ec);
    REQUIRE(mkfifo(pipe.path.c_str(), 0600) == 0);
    CHECK_THROWS_AS(ev::make_vad(pipe.path), echoir::ModelLoadError);
}
#endif

TEST_CASE("make_vad rejects a file that is neither GGUF nor ggml") {
    // Text, an empty file, and a file shorter than the magic.
    const std::vector<std::string> contents = {"this is not a ggml model file", "", "ggm"};
    for (const std::string& text : contents) {
        INFO("contents: \"" << text << "\"");
        TempFile file("echoir_test_make_vad_unknown.bin");
        write_bytes(file.path, std::vector<char>(text.begin(), text.end()));
        CHECK_THROWS_AS(ev::make_vad(file.path), echoir::ModelLoadError);
    }
}

TEST_CASE("make_vad rejects a GGUF file that is not a FireRedVAD model") {
    TempFile file("echoir_test_make_vad_gguf.gguf");
    std::vector<char> bytes = {'G', 'G', 'U', 'F'};
    bytes.resize(64, '\0');
    write_bytes(file.path, bytes);
    CHECK_THROWS_AS(ev::make_vad(file.path), echoir::ModelLoadError);
    const std::string message = model_load_error([&] { ev::make_vad(file.path); });
    CHECK(message.find("is not a FireRedVAD model") != std::string::npos);
}

TEST_CASE("make_vad rejects a Qwen projector GGUF") {
    const auto mmproj = et::model_file("qwen3-asr-0.6b-q8_0", "mmproj");
    ECHOIR_REQUIRE_OR_SKIP(mmproj, "qwen3-asr-0.6b-q8_0 mmproj");
    CHECK_THROWS_AS(ev::make_vad(*mmproj), echoir::ModelLoadError);
}

TEST_CASE("make_vad sends a ggml file to the Silero header checks") {
    // Each message comes from the Silero header check that rejected the file.
    Header bad_window;
    bad_window.n_window = 0;
    TempFile bad_file("echoir_test_make_vad_bad_header.bin");
    write_bytes(bad_file.path, header_only_file(bad_window));
    const std::string window_error = model_load_error([&] { ev::make_vad(bad_file.path); });
    CHECK(window_error.find("Silero VAD model") != std::string::npos);
    CHECK(window_error.find("n_window must be 512") != std::string::npos);

    TempFile no_tensors("echoir_test_make_vad_header_only.bin");
    write_bytes(no_tensors.path, header_only_file(Header{}));
    const std::string tensor_error = model_load_error([&] { ev::make_vad(no_tensors.path); });
    CHECK(tensor_error.find("no tensor data") != std::string::npos);
}

TEST_CASE("silero streaming use does not split long speech, and offline use still does") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    // jfk three times over, about 33 s that Silero hears as one stretch of speech.
    const std::vector<float> once = load_16k_mono(*wav);
    std::vector<float> pcm;
    for (int i = 0; i < 3; ++i) {
        pcm.insert(pcm.end(), once.begin(), once.end());
    }

    const std::vector<ev::Range> offline = ev::SileroVad(*model).detect(pcm);
    const std::vector<ev::Range> streaming =
        ev::SileroVad(*model, 2, {}, ev::VadUse::streaming).detect(pcm);
    REQUIRE(streaming.size() == 1);
    CHECK(streaming[0].end_ms - streaming[0].start_ms > 30000);
    // Offline, whisper.cpp splits the stretch at its max duration of 15 s, plus a little.
    REQUIRE(offline.size() >= 2);
    for (const ev::Range& range : offline) {
        CHECK(range.end_ms - range.start_ms <= 16000);
    }
}

TEST_CASE("make_vad rejects zero threads") {
    const std::filesystem::path missing = "/nonexistent/echoir/ggml-silero-missing.bin";
    CHECK_THROWS_AS(ev::make_vad(missing, 0), std::invalid_argument);
}

TEST_CASE("VAD overrides keep the last value of a key and reject an entry with no key") {
    ev::VadOverrides overrides;
    overrides.add("threshold=0.9");
    overrides.add("speech_pad_ms=0");
    overrides.add("threshold=0.5");
    REQUIRE(overrides.values.size() == 2);
    CHECK(overrides.values.at("threshold") == "0.5");
    CHECK(overrides.values.at("speech_pad_ms") == "0");
    CHECK_THROWS_AS(overrides.add("threshold"), std::invalid_argument);
    CHECK_THROWS_AS(overrides.add("=0.5"), std::invalid_argument);
}

TEST_CASE("VAD setting values must be numbers of the right kind") {
    CHECK(ev::parse_count("min_silence_ms", "1500") == 1500);
    CHECK(ev::parse_count("min_silence_ms", "0") == 0);
    CHECK_THROWS_AS(ev::parse_count("min_silence_ms", "-1"), std::invalid_argument);
    CHECK_THROWS_AS(ev::parse_count("min_silence_ms", "1.5"), std::invalid_argument);
    CHECK_THROWS_AS(ev::parse_count("min_silence_ms", "20ms"), std::invalid_argument);
    CHECK_THROWS_AS(ev::parse_count("min_silence_ms", ""), std::invalid_argument);
    CHECK(ev::parse_real("threshold", "0.25") == 0.25);
    CHECK_THROWS_AS(ev::parse_real("threshold", "high"), std::invalid_argument);
    CHECK_THROWS_AS(ev::parse_real("threshold", "0.2x"), std::invalid_argument);
    CHECK_THROWS_AS(ev::parse_real("threshold", "nan"), std::invalid_argument);
    CHECK(ev::parse_real("threshold", "1e-3") == 1e-3);
    CHECK_THROWS_AS(ev::parse_real("threshold", " 0.2"), std::invalid_argument);
    CHECK_THROWS_AS(ev::parse_real("threshold", "0x1p-2"), std::invalid_argument);
    CHECK_THROWS_AS(ev::parse_real("threshold", "inf"), std::invalid_argument);
}

TEST_CASE("silero VAD rejects a FireRed key or a bad value before reading the model") {
    // The settings are checked first, so the model path can be missing.
    const std::filesystem::path missing = "/nonexistent/echoir/ggml-silero-missing.bin";
    for (const char* assignment :
         {"extend_speech_frame=50", "threshold=1.5", "min_silence_ms=-5", "no_such_setting=1"}) {
        INFO(assignment);
        ev::VadOverrides overrides;
        overrides.add(assignment);
        CHECK_THROWS_AS(ev::SileroVad(missing, 2, overrides), std::invalid_argument);
    }
}

TEST_CASE("the registry's default VADs are VAD entries, one per backend") {
    std::ifstream in(et::source_dir() / "models" / "registry.json");
    REQUIRE(in.is_open());
    const auto doc = nlohmann::json::parse(in);
    REQUIRE(doc.contains("default_vad"));
    const auto& defaults = doc.at("default_vad");
    REQUIRE(defaults.is_object());
    CHECK(defaults.size() == 2);
    for (const char* backend : {"whisper", "qwen"}) {
        INFO("backend " << backend);
        REQUIRE(defaults.contains(backend));
        const std::string id = defaults.at(backend).get<std::string>();
        bool found = false;
        for (const auto& model : doc.at("models")) {
            if (model.at("id") == id) {
                CHECK(model.at("backend") == "vad");
                found = true;
            }
        }
        CHECK(found);
    }
}

TEST_CASE("silero VAD threshold override gives less speech, through make_vad too") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);

    // Padding and a long silence merge jfk into one range, which hides the threshold. Both runs
    // use a short silence and no padding.
    ev::VadOverrides base_overrides;
    base_overrides.add("min_silence_ms=250");
    base_overrides.add("speech_pad_ms=0");
    const std::vector<ev::Range> base = ev::SileroVad(*model, 2, base_overrides).detect(pcm);
    ev::VadOverrides strict_overrides = base_overrides;
    strict_overrides.add("threshold=0.9");
    const std::vector<ev::Range> strict = ev::SileroVad(*model, 2, strict_overrides).detect(pcm);
    CHECK(strict.size() <= base.size());
    CHECK(total_ms(strict) < total_ms(base));

    // make_vad gives the same ranges as the class.
    const std::unique_ptr<ev::IVad> via_factory = ev::make_vad(*model, 2, strict_overrides);
    CHECK(via_factory->detect(pcm).size() == strict.size());
    CHECK(total_ms(via_factory->detect(pcm)) == total_ms(strict));
}

TEST_CASE("silero VAD speech_pad_ms override changes the length of its ranges") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);

    const std::vector<ev::Range> base = ev::SileroVad(*model).detect(pcm);
    ev::VadOverrides no_pad;
    no_pad.add("speech_pad_ms=0");
    const std::vector<ev::Range> unpadded = ev::SileroVad(*model, 2, no_pad).detect(pcm);
    REQUIRE(!base.empty());
    CHECK(total_ms(unpadded) < total_ms(base));
}

TEST_CASE("silero VAD min_silence_ms override bridges the pauses in jfk.wav") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    const auto wav = et::fixture("jfk");
    ECHOIR_REQUIRE_OR_SKIP(wav, "jfk.wav fixture");
    const std::vector<float> pcm = load_16k_mono(*wav);

    // A 250 ms silence splits jfk at its pauses, so the longer silence has pauses to bridge.
    ev::VadOverrides short_silence;
    short_silence.add("min_silence_ms=250");
    const std::vector<ev::Range> base = ev::SileroVad(*model, 2, short_silence).detect(pcm);
    ev::VadOverrides long_silence;
    long_silence.add("min_silence_ms=2000");
    const std::vector<ev::Range> bridged = ev::SileroVad(*model, 2, long_silence).detect(pcm);
    // jfk's pauses are all under 2 s, so fewer, longer ranges remain.
    REQUIRE(base.size() > 1);
    CHECK(bridged.size() < base.size());
}

TEST_CASE("make_vad rejects a FireRed key for a Silero model") {
    const auto model = et::model_file("silero-v6.2.0", "vad");
    ECHOIR_REQUIRE_OR_SKIP(model, "silero-v6.2.0 (vad) model");
    ev::VadOverrides overrides;
    overrides.add("prob_threshold=0.5");
    CHECK_THROWS_AS(ev::make_vad(*model, 2, overrides), std::invalid_argument);
}
