// make_translator: creates the translation model of a TranslatorConfig.
#include <memory>
#include <stdexcept>

#include "backends/translator_backend.h"
#include "errors.h"
#ifndef ECHOIR_FAKE_BACKEND
#ifdef ECHOIR_HAS_QWEN
#include "backends/llama_translator.h"
#endif
#endif

namespace echoir {

std::unique_ptr<translate::ITranslator> make_translator(const TranslatorConfig& config) {
#ifdef ECHOIR_FAKE_BACKEND
    // The fake build reads no model, so any config gives the scripted translator.
    (void)config;
    return fake::make_translator();
#elif defined(ECHOIR_HAS_QWEN)
    if (config.model_path.empty()) {
        throw std::invalid_argument("model_path is required");
    }
    if (config.n_threads < 0) {
        throw std::invalid_argument("n_threads must not be negative");
    }
    return std::make_unique<LlamaTranslator>(config);
#else
    (void)config;
    throw BackendUnavailable("translator not built (ECHOIR_WITH_QWEN=OFF)");
#endif
}

}  // namespace echoir
