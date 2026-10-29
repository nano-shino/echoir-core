// Voice activity detection over 16 kHz mono f32 PCM. make_vad picks the implementation from the
// model file's format.
#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "vad/range.h"
#include "vad/vad_overrides.h"

namespace echoir::vad {

// Where the detector runs. A streaming session cuts a segment at its own length limit, so a
// streaming detector does not split long speech. Offline, the detector's own split is kept.
enum class VadUse { offline, streaming };

class IVad {
public:
    virtual ~IVad() = default;
    IVad(const IVad&) = delete;
    IVad& operator=(const IVad&) = delete;

    // Speech ranges of the span, in ms from its start. Empty input and silence both give an empty
    // vector. Throws std::runtime_error if the detector fails, and std::invalid_argument if the
    // span is too long for it. One call at a time per instance.
    virtual std::vector<Range> detect(std::span<const float> pcm16k_mono) = 0;
    // Short model name: "silero", "firered" or "firered-stream".
    virtual std::string name() const = 0;

protected:
    // Implementations own a loaded model, so they move and never copy.
    IVad() = default;
    IVad(IVad&&) = default;
    IVad& operator=(IVad&&) = default;
};

// Loads the VAD model at model and returns the detector for its format: a Silero ggml .bin, or a
// FireRedVAD .gguf. The caller owns the result. Throws echoir::ModelLoadError if the file is
// missing, unreadable, truncated, or not a VAD model of either format.
// The overrides change the detector's settings (see SileroVad and FireRedVad). A key that the
// model's VAD does not have, or a bad value, throws std::invalid_argument.
// use picks the streaming or offline settings (see VadUse).
// Throws std::invalid_argument if n_threads < 1.
std::unique_ptr<IVad> make_vad(const std::filesystem::path& model, int n_threads = 2,
                               const VadOverrides& overrides = {},
                               VadUse use = VadUse::offline);

}  // namespace echoir::vad
