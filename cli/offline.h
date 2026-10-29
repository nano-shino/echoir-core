// Offline transcription. The VAD finds the speech ranges of the whole file, and each range is
// decoded on its own.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "options.h"

namespace echoir::cli {

// One decoded speech piece. Times are ms from the start of the input.
struct Segment {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string text;  // cleaned UTF-8, never empty
};

// What a run took. The load covers loading the VAD and the model. The wall time covers the VAD and
// decoding. The real-time factor is wall time over audio time.
struct OfflineStats {
    double audio_s = 0.0;
    double load_s = 0.0;
    double wall_s = 0.0;
};

struct OfflineResult {
    std::string backend;  // "whisper" or "qwen", as resolved from --backend or the model
    std::vector<Segment> segments;
    OfflineStats stats;
};

// Called with each segment as soon as it is decoded, so a long file shows its text while it runs.
using SegmentSink = std::function<void(const Segment&)>;

// Transcribes the whole input. A range longer than 20 s is cut at quiet points first, and each
// piece is decoded on its own. Pieces that come out empty are skipped. Throws std::exception, with
// a message for the user, when a model, the input or a decode fails. model_dirs is the search order
// for the registry (see model_search_dirs()).
OfflineResult transcribe_offline(const TranscribeOptions& options,
                                 const std::vector<std::filesystem::path>& model_dirs,
                                 const SegmentSink& on_segment = {});

}  // namespace echoir::cli
