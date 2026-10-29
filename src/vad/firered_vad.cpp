#include "vad/firered_vad.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

#include "audio/fbank.h"

namespace echoir::vad {
namespace {

// Audio is 16 kHz. A frame is 10 ms, and each frame reads a 25 ms window of 400 samples.
constexpr double kSampleRate = 16000.0;
constexpr std::size_t kHopSamples = 160;
constexpr std::size_t kWindowSamples = 400;

// Applies the overrides to the post-processor settings. The keys are the fields of
// VadPostprocessParams, and threshold is a shorter name for prob_threshold.
stream::VadPostprocessParams with_overrides(stream::VadPostprocessParams params,
                                            const VadOverrides& overrides) {
    if (overrides.values.count("prob_threshold") != 0 && overrides.values.count("threshold") != 0) {
        throw std::invalid_argument("give prob_threshold or threshold, not both");
    }
    for (const auto& [key, value] : overrides.values) {
        if (key == "prob_threshold" || key == "threshold") {
            params.prob_threshold = parse_probability(key, value);
        } else if (key == "smooth_window_size") {
            params.smooth_window_size = parse_count(key, value);
        } else if (key == "min_speech_frame") {
            params.min_speech_frame = parse_count(key, value);
        } else if (key == "max_speech_frame") {
            params.max_speech_frame = parse_count(key, value);
        } else if (key == "min_silence_frame") {
            params.min_silence_frame = parse_count(key, value);
        } else if (key == "merge_silence_frame") {
            params.merge_silence_frame = parse_count(key, value);
        } else if (key == "extend_speech_frame") {
            params.extend_speech_frame = parse_count(key, value);
        } else {
            throw std::invalid_argument(
                "unknown setting for the FireRedVAD: " + key +
                " (use smooth_window_size, prob_threshold, min_speech_frame, max_speech_frame, "
                "min_silence_frame, merge_silence_frame or extend_speech_frame)");
        }
    }
    return params;
}

}  // namespace

stream::VadPostprocessParams FireRedVad::default_params(bool stream_variant, VadUse use) {
    stream::VadPostprocessParams params;
    params.smooth_window_size = 5;
    params.prob_threshold = 0.4;
    params.min_speech_frame = stream_variant ? 8 : 20;
    // Offline speech longer than 20 s is split. A streaming session cuts at its own limit instead,
    // so the split is out of reach there.
    params.max_speech_frame = use == VadUse::streaming ? std::numeric_limits<int>::max() : 2000;
    params.min_silence_frame = 20;
    // Gaps under 500 ms are filled, so a comma pause does not split a sentence into separate
    // decodes. The upstream README uses 0.
    params.merge_silence_frame = 50;
    params.extend_speech_frame = 0;
    return params;
}

// The defaults depend on the variant, which the model file tells, so they are taken after the
// model loads. model_ is declared before params_, so it is loaded when params_ is set.
FireRedVad::FireRedVad(const std::filesystem::path& model, int n_threads,
                       const VadOverrides& overrides, VadUse use)
    : model_(model, n_threads),
      params_(with_overrides(default_params(model_.hparams().N2 == 0, use), overrides)),
      post_(params_) {}

FireRedVad::FireRedVad(const std::filesystem::path& model,
                       const stream::VadPostprocessParams& params, int n_threads)
    : model_(model, n_threads), params_(params), post_(params_) {}

std::vector<Range> FireRedVad::detect(std::span<const float> pcm16k_mono) {
    const std::vector<float> probs = speech_probs(pcm16k_mono);
    const double duration_s = static_cast<double>(pcm16k_mono.size()) / kSampleRate;
    return post_.ranges(probs, duration_s);
}

std::vector<float> FireRedVad::speech_probs(std::span<const float> pcm16k_mono) {
    const std::size_t samples = pcm16k_mono.size();
    if (samples < kWindowSamples) {
        return {};
    }
    const std::size_t frames = 1 + (samples - kWindowSamples) / kHopSamples;

    // Output at frame t depends on frames [t - before, t + after] only. Before is the look-back of
    // every FSMN layer, and after is the model's lookahead. A part keeps exactly that much on each
    // side (or up to the clip edge), so its core frames equal those of a whole-clip call.
    const FireRedHParams& hp = model_.hparams();
    const auto look_back = static_cast<std::size_t>(hp.R) * static_cast<std::size_t>(hp.N1 - 1) *
                           static_cast<std::size_t>(hp.S1);
    const auto look_ahead = static_cast<std::size_t>(model_.lookahead());

    // Each part also reads that context on either side, so the largest call is about 3300 frames.
    std::vector<float> probs;
    probs.reserve(frames);
    for (std::size_t core_begin = 0; core_begin < frames; core_begin += kPartFrames) {
        const std::size_t core_end = std::min(frames, core_begin + kPartFrames);
        const std::size_t part_begin = core_begin > look_back ? core_begin - look_back : 0;
        const std::size_t part_end = std::min(frames, core_end + look_ahead);

        // The features of frames [part_begin, part_end) depend only on their own samples, so the
        // part's audio is computed alone. A part of k frames needs 400 + 160 (k - 1) samples.
        const std::size_t first_sample = part_begin * kHopSamples;
        const std::size_t end_sample = (part_end - 1) * kHopSamples + kWindowSamples;
        std::vector<float> feats =
            audio::kaldi_fbank(pcm16k_mono.subspan(first_sample, end_sample - first_sample));
        audio::apply_cmvn(feats, model_.cmvn_mean(), model_.cmvn_istd());
        const std::vector<float> part = model_.probs(feats, part_end - part_begin);

        const auto skip = static_cast<std::ptrdiff_t>(core_begin - part_begin);
        const auto take = static_cast<std::ptrdiff_t>(core_end - part_begin);
        probs.insert(probs.end(), part.begin() + skip, part.begin() + take);
    }
    return probs;
}

std::string FireRedVad::name() const {
    return model_.hparams().N2 > 0 ? "firered" : "firered-stream";
}

}  // namespace echoir::vad
