// The settings of a transcription run, shared by the offline path, the stream path and bench.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "input.h"

namespace echoir::cli {

// A combination of options that cannot work, found while the run is set up. The command reports it
// as a usage error, which exits with the usage status rather than the failure status.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The settings that `transcribe` and `bench` share.
struct RunSettings {
    std::string vad;                // registry id or file; empty takes the backend's default
    std::string language = "auto";  // "auto" or an ISO code
    int threads = 0;                // 0 = default
    bool use_gpu = true;
};

// Options of `echoir transcribe`. A model is a registry id or a file path.
struct TranscribeOptions {
    // Where the audio comes from: a WAV file, standard input (a WAV, or raw samples when raw_pcm
    // is set), or a capture device, which is live and needs --stream.
    InputSpec input;
    // Raw samples on standard input, from --pcm, --rate and --channels. Empty for WAV input.
    std::optional<RawPcmFormat> raw_pcm;
    std::string backend;  // "whisper" or "qwen"; empty takes the model's registry backend
    std::string model;    // registry id or model file
    // Qwen projector: a registry id or a file. Empty takes the model's registry projector.
    std::string mmproj;
    RunSettings run;
    // User text sent with every decode, such as proper names. From --context or --context-file,
    // already folded to one line. Empty for none.
    std::string context;
    // Streaming only: "agreement" or "rollback". Empty takes the backend default: rollback on
    // Qwen3-ASR, agreement on Whisper.
    std::string policy;
    // Streaming only: the audio that must be pending before a decode runs, in ms.
    int min_chunk_ms = 500;
    // Streaming only: the length, in ms, at which an ongoing segment is cut at a quiet point.
    int max_segment_ms = 10000;
    // Capture only: stop after this many seconds. 0 runs until Ctrl-C.
    double duration_seconds = 0.0;
    // Development only: "key=value" settings for the VAD. See vad::VadOverrides.
    std::vector<std::string> vad_params;
};

}  // namespace echoir::cli
