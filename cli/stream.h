// Streaming transcription: the input is pushed through a session in chunks, and the session's
// events reach the sink as they are produced.
#pragma once

#include <filesystem>
#include <vector>

#include "options.h"
#include "stream/session.h"

namespace echoir::cli {

struct LoadedModels;

// The session settings for a run's options and loaded models. Throws UsageError when the policy
// is rollback and the backend has no decoder prefix support.
SessionConfig session_config(const TranscribeOptions& options, const LoadedModels& models);

// Called with each session event, in order, and never from two threads at once. A file or standard
// input run with Pace::none sends every event from the calling thread. Otherwise the session's
// worker thread sends the decoding events, and the calling thread sends the events of the final
// flush and the errors that the run itself reports.
using StreamSink = SessionSink;

// none: chunks are pushed as fast as the decoder takes them, with one tick after each push, so the
// work is deterministic. realtime: the session's worker decodes while chunks are pushed at the
// speed of the audio, as live capture would deliver them.
enum class Pace { none, realtime };

// Pushes the input chunk_ms at a time. The session is flushed at the end, which finalizes the
// segment that was still open. A file, or raw samples on standard input, is decoded as it is
// pushed. A WAV on standard input is read whole first. A capture device (mic or loopback) runs
// live until Ctrl-C, the duration limit in the options, a lost device or a decode error; the pace
// does not apply to it. Throws std::exception when a model, the input or a decode fails. Before it
// throws, the sink receives an ERROR event, unless the session has already sent one.
void transcribe_stream(const TranscribeOptions& options, int chunk_ms, Pace pace,
                       const std::vector<std::filesystem::path>& model_dirs,
                       const StreamSink& sink);

}  // namespace echoir::cli
