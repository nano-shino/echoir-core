#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "backends/translator_backend.h"
#include "text/env.h"
#include "translate/glossary.h"
#include "translate/language.h"

namespace echoir::fake {
namespace {

constexpr long kMaxDelayMs = 10000;

// The per-call delay from ECHOIR_FAKE_TRANSLATE_DELAY_MS. Missing or not a whole number: none.
std::chrono::milliseconds delay_from_env() {
    const std::optional<std::string> text = text::get_env("ECHOIR_FAKE_TRANSLATE_DELAY_MS");
    if (!text) {
        return std::chrono::milliseconds(0);
    }
    try {
        std::size_t used = 0;
        const long ms = std::stol(*text, &used);
        if (used != text->size()) {
            return std::chrono::milliseconds(0);
        }
        return std::chrono::milliseconds(std::clamp(ms, 0L, kMaxDelayMs));
    } catch (const std::exception&) {
        return std::chrono::milliseconds(0);
    }
}

class FakeTranslator final : public translate::ITranslator {
public:
    FakeTranslator() : delay_(delay_from_env()) {}

    translate::Result translate(const translate::Request& request) override {
        const std::size_t first = request.text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            throw std::invalid_argument("text must not be empty");
        }
        if (translate::language_name(request.target).empty()) {
            throw std::invalid_argument("unsupported target language: '" + request.target + "'");
        }
        if (!request.source.empty() && request.source != "auto" &&
            translate::language_name(request.source).empty()) {
            throw std::invalid_argument("unsupported source language: '" + request.source + "'");
        }
        if (delay_.count() > 0) {
            std::this_thread::sleep_for(delay_);
        }
        const std::size_t last = request.text.find_last_not_of(" \t\r\n");
        translate::Result result;
        result.text = "[" + request.target + "] " + request.text.substr(first, last - first + 1);
        // The pairs of the glossary that the real translator would send, as " {term=translation}"
        // each, so a test sees which ones were chosen.
        const std::string line = request.text.substr(first, last - first + 1);
        for (const translate::GlossaryEntry& entry : translate::select_glossary(
                 translate::parse_glossary(request.glossary), line, request.context)) {
            result.text += " {" + entry.term + "=" + entry.translation + "}";
        }
        result.source_tokens = static_cast<int>(result.text.size());
        result.prompt_tokens = result.source_tokens + static_cast<int>(request.context.size());
        result.output_tokens = result.source_tokens;
        return result;
    }

    std::string name() const override { return "fake"; }

private:
    std::chrono::milliseconds delay_;
};

}  // namespace

std::unique_ptr<translate::ITranslator> make_translator() {
    return std::make_unique<FakeTranslator>();
}

}  // namespace echoir::fake
