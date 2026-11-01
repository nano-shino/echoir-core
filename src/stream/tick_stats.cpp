#include "stream/tick_stats.h"

#include <cmath>

#include "json.hpp"

namespace echoir {

void TickStats::add(double decode_ms, double window_ms) noexcept {
    ++ticks_;
    decode_ms_ += decode_ms;
    window_ms_ += window_ms;
}

TickSummary TickStats::summary() const {
    TickSummary out;
    out.rtf = window_ms_ > 0.0 ? decode_ms_ / window_ms_ : 0.0;
    const auto count = static_cast<double>(ticks_);
    out.decode_ms = static_cast<std::int64_t>(std::llround(decode_ms_ / count));
    out.window_ms = static_cast<std::int64_t>(std::llround(window_ms_ / count));
    return out;
}

void TickStats::reset() noexcept {
    ticks_ = 0;
    decode_ms_ = 0.0;
    window_ms_ = 0.0;
}

std::string stats_json(const TickSummary& summary, const std::string& backend) {
    // rtf keeps four decimals, which is enough to compare runs and reads well in logs.
    const double rtf = std::round(summary.rtf * 10000.0) / 10000.0;
    const nlohmann::ordered_json line = {{"rtf", rtf},
                                         {"decode_ms", summary.decode_ms},
                                         {"window_ms", summary.window_ms},
                                         {"backend", backend}};
    return line.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

}  // namespace echoir
