#include "text/repetition.h"

#include <cctype>
#include <cstddef>
#include <string>

#include "text/clean.h"
#include "text/utf.h"

namespace echoir::text {
namespace {

constexpr std::size_t kMinPhrase = 3;
constexpr std::size_t kMaxPhrase = 40;
constexpr std::size_t kCopies = 4;
constexpr std::size_t kMinDistinctLength = 20;
constexpr std::size_t kMaxDistinct = 3;

bool has_runaway_run(std::u32string_view text) {
    std::size_t run = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        run = (i > 0 && text[i] == text[i - 1]) ? run + 1 : 1;
        if (run >= kRunawayMin && !is_ascii_digit(text[i])) {
            return true;
        }
    }
    return false;
}

// Same match as `(.{3,40}?)\1{3,}`. A group with a '\n' can't match, and neither can any longer
// group that starts with it, so the inner loop stops there.
bool has_repeated_phrase(std::u32string_view text) {
    const std::size_t n = text.size();
    for (std::size_t start = 0; start < n; ++start) {
        for (std::size_t len = 1; len <= kMaxPhrase && start + kCopies * len <= n; ++len) {
            if (text[start + len - 1] == U'\n') {
                break;
            }
            if (len < kMinPhrase) {
                continue;
            }
            const std::u32string_view phrase = text.substr(start, len);
            bool repeats = true;
            for (std::size_t k = 1; k < kCopies && repeats; ++k) {
                repeats = text.substr(start + k * len, len) == phrase;
            }
            if (repeats) {
                return true;
            }
        }
    }
    return false;
}

bool has_few_distinct(std::u32string_view text) {
    if (text.size() < kMinDistinctLength) {
        return false;
    }
    // Only needs to count up to kMaxDistinct + 1, so a short linear scan is enough.
    std::u32string seen;
    for (char32_t c : text) {
        if (seen.find(c) == std::u32string::npos) {
            seen.push_back(c);
            if (seen.size() > kMaxDistinct) {
                return false;
            }
        }
    }
    return true;
}

// The text without whitespace and ASCII punctuation, with ASCII letters in lower case, so that
// "Understood, sir." and "understood sir" compare equal.
void append_folded(std::u32string_view text, std::u32string& out) {
    for (char32_t c : text) {
        if (c < 0x80) {
            const auto ascii = static_cast<unsigned char>(c);
            if (std::isspace(ascii) != 0 || std::ispunct(ascii) != 0) {
                continue;
            }
            c = static_cast<char32_t>(std::tolower(ascii));
        } else if (c == U'\u3000') {
            continue;  // ideographic space
        }
        out.push_back(c);
    }
}

}  // namespace

bool is_repetitive(std::u32string_view text) {
    return has_runaway_run(text) || has_repeated_phrase(text) || has_few_distinct(text);
}

bool echoes_history(std::u32string_view text, const std::vector<std::string>& history) {
    std::u32string needle;
    append_folded(text, needle);
    if (needle.empty()) {
        return false;
    }
    std::u32string haystack;
    for (const std::string& entry : history) {
        append_folded(utf8_to_utf32(entry), haystack);
    }
    return haystack.find(needle) != std::u32string::npos;
}

}  // namespace echoir::text
