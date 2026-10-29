// Qwen3-ASR through llama.cpp and libmtmd. This is the only file that uses the mtmd API, which is
// experimental, so a change in that API touches only this file.
#include "backends/qwen_backend.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "backends/backend_log.h"
#include "errors.h"
#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"
#include "stream/signal.h"

namespace echoir {
namespace {

constexpr int kSampleRate = 16000;

// Longest window accepted. The audio encoder works in 30 s windows, and the streaming layer cuts
// segments at 10 s by default. The 30 s limit leaves room for larger segment settings.
constexpr int kMaxWindowSeconds = 30;
constexpr std::size_t kMaxWindowSamples = static_cast<std::size_t>(kMaxWindowSeconds) * kSampleRate;

// Token budget of one decode, used to size the context. The encoder emits about 14 tokens per
// second of audio (171 prompt tokens for the 11 s jfk fixture), so 16 leaves headroom.
constexpr int kAudioTokensPerSecond = 16;
constexpr int kAudioBoundaryTokens = 8;
constexpr int kTemplateTokens = 64;  // ChatML wrapper and the language prefix
constexpr int kContextTokens = 512;  // user context, at the start of the system turn
constexpr int kHistoryTokens = 512;  // the last finals, after the user context
constexpr int kContextAlign = 256;

// Generation cap: 16 tokens plus 15 per second of audio, rounded up. This bounds a runaway decode.
constexpr int kCapBase = 16;
constexpr int kCapPerSecond = 15;

// Windows quieter than this (about -80 dBFS, a few 16-bit steps) are treated as silence. Speech
// still decodes at RMS 0.00014 (about -77 dBFS), so only audio with no audible content is dropped.
constexpr double kSilenceRms = 1e-4;

// Windows under 100 ms have no speech to transcribe. The audio encoder also aborts on a window of
// one sample, so these windows never reach it.
constexpr std::size_t kMinWindowSamples = 1600;

// The decoder writes this tag before the transcript. The text before it is the language header.
constexpr std::string_view kTranscriptTag = "<asr_text>";

// ISO codes that Qwen3-ASR accepts, mapped to the language names the model writes in its header
// (for example "language English<asr_text>"). A forced prefix uses the same names.
struct LanguageName {
    const char* code;
    const char* name;
};
constexpr LanguageName kLanguages[] = {
    {"en", "English"},    {"zh", "Chinese"},    {"yue", "Cantonese"}, {"ar", "Arabic"},
    {"de", "German"},     {"fr", "French"},     {"es", "Spanish"},    {"pt", "Portuguese"},
    {"id", "Indonesian"}, {"it", "Italian"},    {"ko", "Korean"},     {"ru", "Russian"},
    {"th", "Thai"},       {"vi", "Vietnamese"}, {"ja", "Japanese"},   {"tr", "Turkish"},
    {"hi", "Hindi"},      {"ms", "Malay"},      {"nl", "Dutch"},      {"sv", "Swedish"},
    {"da", "Danish"},     {"fi", "Finnish"},    {"pl", "Polish"},     {"cs", "Czech"},
    {"tl", "Filipino"},   {"fa", "Persian"},    {"el", "Greek"},      {"ro", "Romanian"},
    {"hu", "Hungarian"},  {"mk", "Macedonian"},
};

// Returns the model's name for an ISO code, or nullptr for "auto" and "" (detect the language).
// Codes are matched exactly, as the Whisper backend does. Throws std::invalid_argument for any
// other code.
const char* forced_language_name(const std::string& language) {
    if (language.empty() || language == "auto") {
        return nullptr;
    }
    for (const LanguageName& entry : kLanguages) {
        if (language == entry.code) {
            return entry.name;
        }
    }
    throw std::invalid_argument("unsupported language: " + language);
}

// Generation cap for a window: kCapBase plus kCapPerSecond per started second of audio.
int generation_cap(std::size_t n_samples) {
    const auto seconds = static_cast<int>((n_samples + kSampleRate - 1) / kSampleRate);
    return kCapBase + kCapPerSecond * seconds;
}

// Context size for one decode at the longest window: audio, its generation cap, the user context
// and history allowances and the template. Rounded up so the context allocation has a round size.
int context_tokens() {
    const int tokens = kMaxWindowSeconds * kAudioTokensPerSecond + kAudioBoundaryTokens +
                       kContextTokens + kHistoryTokens + kTemplateTokens +
                       generation_cap(kMaxWindowSamples);
    return (tokens + kContextAlign - 1) / kContextAlign * kContextAlign;
}

// The system turn: the user context, then the history, separated by spaces. Without a context the
// history is joined exactly as it was before the context existed, empty entries included.
std::string system_text(std::string_view context, const std::vector<std::string>& history) {
    std::string joined;
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (i > 0) {
            joined += ' ';
        }
        joined += history[i];
    }
    if (context.empty() || joined.empty()) {
        return context.empty() ? joined : std::string(context);
    }
    return std::string(context) + ' ' + joined;
}

// Tokens of text, without the special tokens that the template adds. With parse_special unset, a
// literal "<|im_end|>" stays text. With it set, such a marker is one token, as in the prompt.
std::vector<llama_token> tokenize_text(const llama_vocab* vocab, std::string_view text,
                                       bool parse_special = false) {
    if (text.empty()) {
        return {};
    }
    const auto size = static_cast<int32_t>(text.size());
    // A negative count is the number of tokens the text needs.
    const int32_t needed =
        -llama_tokenize(vocab, text.data(), size, nullptr, 0, false, parse_special);
    std::vector<llama_token> tokens(static_cast<std::size_t>(needed));
    if (llama_tokenize(vocab, text.data(), size, tokens.data(), needed, false, parse_special) !=
        needed) {
        throw std::runtime_error("could not tokenize the Qwen3-ASR text");
    }
    return tokens;
}

// The prompt from the GGUF's chat template, with the audio marker as the user turn. mtmd replaces
// the marker with the audio embeddings and adds the audio start and end tokens around them. The
// system turn is always present, empty when there is no context or history. A forced language is
// prefilled after the assistant turn, so the model continues straight into the transcript. The
// prefix comes last, so the model continues the earlier output.
std::string build_prompt(const llama_model* model, const std::string& system,
                         const char* forced_language, std::string_view prefix) {
    const std::string marker = mtmd_default_marker();
    const llama_chat_message messages[] = {{"system", system.c_str()}, {"user", marker.c_str()}};
    const char* tmpl = llama_model_chat_template(model, nullptr);
    std::vector<char> buf(2 * (system.size() + marker.size()) + 256);
    auto render = [&](std::size_t size) {
        return llama_chat_apply_template(tmpl, messages, 2, true, buf.data(),
                                         static_cast<int32_t>(size));
    };
    int32_t n = render(buf.size());
    if (n > static_cast<int32_t>(buf.size())) {
        // The return value is the size needed, so render again into a buffer of that size.
        buf.resize(static_cast<std::size_t>(n));
        n = render(buf.size());
    }
    if (n < 0) {
        throw std::runtime_error("could not render the Qwen3-ASR prompt");
    }
    std::string prompt(buf.data(), static_cast<std::size_t>(n));
    if (forced_language != nullptr) {
        prompt += "language ";
        prompt += forced_language;
        prompt += kTranscriptTag;
    }
    prompt += prefix;
    return prompt;
}

// Appends the text of one token. A piece longer than the buffer comes back as the negated size
// it needs, so retry once with a buffer of that size.
void append_piece(const llama_vocab* vocab, llama_token token, std::string& out) {
    char small[128];
    int32_t n = llama_token_to_piece(vocab, token, small, sizeof(small), 0, true);
    if (n >= 0) {
        out.append(small, static_cast<std::size_t>(n));
        return;
    }
    std::string large(static_cast<std::size_t>(-n), '\0');
    n = llama_token_to_piece(vocab, token, large.data(), static_cast<int32_t>(large.size()), 0,
                             true);
    if (n < 0) {
        throw std::runtime_error("could not convert a Qwen3-ASR token to text");
    }
    out.append(large.data(), static_cast<std::size_t>(n));
}

// Number of bytes at the end of text that start a character the generation cap cut short, or 0
// when text ends on a whole code point. Byte-level tokens can end the output in the middle of a
// multi-byte character. A code point is at most four bytes, so its lead byte is among the last
// three.
std::size_t incomplete_utf8_tail(std::string_view text) {
    for (std::size_t back = 1; back <= 3 && back <= text.size(); ++back) {
        const auto byte = static_cast<unsigned char>(text[text.size() - back]);
        if ((byte & 0xC0) == 0x80) {
            continue;  // a continuation byte, so the lead byte comes earlier
        }
        std::size_t length = 1;
        if (byte >= 0xF0) {
            length = 4;
        } else if (byte >= 0xE0) {
            length = 3;
        } else if (byte >= 0xC0) {
            length = 2;
        }
        return length > back ? back : 0;
    }
    return 0;
}

// The prefix after its rollback: the last rollback_tokens tokens are removed, then one more while
// the text ends in a partial character, so the decoder never starts inside a character. The result
// is empty when the rollback covers the whole prefix. The prefix is cut the way the prompt is
// tokenized, so the transcript tag is removed whole and never left as a broken fragment.
std::string rolled_back_prefix(const llama_vocab* vocab, std::string_view prefix,
                               int rollback_tokens) {
    const std::vector<llama_token> tokens = tokenize_text(vocab, prefix, true);
    const auto rollback = static_cast<std::size_t>(rollback_tokens);
    std::size_t keep = tokens.size() > rollback ? tokens.size() - rollback : 0;
    const auto render = [&](std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            append_piece(vocab, tokens[i], text);
        }
        return text;
    };
    std::string text = render(keep);
    while (keep > 0 && incomplete_utf8_tail(text) > 0) {
        --keep;
        text = render(keep);
    }
    return text;
}

