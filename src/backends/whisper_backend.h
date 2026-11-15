// whisper.cpp backend that uses the Whisper decode parameters of the streaming engine.
#pragma once

#include <string>
#include <string_view>

#include "backends/asr_backend.h"

struct whisper_context;

namespace echoir {

class WhisperBackend final : public IAsrBackend {
public:
    // Loads the ggml Whisper model at model_path. Throws echoir::ModelLoadError (a
    // std::runtime_error) if the file is missing or unreadable, is not a Whisper model, or does not
    // load cleanly. The header is checked before whisper.cpp reads the file, because bad headers
    // abort in ggml.
    WhisperBackend(const std::string& model_path, bool use_gpu, int n_threads);
    ~WhisperBackend() override;
    WhisperBackend(const WhisperBackend&) = delete;
    WhisperBackend& operator=(const WhisperBackend&) = delete;

    DecodeResult decode(const DecodeRequest& request) override;
    std::string name() const override;
    // Throws std::invalid_argument for a context longer than the whole prompt (223 tokens on
    // the models in the registry).
    void check_context(std::string_view context) const override;

private:
    whisper_context* ctx_ = nullptr;
    int n_threads_;
    bool multilingual_ = false;
};

}  // namespace echoir
