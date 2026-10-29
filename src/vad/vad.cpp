#include "vad/vad.h"

#include <array>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "backends/ggml_header.h"
#include "errors.h"
#ifdef ECHOIR_FAKE_BACKEND
#include "fake/fake.h"
#endif
#include "gguf.h"
#include "vad/firered_vad.h"
#include "vad/silero_vad.h"

namespace echoir::vad {

std::unique_ptr<IVad> make_vad(const std::filesystem::path& model, int n_threads,
                               const VadOverrides& overrides, VadUse use) {
#ifdef ECHOIR_FAKE_BACKEND
    // The fake build reads no model, so any path gives the scripted VAD.
    (void)model;
    (void)n_threads;
    (void)overrides;
    (void)use;
    return fake::make_vad();
#else
    if (n_threads < 1) {
        throw std::invalid_argument("make_vad: n_threads must be at least 1");
    }
    // Checked before the open, because opening a named pipe with no writer blocks.
    if (!backends::is_regular_model_file(model)) {
        throw ModelLoadError("VAD model not found: " + model.string());
    }
    std::ifstream in(model, std::ios::binary);
    if (!in) {
        throw ModelLoadError("cannot read VAD model: " + model.string());
    }

    std::array<char, 4> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    const bool has_magic = in.gcount() == static_cast<std::streamsize>(magic.size());
    // A GGUF file is a FireRedVAD model. FireRedModel rejects a GGUF of any other architecture.
    if (has_magic && std::string_view(magic.data(), magic.size()) == GGUF_MAGIC) {
        return std::make_unique<FireRedVad>(model, n_threads, overrides, use);
    }

    // Any other file must be a Silero ggml model. The header check names the file when the magic
    // is wrong.
    in.clear();
    in.seekg(0);
    backends::GgmlHeader header(in, model.string(), "VAD");
    header.expect_magic();
    return std::make_unique<SileroVad>(model, n_threads, overrides, use);
#endif
}

}  // namespace echoir::vad
