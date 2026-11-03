// `echoir bench`: transcribes one WAV file with each chosen model, scores each transcript against a
// reference text and prints a table with one row per run. With --stream, each run goes through a
// streaming session at the audio's own pace, and the table adds the latency of the text.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "offline.h"

namespace echoir::cli {

struct BenchOptions {
    std::string input;            // WAV file, at any rate and channel count
    std::string ref;              // reference text file, UTF-8
    std::string backend = "all";  // "whisper", "qwen" or "all": which default models run
    std::string model;            // registry id; when set, only this model runs
    RunSettings run;
    bool stream = false;  // run through a streaming session and measure its latency
    std::string policy;   // with stream: "agreement" or "rollback"; empty takes the default
    // With stream: run this many sessions at once on one loaded model and report each one, with
    // its backlog. 0 runs one session per model with the plain streaming table.
    int streams = 0;
};

// Runs each model and prints a table with one row per run, as each run finishes. The rate is WER,
// or CER for CJK text. The real-time factor is the wall time of VAD and decoding over the audio
// time. The model load time is shown in its own column and left out of the factor. Without --model,
// each backend's default model runs when its files and its default VAD are on disk. A default model
// that is missing is skipped with a note on stderr. With stream, a run pushes the audio at its own
// speed and the table shows the first partial and the final of each segment, at the 50th and 95th
// percentile. With streams, each model runs that many sessions at once, each fed the whole file
// from the same moment, and the table has one row per session with its wait, its backlog and a
// verdict. Throws std::exception when no model runs or a run fails.
void run_bench(const BenchOptions& options, const std::vector<std::filesystem::path>& model_dirs);

}  // namespace echoir::cli
