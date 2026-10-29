#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace echoir::text {

// A run of this many identical non-digit code points is runaway, as in the regex `([^0-9])\1{14,}`.
inline constexpr std::size_t kRunawayMin = 15;

// Removes " [BLANK_AUDIO]", collapses runaway runs to one code point, and trims leading U+0020
// only (not other whitespace). Call is_repetitive on the raw text first.
std::u32string clean_segment(std::u32string_view raw);

// Collapses each non-digit run of kRunawayMin or more to a single code point. Digit runs stay,
// so long numbers aren't mangled.
std::u32string collapse_runaway(std::u32string_view text);

}  // namespace echoir::text
