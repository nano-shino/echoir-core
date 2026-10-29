// Scripted stand-ins for the backend and the VAD. The ECHOIR_FAKE_BACKEND build uses them in place
// of the models, so a host or a test can run a real session, with real capture and event queues,
// without any model file.
#pragma once

#include <memory>

#include "backends/asr_backend.h"
#include "vad/vad.h"

namespace echoir::fake {

// Decodes to "fake <n>", where n is the number of 100 ms blocks of audio, so the text grows as a
// segment grows. It takes no prefix, so rollback is refused, as on Whisper.
std::unique_ptr<IAsrBackend> make_backend();

// Reports all audio as speech, split into segments of ECHOIR_FAKE_SEGMENT_MS milliseconds (default
// 1000, clamped to 100..60000). Each segment ends where the next one starts, so a session sends a
// FINAL for every segment but the last.
std::unique_ptr<vad::IVad> make_vad();

}  // namespace echoir::fake
