#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace echoir::text {

// True if the text looks like a decoding loop. Any of these holds:
//   - a runaway run (see kRunawayMin in clean.h)
//   - a phrase of 3..40 code points repeated 4 times back to back, as .NET `(.{3,40}?)\1{3,}`
//     matches it. The phrase may not contain '\n', because .NET's '.' excludes it.
//   - at least 20 code points using 3 or fewer distinct code points
// Call it on the raw decode, before clean_segment: cleaning collapses runs and would hide them.
bool is_repetitive(std::u32string_view text);

// True if the text is nonempty and occurs in the history joined end to end, ignoring case,
// whitespace and ASCII punctuation. A decoder given unclear audio can copy its prompt instead of
// transcribing, so such a decode is suspect. Real speech can match too, when a phrase is said
// again. Call it on the cleaned text.
bool echoes_history(std::u32string_view text, const std::vector<std::string>& history);

}  // namespace echoir::text
