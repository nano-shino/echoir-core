// The audio a session has taken in, for hosts that show or replay it: the newest stretch as 16-bit
// samples, and a min/max envelope of the whole timeline. Both are written from the 16 kHz mono
// audio that the session decodes, so sample n of the store is sample n of the session timeline.
// A session with a tape (stream/tape.h) keeps its audio there instead.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "stream/envelope.h"

namespace echoir::stream {

class AudioStore {
public:
    // The envelope keeps one min/max pair per this many ms.
    static constexpr std::int64_t kPeakCellMs = Envelope::kCellMs;

    // Keeps the newest retain_ms of audio. 0 keeps no audio; the envelope is kept either way.
    // Throws std::invalid_argument for a negative length.
    explicit AudioStore(std::int64_t retain_ms);

    // Appends 16 kHz mono samples (full scale ±1) at the end of the timeline. Thread-safe.
    void append(std::span<const float> pcm);
    // Empties the audio and the envelope, so the timeline restarts at 0. Thread-safe.
    void clear();

    // Copies the samples of [start_ms, end_ms), with end_ms clamped to the newest sample, and
    // returns how many there are. out holds cap samples; with out null, only counts them. Throws
    // StateError when no audio is kept, or when part of the range is no longer kept. Throws
    // std::invalid_argument for a negative start, an end before the start, or a cap too small for
    // the range. Thread-safe.
    std::size_t read_audio(std::int64_t start_ms, std::int64_t end_ms, std::int16_t* out,
                           std::size_t cap) const;

    // The envelope of [start_ms, end_ms), as Envelope::read. Thread-safe.
    std::size_t read_peaks(std::int64_t start_ms, std::int64_t end_ms, std::int64_t bucket_ms,
                           float* out, std::size_t cap) const;

private:
    mutable std::mutex mu_;
    std::vector<std::int16_t> ring_;  // the newest ring_.size() samples; sample n is at n % size
    std::int64_t total_ = 0;          // samples appended since the last clear
    Envelope envelope_;
    std::vector<std::int16_t> scratch_;  // the samples of one append, as 16-bit
};

}  // namespace echoir::stream
