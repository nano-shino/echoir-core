// C ABI entry points. Without the VAD and backends (ECHOIR_HAS_ENGINE undefined), the engine and
// session calls report that the backends are missing. The version, the *_params_init calls, the
// device list and the catalogue work in every build.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
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
#include "stream/tape.h"
#include "text/utf.h"
#include "translate/prompt.h"
#include "translate/translator.h"

#ifdef ECHOIR_HAS_ENGINE
#include "backends/asr_backend.h"
#include "backends/translator_backend.h"
#include "stream/engine_gate.h"
#include "stream/event_queue.h"
#include "stream/session.h"
#include "vad/vad.h"
#endif

namespace {

// Per-thread last failure: its message and its status. The pointer from echoir_last_error() stays
// valid until this thread's next failing call.
thread_local std::string g_last_error;
thread_local echoir_status g_last_status = ECHOIR_OK;

// Per-thread result of the *_json calls. Each call overwrites it, so its pointer stays valid until
// the next *_json call on this thread.
thread_local std::string g_json_result;

// Stores text as this thread's JSON result and returns it. Throws when the store fails.
const char* set_json_result(std::string text) {
    g_json_result = std::move(text);
    return g_json_result.c_str();
}

// Records "<fn>: <what>" and the status the failing call reports. Nothing may throw across the
// ABI, so on allocation failure the previous message stays in place.
void set_last_error(const char* fn, const char* what, echoir_status status) noexcept {
    g_last_status = status;
    try {
        g_last_error = std::string(fn) + ": " + what;
    } catch (...) {
        // Out of memory: keep the old message.
    }
}

// Records the exception being handled as "<fn>: <reason>" with the status it maps to, and returns
// that status. Call it only from a catch block. A call that returns a pointer drops the return
// value, and the host reads the status with echoir_last_status().
echoir_status status_of_current_exception(const char* fn) noexcept {
    try {
        throw;
    } catch (const std::invalid_argument& e) {
        set_last_error(fn, e.what(), ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    } catch (const echoir::ModelLoadError& e) {
        set_last_error(fn, e.what(), ECHOIR_ERR_MODEL_LOAD);
        return ECHOIR_ERR_MODEL_LOAD;
    } catch (const echoir::BackendUnavailable& e) {
        set_last_error(fn, e.what(), ECHOIR_ERR_BACKEND);
        return ECHOIR_ERR_BACKEND;
    } catch (const echoir::BackendFailure& e) {
        set_last_error(fn, e.what(), ECHOIR_ERR_BACKEND);
        return ECHOIR_ERR_BACKEND;
    } catch (const echoir::StateError& e) {
        set_last_error(fn, e.what(), ECHOIR_ERR_STATE);
        return ECHOIR_ERR_STATE;
    } catch (const echoir::NotSupported& e) {
        set_last_error(fn, e.what(), ECHOIR_ERR_NOT_IMPLEMENTED);
        return ECHOIR_ERR_NOT_IMPLEMENTED;
    } catch (const std::exception& e) {
        set_last_error(fn, e.what(), ECHOIR_ERR_INTERNAL);
        return ECHOIR_ERR_INTERNAL;
    } catch (...) {
        set_last_error(fn, "unknown error", ECHOIR_ERR_INTERNAL);
        return ECHOIR_ERR_INTERNAL;
    }
}

}  // namespace

// A tape handle. Sessions that write the tape hold their own reference to it.
struct echoir_tape {
    std::shared_ptr<echoir::stream::Tape> tape;
};

#ifdef ECHOIR_HAS_ENGINE
// What echoir_engine_create loads. Its sessions share the backend and the VAD, and take turns on
// them through the gate.
struct echoir_engine {
    std::unique_ptr<echoir::IAsrBackend> backend;
    std::unique_ptr<echoir::vad::IVad> vad;
    std::shared_ptr<echoir::stream::EngineGate> gate =
        std::make_shared<echoir::stream::EngineGate>();
};
#else
// Without the VAD and the backends there is nothing to load.
struct echoir_engine {};
#endif

// A translator created by echoir_translator_create_from_model(). It owns its model and shares
// nothing with an engine. Calls take the mutex, so they run one at a time.
struct echoir_translator {
#ifdef ECHOIR_HAS_ENGINE
    std::unique_ptr<echoir::translate::ITranslator> model;
#endif
    std::mutex mutex;
    std::string text;  // the text of result, kept alive here
    echoir_translation result{};
};

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

echoir_status echoir_last_status(void) { return g_last_status; }

void echoir_engine_params_init(echoir_engine_params* p) {
    if (p == nullptr) {
        set_last_error("echoir_engine_params_init", "params must not be NULL",
                       ECHOIR_ERR_INVALID_ARG);
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
        set_last_error("echoir_session_params_init", "params must not be NULL",
                       ECHOIR_ERR_INVALID_ARG);
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
        set_last_error("echoir_model_params_init", "params must not be NULL",
                       ECHOIR_ERR_INVALID_ARG);
        return;
    }
    *p = echoir_model_params{};
    p->struct_size = sizeof(echoir_model_params);
    p->use_gpu = 1;
}

void echoir_translator_params_init(echoir_translator_params* p) {
    if (p == nullptr) {
        set_last_error("echoir_translator_params_init", "params must not be NULL",
                       ECHOIR_ERR_INVALID_ARG);
        return;
    }
    *p = echoir_translator_params{};
    p->struct_size = sizeof(echoir_translator_params);
    p->use_gpu = 1;
}

#ifdef ECHOIR_HAS_ENGINE
namespace {

// The longest vad_min_silence_ms. A pause longer than a minute is not a pause in speech.
constexpr int32_t kMaxVadMinSilenceMs = 60000;

// The VAD tuning of the engine params, checked. 0 keeps the VAD's own value.
echoir::vad::VadOverrides vad_tuning(float threshold, int32_t min_silence_ms) {
    // Written so that NaN fails too.
    if (!(threshold >= 0.0F && threshold <= 1.0F)) {
        throw std::invalid_argument("vad_threshold must be from 0 to 1");
    }
    if (min_silence_ms < 0 || min_silence_ms > kMaxVadMinSilenceMs) {
        throw std::invalid_argument("vad_min_silence_ms must be from 0 to 60000");
    }
    echoir::vad::VadOverrides overrides;
    overrides.threshold = threshold;
    overrides.min_silence_ms = min_silence_ms;
    return overrides;
}

// Loads the models that params names. Throws on failure, and the callers map the exception.
std::unique_ptr<echoir_engine> make_engine(const echoir_engine_params& params) {
    constexpr size_t kEndOfThreads =
        offsetof(echoir_engine_params, n_threads) + sizeof(echoir_engine_params::n_threads);
    constexpr size_t kEndOfVadMinSilence = offsetof(echoir_engine_params, vad_min_silence_ms) +
                                           sizeof(echoir_engine_params::vad_min_silence_ms);
    // A larger struct comes from a newer header. This version reads only the fields it knows, and
    // the VAD tuning only when the struct includes it.
    if (params.struct_size < kEndOfThreads) {
        throw std::invalid_argument("struct_size is smaller than the fields up to n_threads");
    }
    const bool has_tuning = params.struct_size >= kEndOfVadMinSilence;
    const echoir::vad::VadOverrides overrides = vad_tuning(
        has_tuning ? params.vad_threshold : 0.0F, has_tuning ? params.vad_min_silence_ms : 0);
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
        echoir::vad::make_vad(vad_path, 2, overrides, echoir::vad::VadUse::streaming);
    return std::unique_ptr<echoir_engine>(new echoir_engine{std::move(backend), std::move(vad)});
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
    constexpr size_t kEndOfVadMinSilence = offsetof(echoir_model_params, vad_min_silence_ms) +
                                           sizeof(echoir_model_params::vad_min_silence_ms);
    if (params.struct_size < kEndOfModelId) {
        throw std::invalid_argument("struct_size is smaller than the fields up to model_id");
    }
    if (params.struct_size > sizeof(echoir_model_params)) {
        throw std::invalid_argument("struct_size is larger than echoir_model_params");
    }
    if (params.model_id == nullptr || params.model_id[0] == '\0') {
        throw std::invalid_argument("model_id is required");
    }
    const bool has_tuning = params.struct_size >= kEndOfVadMinSilence;
    // Checked here too, so a bad value is reported before a missing file.
    (void)vad_tuning(has_tuning ? params.vad_threshold : 0.0F,
                     has_tuning ? params.vad_min_silence_ms : 0);
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
    if (has_tuning) {
        engine.vad_threshold = params.vad_threshold;
        engine.vad_min_silence_ms = params.vad_min_silence_ms;
    }
    return make_engine(engine);
}

// Resolves the translator that params names and loads it. A field after model_id is read only when
// the struct includes it, and otherwise takes its default.
std::unique_ptr<echoir_translator> make_translator(const echoir_translator_params& params) {
    constexpr size_t kEndOfModelId =
        offsetof(echoir_translator_params, model_id) + sizeof(echoir_translator_params::model_id);
    constexpr size_t kEndOfUseGpu =
        offsetof(echoir_translator_params, use_gpu) + sizeof(echoir_translator_params::use_gpu);
    constexpr size_t kEndOfThreads =
        offsetof(echoir_translator_params, n_threads) + sizeof(echoir_translator_params::n_threads);
    if (params.struct_size < kEndOfModelId) {
        throw std::invalid_argument("struct_size is smaller than the fields up to model_id");
    }
    if (params.struct_size > sizeof(echoir_translator_params)) {
        throw std::invalid_argument("struct_size is larger than echoir_translator_params");
    }
#ifndef ECHOIR_FAKE_BACKEND
    if (params.models_dir == nullptr || params.models_dir[0] == '\0') {
        throw std::invalid_argument("models_dir is required");
    }
#endif
    const echoir::models::Registry registry =
        params.registry_json != nullptr ? echoir::models::Registry::parse(params.registry_json)
                                        : echoir::models::Registry::builtin();
    std::string model_id = params.model_id != nullptr ? params.model_id : "";
    if (model_id.empty()) {
        model_id = registry.default_translator();
        if (model_id.empty()) {
            throw std::invalid_argument("model_id is required: the registry names no default");
        }
    }
    const echoir::models::Model* model = registry.find(model_id);
    if (model == nullptr) {
        throw std::invalid_argument("'" + model_id + "' is not an id in the registry");
    }
    if (model->backend != "translator") {
        throw std::invalid_argument("'" + model_id + "' is not a translator");
    }
    const std::string style_name = model->entry.value("prompt_style", std::string());
    const std::optional<echoir::translate::PromptStyle> style =
        echoir::translate::parse_prompt_style(style_name);
    if (!style) {
        throw std::invalid_argument("registry entry '" + model_id +
                                    "' has an unknown prompt_style '" + style_name + "'");
    }
    const std::filesystem::path dir = params.models_dir != nullptr
                                          ? echoir::text::path_from_utf8(params.models_dir)
                                          : std::filesystem::path();
    echoir::TranslatorConfig config;
    config.model_path = echoir::text::path_utf8(echoir::models::resolve(dir, *model, "model"));
    config.style = *style;
    config.use_gpu = params.struct_size >= kEndOfUseGpu ? params.use_gpu != 0 : true;
    config.n_threads = params.struct_size >= kEndOfThreads ? params.n_threads : 0;

    auto translator = std::make_unique<echoir_translator>();
    translator->model = echoir::make_translator(config);
    translator->result.struct_size = sizeof(echoir_translation);
    return translator;
}

}  // namespace

echoir_translator* echoir_translator_create_from_model(const echoir_translator_params* params) {
    try {
        if (params == nullptr) {
            throw std::invalid_argument("params must not be NULL");
        }
        return make_translator(*params).release();
    } catch (...) {
        status_of_current_exception("echoir_translator_create_from_model");
        return nullptr;
    }
}

const echoir_translation* echoir_translator_translate(echoir_translator* translator,
                                                      const echoir_translate_request* request) {
    try {
        constexpr size_t kEndOfTarget =
            offsetof(echoir_translate_request, target) + sizeof(echoir_translate_request::target);
        constexpr size_t kEndOfContext =
            offsetof(echoir_translate_request, context) + sizeof(echoir_translate_request::context);
        if (translator == nullptr) {
            throw std::invalid_argument("translator must not be NULL");
        }
        if (request == nullptr) {
            throw std::invalid_argument("request must not be NULL");
        }
        if (request->struct_size < kEndOfTarget) {
            throw std::invalid_argument("struct_size is smaller than the fields up to target");
        }
        if (request->struct_size > sizeof(echoir_translate_request)) {
            throw std::invalid_argument("struct_size is larger than echoir_translate_request");
        }
        if (request->text == nullptr) {
            throw std::invalid_argument("text must not be NULL");
        }
        if (request->target == nullptr || request->target[0] == '\0') {
            throw std::invalid_argument("target must not be NULL or empty");
        }
        echoir::translate::Request in;
        in.text = request->text;
        in.source = request->source != nullptr ? request->source : "";
        in.target = request->target;
        if (request->struct_size >= kEndOfContext && request->context != nullptr) {
            in.context = request->context;
        }

        const std::lock_guard<std::mutex> lock(translator->mutex);
        const echoir::translate::Result out = translator->model->translate(in);
        translator->text = out.text;
        echoir_translation& result = translator->result;
        result.text = translator->text.c_str();
        result.capped = out.capped ? 1 : 0;
        result.source_tokens = out.source_tokens;
        result.prompt_tokens = out.prompt_tokens;
        result.output_tokens = out.output_tokens;
        result.prefill_ms = out.prefill_ms;
        result.decode_ms = out.decode_ms;
        return &result;
    } catch (...) {
        status_of_current_exception("echoir_translator_translate");
        return nullptr;
    }
}

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
                   "the VAD and backends are not built (ECHOIR_WITH_WHISPER=OFF)",
                   ECHOIR_ERR_NOT_IMPLEMENTED);
    return nullptr;
}

