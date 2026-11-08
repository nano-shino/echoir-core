#include "backends/llama_translator.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "backends/backend_loader.h"
#include "errors.h"
#include "llama.h"
#include "translate/language.h"
#include "translate/output.h"
#include "translate/prompt.h"

namespace echoir {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kDefaultThreads = 4;

// The context holds the prompt (a line of at most one segment, and two lines of background) and the
// generation. The batch holds a whole prompt, so it is evaluated in one call; the micro-batch
// bounds the size of the compute buffers, which dominates the memory of the larger model.
constexpr int kContextTokens = 2048;
constexpr int kMicroBatchTokens = 512;

// Room that must remain for generation. A prompt that leaves less is refused.
constexpr int kMinGenerationTokens = 32;

// Lines of different lengths, to run once at load. A cold GPU shader cache compiles its kernels
// for each new prompt length on first use, which stalled the first lines by several seconds.
constexpr const char* kWarmupLines[] = {
    "Hello.",
    "Thank you all for joining us this morning, and welcome to the show.",
    "The committee will meet again next week to review the budget, the schedule of the new "
    "building, and the questions that members raised about the cost of the project, before it "
    "sends its recommendation to the full council for a vote on Thursday afternoon.",
};
constexpr const char* kWarmupContext = "We start with the news.\nIt is a busy day.";

std::vector<llama_token> tokenize(const llama_vocab* vocab, std::string_view text,
                                  bool parse_special) {
    if (text.empty()) {
        return {};
    }
    const auto size = static_cast<int32_t>(text.size());
    // A negative count is the number of tokens the text needs.
    const int32_t needed =
        -llama_tokenize(vocab, text.data(), size, nullptr, 0, false, parse_special);
    std::vector<llama_token> tokens(static_cast<std::size_t>(std::max(needed, 0)));
    if (needed > 0 && llama_tokenize(vocab, text.data(), size, tokens.data(), needed, false,
                                     parse_special) != needed) {
        throw std::runtime_error("could not tokenize the translation prompt");
    }
    return tokens;
}

// Appends the text of one token. A piece longer than the buffer comes back as the negated size
// it needs, so retry once with a buffer of that size.
void append_piece(const llama_vocab* vocab, llama_token token, std::string& out) {
    char small[128];
    int32_t n = llama_token_to_piece(vocab, token, small, sizeof(small), 0, false);
    if (n >= 0) {
        out.append(small, static_cast<std::size_t>(n));
        return;
    }
    std::string large(static_cast<std::size_t>(-n), '\0');
    n = llama_token_to_piece(vocab, token, large.data(), static_cast<int32_t>(large.size()), 0,
                             false);
    if (n < 0) {
        throw std::runtime_error("could not convert a translation token to text");
    }
    out.append(large.data(), static_cast<std::size_t>(n));
}

std::string trimmed(std::string_view text) {
    const char* spaces = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(spaces);
    if (begin == std::string_view::npos) {
        return {};
    }
    return std::string(text.substr(begin, text.find_last_not_of(spaces) - begin + 1));
}

double millis_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// llama_backend_init sets up process-wide state and must run once, before the first model load.
void init_llama_backend() {
    static std::once_flag once;
    std::call_once(once, [] { llama_backend_init(); });
}

}  // namespace

