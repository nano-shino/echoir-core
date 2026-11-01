// Speed of the decoding ticks of a session, summed over a run of ticks for a STATS event.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace echoir {

// The figures of a STATS event, over the ticks added since the last reset.
struct TickSummary {
    double rtf = 0.0;            // decode time over the audio time of the windows
    std::int64_t decode_ms = 0;  // mean decode time per tick
    std::int64_t window_ms = 0;  // mean audio length per tick, carry included
};

// Accumulates tick timings. Not thread-safe: the session adds ticks under its decode lock.
class TickStats {
public:
    // Adds one tick that decoded a window of window_ms of audio in decode_ms of wall time.
    void add(double decode_ms, double window_ms) noexcept;

    // Ticks added since the last reset.
    std::size_t ticks() const noexcept { return ticks_; }

    // The figures of the ticks added so far. Requires ticks() > 0.
    TickSummary summary() const;

    // Forgets the ticks, so the next summary covers only the ticks added after this call.
    void reset() noexcept;

private:
    std::size_t ticks_ = 0;
    double decode_ms_ = 0.0;
    double window_ms_ = 0.0;
};

// The detail of a STATS event: one JSON object on one line, with rtf, decode_ms, window_ms and
// backend. The backend is a registry-style name such as "whisper".
std::string stats_json(const TickSummary& summary, const std::string& backend);

}  // namespace echoir
