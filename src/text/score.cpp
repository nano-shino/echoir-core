#include "text/score.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "text/score_tables.inc"
#include "text/utf.h"

namespace echoir::text {
namespace {

// Whitespace is checked first. Dropped characters are every P*, S*, Cc and Cf code point (and the
// variation selectors) that isn't whitespace. Both tables come from Unicode categories, so none
// is missed by hand. See tools/gen-score-tables.py.

// The ranges are sorted and disjoint, so the last range that starts at or before `c` is the only
// one that can contain it.
template <std::size_t N>
bool in_table(char32_t c, const char32_t (&table)[N][2]) {
    std::size_t lo = 0;
    std::size_t hi = N;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (table[mid][0] <= c) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo > 0 && c <= table[lo - 1][1];
}

// Scored as one unit each: the Han, Hangul and kana letters, with the kana sound and iteration
// marks. Symbols, numbers and other scripts (Bopomofo, Tangut, Nushu) are not units. Ideographs
// 々 〆 〇 (U+3005..3007) are included. The ranges follow the Unicode 16.0 blocks, and the unit
// tests check one code point from each.
bool is_cjk_unit(char32_t c) {
    return (c >= 0x3005 && c <= 0x3007) ||    // 々 〆 〇
           (c >= 0x3041 && c <= 0x30FF) ||    // hiragana and katakana
           (c >= 0x31F0 && c <= 0x31FF) ||    // katakana phonetic extensions
           (c >= 0x3131 && c <= 0x318E) ||    // Hangul compatibility jamo
           (c >= 0x3400 && c <= 0x4DBF) ||    // CJK extension A
           (c >= 0x4E00 && c <= 0x9FFF) ||    // CJK unified ideographs
           (c >= 0xA960 && c <= 0xA97F) ||    // Hangul jamo extended-A
           (c >= 0xAC00 && c <= 0xD7A3) ||    // Hangul syllables
           (c >= 0xD7B0 && c <= 0xD7FB) ||    // Hangul jamo extended-B
           (c >= 0xF900 && c <= 0xFAFF) ||    // CJK compatibility ideographs
           (c >= 0xFF66 && c <= 0xFF9F) ||    // halfwidth katakana and its sound marks
           (c >= 0xFFA0 && c <= 0xFFDC) ||    // halfwidth Hangul
           (c >= 0x1100 && c <= 0x11FF) ||    // Hangul jamo
           (c >= 0x1AFF0 && c <= 0x1AFFF) ||  // kana extended-B
           (c >= 0x1B000 && c <= 0x1B16F) ||  // kana supplement, kana extended-A, small kana
           (c >= 0x20000 && c <= 0x323AF);    // CJK extensions B to H (I is inside this range)
}

char32_t fold_case(char32_t c) {
    if (c >= U'A' && c <= U'Z') {
        return c + 32;
    }
    // Latin-1 capitals, except × (U+00D7)
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) {
        return c + 32;
    }
    return c;
}

std::u32string normalize(std::u32string_view text) {
    std::u32string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (char32_t c : text) {
        if (in_table(c, score_tables::kSpace)) {
            pending_space = true;
        } else if (in_table(c, score_tables::kDropped)) {
            continue;
        } else {
            if (pending_space && !out.empty()) {
                out.push_back(U' ');
            }
            pending_space = false;
            out.push_back(fold_case(c));
        }
    }
    return out;
}

// Splits normalized text into scoring units. CJK code points are units of their own, even
// without a space between them.
std::vector<std::u32string> to_units(std::u32string_view normalized) {
    std::vector<std::u32string> units;
    std::u32string word;
    for (char32_t c : normalized) {
        if (c == U' ' || is_cjk_unit(c)) {
            if (!word.empty()) {
                units.push_back(word);
                word.clear();
            }
            if (c != U' ') {
                units.emplace_back(1, c);
            }
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) {
        units.push_back(word);
    }
    return units;
}

// The move that reached a cell of the alignment table. The traceback replays these, so the costs
// are only kept for the row being filled.
enum class Move : std::uint8_t { kDiagonal, kDeletion, kInsertion };

ErrorRate align(const std::vector<std::u32string>& ref, const std::vector<std::u32string>& hyp) {
    const std::size_t n = ref.size();
    const std::size_t m = hyp.size();
    const std::size_t width = m + 1;

    // The moves take one byte per cell, about 81 MB for two 9000-unit texts. The costs need only
    // two rows.
    std::vector<Move> moves((n + 1) * width, Move::kDiagonal);
    // prev[j] is the edit distance between the reference units so far and the first j hypothesis
    // units. cur is the row being filled.
    std::vector<std::size_t> prev(width);
    std::vector<std::size_t> cur(width);
    for (std::size_t j = 0; j <= m; ++j) {
        prev[j] = j;
        moves[j] = Move::kInsertion;
    }
    for (std::size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        moves[i * width] = Move::kDeletion;
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t diag = prev[j - 1] + (ref[i - 1] == hyp[j - 1] ? 0 : 1);
            const std::size_t del = prev[j] + 1;
            const std::size_t ins = cur[j - 1] + 1;
            const std::size_t best = std::min({diag, del, ins});
            // On ties, prefer match or substitution, then deletion, then insertion.
            if (best == diag) {
                moves[i * width + j] = Move::kDiagonal;
            } else if (best == del) {
                moves[i * width + j] = Move::kDeletion;
            } else {
                moves[i * width + j] = Move::kInsertion;
            }
            cur[j] = best;
        }
        std::swap(prev, cur);
    }

    // Walk back from the end, replaying the moves.
    ErrorRate result;
    result.ref_len = n;
    std::size_t i = n;
    std::size_t j = m;
    while (i > 0 || j > 0) {
        switch (moves[i * width + j]) {
            case Move::kDiagonal:
                if (ref[i - 1] != hyp[j - 1]) {
                    ++result.subs;
                }
                --i;
                --j;
                break;
            case Move::kDeletion:
                ++result.dels;
                --i;
                break;
            case Move::kInsertion:
                ++result.ins;
                --j;
                break;
        }
    }

    const std::size_t errors = result.subs + result.dels + result.ins;
    result.rate = static_cast<double>(errors) / static_cast<double>(std::max<std::size_t>(n, 1));
    return result;
}

}  // namespace

std::string normalize_for_score(std::string_view utf8) {
    return utf32_to_utf8(normalize(utf8_to_utf32(utf8)));
}

bool is_score_space(char32_t c) { return in_table(c, score_tables::kSpace); }

ErrorRate error_rate(std::string_view hyp_utf8, std::string_view ref_utf8) {
    const std::vector<std::u32string> hyp = to_units(normalize(utf8_to_utf32(hyp_utf8)));
    const std::vector<std::u32string> ref = to_units(normalize(utf8_to_utf32(ref_utf8)));
    return align(ref, hyp);
}

}  // namespace echoir::text
