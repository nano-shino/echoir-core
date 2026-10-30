// C ABI entry points. Without the VAD and backends (ECHOIR_HAS_ENGINE undefined), the engine and
// session calls report that the backends are missing. The version, the *_params_init calls, the
// device list and the catalogue work in every build.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capture/devices.h"
#include "capture/feed.h"
#include "echoir/echoir.h"
#include "errors.h"
#include "models/catalogue.h"
#include "models/devices.h"
#include "models/registry.h"
#include "models/resolve.h"
#include "text/utf.h"

#ifdef ECHOIR_HAS_ENGINE
#include "backends/asr_backend.h"
#include "stream/event_queue.h"
#include "stream/session.h"
#include "vad/vad.h"
#endif

namespace {

// Per-thread last failure. The pointer from echoir_last_error() stays valid until this thread's
// next failing call.
thread_local std::string g_last_error;

// Per-thread result of the *_json calls. Each call overwrites it, so its pointer stays valid until
// the next *_json call on this thread.
thread_local std::string g_json_result;

// Stores text as this thread's JSON result and returns it. Throws when the store fails.
const char* set_json_result(std::string text) {
    g_json_result = std::move(text);
    return g_json_result.c_str();
}

// Records "<fn>: <what>". Nothing may throw across the ABI, so on allocation failure the
// previous message stays in place.
void set_last_error(const char* fn, const char* what) noexcept {
    try {
        g_last_error = std::string(fn) + ": " + what;
    } catch (...) {
        // Out of memory: keep the old message.
    }
}

// Records the exception being handled as "<fn>: <reason>" and returns the status it maps to. Call
// it only from a catch block. Engine create returns a pointer, so the status is dropped there and
// only the message reaches the host. Session functions return it.
echoir_status status_of_current_exception(const char* fn) noexcept {
    try {
        throw;
    } catch (const std::invalid_argument& e) {
        set_last_error(fn, e.what());
        return ECHOIR_ERR_INVALID_ARG;
    } catch (const echoir::ModelLoadError& e) {
        set_last_error(fn, e.what());
        return ECHOIR_ERR_MODEL_LOAD;
    } catch (const echoir::BackendUnavailable& e) {
        set_last_error(fn, e.what());
        return ECHOIR_ERR_BACKEND;
    } catch (const echoir::BackendFailure& e) {
        set_last_error(fn, e.what());
        return ECHOIR_ERR_BACKEND;
    } catch (const echoir::StateError& e) {
        set_last_error(fn, e.what());
        return ECHOIR_ERR_STATE;
    } catch (const echoir::NotSupported& e) {
        set_last_error(fn, e.what());
        return ECHOIR_ERR_NOT_IMPLEMENTED;
    } catch (const std::exception& e) {
        set_last_error(fn, e.what());
        return ECHOIR_ERR_INTERNAL;
    } catch (...) {
        set_last_error(fn, "unknown error");
        return ECHOIR_ERR_INTERNAL;
    }
}

}  // namespace

#ifdef ECHOIR_HAS_ENGINE
// What echoir_engine_create loads. A session takes the backend and the VAD from here, and at most
// one session can be open on an engine.
struct echoir_engine {
    std::unique_ptr<echoir::IAsrBackend> backend;
    std::unique_ptr<echoir::vad::IVad> vad;
    std::atomic<bool> session_open{false};
};
#else
// Without the VAD and the backends there is nothing to load.
struct echoir_engine {};
#endif

#ifdef ECHOIR_HAS_ENGINE
// A session created by echoir_session_create(). The session refers to the engine's models, so it
// must be freed before its engine.
struct echoir_session {
    echoir_engine* engine = nullptr;
    echoir_event_cb cb = nullptr;  // callback mode when set
    void* user = nullptr;
    std::unique_ptr<echoir::EventQueue> queue;  // queue mode only
    echoir::SessionEvent polled;                // the last event from poll; keeps its text alive
    std::unique_ptr<echoir::Session> session;
    // Set while the session captures its own audio. Declared after the session, so the capture
    // stops before the session is destroyed.
    std::unique_ptr<echoir::capture::CaptureFeed> feed;
};
#endif

