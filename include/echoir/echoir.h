/*
 * echoir.h - public C ABI of echoir-core.
 *
 * Plain C99, usable from C, C++ and managed hosts. Names, enum values, struct fields and their
 * order are the binary interface: changing any of them breaks existing callers.
 *
 * Rules that apply to every function unless its comment says otherwise:
 *  - A function that can fail returns echoir_status, or a pointer that is NULL on failure. The
 *    reason is in echoir_last_error(), which is per thread. The void functions (the *_params_init
 *    and *_free calls) are the exception.
 *  - No C++ exception crosses this boundary.
 *  - A handle (engine or session) is used by one thread at a time. The echoir_session_push_*
 *    functions are the exception: they are thread-safe.
 *  - Freeing NULL is a no-op.
 */
#ifndef ECHOIR_ECHOIR_H
#define ECHOIR_ECHOIR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Export macro. ECHOIR_BUILD is defined while building the library itself. ECHOIR_STATIC is
 * defined by consumers of a static build, where no import or export decoration is needed.
 */
#if defined(ECHOIR_STATIC)
#define ECHOIR_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#if defined(ECHOIR_BUILD)
#define ECHOIR_API __declspec(dllexport)
#else
#define ECHOIR_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ECHOIR_API __attribute__((visibility("default")))
#else
#define ECHOIR_API
#endif

// The declarations below are aligned by hand, so clang-format is off for this block.
// clang-format off

// Result of a call. ECHOIR_OK is 0. Every other value is a failure, and echoir_last_error()
// gives the reason.
//   ECHOIR_ERR_INVALID_ARG       a NULL or out-of-range argument, such as a bad sample rate.
//   ECHOIR_ERR_MODEL_LOAD        a model file is missing, unreadable or not the expected format.
//   ECHOIR_ERR_BACKEND           a backend (whisper.cpp or llama.cpp) failed to set up or decode.
//   ECHOIR_ERR_STATE             the call is not valid now: wrong driving mode, the engine already
//                                serves a session, or the event queue is empty.
//   ECHOIR_ERR_NOT_IMPLEMENTED   the function is declared but not available in this build.
//   ECHOIR_ERR_INTERNAL          an unexpected failure inside the core.
typedef enum { ECHOIR_OK = 0, ECHOIR_ERR_INVALID_ARG, ECHOIR_ERR_MODEL_LOAD,
               ECHOIR_ERR_BACKEND, ECHOIR_ERR_STATE, ECHOIR_ERR_NOT_IMPLEMENTED,
               ECHOIR_ERR_INTERNAL } echoir_status;

// Model family. Whisper takes a ggml .bin. Qwen3-ASR takes a text .gguf and a projector .gguf.
typedef enum { ECHOIR_BACKEND_WHISPER = 1, ECHOIR_BACKEND_QWEN3_ASR = 2 } echoir_backend;

// Model configuration for echoir_engine_create(). Start from echoir_engine_params_init() and
// change the fields you need. Paths are UTF-8 on every platform, and are read during the create
// call only.
typedef struct {
  uint32_t       struct_size;          // sizeof(echoir_engine_params); lets the struct grow
  echoir_backend backend;
  const char*    model_path;           // whisper .bin | qwen text .gguf
  const char*    mmproj_path;          // qwen only, else NULL
  const char*    vad_model_path;       // Silero ggml .bin or FireRedVAD .gguf (required)
  int32_t        use_gpu;              // 1 = GPU backend built into the library, 0 = CPU
  int32_t        n_threads;            // CPU threads; 0 = default (4)
} echoir_engine_params;

// How the text of the ongoing segment (its PARTIAL events) is stabilized, for
// echoir_session_params.stream_policy.
//   DEFAULT    ROLLBACK on a backend with decoder prefix support (Qwen3-ASR), else AGREEMENT.
//   AGREEMENT  each PARTIAL is stabilized against the earlier decodes of its segment, so words
//              that they agree on are kept. Any backend.
//   ROLLBACK   each decode of the segment gets the previous decode's output as a decoder prefix,
//              minus its last 5 tokens, so the start of the text stays fixed. The first two decodes
//              of a segment get no prefix. Needs a backend with decoder prefix support (Qwen3-ASR);
//              echoir_session_create() returns NULL on other backends.
typedef enum { ECHOIR_POLICY_DEFAULT = 0, ECHOIR_POLICY_AGREEMENT = 1,
               ECHOIR_POLICY_ROLLBACK = 2 } echoir_stream_policy;

