// FireRedVAD as an IVad: the Kaldi front end, the DFSMN network on ggml, and the post-processor
// that turns frame probabilities into speech ranges.
#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "stream/vad_postprocess.h"
#include "vad/firered_model.h"
#include "vad/range.h"
#include "vad/vad.h"
#include "vad/vad_overrides.h"

namespace echoir::vad {

class FireRedVad : public IVad {
public:
    // Post-processor settings from the upstream README defaults, except that gaps under 500 ms are
    // merged (see default_params). Stream-VAD uses a minimum speech run of 8 frames instead of 20.
    // Streaming use never splits long speech (see VadUse).
    static stream::VadPostprocessParams default_params(bool stream_variant,
                                                       VadUse use = VadUse::offline);

    // Output frames per model call (about 30 s), not counting the context read on each side. The
    // model's buffers grow with the frames in one call and stay at the largest size seen, so a
    // longer span is run in parts of this size.
    static constexpr std::size_t kPartFrames = 3000;

    // Loads the GGUF model at model and uses default_params for its variant and use, with the
    // overrides applied. The overrides name the fields of stream::VadPostprocessParams, and
    // threshold is an alias of prob_threshold. Throws echoir::ModelLoadError if the file is
    // missing, unreadable, truncated, or not a FireRedVAD model, std::invalid_argument if
    // n_threads < 1, an override is unknown or bad, or max_speech_frame < 1, and
    // std::runtime_error if ggml setup fails.
    explicit FireRedVad(const std::filesystem::path& model, int n_threads = 2,
                        const VadOverrides& overrides = {}, VadUse use = VadUse::offline);
    // Like the constructor above, but the post-processor uses params as given, with no defaults or
    // overrides. Also throws std::invalid_argument if params.max_speech_frame < 1.
    FireRedVad(const std::filesystem::path& model, const stream::VadPostprocessParams& params,
               int n_threads = 2);
    ~FireRedVad() = default;
    FireRedVad(FireRedVad&&) noexcept = default;
    FireRedVad& operator=(FireRedVad&&) noexcept = default;
    FireRedVad(const FireRedVad&) = delete;
    FireRedVad& operator=(const FireRedVad&) = delete;

    // Speech ranges in 16 kHz mono f32 PCM. Empty input, input shorter than one 25 ms window, and
    // silence all give an empty vector. Throws std::runtime_error if ggml fails. One call at a
    // time per instance.
    std::vector<Range> detect(std::span<const float> pcm16k_mono) override;

    // Speech probability of each 10 ms frame, before post-processing. Frame i covers samples
    // [160 i, 160 i + 400), so a span of n samples has 1 + (n - 400) / 160 frames, or none when
    // n < 400. Long spans are run in parts, so memory does not grow with the span. The result
    // matches a single model call over the whole span to within float rounding.
    std::vector<float> speech_probs(std::span<const float> pcm16k_mono);

    // "firered" for VAD, and "firered-stream" for Stream-VAD, which has no lookahead.
    std::string name() const override;

    // The post-processor settings in use.
    const stream::VadPostprocessParams& params() const { return params_; }

private:
    FireRedModel model_;
    stream::VadPostprocessParams params_;
    stream::VadPostprocessor post_;
};

}  // namespace echoir::vad
