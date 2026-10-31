#include "backends/whisper_backend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "backends/backend_loader.h"
#include "backends/backend_log.h"
#include "backends/ggml_header.h"
#include "errors.h"
#include "text/utf.h"
#include "whisper.h"

namespace echoir {
namespace {

// Published Whisper models have 51864 to 51866 tokens, and the timestamp tokens sit at the top.
constexpr std::int32_t kMinVocab = 51864;
// whisper.cpp names n_vocab - 51766 languages (multilingual) from a 100-entry table. A larger
// n_vocab makes its loader ask for an unnamed language and crash, so the bound stays.
constexpr std::int32_t kMaxVocab = 51766 + 100;
// Larger layer and state counts exhaust memory or abort in ggml.
constexpr std::int32_t kMaxLayers = 32;
constexpr std::int32_t kMaxState = 4096;

// Header fields after the magic, in whisper's file order.
enum Field : std::size_t {
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
    kFieldCount,
};

// ggml_ftype_to_ggml_type maps 0..28 except 4, 5 and 6, and asserts (aborts) on the rest. The
// values 5 and 6 have no enum names. whisper.cpp reads the base type after dividing by
// GGML_QNT_VERSION_FACTOR, so this does the same.
bool is_known_ftype(std::int32_t ftype) {
    const std::int32_t base = ftype % GGML_QNT_VERSION_FACTOR;
    return base >= 0 && base <= GGML_FTYPE_MOSTLY_Q2_0 && base != GGML_FTYPE_MOSTLY_Q4_1_SOME_F16 &&
           base != 5 && base != 6;
}

// The header is read here before whisper.cpp sees the file, because whisper trusts it.
void check_model_header(const std::string& path) {
    // whisper.cpp reads its own path as UTF-8, so this check opens the file the same way.
    const std::filesystem::path file_path = text::path_from_utf8(path);
    std::ifstream in;
    // Only a regular file is opened, because opening a named pipe with no writer blocks.
    if (backends::is_regular_model_file(file_path)) {
        in.open(file_path, std::ios::binary);
    }
    if (!in) {
        throw ModelLoadError("cannot open model " + path);
    }
    backends::GgmlHeader header(in, path, "Whisper");
    header.expect_magic();
    std::array<std::int32_t, kFieldCount> f{};
    for (std::int32_t& value : f) {
        value = header.field();
    }

    header.require_range("n_vocab", f[kNVocab], kMinVocab, kMaxVocab);
    header.require_range("n_audio_layer", f[kNAudioLayer], 1, kMaxLayers);
    header.require_range("n_text_layer", f[kNTextLayer], 1, kMaxLayers);
    header.require_range("n_audio_state", f[kNAudioState], 1, kMaxState);
    header.require_range("n_text_state", f[kNTextState], 1, kMaxState);
    if (f[kNAudioState] != f[kNTextState]) {
        header.reject("inconsistent state sizes");
    }
    // whisper.cpp reshapes each state into heads, so a zero or non-divisor head aborts in ggml.
    const auto heads_divide = [](std::int32_t state, std::int32_t heads) {
        return heads > 0 && state % heads == 0;
    };
    if (!heads_divide(f[kNAudioState], f[kNAudioHead]) ||
        !heads_divide(f[kNTextState], f[kNTextHead])) {
        header.reject("bad head count");
    }
    if (!is_known_ftype(f[kFtype])) {
        header.reject("unknown ftype " + std::to_string(f[kFtype]));
    }
}

// The number of prompt tokens of text. A token covers at least one byte, so a buffer with one
// entry per byte always fits, and whisper.cpp logs an error only when the buffer is too small.
int token_count(whisper_context* ctx, const std::string& text) {
    std::vector<whisper_token> tokens(text.size() + 1);
    return whisper_tokenize(ctx, text.c_str(), tokens.data(), static_cast<int>(tokens.size()));
}

// The initial prompt: the context, then each history entry as " " + text. Whisper keeps the tail
// of an over-long prompt, which would cut the start of the context. So the oldest history entries
// go first, and a context that is too long by itself is an error.
std::string build_prompt(whisper_context* ctx, std::string_view context,
                         const std::vector<std::string>& history) {
    // Half the text positions, less one: the same limit whisper.cpp applies to the prompt.
    const int limit = whisper_n_text_ctx(ctx) / 2 - 1;
    const std::string head(context);
    const int head_tokens = token_count(ctx, head);
    if (head_tokens > limit) {
        throw std::invalid_argument("context is " + std::to_string(head_tokens) +
                                    " tokens; the Whisper prompt holds at most " +
                                    std::to_string(limit));
    }
    for (std::size_t first = 0; first < history.size(); ++first) {
        std::string prompt = head;
        for (std::size_t i = first; i < history.size(); ++i) {
            prompt += ' ';
            prompt += history[i];
        }
        if (token_count(ctx, prompt) <= limit) {
            return prompt;
        }
    }
    return head;
}

// Loads the model and reports whether whisper logged a WARN or ERROR while doing so. Whisper
// reports a bad load that way rather than through its return value.
whisper_context* load_model(const std::string& path, bool use_gpu, bool& problem) {
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = use_gpu;

    // A file with no tensors still returns a context, so the log is the only sign of it.
    backends::LoadWatch watch;
    whisper_context* ctx = whisper_init_from_file_with_params(path.c_str(), cparams);
    problem = watch.problem();
    return ctx;
}

}  // namespace

WhisperBackend::WhisperBackend(const std::string& model_path, bool use_gpu, int n_threads)
    : n_threads_(n_threads) {
    if (n_threads < 1) {
        throw std::invalid_argument("n_threads must be at least 1");
    }
    backends::load_backends();
    check_model_header(model_path);

    // A GPU that fails to start is not a bad model. Whisper then runs on the CPU but logs an
    // ERROR, so a load that asked for the GPU is retried on the CPU.
    bool problem = false;
    whisper_context* ctx = load_model(model_path, use_gpu, problem);
    if (use_gpu && (ctx == nullptr || problem)) {
        whisper_free(ctx);
        ctx = load_model(model_path, false, problem);
    }
    if (ctx == nullptr) {
        throw ModelLoadError("could not load Whisper model " + model_path);
    }
    if (problem) {
        whisper_free(ctx);
        throw ModelLoadError("Whisper model did not load cleanly: " + model_path);
    }
    ctx_ = ctx;
    multilingual_ = whisper_is_multilingual(ctx_) != 0;
}

WhisperBackend::~WhisperBackend() { whisper_free(ctx_); }

DecodeResult WhisperBackend::decode(const DecodeRequest& request) {
    // whisper.cpp has no way to continue a decode, so a prefix would be silently ignored.
    if (!request.prefix.empty()) {
        throw std::invalid_argument("the Whisper backend does not support a decoder prefix");
    }
    // Empty or "auto" means detect. English-only models have no language tokens to detect, so
    // they run as "en" (whisper's own CLI does the same). Any other code must name a language the
    // model has, because whisper only logs an unknown code and then decodes with a bad id.
    std::string language = request.language.empty() ? "auto" : request.language;
    if (language == "auto") {
        if (!multilingual_) {
            language = "en";
        }
    } else {
        const int lang_id = whisper_lang_id(language.c_str());
        // An English-only model has one language, "en", which is id 0.
        if (lang_id < 0 || (!multilingual_ && lang_id != 0)) {
            throw std::invalid_argument("unsupported language: " + language);
        }
    }

    // The prompt is checked before the empty-input return, so a bad context fails on every call.
    const std::string prompt = build_prompt(ctx_, request.context, request.history);

    // Nothing to decode, so don't call into whisper at all.
    if (request.pcm.empty()) {
        return {};
    }
    if (request.pcm.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("pcm is too long for one whisper_full call");
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = n_threads_;
    params.no_context = true;
    params.greedy.best_of = 1;
    // Printing goes to stdout, which the CLI's JSONL output needs, so all four print flags are off.
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.print_special = false;
    // whisper's default language is "en", so it has to be set on every call.
    params.language = language.c_str();
    params.initial_prompt = prompt.empty() ? nullptr : prompt.c_str();
    // whisper.cpp drops the prompt when a temperature fallback reaches 0.5 or more, so those
    // retries run without the context. The temperature and suppress flags keep whisper's defaults.

    const int rc =
        whisper_full(ctx_, params, request.pcm.data(), static_cast<int>(request.pcm.size()));
    if (rc != 0) {
        throw std::runtime_error("whisper_full failed with code " + std::to_string(rc));
    }

    DecodeResult result;
    const int n_segments = whisper_full_n_segments(ctx_);
    for (int i = 0; i < n_segments; ++i) {
        // The pointer is only valid until the next whisper call, so copy it now.
        result.text += whisper_full_get_segment_text(ctx_, i);
    }
    result.continuation = result.text;
    return result;
}

std::string WhisperBackend::name() const { return "whisper"; }

}  // namespace echoir