// Streaming options for echoir_session_create(). Start from echoir_session_params_init().
typedef struct {
  uint32_t    struct_size;             // sizeof(echoir_session_params). A smaller value is accepted
                                       // if it covers every field up to emit_partials; the fields
                                       // after it take their defaults. A larger value is refused.
  const char* language;                // "en", "zh", ...; "auto" lets Whisper detect (default)
  int32_t     min_chunk_ms;            // a decode runs once more than this many ms are buffered
                                       // (default 500)
  // Forced cut: a segment that reaches this length, in ms counted from its start, is split at its
  // latest pause, or without one at a quiet point in the 3 s before that limit. At least 200.
  int32_t     max_segment_ms;          // default 10000
  int32_t     emit_partials;           // 1 = emit PARTIAL events for the ongoing segment (default)
  int32_t     stream_policy;           // echoir_stream_policy; default ECHOIR_POLICY_DEFAULT
  // How many ms of the newest audio the session keeps for echoir_session_read_audio(), as 16 kHz
  // mono 16-bit samples (about 1.9 MB per minute). Default 0: no audio is kept. The level envelope
  // of echoir_session_read_peaks() is kept either way.
  int32_t     retain_audio_ms;
} echoir_session_params;

// Kind of an echoir_event.
//   PARTIAL  provisional text of the ongoing segment. A FINAL with the same segment_id replaces it.
//   FINAL    text of a finished segment.
//   CLEARED  the transcript was cleared by echoir_session_clear(). Drop what is shown.
//   ERROR    a failure while streaming. detail holds the message.
//   STATS    reserved. This version never sends it. When it is sent, detail holds a JSON object.
typedef enum { ECHOIR_EVENT_PARTIAL = 1, ECHOIR_EVENT_FINAL = 2, ECHOIR_EVENT_CLEARED = 3,
               ECHOIR_EVENT_ERROR = 4, ECHOIR_EVENT_STATS = 5 } echoir_event_type;

// One event from a session, delivered by callback or taken with echoir_session_poll().
typedef struct {
  echoir_event_type type;
  int64_t     segment_id;              // monotonic per session; a PARTIAL and its FINAL share it
  int64_t     start_ms, end_ms;        // milliseconds since session start or the last reset
  const char* text;                    // UTF-8. Valid only during the callback, or until the next
                                       // poll or free in queue mode.
  const char* detail;                  // ERROR message; NULL otherwise
} echoir_event;

// Event callback, given to echoir_session_create(). It runs on the thread that drives the
// session (see echoir_session_process() and echoir_session_start()). ev is valid only during the
// call, so copy what you need. user is the pointer given at create, passed back unchanged. The
// callback must not call any function on its own session, except echoir_session_push_*.
typedef void (*echoir_event_cb)(const echoir_event* ev, void* user);

// Loaded models. Heavy to create, so create one, reuse it and free it when done. An engine
// serves one session at a time.
typedef struct echoir_engine  echoir_engine;
// Streaming state over one engine. Free it before the engine it was created from.
typedef struct echoir_session echoir_session;

// Library version, for example "3.0.0". Static storage, never NULL.
ECHOIR_API const char*    echoir_version(void);

// Message of this thread's most recent failing call, in the form "<function>: <reason>". Empty
// before any failure, never NULL. The pointer stays valid until this thread's next failing call.
ECHOIR_API const char*    echoir_last_error(void);

// Sets *p to the defaults (Whisper backend, use_gpu 1, n_threads 0, all paths NULL) and sets
// struct_size. With p NULL, nothing is written and the last error is set.
ECHOIR_API void           echoir_engine_params_init(echoir_engine_params*);

// Sets *p to the defaults (language "auto", min_chunk_ms 500, max_segment_ms 10000,
// emit_partials 1, stream_policy ECHOIR_POLICY_DEFAULT) and sets struct_size. With p NULL, nothing
// is written and the last error is set.
ECHOIR_API void           echoir_session_params_init(echoir_session_params*);

// Loads the models named in *params and returns a new engine, or NULL on failure. Loading takes a
// while, so run it off the UI thread. Free the engine with echoir_engine_free(). The path strings
// are read during this call only.
ECHOIR_API echoir_engine* echoir_engine_create(const echoir_engine_params*);

