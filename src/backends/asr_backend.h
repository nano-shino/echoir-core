// Backend interface for one-shot decoding. The streaming layer calls decode() with a window of
// audio and the prompt history. The backend returns the raw text, and the caller cleans and checks
// it.
#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "echoir/echoir.h"

namespace echoir {

struct DecodeRequest {
    std::span<const float> pcm;               // 16 kHz mono f32 samples
    const std::vector<std::string>& history;  // last finals, used as the prompt
    std::string language;                     // "auto" (or empty) to detect, else e.g. "en"
    // User text, such as a vocabulary of proper names. It goes ahead of the history in every
    // decode. Empty for none. The view only has to live for the call.
    std::string_view context{};
    // The continuation of an earlier decode of the same audio (DecodeResult::continuation). The
    // decoder keeps it as the start of its output. Empty for none. Only backends whose
    // supports_prefix() is true accept a non-empty prefix. The view only has to live for the call.
    std::string_view prefix{};
    // Tokens to remove from the end of prefix before it is used, so the decoder can revise the last
    // words. Zero for none. Must not be negative. Ignored when prefix is empty.
    int rollback_tokens = 0;
};

struct DecodeResult {
    std::string text;  // raw, uncleaned
    // What to pass back as the prefix of the next decode of the same audio: the prefix that was
    // used, minus its rollback, plus the new output. It can hold text that text leaves out, such as
    // a language header, so it is not cleaned. Equal to text for a backend without prefix support.
    std::string continuation;
};

// decode() is not reentrant. The engine serves one session at a time, so calls never overlap.
// It throws std::invalid_argument for a language the model does not have, for a context that
// does not fit, and for a prefix on a backend without prefix support. An empty pcm returns empty
// text after those checks, so a caller can validate a request before any audio exists.
class IAsrBackend {
public:
    virtual ~IAsrBackend() = default;
    virtual DecodeResult decode(const DecodeRequest& request) = 0;
    virtual std::string name() const = 0;  // "whisper" or "qwen3-asr"
    // True when decode() takes a non-empty DecodeRequest::prefix.
    virtual bool supports_prefix() const { return false; }
    // The history that the stream session gives the segment that starts at a forced cut: the FINAL
    // before the cut when true, none when false. Whisper decodes that audio best with no history.
    virtual bool history_at_cut() const { return false; }
};

// Creates the backend that params.backend names. Throws std::invalid_argument for bad params (a
// null or empty model_path, a negative n_threads). Throws echoir::BackendUnavailable when the
// backend is not built, and echoir::ModelLoadError when the model fails to load. Both derive from
// std::runtime_error. n_threads 0 means 4.
std::unique_ptr<IAsrBackend> make_backend(const echoir_engine_params& params);

}  // namespace echoir