// llama_backend_init sets up process-wide state and must run once, before the first model load.
void init_llama_backend() {
    static std::once_flag once;
    std::call_once(once, [] { llama_backend_init(); });
}

}  // namespace

QwenBackend::QwenBackend(const std::string& model_path, const std::string& mmproj_path,
                         bool use_gpu, int n_threads)
    : model_(nullptr, &llama_model_free),
      ctx_(nullptr, &llama_free),
      mtmd_(nullptr, &mtmd_free),
      n_ctx_(context_tokens()) {
    if (n_threads < 1) {
        throw std::invalid_argument("n_threads must be at least 1");
    }
    backends::configure_backend_logs();
    init_llama_backend();

    // A negative n_gpu_layers offloads every layer.
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = use_gpu ? -1 : 0;
    model_.reset(llama_model_load_from_file(model_path.c_str(), model_params));
    if (!model_) {
        throw ModelLoadError("could not load Qwen3-ASR model " + model_path);
    }
    if (llama_model_chat_template(model_.get(), nullptr) == nullptr) {
        throw ModelLoadError("Qwen3-ASR model has no chat template: " + model_path);
    }

    // The batch holds a whole decode, so the audio embeddings go in one llama_decode call.
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = static_cast<uint32_t>(n_ctx_);
    ctx_params.n_batch = static_cast<uint32_t>(n_ctx_);
    ctx_params.n_threads = n_threads;
    ctx_params.n_threads_batch = n_threads;
    ctx_.reset(llama_init_from_model(model_.get(), ctx_params));
    if (!ctx_) {
        throw ModelLoadError("could not create a context for Qwen3-ASR model " + model_path);
    }

    mtmd_context_params mtmd_params = mtmd_context_params_default();
    mtmd_params.use_gpu = use_gpu;
    mtmd_params.n_threads = n_threads;
    mtmd_params.print_timings = false;
    mtmd_.reset(mtmd_init_from_file(mmproj_path.c_str(), model_.get(), mtmd_params));
    if (!mtmd_) {
        throw ModelLoadError("could not load mmproj " + mmproj_path);
    }
    if (!mtmd_support_audio(mtmd_.get()) ||
        mtmd_get_audio_sample_rate(mtmd_.get()) != kSampleRate) {
        throw ModelLoadError("mmproj has no 16 kHz audio encoder: " + mmproj_path);
    }
}

