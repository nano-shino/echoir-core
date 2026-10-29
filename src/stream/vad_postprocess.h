// Turns per-frame speech probabilities from FireRedVAD into speech ranges.
// Port of fireredvad/core/vad_postprocessor.py (Apache-2.0, FireRedTeam/FireRedVAD).
// The stages keep upstream's order, and the double arithmetic that changes a result matches it.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "vad/range.h"

namespace echoir::stream {

// Frames are 10 ms apart. The field names match upstream's config. The defaults are its README
// values; the streaming model uses min_speech_frame 8.
struct VadPostprocessParams {
    int smooth_window_size = 5;   // trailing moving average over this many frames; <= 1 disables it
    double prob_threshold = 0.4;  // a smoothed probability >= this counts as speech
    int min_speech_frame = 20;    // speech shorter than this is dropped
    int max_speech_frame = 2000;  // longer speech is split at its quietest frame (20 s)
    int min_silence_frame = 20;   // a silence dip shorter than this inside speech is bridged
    int merge_silence_frame = 0;  // a silence gap shorter than this between speech is filled
    int extend_speech_frame = 0;  // speech grows by this many frames on each side
};

// Stateless after construction: the methods are const, so one instance can be shared between
// threads. Results are returned by value.
class VadPostprocessor {
public:
    // Throws std::invalid_argument if max_speech_frame < 1, since splitting needs a window.
    explicit VadPostprocessor(const VadPostprocessParams& params);

    // One 0 (silence) or 1 (speech) per frame. The length is probs.size(), except when
    // extend_speech_frame makes the window 2 * extend_speech_frame + 1 longer than the input. Then
    // it is the window's length, as upstream returns.
    std::vector<std::uint8_t> process(std::span<const float> probs) const;

    // Speech ranges for frame probabilities of a clip duration_s seconds long. Frame i starts at
    // 10 ms * i, so the probabilities should cover no more than the clip. Ends are capped at
    // duration_s (in whole ms), and a range left empty by the cap is dropped. Throws
    // std::invalid_argument if duration_s is negative or not finite.
    std::vector<vad::Range> ranges(std::span<const float> probs, double duration_s) const;

private:
    VadPostprocessParams params_;
};

}  // namespace echoir::stream
