// The prompt of a translation model, written by the library for each model family. The GGUF chat
// templates are not used: llama.cpp renders the Hy-MT2 ones in another layout than the model was
// trained on, and the 1.8B and 7B models wrap the same instruction in different control tokens.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace echoir::translate {

// How a model wraps its instruction.
//   hy-mt2-1.8b  <hy_begin▁of▁sentence><hy_User> instruction <hy_Assistant>, each in the
//                fullwidth-bar form the vocabulary spells
//   hy-mt2-7b    <|startoftext|> instruction <|extra_0|>
enum class PromptStyle { hy_mt2_1_8b, hy_mt2_7b };

// The style a registry "prompt_style" value names, or nothing for an unknown one.
std::optional<PromptStyle> parse_prompt_style(std::string_view name);

// A prompt in three parts. The prefix and suffix hold control tokens and are tokenized with them
// parsed. The instruction holds the line, and is tokenized as plain text, so a line that happens
// to contain a control token's spelling stays text.
struct Prompt {
    std::string prefix;
    std::string instruction;
    std::string suffix;
};

// Renders the prompt that asks for text in target_name (an English language name). A non-empty
// context (earlier source lines) goes in the model's background-information form.
Prompt render_prompt(PromptStyle style, std::string_view target_name, std::string_view text,
                     std::string_view context);

}  // namespace echoir::translate
