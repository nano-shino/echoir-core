#include "text/stabilizer.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace echoir::text {

std::u32string longest_common_prefix(std::u32string_view a, std::u32string_view b) {
    const std::size_t limit = std::min(a.size(), b.size());
    std::size_t common = 0;
    while (common < limit && a[common] == b[common]) {
        ++common;
    }
    return std::u32string(a.substr(0, common));
}

std::u32string Stabilizer::stabilize(std::u32string_view new_text) {
    // Compare the last two decodes. With only one decode in history, compare it with new_text.
    std::u32string common;
    if (history_.size() == 1) {
        common = longest_common_prefix(history_[0], new_text);
    } else if (history_.size() >= 2) {
        common = longest_common_prefix(history_[history_.size() - 2], history_.back());
    }

    // The prefix only grows, so it stays locked while later decodes agree with it.
    if (common.size() > stable_prefix_.size()) {
        stable_prefix_ = common;
    }

    std::u32string display(new_text);
    if (stable_prefix_.size() >= kAnchorLen) {
        const std::u32string_view anchor =
            std::u32string_view(stable_prefix_).substr(stable_prefix_.size() - kAnchorLen);
        // Last occurrence, as .NET LastIndexOf(Ordinal) finds it.
        const std::size_t pos = new_text.rfind(anchor);
        if (pos != std::u32string_view::npos) {
            display = stable_prefix_ + std::u32string(new_text.substr(pos + kAnchorLen));
        } else {
            // The new output contradicts the locked prefix. Drop it rather than freeze the display.
            stable_prefix_.clear();
        }
    }

    history_.emplace_back(new_text);
    return display;
}

void Stabilizer::reset() {
    history_.clear();
    stable_prefix_.clear();
}

}  // namespace echoir::text