const char* echoir_version(void) { return ECHOIR_VERSION_STRING; }

const char* echoir_last_error(void) { return g_last_error.c_str(); }

void echoir_engine_params_init(echoir_engine_params* p) {
    if (p == nullptr) {
        set_last_error("echoir_engine_params_init", "params must not be NULL");
        return;
    }
    // Zero first, so pointer fields start NULL and any field added later starts at 0.
    *p = echoir_engine_params{};
    p->struct_size = sizeof(echoir_engine_params);
    p->backend = ECHOIR_BACKEND_WHISPER;
    // ggml uses the GPU backend this build has (Metal, Vulkan) and CPU otherwise.
    p->use_gpu = 1;
}

void echoir_session_params_init(echoir_session_params* p) {
    if (p == nullptr) {
        set_last_error("echoir_session_params_init", "params must not be NULL");
        return;
    }
    *p = echoir_session_params{};
    p->struct_size = sizeof(echoir_session_params);
    p->language = "auto";       // whisper detects the language
    p->min_chunk_ms = 500;      // 8000 samples (500 ms) of audio before a decode runs.
    p->max_segment_ms = 10000;  // Longest ongoing segment before a forced cut.
    p->emit_partials = 1;
    p->retain_audio_ms = 0;
}

void echoir_model_params_init(echoir_model_params* p) {
    if (p == nullptr) {
        set_last_error("echoir_model_params_init", "params must not be NULL");
        return;
    }
    *p = echoir_model_params{};
    p->struct_size = sizeof(echoir_model_params);
    p->use_gpu = 1;
}

