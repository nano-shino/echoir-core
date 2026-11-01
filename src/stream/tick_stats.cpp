#include "stream/tick_stats.h"

#include <algorithm>
#include <cmath>

#include "json.hpp"

namespace echoir {
namespace {

std::int64_t round_ms(double ms) { return static_cast<std::int64_t>(std::llround(ms)); }

// Four decimals are enough to compare runs and read well in logs.
double round4(double value) { return std::round(value * 10000.0) / 10000.0; }

}  // namespace

void TickStats::add(double decode_ms, double window_ms, double new_ms, double speech_ms) noexcept {
    ++ticks_;
    decode_ms_ += decode_ms;
    window_ms_ += window_ms;
    new_ms_ += new_ms;
    speech_ms_ += speech_ms;
    decode_max_ms_ = std::max(decode_max_ms_, decode_ms);
    if (speech_ms > 0.0) {
        speech_decode_ms_ += decode_ms;
        speech_window_ms_ += window_ms;
    }
}

TickSummary TickStats::summary() const {
    TickSummary out;
    out.rtf = window_ms_ > 0.0 ? decode_ms_ / window_ms_ : 0.0;
    const auto count = static_cast<double>(ticks_);
    out.decode_ms = round_ms(decode_ms_ / count);
    out.window_ms = round_ms(window_ms_ / count);
    out.ticks = static_cast<std::int64_t>(ticks_);
    out.new_ms = round_ms(new_ms_ / count);
    out.speech_ms = round_ms(speech_ms_ / count);
    out.decode_max_ms = round_ms(decode_max_ms_);
    out.load = new_ms_ > 0.0 ? decode_ms_ / new_ms_ : 0.0;
    out.speech_rtf = speech_window_ms_ > 0.0 ? speech_decode_ms_ / speech_window_ms_ : 0.0;
    return out;
}

void TickStats::reset() noexcept { *this = TickStats{}; }

std::string stats_json(const TickSummary& summary, const std::string& backend) {
    const nlohmann::ordered_json line = {{"rtf", round4(summary.rtf)},
                                         {"decode_ms", summary.decode_ms},
                                         {"window_ms", summary.window_ms},
                                         {"ticks", summary.ticks},
                                         {"new_ms", summary.new_ms},
                                         {"speech_ms", summary.speech_ms},
                                         {"decode_max_ms", summary.decode_max_ms},
                                         {"load", round4(summary.load)},
                                         {"speech_rtf", round4(summary.speech_rtf)},
                                         {"backend", backend}};
    return line.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

}  // namespace echoir
