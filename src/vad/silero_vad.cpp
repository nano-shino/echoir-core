#include "vad/silero_vad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <istream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "backends/backend_loader.h"
#include "backends/ggml_header.h"
#include "errors.h"
#include "whisper.h"

namespace echoir::vad {
namespace {

// 16 kHz input: 16 samples per ms.
constexpr std::int64_t kSamplesPerMs = 16;

// The LSTM weights grow with the square of its size, and huge values overflow ggml's element count
// and abort. Silero models use 128, so this bound leaves room for much larger models.
constexpr std::int32_t kMaxLstmHidden = 4096;

// After the last tensor, whisper.cpp reads the next entry's three int32 fields to find the end of
// the list. Those reads run past the end of a complete file and fail. Any further shortfall is
// missing data.
constexpr std::size_t kEndProbeBytes = 3 * sizeof(std::int32_t);

// whisper.cpp re-merges pieces split at the max duration when the gap between them is under 200 ms,
// so the 15 s cap is best effort. Callers must cap ranges themselves; the CLI's offline path cuts
// anything over 20 s. A streaming session cuts at its own limit, so it takes the no-limit value
// (whisper.cpp's default). Only the settings in SileroSettings can be overridden.
whisper_vad_params vad_params(const SileroSettings& s, VadUse use) {
    whisper_vad_params p = whisper_vad_default_params();
    p.threshold = s.threshold;
    p.min_speech_duration_ms = 100;
    p.min_silence_duration_ms = s.min_silence_ms;
    p.max_speech_duration_s = use == VadUse::streaming ? std::numeric_limits<float>::max() : 15.0f;
    p.speech_pad_ms = s.speech_pad_ms;
    p.samples_overlap = 0.1f;
    return p;
}

// The settings named by the overrides, on top of the defaults. Any other key is an error, so a
// FireRedVAD key given to this model fails with a message that names the keys it takes.
SileroSettings settings_from(const VadOverrides& overrides) {
    SileroSettings s;
    for (const auto& [key, value] : overrides.values) {
        if (key == "threshold") {
            s.threshold = static_cast<float>(parse_probability(key, value));
        } else if (key == "min_silence_ms") {
            s.min_silence_ms = parse_count(key, value);
        } else if (key == "speech_pad_ms") {
            s.speech_pad_ms = parse_count(key, value);
        } else {
            throw std::invalid_argument("unknown setting for the Silero VAD: " + key +
                                        " (use threshold, min_silence_ms or speech_pad_ms)");
        }
    }
    return s;
}

// The model file as whisper.cpp reads it. It counts the bytes that whisper asks for and the file
// cannot supply, because whisper.cpp ignores failed reads.
struct ModelReader {
    std::ifstream in;
    std::size_t missing = 0;
};

std::size_t read_model(void* ctx, void* out, std::size_t n) {
    auto& reader = *static_cast<ModelReader*>(ctx);
    reader.in.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
    const auto got = static_cast<std::size_t>(reader.in.gcount());
    reader.missing += n - got;
    return got;
}

bool model_eof(void* ctx) { return static_cast<ModelReader*>(ctx)->in.eof(); }

// whisper.cpp does not close a VAD model stream. ModelReader closes it when it goes out of scope.
void no_close(void* /*ctx*/) {}

// whisper.cpp checks only the magic, and some bad header values abort in its graph code or leak a
// context after it allocates one. So the header is checked here first. The version fields,
// n_context and final_conv_out are read but not checked, because the graph never uses them.
// Returns the header size in bytes.
std::size_t check_header(std::istream& in, const std::filesystem::path& model) {
    backends::GgmlHeader header(in, model.string(), "Silero VAD");
    header.expect_magic();

    const std::int32_t name_len = header.field();
    if (name_len < 1 || name_len > 64) {
        header.reject("bad model name length");
    }
    header.skip(static_cast<std::size_t>(name_len));
    for (int i = 0; i < 3; ++i) {  // version major, minor, patch
        header.field();
    }
    const std::int32_t n_window = header.field();
    header.field();  // n_context
    // The graph is built for 512-sample windows. Other sizes give wrong results or abort.
    if (n_window != 512) {
        header.reject("n_window must be 512");
    }

    if (header.field() != 4) {
        header.reject("expected 4 encoder layers");
    }
    std::array<std::array<std::int32_t, 3>, 4> encoder{};  // in, out, kernel per layer
    for (auto& layer : encoder) {
        for (std::int32_t& value : layer) {
            value = header.field();
        }
    }
    for (const auto& layer : encoder) {
        if (layer[0] <= 0 || layer[1] <= 0 || layer[2] <= 0) {
            header.reject("encoder sizes must be positive");
        }
    }

    const std::int32_t lstm_input = header.field();
    const std::int32_t lstm_hidden = header.field();
    const std::int32_t final_conv_in = header.field();
    header.field();  // final_conv_out
    // The LSTM reads the last encoder layer. whisper.cpp also uses lstm_input as the STFT hop, so
    // another value loads and gives wrong results.
    if (lstm_input != encoder.back()[1]) {
        header.reject("lstm_input must equal the last encoder layer's output size");
    }
    // A zero size makes whisper.cpp abort, and so does a huge LSTM (see kMaxLstmHidden).
    header.require_range("lstm_hidden", lstm_hidden, 1, kMaxLstmHidden);
    if (final_conv_in <= 0) {
        header.reject("final conv size must be positive");
    }
    return static_cast<std::size_t>(in.tellg());
}

// Frees the segment list on every exit path, including a throw.
using SegmentList = std::unique_ptr<whisper_vad_segments, decltype(&whisper_vad_free_segments)>;

}  // namespace

void SileroVad::Deleter::operator()(whisper_vad_context* ctx) const noexcept {
    whisper_vad_free(ctx);
}

SileroVad::SileroVad(const std::filesystem::path& model, int n_threads,
                     const VadOverrides& overrides, VadUse use)
    : use_(use) {
    if (n_threads < 1) {
        throw std::invalid_argument("SileroVad: n_threads must be at least 1");
    }
    // Checked before the model is read, so a bad setting needs no model file.
    settings_ = settings_from(overrides);
    // whisper.cpp returns null for a missing file without saying why. Checking first gives a
    // clear message, and an unreadable file fails the same check. The check also keeps a named
    // pipe from blocking the open below.
    if (!backends::is_regular_model_file(model)) {
        throw ModelLoadError("VAD model not found: " + model.string());
    }
    ModelReader reader;
    // Opening from the path (not a narrow copy of it) lets MSVC use the wide file name API.
    reader.in.open(model, std::ios::binary);
    if (!reader.in) {
        throw ModelLoadError("cannot read VAD model: " + model.string());
    }
    const std::size_t header_bytes = check_header(reader.in, model);
    // whisper.cpp loads a file that ends after the header, with no weights, and only warns.
    std::error_code ec;
    const std::uintmax_t file_bytes = std::filesystem::file_size(model, ec);
    if (ec || file_bytes <= header_bytes) {
        throw ModelLoadError("VAD model has no tensor data: " + model.string());
    }
    reader.in.clear();
    reader.in.seekg(0);

    whisper_model_loader loader = {};
    loader.context = &reader;
    loader.read = &read_model;
    loader.eof = &model_eof;
    loader.close = &no_close;

    backends::load_backends();
    whisper_vad_context_params cparams = whisper_vad_default_context_params();
    cparams.n_threads = n_threads;
    cparams.use_gpu = false;  // VAD runs on CPU.
    whisper_vad_context* loaded = nullptr;
    try {
        loaded = whisper_vad_init_with_params(&loader, cparams);
    } catch (const std::exception& e) {
        // A corrupt tensor entry can make whisper.cpp throw, for example with a negative name
        // length.
        throw ModelLoadError(std::string("cannot load VAD model (") + e.what() +
                             "): " + model.string());
    }
    if (!loaded) {
        throw ModelLoadError("cannot load VAD model: " + model.string());
    }
    // Freed by the member destructor if the check below throws.
    ctx_.reset(loaded);
    if (reader.missing > kEndProbeBytes) {
        throw ModelLoadError("VAD model is truncated: " + model.string());
    }
}

std::vector<Range> SileroVad::detect(std::span<const float> pcm16k_mono) {
    if (!ctx_) {
        throw std::runtime_error("SileroVad: no model loaded (moved-from object)");
    }
    // No audio means no speech, and whisper.cpp would run an empty graph for it.
    if (pcm16k_mono.empty()) {
        return {};
    }
    if (pcm16k_mono.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("SileroVad::detect: span is too long for whisper.cpp");
    }

    const whisper_vad_params params = vad_params(settings_, use_);
    SegmentList segments(whisper_vad_segments_from_samples(ctx_.get(), params, pcm16k_mono.data(),
                                                           static_cast<int>(pcm16k_mono.size())),
                         &whisper_vad_free_segments);
    // A null list is an error. A list with no entries means no speech.
    if (!segments) {
        throw std::runtime_error("VAD failed to detect speech");
    }

    // whisper.cpp returns centiseconds as floats, so multiply by 10 to get milliseconds.
    const auto to_ms = [](float cs) {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(cs) * 10.0));
    };
    const std::int64_t span_ms = static_cast<std::int64_t>(pcm16k_mono.size()) / kSamplesPerMs;

    const int n = whisper_vad_segments_n_segments(segments.get());
    std::vector<Range> ranges;
    ranges.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        // Padding and the window grid can push a range past the span, so clamp both ends.
        const std::int64_t start =
            std::clamp(to_ms(whisper_vad_segments_get_segment_t0(segments.get(), i)),
                       std::int64_t{0}, span_ms);
        const std::int64_t end =
            std::clamp(to_ms(whisper_vad_segments_get_segment_t1(segments.get(), i)),
                       std::int64_t{0}, span_ms);
        if (end <= start) {
            continue;
        }
        ranges.push_back({start, end});
    }
    return ranges;
}

}  // namespace echoir::vad
