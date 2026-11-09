#include "translate/glossary.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace echoir::translate {
namespace {

std::string_view trimmed(std::string_view text) {
    const char* spaces = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(spaces);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(spaces);
    return text.substr(begin, end - begin + 1);
}

// Lower case of one code point for the scripts that have a simple one-to-one rule.
char32_t fold(char32_t c) {
    if (c < 0x80) {
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) {
        return c + 32;
    }
    if (c >= 0x100 && c <= 0x17F) {
        // Latin Extended-A: pairs of upper (even) and lower (odd), except two stretches that are
        // offset by one.
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
            return (c % 2 == 1) ? c + 1 : c;
        }
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x178 || c == 0x17F) {
            return c;
        }
        return (c % 2 == 0) ? c + 1 : c;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) {
        return c + 32;
    }
    if (c >= 0x410 && c <= 0x42F) {
        return c + 32;
    }
    if (c >= 0x400 && c <= 0x40F) {
        return c + 80;
    }
    return c;
}

// Decodes text into code points. Bytes that are not valid UTF-8 become U+DC00 + the byte, which
// no valid text holds, so they only equal themselves.
std::u32string decode(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        char32_t cp = b;
        if (b >= 0xF0 && b <= 0xF4) {
            len = 4;
            cp = b & 0x07;
        } else if (b >= 0xE0) {
            len = (b <= 0xEF) ? 3 : 1;
            cp = b & 0x0F;
        } else if (b >= 0xC2 && b < 0xE0) {
            len = 2;
            cp = b & 0x1F;
        } else if (b >= 0x80) {
            len = 1;
        }
        bool ok = len > 1 && i + len <= text.size();
        for (std::size_t k = 1; ok && k < len; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                ok = false;
            } else {
                cp = (cp << 6) | (next & 0x3F);
            }
        }
        if (len == 1 && b < 0x80) {
            out.push_back(fold(b));
            ++i;
        } else if (ok) {
            out.push_back(fold(cp));
            i += len;
        } else {
            out.push_back(0xDC00 + b);
            ++i;
        }
    }
    return out;
}

}  // namespace

bool contains_ignoring_case(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return decode(haystack).find(decode(needle)) != std::u32string::npos;
}

std::vector<GlossaryEntry> parse_glossary(std::string_view text) {
    std::vector<GlossaryEntry> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos) {
            continue;
        }
        const std::string_view term = trimmed(line.substr(0, tab));
        const std::string_view translation = trimmed(line.substr(tab + 1));
        if (term.empty() || translation.empty()) {
            continue;
        }
        out.push_back({std::string(term), std::string(translation)});
    }
    return out;
}

std::vector<GlossaryEntry> select_glossary(const std::vector<GlossaryEntry>& entries,
                                           std::string_view text, std::string_view context,
                                           std::size_t max_entries) {
    const std::u32string haystack = decode(text) + U'\n' + decode(context);
    std::vector<GlossaryEntry> hits;
    std::vector<std::u32string> folded;
    for (const GlossaryEntry& entry : entries) {
        std::u32string term = decode(entry.term);
        if (term.empty() || haystack.find(term) == std::u32string::npos) {
            continue;
        }
        hits.push_back(entry);
        folded.push_back(std::move(term));
    }
    std::vector<GlossaryEntry> out;
    for (std::size_t i = 0; i < hits.size() && out.size() < max_entries; ++i) {
        bool covered = false;
        for (std::size_t j = 0; j < hits.size() && !covered; ++j) {
            covered = j != i && folded[j].size() > folded[i].size() &&
                      folded[j].find(folded[i]) != std::u32string::npos;
        }
        if (!covered) {
            out.push_back(std::move(hits[i]));
        }
    }
    return out;
}

}  // namespace echoir::translate
