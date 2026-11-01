#include "latency.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <stdexcept>

namespace echoir::cli {
namespace {

// The time at which the audio up to end_ms was in the session: the first push whose audio reaches
// end_ms. Past the last push, the last push counts.
double available_ms(const std::vector<PushTime>& pushes, std::int64_t end_ms) {
    if (pushes.empty()) {
        return 0.0;
    }
    const auto first = std::lower_bound(
        pushes.begin(), pushes.end(), end_ms,
        [](const PushTime& push, std::int64_t end) { return push.audio_end_ms < end; });
    return (first == pushes.end() ? pushes.back() : *first).at_ms;
}

// The latency of the first text of each segment, in delivery order.
std::vector<double> first_per_segment(const std::vector<PushTime>& pushes,
                                      const std::vector<TextTime>& texts) {
    std::set<std::int64_t> seen;
    std::vector<double> out;
    for (const TextTime& text : texts) {
        if (!seen.insert(text.segment_id).second) {
            continue;
        }
        const double latency = text.at_ms - available_ms(pushes, text.end_ms);
        // A text cannot be delivered before its audio is pushed. Rounding can make a difference
        // of a fraction of a millisecond negative, so it is clamped.
        out.push_back(std::max(0.0, latency));
    }
    return out;
}

}  // namespace

std::optional<double> percentile(std::vector<double> values, double p) {
    if (!(p >= 0.0 && p <= 100.0)) {
        throw std::invalid_argument("percentile must be between 0 and 100");
    }
    if (values.empty()) {
        return std::nullopt;
    }
    std::sort(values.begin(), values.end());
    // The rank is 1-based, and it is at least 1 so that p = 0 gives the smallest value.
    const auto n = static_cast<double>(values.size());
    const auto rank = static_cast<std::size_t>(std::max(1.0, std::ceil(p / 100.0 * n)));
    return values[rank - 1];
}

SegmentLatencies segment_latencies(const std::vector<PushTime>& pushes,
                                   const std::vector<TextTime>& partials,
                                   const std::vector<TextTime>& finals) {
    SegmentLatencies out;
    out.first_partial_ms = first_per_segment(pushes, partials);
    out.final_ms = first_per_segment(pushes, finals);
    return out;
}

}  // namespace echoir::cli