echoir_engine* echoir_engine_create_from_model(const echoir_model_params*) {
    set_last_error("echoir_engine_create_from_model",
                   "the VAD and backends are not built (ECHOIR_WITH_WHISPER=OFF)",
                   ECHOIR_ERR_NOT_IMPLEMENTED);
    return nullptr;
}

echoir_translator* echoir_translator_create_from_model(const echoir_translator_params*) {
    set_last_error("echoir_translator_create_from_model",
                   "the backends are not built (ECHOIR_WITH_WHISPER=OFF)",
                   ECHOIR_ERR_NOT_IMPLEMENTED);
    return nullptr;
}

const echoir_translation* echoir_translator_translate(echoir_translator*,
                                                      const echoir_translate_request*) {
    set_last_error("echoir_translator_translate",
                   "the backends are not built (ECHOIR_WITH_WHISPER=OFF)",
                   ECHOIR_ERR_NOT_IMPLEMENTED);
    return nullptr;
}
#endif

const char* echoir_models_json(const char* registry_json, const char* models_dir) {
    const char* fn = "echoir_models_json";
    try {
        const echoir::models::Registry registry =
            registry_json != nullptr ? echoir::models::Registry::parse(registry_json)
                                     : echoir::models::Registry::builtin();
        const std::filesystem::path dir = models_dir != nullptr
                                              ? echoir::text::path_from_utf8(models_dir)
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

void echoir_translator_free(echoir_translator* translator) { delete translator; }

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
    const bool has_detail = ev.type == ECHOIR_EVENT_ERROR || ev.type == ECHOIR_EVENT_STATS;
    out.detail = has_detail ? ev.detail.c_str() : nullptr;
    return out;
}

// Runs one session call and maps its exception to a status.
template <typename Call>
echoir_status run_session_call(echoir_session* s, const char* fn, Call&& call) noexcept {
    if (s == nullptr) {
        set_last_error(fn, "session must not be NULL", ECHOIR_ERR_INVALID_ARG);
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
        set_last_error(fn, "channels must be positive", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (n_frames > 0 && pcm == nullptr) {
        set_last_error(fn, "pcm must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (n_frames > SIZE_MAX / static_cast<size_t>(channels)) {
        set_last_error(fn, "n_frames is too large", ECHOIR_ERR_INVALID_ARG);
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
    constexpr size_t kEndOfTapeTrack =
        offsetof(echoir_session_params, tape_track) + sizeof(params.tape_track);
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
    config.stats_every_ticks = echoir::kStatsEveryTicks;
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
    // The tape and its track are read together: a struct that ends between them has no tape.
    if (params.struct_size >= kEndOfTapeTrack && params.tape != nullptr) {
        if (params.tape_track == nullptr || params.tape_track[0] == '\0') {
            throw std::invalid_argument("tape_track is required with a tape");
        }
        config.tape = params.tape->tape;
        config.tape_track = params.tape_track;
    }
    return config;
}

}  // namespace

echoir_session* echoir_session_create(echoir_engine* engine, const echoir_session_params* params,
                                      echoir_event_cb cb, void* user) {
    const char* fn = "echoir_session_create";
    if (params == nullptr) {
        set_last_error(fn, "params must not be NULL", ECHOIR_ERR_INVALID_ARG);
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
        set_last_error(fn, "engine must not be NULL", ECHOIR_ERR_INVALID_ARG);
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
        s->session = std::make_unique<echoir::Session>(
            *engine->backend, *engine->vad, std::move(config), std::move(sink), engine->gate);
        return s.release();
    } catch (...) {
        status_of_current_exception(fn);
        return nullptr;
    }
}

void echoir_session_free(echoir_session* s) { delete s; }

echoir_status echoir_session_poll(echoir_session* s, echoir_event* out) {
    const char* fn = "echoir_session_poll";
    if (s == nullptr || out == nullptr) {
        set_last_error(fn, "session and out must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (s->queue == nullptr) {
        set_last_error(fn, "the session is in callback mode", ECHOIR_ERR_STATE);
        return ECHOIR_ERR_STATE;
    }
    try {
        if (!s->queue->pop(s->polled)) {
            // An empty queue is not a failure, so the last error and status stay as they are.
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
        set_last_error(fn, "the session captures its own audio", ECHOIR_ERR_STATE);
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
        set_last_error(fn, "the session captures its own audio", ECHOIR_ERR_STATE);
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
        set_last_error(fn, "session must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (source != ECHOIR_SOURCE_MIC && source != ECHOIR_SOURCE_LOOPBACK) {
        set_last_error(fn, "source is not a known source", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    if (s->feed != nullptr) {
        set_last_error(fn, "the session already captures its own audio", ECHOIR_ERR_STATE);
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

echoir_status echoir_session_capture_state(echoir_session* s, int32_t* out_state) {
    const char* fn = "echoir_session_capture_state";
    if (s == nullptr || out_state == nullptr) {
        set_last_error(fn, "session and out_state must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    // The feed is set by start_capture and dropped by stop, so no feed is a session that does not
    // capture.
    const echoir::capture::CaptureFeed* feed = s->feed.get();
    if (feed == nullptr) {
        *out_state = ECHOIR_CAPTURE_OFF;
    } else if (feed->ended()) {
        *out_state = ECHOIR_CAPTURE_ENDED;
    } else if (feed->waiting()) {
        *out_state = ECHOIR_CAPTURE_WAITING;
    } else {
        *out_state = ECHOIR_CAPTURE_LIVE;
    }
    return ECHOIR_OK;
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
        set_last_error(fn, "n_frames must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    *n_frames = 0;
    return run_session_call(s, fn, [&](echoir::Session& session) {
        *n_frames = session.read_audio(start_ms, end_ms, out, cap);
    });
}

echoir_status echoir_session_read_peaks(echoir_session* s, int64_t start_ms, int64_t end_ms,
                                        int32_t bucket_ms, float* out_minmax, size_t cap,
                                        size_t* n_buckets) {
    const char* fn = "echoir_session_read_peaks";
    if (n_buckets == nullptr) {
        set_last_error(fn, "n_buckets must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    *n_buckets = 0;
    return run_session_call(s, fn, [&](echoir::Session& session) {
        *n_buckets = session.read_peaks(start_ms, end_ms, bucket_ms, out_minmax, cap);
    });
}

echoir_status echoir_session_set_input_muted(echoir_session* s, int32_t muted) {
    return run_session_call(s, "echoir_session_set_input_muted", [muted](echoir::Session& session) {
        session.set_input_muted(muted != 0);
    });
}

echoir_status echoir_session_read_lag(echoir_session* s, echoir_lag* out) {
    const char* fn = "echoir_session_read_lag";
    if (out == nullptr) {
        set_last_error(fn, "out must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    // The size of the first version of echoir_lag. A host built against it keeps working when the
    // struct grows; a host built against a later header gets the fields past this version zeroed.
    constexpr size_t kEndOfTickMs = offsetof(echoir_lag, tick_ms) + sizeof(out->tick_ms);
    if (out->struct_size < kEndOfTickMs) {
        set_last_error(fn, "struct_size is smaller than the fields up to tick_ms",
                       ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    return run_session_call(s, fn, [out](echoir::Session& session) {
        const echoir::SessionLag lag = session.lag();
        if (out->struct_size > sizeof(echoir_lag)) {
            std::memset(reinterpret_cast<unsigned char*>(out) + sizeof(echoir_lag), 0,
                        out->struct_size - sizeof(echoir_lag));
        }
        out->head_ms = lag.head_ms;
        out->decoded_ms = lag.decoded_ms;
        out->backlog_ms = lag.backlog_ms;
        out->tick_ms = lag.tick_ms;
    });
}
echoir_status echoir_session_tape_offset_ms(echoir_session* s, int64_t* out_ms) {
    const char* fn = "echoir_session_tape_offset_ms";
    if (out_ms == nullptr) {
        set_last_error(fn, "out_ms must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    return run_session_call(s, fn, [out_ms](echoir::Session& session) {
        const std::int64_t offset = session.tape_offset_ms();
        if (offset < 0) {
            throw echoir::StateError("the session has no tape");
        }
        *out_ms = offset;
    });
}
#else
// Without the engine no session can be created, so every session call reports that the backends
// are missing.
namespace {
echoir_status not_built(const char* fn) noexcept {
    set_last_error(fn, "the VAD and backends are not built (ECHOIR_WITH_WHISPER=OFF)",
                   ECHOIR_ERR_NOT_IMPLEMENTED);
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

echoir_status echoir_session_capture_state(echoir_session*, int32_t*) {
    return not_built("echoir_session_capture_state");
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

echoir_status echoir_session_read_lag(echoir_session*, echoir_lag*) {
    return not_built("echoir_session_read_lag");
}

echoir_status echoir_session_tape_offset_ms(echoir_session*, int64_t*) {
    return not_built("echoir_session_tape_offset_ms");
}
#endif

namespace {

// Runs call on the tape of t, mapping exceptions to a status.
template <typename Call>
echoir_status run_tape_call(echoir_tape* t, const char* fn, Call&& call) noexcept {
    if (t == nullptr) {
        set_last_error(fn, "tape must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    try {
        call(*t->tape);
        return ECHOIR_OK;
    } catch (...) {
        return status_of_current_exception(fn);
    }
}

}  // namespace

echoir_tape* echoir_tape_create(const char* folder) {
    const char* fn = "echoir_tape_create";
    try {
        const std::filesystem::path path = folder != nullptr && folder[0] != '\0'
                                               ? echoir::text::path_from_utf8(folder)
                                               : std::filesystem::path();
        auto t = std::make_unique<echoir_tape>();
        t->tape = echoir::stream::Tape::create(path);
        return t.release();
    } catch (...) {
        status_of_current_exception(fn);
        return nullptr;
    }
}

echoir_tape* echoir_tape_open(const char* folder) {
    const char* fn = "echoir_tape_open";
    if (folder == nullptr || folder[0] == '\0') {
        set_last_error(fn, "folder must not be NULL or empty", ECHOIR_ERR_INVALID_ARG);
        return nullptr;
    }
    try {
        auto t = std::make_unique<echoir_tape>();
        t->tape = echoir::stream::Tape::open(echoir::text::path_from_utf8(folder));
        return t.release();
    } catch (...) {
        status_of_current_exception(fn);
        return nullptr;
    }
}

echoir_status echoir_tape_add_track(echoir_tape* t, const char* name, const char* label) {
    const char* fn = "echoir_tape_add_track";
    if (name == nullptr) {
        set_last_error(fn, "name must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    return run_tape_call(t, fn, [&](echoir::stream::Tape& tape) {
        tape.add_track(name, label != nullptr ? label : "");
    });
}

const char* echoir_tape_tracks_json(echoir_tape* t) {
    const char* result = nullptr;
    const echoir_status status = run_tape_call(
        t, "echoir_tape_tracks_json",
        [&](echoir::stream::Tape& tape) { result = set_json_result(tape.tracks_json()); });
    return status == ECHOIR_OK ? result : nullptr;
}

echoir_status echoir_tape_duration_ms(echoir_tape* t, int64_t* out_ms) {
    const char* fn = "echoir_tape_duration_ms";
    if (out_ms == nullptr) {
        set_last_error(fn, "out_ms must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    return run_tape_call(t, fn,
                         [out_ms](echoir::stream::Tape& tape) { *out_ms = tape.duration_ms(); });
}

echoir_status echoir_tape_read_audio(echoir_tape* t, const char* track, int64_t start_ms,
                                     int64_t end_ms, int16_t* out, size_t cap, size_t* n_frames) {
    const char* fn = "echoir_tape_read_audio";
    if (n_frames == nullptr || track == nullptr) {
        set_last_error(fn, "track and n_frames must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    *n_frames = 0;
    return run_tape_call(t, fn, [&](echoir::stream::Tape& tape) {
        *n_frames = tape.read_audio(track, start_ms, end_ms, out, cap);
    });
}

echoir_status echoir_tape_read_peaks(echoir_tape* t, const char* track, int64_t start_ms,
                                     int64_t end_ms, int32_t bucket_ms, float* out_minmax,
                                     size_t cap, size_t* n_buckets) {
    const char* fn = "echoir_tape_read_peaks";
    if (n_buckets == nullptr || track == nullptr) {
        set_last_error(fn, "track and n_buckets must not be NULL", ECHOIR_ERR_INVALID_ARG);
        return ECHOIR_ERR_INVALID_ARG;
    }
    *n_buckets = 0;
    return run_tape_call(t, fn, [&](echoir::stream::Tape& tape) {
        *n_buckets = tape.read_peaks(track, start_ms, end_ms, bucket_ms, out_minmax, cap);
    });
}

echoir_status echoir_tape_checkpoint(echoir_tape* t) {
    return run_tape_call(t, "echoir_tape_checkpoint",
                         [](echoir::stream::Tape& tape) { tape.checkpoint(); });
}

echoir_status echoir_tape_close(echoir_tape* t) {
    return run_tape_call(t, "echoir_tape_close", [](echoir::stream::Tape& tape) { tape.close(); });
}

void echoir_tape_free(echoir_tape* t) {
    if (t == nullptr) {
        return;
    }
    // A close that fails leaves the page files, which is all that can be done here.
    try {
        t->tape->close();
    } catch (...) {
    }
    delete t;
}
