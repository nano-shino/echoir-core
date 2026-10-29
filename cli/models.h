// Resolves and loads the VAD and the speech model of a transcription run.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "backends/asr_backend.h"
#include "options.h"
#include "vad/vad.h"

namespace echoir::cli {

// The loaded pieces of a run. backend is "whisper" or "qwen".
struct LoadedModels {
    std::string backend;
    std::unique_ptr<vad::IVad> vad;
    std::unique_ptr<IAsrBackend> asr;
};

// Resolves the backend, the model, the projector and the VAD from the options, then loads them.
// model_dirs is the search order for the catalogue (see model_search_dirs()). use is the VAD's
// use: streaming for a live or streamed run, offline for a whole file. Throws UsageError when the
// options do not fit together or a name is unknown, and std::exception for other failures, such
// as a model that fails to load.
LoadedModels load_models(const TranscribeOptions& options,
                         const std::vector<std::filesystem::path>& model_dirs, vad::VadUse use);

// Checks the decode request of a run against the speech model: the language and the context. The
// check uses an empty window, so a bad value fails before any audio is read, and also when the
// audio has no speech for the decoder to run on. Throws std::invalid_argument, as decode does.
void check_request(IAsrBackend& asr, const TranscribeOptions& options);

}  // namespace echoir::cli
