// Silero VAD through whisper.cpp's whisper_vad_* API.
#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "vad/vad.h"
#include "vad/vad_overrides.h"

// Declared at global scope, as whisper.h does, so this header does not need whisper.h.
struct whisper_vad_context;

namespace echoir::vad {

// The settings that overrides may change. The defaults are the ones used without overrides.
struct SileroSettings {
    float threshold = 0.2f;     // speech probability needed to start a segment
    int min_silence_ms = 1500;  // silence that ends a segment
    int speech_pad_ms = 500;    // audio kept on each side of a segment
};

class SileroVad : public IVad {
public:
    // Loads the model. Throws echoir::ModelLoadError (a std::runtime_error) if the file is missing,
    // unreadable, truncated, or not a VAD model (including a header whose sizes whisper.cpp cannot
    // run), and std::invalid_argument if n_threads < 1. The overrides name threshold,
    // min_silence_ms and speech_pad_ms. An unknown key or a bad value throws std::invalid_argument
    // before the model is read. A streaming use does not split long speech (see VadUse).
    explicit SileroVad(const std::filesystem::path& model, int n_threads = 2,
                       const VadOverrides& overrides = {}, VadUse use = VadUse::offline);
    ~SileroVad() = default;
    SileroVad(SileroVad&&) noexcept = default;
    SileroVad& operator=(SileroVad&&) noexcept = default;
    SileroVad(const SileroVad&) = delete;
    SileroVad& operator=(const SileroVad&) = delete;

    // Speech ranges in 16 kHz mono f32 PCM. Empty input and silence both give an empty vector.
    // Throws std::runtime_error if whisper.cpp rejects the input, and std::invalid_argument if the
    // span is longer than INT_MAX samples. A compute error inside whisper.cpp is only logged, so
    // this call cannot see it: the ranges can then come from stale or zero probabilities.
    // One call at a time per instance.
    std::vector<Range> detect(std::span<const float> pcm16k_mono) override;
    std::string name() const override { return "silero"; }

private:
    struct Deleter {
        void operator()(whisper_vad_context* ctx) const noexcept;
    };
    std::unique_ptr<whisper_vad_context, Deleter> ctx_;
    SileroSettings settings_;
    VadUse use_ = VadUse::offline;
};

}  // namespace echoir::vad
