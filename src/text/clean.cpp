#include "text/clean.h"

#include <cstddef>

#include "text/utf.h"

namespace echoir::text {
namespace {

constexpr std::u32string_view kBlankAudio = U" [BLANK_AUDIO]";

}  // namespace

std::u32string collapse_runaway(std::u32string_view text) {
    std::u32string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const char32_t c = text[i];
        std::size_t run = 1;
        while (i + run < text.size() && text[i + run] == c) {
            ++run;
        }
        if (run >= kRunawayMin && !is_ascii_digit(c)) {
            out.push_back(c);
        } else {
            out.append(run, c);
        }
        i += run;
    }
    return out;
}

std::u32string clean_segment(std::u32string_view raw) {
    // Remove the marker first, then collapse runs, then trim.
    std::u32string without_marker;
    without_marker.reserve(raw.size());
    std::size_t pos = 0;
    std::size_t hit = raw.find(kBlankAudio, pos);
    while (hit != std::u32string_view::npos) {
        without_marker.append(raw.substr(pos, hit - pos));
        pos = hit + kBlankAudio.size();
        hit = raw.find(kBlankAudio, pos);
    }
    without_marker.append(raw.substr(pos));

    const std::u32string collapsed = collapse_runaway(without_marker);
    const std::size_t first = collapsed.find_first_not_of(U' ');
    if (first == std::u32string::npos) {
        return {};
    }
    return collapsed.substr(first);
}

}  // namespace echoir::text
