#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace echoir::text {

// Word error rate, or character error rate for CJK text. Inputs are UTF-8.
struct ErrorRate {
    double rate = 0.0;        // (subs + dels + ins) / max(ref_len, 1)
    std::size_t subs = 0;     // reference unit replaced by a different hypothesis unit
    std::size_t dels = 0;     // reference unit missing from the hypothesis
    std::size_t ins = 0;      // hypothesis unit that isn't in the reference
    std::size_t ref_len = 0;  // reference units
};

// Lowercases ASCII and Latin-1, drops punctuation and symbols, and collapses whitespace to single
// spaces. "Punctuation and symbols" means Unicode categories P*, S*, Cc and Cf (except whitespace),
// plus the variation selectors. Dropped characters leave no space behind, so "don't" becomes
// "dont". Digits, letters and marks are kept.
std::string normalize_for_score(std::string_view utf8);

// True for the code points that normalize_for_score turns into a word break.
bool is_score_space(char32_t c);

// Levenshtein alignment over normalized units. A unit is a word, except that each CJK, kana or
// Hangul code point is its own unit.
ErrorRate error_rate(std::string_view hyp_utf8, std::string_view ref_utf8);

}  // namespace echoir::text
