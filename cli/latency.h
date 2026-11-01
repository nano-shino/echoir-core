// Latency of streamed text, measured by `echoir bench --stream`. The functions take the times of
// the audio pushes and of the text events, so they need no model and can be tested on their own.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace echoir::cli {

// The value at percentile p (0 to 100) by the nearest-rank method: the smallest value that at
// least p percent of the values do not exceed. Empty when there are no values. Throws
// std::invalid_argument when p is outside [0, 100].
std::optional<double> percentile(std::vector<double> values, double p);

// A push of audio to the session: the audio up to audio_end_ms (ms from the start of the input) was
// in the session from at_ms on (ms from the start of the run). Pushes are in input order.
struct PushTime {
    std::int64_t audio_end_ms = 0;
    double at_ms = 0.0;
};

// A text event: the segment it belongs to, the end of the audio its text covers (ms from the start
// of the input), and when it was delivered (ms from the start of the run).
struct TextTime {
    std::int64_t segment_id = 0;
    std::int64_t end_ms = 0;
    double at_ms = 0.0;
};

// Per-segment latencies in ms, one value per segment. The latency of a text is the delay from
// the moment the audio up to the text's end reached the session to the moment the text was
// delivered. The first partial of a segment and its final are measured. Pass the events in
// delivery order, with only the texts that are not empty.
struct SegmentLatencies {
    std::vector<double> first_partial_ms;
    std::vector<double> final_ms;
};

SegmentLatencies segment_latencies(const std::vector<PushTime>& pushes,
                                   const std::vector<TextTime>& partials,
                                   const std::vector<TextTime>& finals);

}  // namespace echoir::cli
