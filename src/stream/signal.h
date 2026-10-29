#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace echoir::stream {

// Audio is 16 kHz mono f32 throughout the core, so a millisecond is 16 samples.
inline constexpr std::int64_t kSamplesPerMs = 16;

// Window that find_quietest_point() slides over the audio: 100 ms at 16 kHz.
inline constexpr std::size_t kQuietBlock = 1600;

// How far before a cut limit the quiet point is searched. Offline pieces use 8 s at 16 kHz, and
// streaming uses 3 s, so a segment that reaches its limit is finalized sooner.
inline constexpr std::size_t kCutSearch = 128000;
inline constexpr std::size_t kCutSearchStream = 48000;

// Finds the quietest stretch of [lo, hi) to cut at. Slides a kQuietBlock window over the range
// and returns the centre of the block with the lowest energy. Input is 16 kHz mono f32.
//
// Boundary conventions:
//  - hi is exclusive and a block must fit inside it, so block starts run over
//    [lo, hi - kQuietBlock].
//  - The result is block_start + kQuietBlock / 2. The true midpoint is block_start + 799.5, so
//    the result is the sample just past it.
//  - Ties keep the earliest block (strict <). A silent gap therefore yields its first block.
//
// Throws std::invalid_argument unless lo <= hi <= samples.size() and hi - lo >= kQuietBlock.
std::size_t find_quietest_point(std::span<const float> samples, std::size_t lo, std::size_t hi);

// The cut for a segment that starts at `start` and ends at `end`, and must end by `cap` at the
// latest. The cut is the quiet point of find_quietest_point() over
// [max(start, cap - search), min(end - kQuietBlock, cap)). search is kCutSearch offline and
// kCutSearchStream when streaming. Both the streaming session and the offline path cut long
// ranges this way. Throws like find_quietest_point() when the range has no room for a quiet
// block. Both callers only ask for cuts that have it.
std::size_t find_cut(std::span<const float> samples, std::size_t start, std::size_t cap,
                     std::size_t end, std::size_t search);

// Root mean square of the samples. Returns 0 for an empty span.
double rms(std::span<const float> samples) noexcept;

}  // namespace echoir::stream