// Frees an engine and its models. Free its sessions first.
ECHOIR_API void           echoir_engine_free(echoir_engine*);

// Creates a session on engine, or returns NULL on failure (for example when the engine already
// serves a session, max_segment_ms is below 200, or stream_policy is unknown or ROLLBACK on a
// backend without prefix support). *params is read during this call only. With cb
// set (callback mode), events go to cb on the driving thread. With cb NULL (queue mode), events
// wait for echoir_session_poll(). The mode is fixed here. user is passed back to cb unchanged. The
// engine and, in callback mode, cb and user must stay valid until the session is freed.
ECHOIR_API echoir_session* echoir_session_create(echoir_engine*, const echoir_session_params*,
                                                 echoir_event_cb cb, void* user);

// Frees a session. If its worker is running, the worker is stopped first, as by
// echoir_session_stop(). Do not call it from the session's own callback: the process then aborts.
ECHOIR_API void           echoir_session_free(echoir_session*);

// Queue mode only. Copies the next queued event into *out and returns ECHOIR_OK. Returns
// ECHOIR_ERR_STATE when the queue is empty, or when the session is in callback mode. out->text and
// out->detail stay valid until the next poll or free. Consecutive PARTIALs of the same segment
// coalesce into the newest one. Once 1024 events are queued, a new PARTIAL is dropped, and a new
// FINAL, CLEARED or ERROR event evicts the oldest queued PARTIAL, if there is one, to make room.
// FINAL, CLEARED and ERROR events are never dropped, so the queue grows past 1024 if the host stops
// polling. Poll from one thread, for example a UI timer.
ECHOIR_API echoir_status  echoir_session_poll(echoir_session*, echoir_event* out);

// Appends audio. pcm holds n_frames frames of interleaved samples (channels samples per frame),
// nominally in [-1, 1]. sample_rate is in Hz and may be any rate. Audio is resampled internally
// to 16 kHz mono f32. The call copies the samples and never waits for a decode. Thread-safe.
// Returns ECHOIR_ERR_INVALID_ARG for a sample_rate or channels that is not positive, a NULL pcm
// with frames, or a NaN or infinite sample (push_f32 only). Returns ECHOIR_ERR_STATE while the
// session captures its own audio.
ECHOIR_API echoir_status  echoir_session_push_f32(echoir_session*, const float* pcm,
                                                  size_t n_frames, int32_t sample_rate,
                                                  int32_t channels);

// As echoir_session_push_f32(), for interleaved signed 16-bit samples (full scale is 32768).
ECHOIR_API echoir_status  echoir_session_push_s16(echoir_session*, const int16_t* pcm,
                                                  size_t n_frames, int32_t sample_rate,
                                                  int32_t channels);

// Driving mode A. Runs one decode tick on the calling thread if enough audio (min_chunk_ms) is
// buffered, and does nothing otherwise. Events go to the callback on this thread, or to the queue
// in queue mode. Returns ECHOIR_ERR_STATE if the session runs in mode B. Returns ECHOIR_ERR_BACKEND
// when the VAD or the decoder fails, after an ERROR event. The audio stays buffered, so a later
// call retries it. Returns ECHOIR_ERR_INVALID_ARG when the decoder rejects the request, such as an
// unsupported language, or when the VAD rejects the audio window as too long for it.
ECHOIR_API echoir_status  echoir_session_process(echoir_session*);

// Driving mode B. Starts the session's worker thread, which decodes audio as it arrives. Events
// are delivered on the worker thread, or queued for poll. flush, reset and clear deliver their
// events on the calling thread, as in mode A. Returns ECHOIR_ERR_STATE if the session runs in
// mode A, or is already started. Stop it with echoir_session_stop().
ECHOIR_API echoir_status  echoir_session_start(echoir_session*);

// Driving mode B. Stops the worker and waits for it to exit. No callback runs after this returns.
// Does not flush: audio that was not decoded stays buffered. The exception is a session that
// captures its own audio: then the device closes first, the audio it captured is decoded and the
// ongoing segment is finalized, as echoir_session_flush() does, and FINAL events may arrive before
// this returns. Does nothing if the session is not started. The session stays in mode B: process()
// still returns ECHOIR_ERR_STATE afterwards, and echoir_session_start() can run again.
ECHOIR_API echoir_status  echoir_session_stop(echoir_session*);

