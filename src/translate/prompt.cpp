#include "translate/prompt.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace echoir::translate {
namespace {

std::string trimmed(std::string_view text) {
    const char* spaces = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(spaces);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(spaces);
    return std::string(text.substr(begin, end - begin + 1));
}

// The instruction of Hy-MT2. It takes no system prompt.
std::string hy_instruction(std::string_view target_name, std::string_view text,
                           std::string_view context, const std::vector<GlossaryEntry>& glossary) {
    const std::string line = trimmed(text);
    const std::string background = trimmed(context);
    if (!glossary.empty()) {
        std::string terms = "Reference the following translations:\n";
        for (const GlossaryEntry& entry : glossary) {
            terms += entry.term + " translates to " + entry.translation + "\n";
        }
        if (background.empty()) {
            return terms + "\nTranslate the following text into " + std::string(target_name) +
                   ". Note that you must ONLY output the translated result without any additional "
                   "explanation:\n\n" +
                   line;
        }
        return "[Background Information]\n" + background + "\n\n" + terms +
               "\nPlease translate the following text into " + std::string(target_name) +
               ", taking the provided background information and the reference translations into "
               "consideration. Only output the translation of the Source Text.\n\n[Source "
               "Text]\n" +
               line;
    }
    if (background.empty()) {
        return "Translate the following text into " + std::string(target_name) +
               ". Note that you should only output the translated result without any additional "
               "explanation:\n\n" +
               line;
    }
    return "[Background Information]\n" + background +
           "\n\nPlease translate the following text into " + std::string(target_name) +
           ", taking the provided background information into consideration. Only output the "
           "translation of the Source Text.\n\n[Source Text]\n" +
           line;
}

}  // namespace

std::optional<PromptStyle> parse_prompt_style(std::string_view name) {
    if (name == "hy-mt2-1.8b") {
        return PromptStyle::hy_mt2_1_8b;
    }
    if (name == "hy-mt2-7b") {
        return PromptStyle::hy_mt2_7b;
    }
    return std::nullopt;
}

Prompt render_prompt(PromptStyle style, std::string_view target_name, std::string_view text,
                     std::string_view context, const std::vector<GlossaryEntry>& glossary) {
    Prompt prompt;
    prompt.instruction = hy_instruction(target_name, text, context, glossary);
    switch (style) {
        case PromptStyle::hy_mt2_1_8b:
            // The markers use U+FF5C (fullwidth vertical line) and U+2581 (lower one eighth block).
            prompt.prefix =
                "<\xEF\xBD\x9Chy_begin\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C>"
                "<\xEF\xBD\x9Chy_User\xEF\xBD\x9C>";
            prompt.suffix = "<\xEF\xBD\x9Chy_Assistant\xEF\xBD\x9C>";
            break;
        case PromptStyle::hy_mt2_7b:
            prompt.prefix = "<|startoftext|>";
            prompt.suffix = "<|extra_0|>";
            break;
    }
    return prompt;
}

}  // namespace echoir::translate