QwenBackend::~QwenBackend() = default;

DecodeResult QwenBackend::decode(const DecodeRequest& request) {
    // Validate the arguments first, so a bad language, context or prefix fails even for silent
    // input.
    const char* forced_language = forced_language_name(request.language);
    if (request.pcm.size() > kMaxWindowSamples) {
        throw std::invalid_argument("window is longer than " + std::to_string(kMaxWindowSeconds) +
                                    " s");
    }
    // The media marker in the text would be read as a second audio slot, and the tokenizer would
    // then fail with a message that does not name the context.
    if (request.context.find(mtmd_default_marker()) != std::string_view::npos) {
        throw std::invalid_argument("context must not contain " +
                                    std::string(mtmd_default_marker()));
    }
    const llama_vocab* vocab = llama_model_get_vocab(model_.get());
    const std::size_t context_count = tokenize_text(vocab, request.context).size();
    if (context_count > static_cast<std::size_t>(kContextTokens)) {
        throw std::invalid_argument("context is " + std::to_string(context_count) +
                                    " tokens; the Qwen3-ASR context holds at most " +
                                    std::to_string(kContextTokens));
    }
    if (request.prefix.find(mtmd_default_marker()) != std::string_view::npos) {
        throw std::invalid_argument("prefix must not contain " +
                                    std::string(mtmd_default_marker()));
    }
    if (request.rollback_tokens < 0) {
        throw std::invalid_argument("rollback_tokens must not be negative");
    }
    const std::string prefix = rolled_back_prefix(vocab, request.prefix, request.rollback_tokens);

    // NaN and infinite samples would reach the model as garbage text, so they are an error.
    const double level = stream::rms(request.pcm);
    if (!std::isfinite(level)) {
        throw std::invalid_argument("window has NaN or infinite samples");
    }

    // Short or silent input has no speech to transcribe. Skip the decode entirely.
    if (request.pcm.size() < kMinWindowSamples || level < kSilenceRms) {
        return {};
    }

    // Each decode is independent, like Whisper's no_context. Clearing the memory here drops the
    // KV cache of the previous decode.
    llama_memory_clear(llama_get_memory(ctx_.get()), true);

    const std::string prompt = build_prompt(
        model_.get(), system_text(request.context, request.history), forced_language, prefix);

    // The bitmap and chunks are freed by their RAII wrappers on every path.
    mtmd::bitmap_ptr audio(mtmd_bitmap_init_from_audio(request.pcm.size(), request.pcm.data()));
    if (!audio) {
        throw std::runtime_error("could not wrap the audio for Qwen3-ASR");
    }
    mtmd::input_chunks_ptr chunks(mtmd_input_chunks_init());
    if (!chunks) {
        throw std::runtime_error("could not allocate the Qwen3-ASR input chunks");
    }
    const mtmd_input_text text{prompt.c_str(), prompt.size(), true, true};
    const mtmd_bitmap* bitmaps[] = {audio.get()};
    if (mtmd_tokenize(mtmd_.get(), chunks.get(), &text, bitmaps, 1) != 0) {
        throw std::runtime_error("mtmd_tokenize failed for the Qwen3-ASR prompt");
    }

    // The prompt and the generation cap must both fit, or the decode would overflow the context.
    const int cap = generation_cap(request.pcm.size());
    const std::size_t n_prompt = mtmd_helper_get_n_tokens(chunks.get());
    if (n_prompt + static_cast<std::size_t>(cap) > static_cast<std::size_t>(n_ctx_)) {
        throw std::invalid_argument(
            "context and history are too long for the Qwen3-ASR context with this window");
    }

    llama_pos n_past = 0;
    if (mtmd_helper_eval_chunks(mtmd_.get(), ctx_.get(), chunks.get(), 0, 0, n_ctx_, true,
                                &n_past) != 0) {
        throw std::runtime_error("Qwen3-ASR failed to evaluate the prompt");
    }

    // Greedy decoding, so a repeat of the same audio gives the same text.
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
        llama_sampler_init_greedy(), &llama_sampler_free);
    std::string raw;
    for (int i = 0; i < cap; ++i) {
        llama_token token = llama_sampler_sample(sampler.get(), ctx_.get(), -1);
        if (llama_vocab_is_eog(vocab, token)) {
            break;
        }
        append_piece(vocab, token, raw);
        if (llama_decode(ctx_.get(), llama_batch_get_one(&token, 1)) != 0) {
            throw std::runtime_error("Qwen3-ASR failed to decode a token");
        }
    }

    // The generation cap can end the output inside a character. That partial character goes.
    raw.erase(raw.size() - incomplete_utf8_tail(raw));

    // The continuation keeps the header, so a prefix taken from it has the same shape as the
    // output of a decode without one.
    DecodeResult result;
    result.continuation = prefix + raw;
    result.text = result.continuation;

    // Without a forced language the output starts with "language <Name><asr_text>". With one, the
    // prefill already holds the header, so the tag may be missing, and the text is kept as is.
    const std::size_t tag_at = result.text.find(kTranscriptTag);
    if (tag_at != std::string::npos) {
        result.text.erase(0, tag_at + kTranscriptTag.size());
    }
    return result;
}

std::string QwenBackend::name() const { return "qwen3-asr"; }

}  // namespace echoir
