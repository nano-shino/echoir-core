// A glossary of terms the user wants translated in one way, such as the names of people and
// places, and the part of it that a request needs.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace echoir::translate {

struct GlossaryEntry {
    std::string term;         // in the source language
    std::string translation;  // in the request's target language
};

// Parses one "term<TAB>translation" pair per line (LF or CRLF). A line without a tab, and a line
// whose term or translation is blank after trimming, is skipped. Only the first tab splits, so a
// translation may hold tabs.
std::vector<GlossaryEntry> parse_glossary(std::string_view text);

// The entries that a request needs: those whose term occurs in text or in context, ignoring
// case. When a term holds another selected term, the shorter one is left out, since the longer
// pair already covers it. The order of the glossary is kept, and at most max_entries are returned
// (the earliest).
std::vector<GlossaryEntry> select_glossary(const std::vector<GlossaryEntry>& entries,
                                           std::string_view text, std::string_view context,
                                           std::size_t max_entries = 32);

// True when haystack holds needle, ignoring case. Letters fold by simple one-to-one rules for
// ASCII, Latin, Greek and Cyrillic; other characters compare as they are. Invalid UTF-8 compares
// byte for byte.
bool contains_ignoring_case(std::string_view haystack, std::string_view needle);

}  // namespace echoir::translate