// Decodes all buffered audio and finalizes the ongoing segment as a FINAL event. The events are
// delivered on the calling thread, also in mode B. In mode B the call waits for a decode that the
// worker is running. Errors are those of echoir_session_process().
ECHOIR_API echoir_status  echoir_session_flush(echoir_session*);

// As echoir_session_flush(), then drops all audio and text state, even when the final decode
// fails. Timestamps count from this call, so the next segment starts at 0.
ECHOIR_API echoir_status  echoir_session_reset(echoir_session*);

// Drops the in-flight transcript text and emits ECHOIR_EVENT_CLEARED on the calling thread.
// Timestamps keep running. Use echoir_session_reset() to start the timeline over.
ECHOIR_API echoir_status  echoir_session_clear(echoir_session*);

// Audio store, level envelope and input mute. The session keeps the audio it takes in, after
// conversion to 16 kHz mono, on the session timeline: the same milliseconds as the event times,
// including the silence pushed for capture gaps, and restarting at 0 with echoir_session_reset().
// The store lives until the session is freed, so it can be read after echoir_session_stop(). These
// calls are thread-safe and may run on any thread, in any mode.

// Copies the kept audio of [start_ms, end_ms) as 16 kHz mono 16-bit samples into out, which holds
// cap samples, and sets *n_frames to the number copied. end_ms is clamped to the newest sample, so
// a range past it is shorter, or empty. With out NULL, only *n_frames is set: the number the range
// needs. Returns ECHOIR_ERR_STATE when the session keeps no audio (retain_audio_ms 0), or when any
// part of the range is older than what is kept. Returns ECHOIR_ERR_INVALID_ARG for a negative
// start_ms, end_ms before start_ms, n_frames NULL, or a cap too small for the range.
ECHOIR_API echoir_status  echoir_session_read_audio(echoir_session*, int64_t start_ms,
                                                    int64_t end_ms, int16_t* out, size_t cap,
                                                    size_t* n_frames);

// The level envelope of [start_ms, end_ms): one min/max pair (full scale ±1) per bucket of
// bucket_ms, a positive multiple of 10. The envelope keeps one pair per 10 ms for the whole
// session. out_minmax holds 2 floats per bucket, min then max, for cap buckets, and *n_buckets is
// set to the number written. end_ms is clamped to the newest whole 10 ms, and the last bucket may
// be shorter. With out_minmax NULL, only *n_buckets is set: the number the range needs. The count
// over [0, INT64_MAX) at bucket_ms 10 is the envelope's length in 10 ms steps, and its newest
// bucket is the input level. Returns ECHOIR_ERR_INVALID_ARG for a bad bucket_ms, a negative start_ms, end_ms before
// start_ms, n_buckets NULL, or a cap too small for the range.
ECHOIR_API echoir_status  echoir_session_read_peaks(echoir_session*, int64_t start_ms,
                                                    int64_t end_ms, int32_t bucket_ms,
                                                    float* out_minmax, size_t cap,
                                                    size_t* n_buckets);

// muted 1: audio entering the session, pushed or captured, is replaced by silence of the same
// length, so the timeline keeps running and the VAD ends the ongoing segment. The store keeps the
// silence. muted 0 resumes. Takes effect from the next audio the session converts.
ECHOIR_API echoir_status  echoir_session_set_input_muted(echoir_session*, int32_t muted);

// Capture. The library opens an audio device and pushes its audio into a session in live mode.
// The device's samples are read on a library thread, so the host only starts and stops it.
typedef enum { ECHOIR_SOURCE_MIC = 1, ECHOIR_SOURCE_LOOPBACK = 2 } echoir_source;

// The audio devices as JSON, UTF-8:
//   {"capture":[{"id":"…","name":"…","default":true}, …],"loopback":[…]}
// The id is stable across runs, and is the device_id of echoir_session_start_capture(). loopback
// is [] where the audio backend cannot record playback devices (only WASAPI can, on Windows). The
// string is owned by the library and stays valid until the next *_json call on this thread. NULL
// on failure. With ECHOIR_AUDIO_BACKEND=null in the environment, the list holds the null backend's
// one silent device, which is for tests.
ECHOIR_API const char*    echoir_devices_json(void);

