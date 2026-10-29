// Helpers for the streaming integration tests: model loading, a whole-file run through a session,
// and the invariants that every run must keep. The loaders return nothing when a model file is
// missing, so the caller skips the test (test_support.h).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "audio/wav.h"
#include "backends/asr_backend.h"
#include "doctest.h"
#include "stream/session.h"
#include "test_support.h"
#include "vad/vad.h"

namespace echoir::test {

// The segment cap of the streaming tests (stream_config). No segment may be longer than this.
inline constexpr std::int64_t kMaxSegmentMs = 20000;
// The audio is pushed in steps of this size, as the CLI does by default.
inline constexpr std::size_t kChunkMs = 500;

// A registry backend and the VAD it uses by default.
struct BackendCase {
    const char* model_id;
    const char* vad_id;
    bool qwen;
};

inline constexpr BackendCase kWhisper{"whisper-tiny-en", "silero-v6.2.0", false};
inline constexpr BackendCase kQwen{"qwen3-asr-0.6b-q8_0", "firered-vad-v1", true};

// Loads the backend alone. Returns nothing when a model file is missing.
inline std::optional<std::unique_ptr<IAsrBackend>> load_asr(const BackendCase& b,
                                                            bool use_gpu = false) {
    const auto model = model_file(b.model_id);
    const auto mmproj = b.qwen ? model_file(b.model_id, "mmproj") : std::nullopt;
    if (!model || (b.qwen && !mmproj)) return std::nullopt;

    const std::string model_path = model->string();
    const std::string mmproj_path = mmproj ? mmproj->string() : std::string();
    echoir_engine_params params{};
    params.struct_size = sizeof(params);
    params.backend = b.qwen ? ECHOIR_BACKEND_QWEN3_ASR : ECHOIR_BACKEND_WHISPER;
    params.model_path = model_path.c_str();
    params.mmproj_path = b.qwen ? mmproj_path.c_str() : nullptr;
    params.use_gpu = use_gpu ? 1 : 0;
    params.n_threads = 0;  // default
    return make_backend(params);
}

// A backend and the VAD it runs with.
struct Engine {
    std::unique_ptr<IAsrBackend> asr;
    std::unique_ptr<vad::IVad> vad;
};

// Loads the backend and its real VAD. Returns nothing when a model file is missing.
inline std::optional<Engine> load_engine(const BackendCase& b, bool use_gpu = false) {
    const auto vad_file = model_file(b.vad_id, "vad");
    if (!vad_file) return std::nullopt;
    auto asr = load_asr(b, use_gpu);
    if (!asr) return std::nullopt;

    Engine engine;
    engine.vad = vad::make_vad(*vad_file, 2, {}, vad::VadUse::streaming);
    engine.asr = std::move(*asr);
    return engine;
}

// The session settings of the streaming tests: the defaults, with a 20 s segment cap.
inline SessionConfig stream_config() {
    SessionConfig config;
    config.max_segment_ms = kMaxSegmentMs;
    return config;
}

// Pushes the whole input through a session, the way the CLI does, and returns every event.
// on_chunk, when set, runs after each chunk with the audio position in ms.
inline std::vector<SessionEvent> stream_all(IAsrBackend& asr, vad::IVad& vad,
                                            const audio::PcmBuffer& wav,
                                            const std::function<void(std::int64_t)>& on_chunk = {}) {
    std::vector<SessionEvent> events;
    Session session(asr, vad, stream_config(),
                    [&events](const SessionEvent& ev) { events.push_back(ev); });
    const auto rate = static_cast<std::int32_t>(wav.rate);
    const auto channels = static_cast<std::int32_t>(wav.channels);
    const std::size_t total_frames = wav.samples.size() / wav.channels;
    const std::size_t chunk_frames = wav.rate * kChunkMs / 1000;
    for (std::size_t frame = 0; frame < total_frames; frame += chunk_frames) {
        const std::size_t frames = std::min(chunk_frames, total_frames - frame);
        session.push(std::span<const float>(wav.samples)
                         .subspan(frame * wav.channels, frames * wav.channels),
                     rate, channels);
        session.process();
        if (on_chunk) {
            on_chunk(static_cast<std::int64_t>((frame + frames) * 1000 / wav.rate));
        }
    }
    session.flush();
    return events;
}

// The properties that hold for any input, checked over every event of one run. A run over silence
// may have no text at all, so expect_text is false there.
inline void check_invariants(const std::vector<SessionEvent>& events, std::int64_t duration_ms,
                             bool expect_text = true) {
    std::set<std::int64_t> finalized;  // ids already sent as FINAL
    std::int64_t last_final_id = 0;
    std::int64_t last_final_end = 0;
    std::size_t finals_with_text = 0;
    for (const SessionEvent& ev : events) {
        INFO("error event: " << ev.detail);
        REQUIRE(ev.type != ECHOIR_EVENT_ERROR);
        CHECK(ev.start_ms >= 0);
        CHECK(ev.start_ms <= ev.end_ms);
        CHECK(ev.end_ms <= duration_ms + 1);
        CHECK(ev.end_ms - ev.start_ms <= kMaxSegmentMs);
        if (ev.type == ECHOIR_EVENT_PARTIAL) {
            // A segment is never shown again once it is final.
            CHECK(finalized.count(ev.segment_id) == 0);
        }
        if (ev.type != ECHOIR_EVENT_FINAL) continue;
        CHECK(finalized.insert(ev.segment_id).second);
        CHECK(ev.segment_id > last_final_id);
        last_final_id = ev.segment_id;
        // Finals are in time order and never overlap, so no audio is reported twice.
        CHECK(ev.start_ms >= last_final_end);
        last_final_end = ev.end_ms;
        if (!ev.text.empty()) ++finals_with_text;
    }
    if (expect_text) CHECK(finals_with_text > 0);
}

inline std::vector<SessionEvent> finals_of(const std::vector<SessionEvent>& events) {
    std::vector<SessionEvent> finals;
    for (const SessionEvent& ev : events) {
        if (ev.type == ECHOIR_EVENT_FINAL) finals.push_back(ev);
    }
    return finals;
}

inline std::int64_t duration_ms_of(const audio::PcmBuffer& wav) {
    return static_cast<std::int64_t>(wav.samples.size() / wav.channels) * 1000 / wav.rate;
}

}  // namespace echoir::test
