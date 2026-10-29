// The FireRedVAD network (a DFSMN) on the ggml CPU backend. It loads a GGUF file written by
// tools/convert-firered-vad.py and gives the speech probability of every frame of a clip.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace echoir::vad {

// Hyper-parameters stored in the file under firered-vad.*. Names match the upstream code.
struct FireRedHParams {
    int idim = 0;  // input features per frame, always 80
    int R = 0;     // FSMN layers: the first one, then R - 1 DFSMN blocks
    int M = 0;     // DNN layers after the blocks
    int H = 0;     // hidden size
    int P = 0;     // projection size
    int N1 = 0;    // lookback taps per FSMN
    int S1 = 0;    // lookback stride
    int N2 = 0;    // lookahead taps per FSMN; 0 for Stream-VAD
    int S2 = 0;    // lookahead stride
    int odim = 0;  // outputs, always 1
};

// The network and its ggml backend state. A model is used by one thread at a time.
class FireRedModel {
public:
    // Loads the model at model. Throws echoir::ModelLoadError if the file is missing, unreadable,
    // truncated, not a FireRedVAD GGUF, or its tensors do not match its hyper-parameters. Throws
    // std::invalid_argument if n_threads < 1. A larger n_threads is reduced to the core count.
    explicit FireRedModel(const std::filesystem::path& model, int n_threads = 2);
    ~FireRedModel();
    FireRedModel(FireRedModel&&) noexcept;
    FireRedModel& operator=(FireRedModel&&) noexcept;
    FireRedModel(const FireRedModel&) = delete;
    FireRedModel& operator=(const FireRedModel&) = delete;

    // Speech probability of each frame. feats holds frames rows of 80 CMVN-normalized log-fbank
    // values, row-major. Zero frames give an empty vector. Throws std::invalid_argument if feats
    // does not hold exactly frames rows, and std::runtime_error if ggml fails. One call at a time.
    //
    // Frame t reads the frames around it. A part is exact at t when it holds R * (N1 - 1) * S1
    // frames before t and lookahead() frames after it, or the clip ends there. Memory grows with
    // frames, about 5 KB per frame, and the buffers stay at the largest size seen, so the caller
    // splits a long clip into parts.
    std::vector<float> probs(std::span<const float> feats, std::size_t frames);

    const FireRedHParams& hparams() const { return hparams_; }

    // Frames after frame t that its probability reads: N2 * S2 for each FSMN layer, summed over
    // the layers. The VAD model has 8 layers of 20, so 160 frames (1.6 s); Stream-VAD has 0.
    int lookahead() const { return lookahead_; }

    // The CMVN statistics stored in the file, one value per feature. Apply them to the features
    // (x - mean) * istd before probs().
    std::span<const float> cmvn_mean() const { return cmvn_mean_; }
    std::span<const float> cmvn_istd() const { return cmvn_istd_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    FireRedHParams hparams_;
    int lookahead_ = 0;
    std::vector<float> cmvn_mean_;
    std::vector<float> cmvn_istd_;
};

}  // namespace echoir::vad
