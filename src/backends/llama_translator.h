// A translation model through llama.cpp. Each translation is independent: the memory is cleared
// before the prompt, and the output is greedy, so the same line gives the same text.
#pragma once

#include <memory>
#include <string>

#include "backends/translator_backend.h"

struct llama_context;
struct llama_model;

namespace echoir {

class LlamaTranslator final : public translate::ITranslator {
public:
    // Loads the GGUF at config.model_path with every layer on the GPU backend when use_gpu is set,
    // creates its context, and warms up. Throws ModelLoadError when the file does not load.
    explicit LlamaTranslator(const TranslatorConfig& config);
    ~LlamaTranslator() override;
    LlamaTranslator(const LlamaTranslator&) = delete;
    LlamaTranslator& operator=(const LlamaTranslator&) = delete;

    translate::Result translate(const translate::Request& request) override;
    std::string name() const override;

private:
    // The model is declared first and freed last, because the context uses it.
    std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
    std::unique_ptr<llama_context, void (*)(llama_context*)> ctx_;
    translate::PromptStyle style_;
};

}  // namespace echoir
