// Qwen3-ASR backend. The text model runs in llama.cpp, and the audio encoder (the mmproj file)
// runs through libmtmd. Each decode is independent: the KV cache is cleared before it.
#pragma once

#include <memory>
#include <string>

#include "backends/asr_backend.h"

struct llama_context;
struct llama_model;
struct mtmd_context;

namespace echoir {

class QwenBackend final : public IAsrBackend {
public:
    // Loads the text GGUF at model_path and the mmproj at mmproj_path. Throws
    // echoir::ModelLoadError (a std::runtime_error) if either file is missing or does not load, or
    // if the mmproj has no 16 kHz audio encoder.
    // use_gpu offloads all layers to the GPU backend. n_threads must be at least 1.
    QwenBackend(const std::string& model_path, const std::string& mmproj_path, bool use_gpu,
                int n_threads);
    ~QwenBackend() override;
    QwenBackend(const QwenBackend&) = delete;
    QwenBackend& operator=(const QwenBackend&) = delete;

    // Decodes one window of 16 kHz mono audio. Returns the transcript without the language
    // header. Windows shorter than 100 ms and windows quieter than about -80 dBFS return empty
    // text without a decode. Throws std::invalid_argument for a language that is neither "auto"
    // nor a supported ISO code, for a window longer than 30 s, for NaN or infinite samples, for a
    // context over 512 tokens or one containing the media marker, for a prefix containing the
    // media marker, for a negative rollback_tokens, and for context, history and prefix that do
    // not fit in the context with the window and generation.
    DecodeResult decode(const DecodeRequest& request) override;
    std::string name() const override;
    bool supports_prefix() const override { return true; }
    bool history_at_cut() const override { return true; }

private:
    // Members are destroyed in reverse declaration order. The llama and mmproj contexts both use
    // the model, so the model is declared first and freed last.
    std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
    std::unique_ptr<llama_context, void (*)(llama_context*)> ctx_;
    std::unique_ptr<mtmd_context, void (*)(mtmd_context*)> mtmd_;
    int n_ctx_ = 0;
};

}  // namespace echoir
