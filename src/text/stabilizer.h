#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace echoir::text {

// Longest common prefix of a and b, in code points.
std::u32string longest_common_prefix(std::u32string_view a, std::u32string_view b);

// Keeps the text shown for an ongoing segment steady. Each decode is compared with the earlier
// ones, and a common prefix is locked once it is long enough. One instance per ongoing segment.
// Not thread-safe.
class Stabilizer {
public:
    // Returns the text to display for this decode, and appends new_text to history(). Callers
    // must not append new_text to history() themselves.
    std::u32string stabilize(std::u32string_view new_text);

    // Clears history and the locked prefix. Call it when the segment is finalized or the session
    // is cleared.
    void reset();

    // Decodes of the current ongoing segment, oldest first.
    const std::vector<std::u32string>& history() const { return history_; }

private:
    // The prefix locks only once it is at least this long. The last kAnchorLen code points of
    // the locked prefix must then appear in the new text, or the lock is dropped.
    static constexpr std::size_t kAnchorLen = 10;

    std::vector<std::u32string> history_;
    std::u32string stable_prefix_;
};

}  // namespace echoir::text
