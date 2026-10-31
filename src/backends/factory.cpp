// make_backend: creates the backend that echoir_engine_params names.
#include <memory>
#include <stdexcept>

#include "backends/asr_backend.h"
#include "backends/backend_loader.h"
#include "backends/whisper_backend.h"
#include "errors.h"
#ifdef ECHOIR_FAKE_BACKEND
#include "fake/fake.h"
#endif
#ifdef ECHOIR_HAS_QWEN
#include "backends/qwen_backend.h"
#endif

namespace echoir {
namespace {

// n_threads 0 selects this default.
constexpr int kDefaultThreads = 4;

}  // namespace

std::unique_ptr<IAsrBackend> make_backend(const echoir_engine_params& params) {
#ifdef ECHOIR_FAKE_BACKEND
    // The fake build reads no model, so any params give the scripted backend.
    (void)params;
    return fake::make_backend();
#else
    if (params.model_path == nullptr || params.model_path[0] == '\0') {
        throw std::invalid_argument("model_path is required");
    }
    if (params.n_threads < 0) {
        throw std::invalid_argument("n_threads must not be negative");
    }
    const int n_threads = params.n_threads == 0 ? kDefaultThreads : params.n_threads;
    backends::load_backends();

    switch (params.backend) {
        case ECHOIR_BACKEND_WHISPER:
            return std::make_unique<WhisperBackend>(params.model_path, params.use_gpu != 0,
                                                    n_threads);
        case ECHOIR_BACKEND_QWEN3_ASR:
#ifdef ECHOIR_HAS_QWEN
            // The audio encoder lives in a separate mmproj file, so Qwen needs both paths.
            if (params.mmproj_path == nullptr || params.mmproj_path[0] == '\0') {
                throw std::invalid_argument("mmproj_path is required for qwen3-asr");
            }
            return std::make_unique<QwenBackend>(params.model_path, params.mmproj_path,
                                                 params.use_gpu != 0, n_threads);
#else
            throw BackendUnavailable("qwen backend not built (ECHOIR_WITH_QWEN=OFF)");
#endif
    }
    throw std::invalid_argument("unknown backend");
#endif
}

}  // namespace echoir