#ifdef ECHOIR_HAS_ENGINE
namespace {

// Loads the models that params names. Throws on failure, and the callers map the exception.
std::unique_ptr<echoir_engine> make_engine(const echoir_engine_params& params) {
    // A larger struct comes from a newer header. This version reads only the fields it knows.
    if (params.struct_size < sizeof(echoir_engine_params)) {
        throw std::invalid_argument("struct_size is smaller than echoir_engine_params");
    }
#ifndef ECHOIR_FAKE_BACKEND
    // The VAD is always needed, so a missing path fails before any model loads.
    if (params.vad_model_path == nullptr || params.vad_model_path[0] == '\0') {
        throw std::invalid_argument("vad_model_path is required");
    }
#endif
    std::unique_ptr<echoir::IAsrBackend> backend = echoir::make_backend(params);
    // Paths are UTF-8 on every platform. A narrow path would be read as ANSI on Windows.
    const std::filesystem::path vad_path = params.vad_model_path != nullptr
                                               ? echoir::text::path_from_utf8(params.vad_model_path)
                                               : std::filesystem::path();
    std::unique_ptr<echoir::vad::IVad> vad =
        echoir::vad::make_vad(vad_path, 2, {}, echoir::vad::VadUse::streaming);
    return std::unique_ptr<echoir_engine>(
        new echoir_engine{std::move(backend), std::move(vad)});
}

// Resolves the model files that params names, then loads them as make_engine does. A field after
// model_id is read only when the struct includes it, and otherwise takes its default.
std::unique_ptr<echoir_engine> make_engine_from_model(const echoir_model_params& params) {
    constexpr size_t kEndOfModelId =
        offsetof(echoir_model_params, model_id) + sizeof(echoir_model_params::model_id);
    constexpr size_t kEndOfVadId =
        offsetof(echoir_model_params, vad_id) + sizeof(echoir_model_params::vad_id);
    constexpr size_t kEndOfUseGpu =
        offsetof(echoir_model_params, use_gpu) + sizeof(echoir_model_params::use_gpu);
    constexpr size_t kEndOfThreads =
        offsetof(echoir_model_params, n_threads) + sizeof(echoir_model_params::n_threads);
    if (params.struct_size < kEndOfModelId) {
        throw std::invalid_argument("struct_size is smaller than the fields up to model_id");
    }
    if (params.struct_size > sizeof(echoir_model_params)) {
        throw std::invalid_argument("struct_size is larger than echoir_model_params");
    }
    if (params.model_id == nullptr || params.model_id[0] == '\0') {
        throw std::invalid_argument("model_id is required");
    }
#ifndef ECHOIR_FAKE_BACKEND
    if (params.models_dir == nullptr || params.models_dir[0] == '\0') {
        throw std::invalid_argument("models_dir is required");
    }
#endif

    const echoir::models::Registry registry =
        params.registry_json != nullptr ? echoir::models::Registry::parse(params.registry_json)
                                        : echoir::models::Registry::builtin();
    const std::string model_id = params.model_id;
    const echoir::models::Model* model = registry.find(model_id);
    if (model == nullptr) {
        throw std::invalid_argument("'" + model_id + "' is not an id in the registry");
    }
    const std::string backend = echoir::models::backend_key(model->backend);
    if (backend.empty()) {
        throw std::invalid_argument("'" + model_id + "' is not a speech model");
    }

    const std::filesystem::path dir = params.models_dir != nullptr
                                          ? echoir::text::path_from_utf8(params.models_dir)
                                          : std::filesystem::path();
    const std::string model_path =
        echoir::text::path_utf8(echoir::models::resolve(dir, *model, "model"));
    const std::string mmproj_path =
        backend == "qwen" ? echoir::text::path_utf8(echoir::models::resolve(dir, *model, "mmproj"))
                          : std::string();
    const char* vad_arg = params.struct_size >= kEndOfVadId ? params.vad_id : nullptr;
    const std::string vad_id = vad_arg != nullptr && vad_arg[0] != '\0'
                                   ? std::string(vad_arg)
                                   : registry.default_vad(backend);
    const echoir::models::Model* vad = registry.find(vad_id);
    if (vad == nullptr) {
        throw std::invalid_argument("'" + vad_id + "' is not an id in the registry");
    }
    const std::string vad_path = echoir::text::path_utf8(echoir::models::resolve(dir, *vad, "vad"));

    echoir_engine_params engine{};
    engine.struct_size = static_cast<uint32_t>(sizeof(engine));
    engine.backend = backend == "qwen" ? ECHOIR_BACKEND_QWEN3_ASR : ECHOIR_BACKEND_WHISPER;
    engine.model_path = model_path.c_str();
    engine.mmproj_path = mmproj_path.empty() ? nullptr : mmproj_path.c_str();
    engine.vad_model_path = vad_path.c_str();
    engine.use_gpu = params.struct_size >= kEndOfUseGpu ? params.use_gpu : 1;
    engine.n_threads = params.struct_size >= kEndOfThreads ? params.n_threads : 0;
    return make_engine(engine);
}

}  // namespace

echoir_engine* echoir_engine_create(const echoir_engine_params* params) {
    try {
        if (params == nullptr) {
            throw std::invalid_argument("params must not be NULL");
        }
        return make_engine(*params).release();
    } catch (...) {
        status_of_current_exception("echoir_engine_create");
        return nullptr;
    }
}

echoir_engine* echoir_engine_create_from_model(const echoir_model_params* params) {
    try {
        if (params == nullptr) {
            throw std::invalid_argument("params must not be NULL");
        }
        return make_engine_from_model(*params).release();
    } catch (...) {
        status_of_current_exception("echoir_engine_create_from_model");
        return nullptr;
    }
}
#else
echoir_engine* echoir_engine_create(const echoir_engine_params*) {
    set_last_error("echoir_engine_create",
                   "the VAD and backends are not built (ECHOIR_WITH_WHISPER=OFF)");
    return nullptr;
}

echoir_engine* echoir_engine_create_from_model(const echoir_model_params*) {
    set_last_error("echoir_engine_create_from_model",
                   "the VAD and backends are not built (ECHOIR_WITH_WHISPER=OFF)");
    return nullptr;
}
#endif

