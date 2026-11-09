// Text translation of one line at a time. The interface is what the C ABI drives; the model
// behind it is llama.cpp in a real build and a scripted stand-in in the fake build.
#pragma once

#include <memory>
#include <string>

namespace echoir::translate {

struct Request {
    std::string text;      // the line to translate, UTF-8
    std::string source;    // ISO code of the language of text, or "auto" or ""
    std::string target;    // ISO code of the language to write
    std::string context;   // earlier source lines, oldest first, one per line; may be empty
    std::string glossary;  // "term<TAB>translation" lines for the target language; may be empty
};

struct Result {
    std::string text;       // the translation, whole UTF-8
    bool capped = false;    // the generation hit its limit, so text may end mid-sentence
    int source_tokens = 0;  // tokens of the source line alone
    int prompt_tokens = 0;  // tokens of the whole prompt, context included
    int output_tokens = 0;
    double prefill_ms = 0;  // the prompt, up to the first generated token
    double decode_ms = 0;   // the rest of the generation
};

class ITranslator {
public:
    virtual ~ITranslator() = default;

    // Translates one line. Each call is independent: no state is kept from the one before. Not
    // thread-safe; the caller serializes calls. Throws std::invalid_argument for an empty or too
    // long text and for a language the model does not support, and a std::runtime_error when the
    // model fails.
    virtual Result translate(const Request& request) = 0;

    virtual std::string name() const = 0;
};

// The generation limit for a line of source_tokens tokens: output in another language can be
// several times longer in tokens than the source, so the limit is generous but finite, because a
// model that loops would otherwise run until the context is full.
inline int generation_cap(int source_tokens) { return 5 * source_tokens + 32; }

}  // namespace echoir::translate