// Starts the session in live mode, as echoir_session_start(), with an open device whose audio is
// pushed into the session. device_id NULL or "" is the system default device, and the capture
// follows the default when it changes. Otherwise device_id is an id from echoir_devices_json().
// While the session captures, the push functions return ECHOIR_ERR_STATE. Loopback where the audio
// backend has none returns ECHOIR_ERR_NOT_IMPLEMENTED, and an unknown device_id returns
// ECHOIR_ERR_INVALID_ARG. Works in callback and queue mode.
//
// Timestamps follow the wall clock from the start of the capture: when the device sends nothing
// for a moment (loopback while no sound plays, or a lost default device), silence is pushed for
// the time that passed.
//
// If the chosen device is lost, an ERROR event with detail "device_lost" is sent, and the capture
// ends. The session keeps running until it is stopped. If the default device is lost, the capture
// reopens the new default, and when the system has no device at all it keeps trying (every second)
// until one appears or the session is stopped. No event is sent for that wait.
ECHOIR_API echoir_status  echoir_session_start_capture(echoir_session*, echoir_source,
                                                       const char* device_id);

// Model catalogue. The entries are those of models/registry.json, which the library builds in.
// Each file of an entry lives at <models_dir>/<file name>. The catalogue gives the size of each
// file, and a file is present when it exists there with that size.

// Model selection for echoir_engine_create_from_model(). Start from echoir_model_params_init(),
// then set model_id and models_dir. The paths are read during the call only.
//
// The engine loads the model's own files, and the VAD named by vad_id, or the backend's default VAD
// from the catalogue when vad_id is NULL or "". registry_json replaces the built-in catalogue for
// a host with its own list.
typedef struct {
  uint32_t    struct_size;             // sizeof(echoir_model_params). A smaller value is accepted if
                                       // it covers every field up to model_id; the fields after it
                                       // take their defaults. A larger value is refused.
  const char* registry_json;           // catalogue text in the registry format; NULL = built-in
  const char* models_dir;              // UTF-8 directory with the model files (required for create)
  const char* model_id;                // registry id of a whisper or qwen3-asr entry
  const char* vad_id;                  // registry id of a VAD; NULL or "" = the default VAD of the
                                       // model's backend
  int32_t     use_gpu;                 // 1 = GPU backend built into the library, 0 = CPU
  int32_t     n_threads;               // CPU threads; 0 = default
} echoir_model_params;

// Sets *p to the defaults (registry_json, models_dir, model_id and vad_id NULL; use_gpu 1;
// n_threads 0) and sets struct_size. With p NULL, nothing is written and the last error is set.
ECHOIR_API void           echoir_model_params_init(echoir_model_params*);

// Resolves model_id and its VAD (and, for Qwen3-ASR, its projector) in the catalogue, checks that
// their files are in models_dir with the catalogue's sizes, and then creates the engine as
// echoir_engine_create() does. Free the engine with echoir_engine_free(). Returns NULL on failure.
// The last error names the function, the file and the registry id. An unknown or non-speech id is
// ECHOIR_ERR_INVALID_ARG, and a missing or wrong-size file is ECHOIR_ERR_MODEL_LOAD.
ECHOIR_API echoir_engine* echoir_engine_create_from_model(const echoir_model_params*);

// The catalogue and the machine as JSON, UTF-8:
//   {"devices":[{"name","type":"gpu"|"cpu","description","integrated","memory_total_mb",
//                "memory_free_mb"}],
//    "default_vad":{"whisper":id,"qwen":id},
//    "models":[{<entry>,"files":[{<file>,"present":bool}],"present":bool,"fits":bool}]}
// A device name is ggml's device id and description its human-readable name, which may be empty.
// integrated marks a GPU that shares system memory, so its memory size says little about speed.
// registry_json NULL = built-in catalogue. models_dir NULL or "" = no file is present. A file is
// present when it is in models_dir and has the size the catalogue gives. fits: min_vram_mb is at
// most the free memory of the best device: the GPU with the most free memory, else the CPU. An
// entry without min_vram_mb fits. devices is empty in a build without the backends. The string is
// owned by the library, and stays valid until the next *_json call on this thread. Returns NULL on
// failure, such as a registry_json that does not parse.
ECHOIR_API const char*    echoir_models_json(const char* registry_json, const char* models_dir);
// clang-format on

#ifdef __cplusplus
}
#endif

#endif /* ECHOIR_ECHOIR_H */