const char* echoir_models_json(const char* registry_json, const char* models_dir) {
    const char* fn = "echoir_models_json";
    try {
        const echoir::models::Registry registry =
            registry_json != nullptr ? echoir::models::Registry::parse(registry_json)
                                     : echoir::models::Registry::builtin();
        const std::filesystem::path dir =
            models_dir != nullptr ? echoir::text::path_from_utf8(models_dir)
                                  : std::filesystem::path();
#ifdef ECHOIR_HAS_ENGINE
        const std::vector<echoir::models::Device> devices = echoir::models::query_devices();
#else
        const std::vector<echoir::models::Device> devices;
#endif
        return set_json_result(echoir::models::catalogue_json(registry, dir, devices));
    } catch (...) {
        status_of_current_exception(fn);
        return nullptr;
    }
}

const char* echoir_devices_json(void) {
    try {
        return set_json_result(echoir::capture::devices_json(echoir::capture::list_devices()));
    } catch (...) {
        status_of_current_exception("echoir_devices_json");
        return nullptr;
    }
}

// Deleting NULL does nothing, so NULL needs no check. The sessions must be freed first.
void echoir_engine_free(echoir_engine* engine) { delete engine; }

#ifdef ECHOIR_HAS_ENGINE
namespace {

// Converts a session event to the ABI form. The pointers refer to the event's own strings, so
// they are valid as long as the event is.
echoir_event to_abi(const echoir::SessionEvent& ev) {
    echoir_event out{};
    out.type = ev.type;
    out.segment_id = ev.segment_id;
    out.start_ms = ev.start_ms;
    out.end_ms = ev.end_ms;
    out.text = ev.text.c_str();
    out.detail = ev.type == ECHOIR_EVENT_ERROR ? ev.detail.c_str() : nullptr;
    return out;
}

// Runs one session call and maps its exception to a status.
template <typename Call>
echoir_status run_session_call(echoir_session* s, const char* fn, Call&& call) noexcept {
    if (s == nullptr) {
        set_last_error(fn, "session must not be NULL");
        return ECHOIR_ERR_INVALID_ARG;
    }
    try {
        call(*s->session);
        return ECHOIR_OK;
    } catch (...) {
        return status_of_current_exception(fn);
    }
}

// The argument checks that the two push functions share. Returns ECHOIR_OK, or records the reason
// and returns ECHOIR_ERR_INVALID_ARG.
echoir_status check_push_args(const char* fn, const void* pcm, size_t n_frames, int32_t channels) {
    if (channels <= 0) {
        set_last_error(fn, "channels must be positive");
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (n_frames > 0 && pcm == nullptr) {
        set_last_error(fn, "pcm must not be NULL");
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (n_frames > SIZE_MAX / static_cast<size_t>(channels)) {
        set_last_error(fn, "n_frames is too large");
        return ECHOIR_ERR_INVALID_ARG;
    }
    return ECHOIR_OK;
}

// Reads the session settings from the struct a caller passed. A struct that ends after
// emit_partials is accepted, and the fields it does not cover take their defaults. Throws
// std::invalid_argument for a struct too small to include emit_partials, a struct from a newer
// header, an unknown policy, and a negative retain_audio_ms.
echoir::SessionConfig session_config_from_abi(const echoir_session_params& params) {
    constexpr size_t kEndOfEmitPartials = offsetof(echoir_session_params, stream_policy);
    constexpr size_t kEndOfStreamPolicy = kEndOfEmitPartials + sizeof(params.stream_policy);
    constexpr size_t kEndOfRetainAudio =
        offsetof(echoir_session_params, retain_audio_ms) + sizeof(params.retain_audio_ms);
    if (params.struct_size < kEndOfEmitPartials) {
        throw std::invalid_argument("struct_size is smaller than the fields up to emit_partials");
    }
    if (params.struct_size > sizeof(echoir_session_params)) {
        throw std::invalid_argument("struct_size is larger than echoir_session_params");
    }
    echoir::SessionConfig config;
    config.language = params.language != nullptr ? params.language : "auto";
    config.min_chunk_ms = params.min_chunk_ms;
    config.max_segment_ms = params.max_segment_ms;
    config.emit_partials = params.emit_partials != 0;
    config.policy = echoir::StreamPolicy::backend_default;
    // A caller that stops before stream_policy keeps the default. Its bytes are not read.
    if (params.struct_size >= kEndOfStreamPolicy) {
        switch (params.stream_policy) {
            case ECHOIR_POLICY_DEFAULT:
                break;
            case ECHOIR_POLICY_AGREEMENT:
                config.policy = echoir::StreamPolicy::agreement;
                break;
            case ECHOIR_POLICY_ROLLBACK:
                config.policy = echoir::StreamPolicy::rollback;
                break;
            default:
                throw std::invalid_argument("stream_policy is not a known policy");
        }
    }
    if (params.struct_size >= kEndOfRetainAudio) {
        if (params.retain_audio_ms < 0) {
            throw std::invalid_argument("retain_audio_ms must not be negative");
        }
        config.retain_audio_ms = params.retain_audio_ms;
    }
    return config;
}

}  // namespace

echoir_session* echoir_session_create(echoir_engine* engine, const echoir_session_params* params,
                                      echoir_event_cb cb, void* user) {
    const char* fn = "echoir_session_create";
    if (params == nullptr) {
        set_last_error(fn, "params must not be NULL");
        return nullptr;
    }
    // The settings are read before the engine is checked, so a bad struct is reported even when
    // no engine is given.
    echoir::SessionConfig config;
    try {
        config = session_config_from_abi(*params);
    } catch (...) {
        status_of_current_exception(fn);
        return nullptr;
    }
    if (engine == nullptr) {
        set_last_error(fn, "engine must not be NULL");
        return nullptr;
    }
    bool expected = false;
    if (!engine->session_open.compare_exchange_strong(expected, true)) {
        set_last_error(fn, "the engine already serves a session");
        return nullptr;
    }
    try {
        auto s = std::make_unique<echoir_session>();
        s->engine = engine;
        s->cb = cb;
        s->user = user;
        if (cb == nullptr) {
            s->queue = std::make_unique<echoir::EventQueue>();
        }

        // The sink reads the callback or the queue at delivery time. The session is destroyed
        // before this struct, so the raw pointer stays valid.
        echoir_session* raw = s.get();
        auto sink = [raw](const echoir::SessionEvent& ev) {
            if (raw->cb != nullptr) {
                const echoir_event abi = to_abi(ev);
                raw->cb(&abi, raw->user);
            } else {
                raw->queue->push(ev);
            }
        };
        s->session = std::make_unique<echoir::Session>(*engine->backend, *engine->vad,
                                                       std::move(config), std::move(sink));
        return s.release();
    } catch (...) {
        engine->session_open.store(false);
        status_of_current_exception(fn);
        return nullptr;
    }
}

void echoir_session_free(echoir_session* s) {
    if (s == nullptr) {
        return;
    }
    echoir_engine* engine = s->engine;
    delete s;
    engine->session_open.store(false);
}

echoir_status echoir_session_poll(echoir_session* s, echoir_event* out) {
    const char* fn = "echoir_session_poll";
    if (s == nullptr || out == nullptr) {
        set_last_error(fn, "session and out must not be NULL");
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (s->queue == nullptr) {
        set_last_error(fn, "the session is in callback mode");
        return ECHOIR_ERR_STATE;
    }
    try {
        if (!s->queue->pop(s->polled)) {
            set_last_error(fn, "the event queue is empty");
            return ECHOIR_ERR_STATE;
        }
        *out = to_abi(s->polled);
        return ECHOIR_OK;
    } catch (...) {
        return status_of_current_exception(fn);
    }
}

echoir_status echoir_session_push_f32(echoir_session* s, const float* pcm, size_t n_frames,
                                      int32_t sample_rate, int32_t channels) {
    const char* fn = "echoir_session_push_f32";
    if (const echoir_status bad = check_push_args(fn, pcm, n_frames, channels); bad != ECHOIR_OK) {
        return bad;
    }
    if (s != nullptr && s->feed != nullptr) {
        set_last_error(fn, "the session captures its own audio");
        return ECHOIR_ERR_STATE;
    }
    const auto ch = static_cast<size_t>(channels);
    return run_session_call(s, fn, [&](echoir::Session& session) {
        session.push(std::span<const float>(pcm, n_frames * ch), sample_rate, channels);
    });
}

echoir_status echoir_session_push_s16(echoir_session* s, const int16_t* pcm, size_t n_frames,
                                      int32_t sample_rate, int32_t channels) {
    const char* fn = "echoir_session_push_s16";
    if (const echoir_status bad = check_push_args(fn, pcm, n_frames, channels); bad != ECHOIR_OK) {
        return bad;
    }
    if (s != nullptr && s->feed != nullptr) {
        set_last_error(fn, "the session captures its own audio");
        return ECHOIR_ERR_STATE;
    }
    const auto ch = static_cast<size_t>(channels);
    return run_session_call(s, fn, [&](echoir::Session& session) {
        // Full scale of 16-bit samples is 32768, which maps to 1.0.
        std::vector<float> samples(n_frames * ch);
        for (size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<float>(pcm[i]) / 32768.0f;
        }
        session.push(samples, sample_rate, channels);
    });
}

echoir_status echoir_session_process(echoir_session* s) {
    return run_session_call(s, "echoir_session_process",
                            [](echoir::Session& session) { session.process(); });
}

echoir_status echoir_session_start(echoir_session* s) {
    return run_session_call(s, "echoir_session_start",
                            [](echoir::Session& session) { session.start(); });
}

echoir_status echoir_session_start_capture(echoir_session* s, echoir_source source,
                                           const char* device_id) {
    const char* fn = "echoir_session_start_capture";
    if (s == nullptr) {
        set_last_error(fn, "session must not be NULL");
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (source != ECHOIR_SOURCE_MIC && source != ECHOIR_SOURCE_LOOPBACK) {
        set_last_error(fn, "source is not a known source");
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (s->feed != nullptr) {
        set_last_error(fn, "the session already captures its own audio");
        return ECHOIR_ERR_STATE;
    }
    try {
        const auto kind = source == ECHOIR_SOURCE_LOOPBACK ? echoir::capture::Source::loopback
                                                           : echoir::capture::Source::mic;
        // The device opens first, so a device that cannot open leaves the session as it was.
        auto feed = std::make_unique<echoir::capture::CaptureFeed>(
            kind, device_id != nullptr ? device_id : "", *s->session);
        s->session->start();
        try {
            feed->start();
        } catch (...) {
            s->session->stop();
            throw;
        }
        s->feed = std::move(feed);
        return ECHOIR_OK;
    } catch (...) {
        return status_of_current_exception(fn);
    }
}

// A session that captures its own audio is flushed here, so its last segment is final.
echoir_status echoir_session_stop(echoir_session* s) {
    return run_session_call(s, "echoir_session_stop", [s](echoir::Session& session) {
        if (s->feed == nullptr) {
            session.stop();
            return;
        }
        // The device closes first, so no more audio arrives. The audio it captured is decoded and
        // the segment in progress is finalized, before the worker stops.
        s->feed->stop();
        s->feed.reset();
        try {
            session.flush();
        } catch (...) {
            session.stop();
            throw;
        }
        session.stop();
    });
}

echoir_status echoir_session_flush(echoir_session* s) {
    return run_session_call(s, "echoir_session_flush",
                            [](echoir::Session& session) { session.flush(); });
}

echoir_status echoir_session_reset(echoir_session* s) {
    return run_session_call(s, "echoir_session_reset",
                            [](echoir::Session& session) { session.reset(); });
}

echoir_status echoir_session_clear(echoir_session* s) {
    return run_session_call(s, "echoir_session_clear",
                            [](echoir::Session& session) { session.clear(); });
}

echoir_status echoir_session_read_audio(echoir_session* s, int64_t start_ms, int64_t end_ms,
                                        int16_t* out, size_t cap, size_t* n_frames) {
    const char* fn = "echoir_session_read_audio";
    if (n_frames == nullptr) {
        set_last_error(fn, "n_frames must not be NULL");
        return ECHOIR_ERR_INVALID_ARG;
    }
    *n_frames = 0;
    return run_session_call(s, fn, [&](echoir::Session& session) {
        *n_frames = session.audio().read_audio(start_ms, end_ms, out, cap);
    });
}

echoir_status echoir_session_read_peaks(echoir_session* s, int64_t start_ms, int64_t end_ms,
                                        int32_t bucket_ms, float* out_minmax, size_t cap,
                                        size_t* n_buckets) {
    const char* fn = "echoir_session_read_peaks";
    if (n_buckets == nullptr) {
        set_last_error(fn, "n_buckets must not be NULL");
        return ECHOIR_ERR_INVALID_ARG;
    }
    *n_buckets = 0;
    return run_session_call(s, fn, [&](echoir::Session& session) {
        *n_buckets = session.audio().read_peaks(start_ms, end_ms, bucket_ms, out_minmax, cap);
    });
}

echoir_status echoir_session_set_input_muted(echoir_session* s, int32_t muted) {
    return run_session_call(s, "echoir_session_set_input_muted", [muted](echoir::Session& session) {
        session.set_input_muted(muted != 0);
    });
}
#else
// Without the engine no session can be created, so every session call reports that the backends
// are missing.
namespace {
echoir_status not_built(const char* fn) noexcept {
    set_last_error(fn, "the VAD and backends are not built (ECHOIR_WITH_WHISPER=OFF)");
    return ECHOIR_ERR_NOT_IMPLEMENTED;
}
}  // namespace

echoir_session* echoir_session_create(echoir_engine*, const echoir_session_params*, echoir_event_cb,
                                      void*) {
    not_built("echoir_session_create");
    return nullptr;
}

void echoir_session_free(echoir_session*) {}

echoir_status echoir_session_poll(echoir_session*, echoir_event*) {
    return not_built("echoir_session_poll");
}

echoir_status echoir_session_push_f32(echoir_session*, const float*, size_t, int32_t, int32_t) {
    return not_built("echoir_session_push_f32");
}

echoir_status echoir_session_push_s16(echoir_session*, const int16_t*, size_t, int32_t, int32_t) {
    return not_built("echoir_session_push_s16");
}

echoir_status echoir_session_process(echoir_session*) {
    return not_built("echoir_session_process");
}

echoir_status echoir_session_start(echoir_session*) { return not_built("echoir_session_start"); }

echoir_status echoir_session_start_capture(echoir_session*, echoir_source, const char*) {
    return not_built("echoir_session_start_capture");
}

echoir_status echoir_session_stop(echoir_session*) { return not_built("echoir_session_stop"); }

echoir_status echoir_session_flush(echoir_session*) { return not_built("echoir_session_flush"); }

echoir_status echoir_session_reset(echoir_session*) { return not_built("echoir_session_reset"); }

echoir_status echoir_session_clear(echoir_session*) { return not_built("echoir_session_clear"); }

echoir_status echoir_session_read_audio(echoir_session*, int64_t, int64_t, int16_t*, size_t,
                                        size_t*) {
    return not_built("echoir_session_read_audio");
}

echoir_status echoir_session_read_peaks(echoir_session*, int64_t, int64_t, int32_t, float*, size_t,
                                        size_t*) {
    return not_built("echoir_session_read_peaks");
}

echoir_status echoir_session_set_input_muted(echoir_session*, int32_t) {
    return not_built("echoir_session_set_input_muted");
}
#endif
