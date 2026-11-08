// The translation model behind echoir_translator: a text model run through llama.cpp. Like the
// speech backends, it keeps its own llama.cpp context and takes no engine's gate.
#pragma once

#include <memory>
#include <string>

#include "translate/prompt.h"
#include "translate/translator.h"

namespace echoir {

struct TranslatorConfig {
    std::string model_path;
    translate::PromptStyle style = translate::PromptStyle::hy_mt2_1_8b;
    bool use_gpu = true;
    int n_threads = 0;  // 0 = the default
};

// Loads the model and runs a few short translations so the first real call is not slow. Throws
// ModelLoadError when the model does not load, BackendUnavailable when llama.cpp is not built in,
// and std::invalid_argument for a negative n_threads. The fake build returns a scripted
// translator that reads no file.
std::unique_ptr<translate::ITranslator> make_translator(const TranslatorConfig& config);

namespace fake {
// "[<target>] <text>": reads no model, and takes ECHOIR_FAKE_TRANSLATE_DELAY_MS milliseconds per
// call when that is set (clamped to 0..10000).
std::unique_ptr<translate::ITranslator> make_translator();
}  // namespace fake

}  // namespace echoir
