// Speed of the decoding ticks of a session, summed over a run of ticks for a STATS event.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace echoir {

// The figures of a STATS event, over the ticks added since the last reset.
struct TickSummary {
    double rtf = 0.0;                // decode time over the audio time of the windows
    std::int64_t decode_ms = 0;      // mean decode time per tick
    std::int64_t window_ms = 0;      // mean audio length per tick, carry included
    std::int64_t ticks = 0;          // ticks covered
    std::int64_t new_ms = 0;         // mean length of the audio a tick took in, carry excluded
    std::int64_t speech_ms = 0;      // mean length of the speech ranges a tick decoded
    std::int64_t decode_max_ms = 0;  // the slowest tick
    // Decode time over the new audio: the share of real time the decoder is busy. At 1 or more the
    // session cannot keep up, and the audio waiting to be decoded grows.
    double load = 0.0;
    // The load over the ticks that decoded speech only, so silence does not make a model look
    // faster than it is on speech. 0 when no tick decoded speech.
    double speech_load = 0.0;
};

// Accumulates tick timings. Not thread-safe: the session adds ticks under its decode lock.
class TickStats {
public:
    // Adds one tick that decoded a window of window_ms of audio in decode_ms of wall time. Of the
    // window, new_ms was audio the tick took in (the rest was carried), and speech_ms is the length
    // of the speech ranges it decoded.
    void add(double decode_ms, double window_ms, double new_ms = 0.0,
             double speech_ms = 0.0) noexcept;

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
    double new_ms_ = 0.0;
    double speech_ms_ = 0.0;
    double decode_max_ms_ = 0.0;
    double speech_decode_ms_ = 0.0;  // decode time of the ticks that decoded speech
    double speech_new_ms_ = 0.0;     // new audio of the ticks that decoded speech
};

// The detail of a STATS event: one JSON object on one line, with rtf, decode_ms, window_ms, ticks,
// new_ms, speech_ms, decode_max_ms, load, speech_load and backend. The backend is a registry-style
// name such as "whisper".
std::string stats_json(const TickSummary& summary, const std::string& backend);

}  // namespace echoir
