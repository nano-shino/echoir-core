// The level envelope of an audio timeline: one min/max pair of 16-bit samples per 10 ms cell, for
// hosts that draw a waveform or show an input level. It costs 4 bytes per cell (about 1.4 MB per
// hour), so it covers the whole timeline. Not thread-safe: the owner locks around it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace echoir::stream {

// A sample in full scale ±1 as a 16-bit sample, rounded and clamped.
inline std::int16_t to_s16(float sample) {
    const float scaled = std::round(sample * 32768.0f);
    return static_cast<std::int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
}

class Envelope {
public:
    // The envelope keeps one min/max pair per this many ms.
    static constexpr std::int64_t kCellMs = 10;

    // Appends samples at the end of the timeline.
    void append(std::span<const std::int16_t> samples);
    // Appends n samples of silence.
    void append_silence(std::int64_t n);
    // Empties the envelope, so the timeline restarts at 0.
    void clear();

    // The number of whole cells.
    std::int64_t cells() const noexcept { return static_cast<std::int64_t>(cells_.size() / 2); }

    // Writes one min/max pair (full scale ±1) per bucket of bucket_ms, a multiple of kCellMs, over
    // [start_ms, end_ms), with end_ms clamped to the newest whole cell, or to limit_cells cells
    // when that is fewer, and returns the number of buckets. out holds cap buckets of two floats,
    // min then max; with out null, only counts them. The last bucket may cover fewer cells. Throws
    // std::invalid_argument for a bucket that is not a positive multiple of kCellMs, a negative
    // start, an end before the start, or a cap too small for the range.
    std::size_t read(std::int64_t start_ms, std::int64_t end_ms, std::int64_t bucket_ms, float* out,
                     std::size_t cap, std::int64_t limit_cells = INT64_MAX) const;

private:
    // Completed cells, as min and max in 16-bit steps, two values per cell.
    std::vector<std::int16_t> cells_;
    std::int16_t cell_min_ = 0;  // the cell being filled
    std::int16_t cell_max_ = 0;
    std::int64_t cell_fill_ = 0;
};

}  // namespace echoir::stream