LlamaTranslator::LlamaTranslator(const TranslatorConfig& config)
    : model_(nullptr, &llama_model_free), ctx_(nullptr, &llama_free), style_(config.style) {
    const int n_threads = config.n_threads == 0 ? kDefaultThreads : config.n_threads;
    backends::load_backends();
    init_llama_backend();

    // A negative n_gpu_layers offloads every layer.
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config.use_gpu ? -1 : 0;
    model_.reset(llama_model_load_from_file(config.model_path.c_str(), model_params));
    if (!model_) {
        throw ModelLoadError("could not load translation model " + config.model_path);
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = kContextTokens;
    ctx_params.n_batch = kContextTokens;
    ctx_params.n_ubatch = kMicroBatchTokens;
    ctx_params.n_threads = n_threads;
    ctx_params.n_threads_batch = n_threads;
    ctx_.reset(llama_init_from_model(model_.get(), ctx_params));
    if (!ctx_) {
        throw ModelLoadError("could not create a context for translation model " +
                             config.model_path);
    }

    try {
        for (const char* line : kWarmupLines) {
            translate(translate::Request{line, "en", "vi", ""});
        }
        translate(translate::Request{kWarmupLines[1], "en", "vi", kWarmupContext});
    } catch (const std::exception& e) {
        throw ModelLoadError(std::string("translation model ") + config.model_path +
                             " failed its first translations: " + e.what());
    }
}

LlamaTranslator::~LlamaTranslator() = default;

std::string LlamaTranslator::name() const { return "llama"; }

translate::Result LlamaTranslator::translate(const translate::Request& request) {
    const std::string line = trimmed(request.text);
    if (line.empty()) {
        throw std::invalid_argument("text must not be empty");
    }
    const std::string target_name = translate::language_name(request.target);
    if (target_name.empty()) {
        throw std::invalid_argument("unsupported target language: '" + request.target + "'");
    }
    if (!request.source.empty() && request.source != "auto" &&
        translate::language_name(request.source).empty()) {
        throw std::invalid_argument("unsupported source language: '" + request.source + "'");
    }

    const llama_vocab* vocab = llama_model_get_vocab(model_.get());
    const translate::Prompt prompt =
        translate::render_prompt(style_, target_name, line, request.context);
    std::vector<llama_token> tokens = tokenize(vocab, prompt.prefix, true);
    const std::vector<llama_token> body = tokenize(vocab, prompt.instruction, false);
    const std::vector<llama_token> end = tokenize(vocab, prompt.suffix, true);
    tokens.insert(tokens.end(), body.begin(), body.end());
    tokens.insert(tokens.end(), end.begin(), end.end());

    translate::Result result;
    result.source_tokens = static_cast<int>(tokenize(vocab, line, false).size());
    result.prompt_tokens = static_cast<int>(tokens.size());
    const int room = kContextTokens - result.prompt_tokens;
    if (room < kMinGenerationTokens) {
        throw std::invalid_argument(
            "text and context are too long: " + std::to_string(result.prompt_tokens) +
            " prompt tokens, the " + "translator holds " + std::to_string(kContextTokens) +
            " with its output");
    }
    // One token more than the limit is sampled to tell a line that ends there from one that is cut.
    const int limit = std::min(translate::generation_cap(result.source_tokens), room - 1);

    // Each translation is independent: nothing of the one before stays in the memory.
    llama_memory_clear(llama_get_memory(ctx_.get()), true);

    // Greedy decoding, so a repeat of the same line gives the same text.
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
        llama_sampler_init_greedy(), &llama_sampler_free);
    const Clock::time_point start = Clock::now();
    if (llama_decode(ctx_.get(), llama_batch_get_one(tokens.data(),
                                                     static_cast<int32_t>(tokens.size()))) != 0) {
        throw std::runtime_error("the translator failed to evaluate the prompt");
    }
    llama_token token = llama_sampler_sample(sampler.get(), ctx_.get(), -1);
    result.prefill_ms = millis_since(start);

    const Clock::time_point decode_start = Clock::now();
    std::string raw;
    for (;;) {
        if (llama_vocab_is_eog(vocab, token)) {
            break;
        }
        if (result.output_tokens >= limit) {
            result.capped = true;
            break;
        }
        append_piece(vocab, token, raw);
        ++result.output_tokens;
        if (llama_decode(ctx_.get(), llama_batch_get_one(&token, 1)) != 0) {
            throw std::runtime_error("the translator failed to decode a token");
        }
        token = llama_sampler_sample(sampler.get(), ctx_.get(), -1);
    }
    result.decode_ms = millis_since(decode_start);
    result.text = translate::clean_output(raw);
    return result;
}

}  // namespace echoir
