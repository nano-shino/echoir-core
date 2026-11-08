/*
 * echoir.h - public C ABI of echoir-core.
 *
 * Plain C99, usable from C, C++ and managed hosts. Names, enum values, struct fields and their
 * order are the binary interface: changing any of them breaks existing callers.
 *
 * Rules that apply to every function unless its comment says otherwise:
 *  - A function that can fail returns echoir_status, or a pointer that is NULL on failure. The
 *    reason is in echoir_last_error() and the status in echoir_last_status(), both per thread.
 *    The void functions (the *_params_init and *_free calls) are the exception.
 *  - No C++ exception crosses this boundary.
 *  - A handle (engine or session) is used by one thread at a time. The echoir_session_push_*
 *    functions are the exception: they are thread-safe. Sessions of one engine are separate
 *    handles, so each may be driven on its own thread. A translator handle is thread-safe too.
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
//   ECHOIR_ERR_STATE             the call is not valid now, such as the wrong driving mode.
//                                echoir_session_poll() also returns it for an empty queue, which
//                                is not a failure.
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
//
// vad_threshold and vad_min_silence_ms tune the VAD; 0 keeps its own value (Silero: 0.2 and
// 1500 ms; FireRedVAD: 0.4 and 500 ms). vad_threshold is the speech probability needed for
// speech, above 0 and at most 1: higher hears less as speech. vad_min_silence_ms is the shortest
// pause that ends a speech range, from 0 to 60000: lower ends lines sooner. Other values make the
// create call fail with ECHOIR_ERR_INVALID_ARG.
typedef struct {
  uint32_t       struct_size;          // sizeof(echoir_engine_params). A smaller value is accepted
                                       // if it covers every field up to n_threads; the fields
                                       // after it take their defaults.
  echoir_backend backend;
  const char*    model_path;           // whisper .bin | qwen text .gguf
  const char*    mmproj_path;          // qwen only, else NULL
  const char*    vad_model_path;       // Silero ggml .bin or FireRedVAD .gguf (required)
  int32_t        use_gpu;              // 1 = GPU backend built into the library, 0 = CPU
  int32_t        n_threads;            // CPU threads of the speech model; 0 = default (4)
  float          vad_threshold;        // 0 = the VAD's own value (default)
  int32_t        vad_min_silence_ms;   // 0 = the VAD's own value (default)
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

// A session tape: the audio of a whole recording session across the sessions that write it. See
// the echoir_tape_* functions below.
typedef struct echoir_tape echoir_tape;

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
  // of echoir_session_read_peaks() is kept either way. A negative value makes
  // echoir_session_create() fail. Not used when tape is set.
  int32_t     retain_audio_ms;
  // The tape that keeps the session's audio, or NULL to keep it in the session (retain_audio_ms).
  // The session holds its own reference to the tape, so the tape may be freed first. With a tape,
  // echoir_session_read_audio() and echoir_session_read_peaks() read through to it.
  echoir_tape* tape;                   // default NULL
  // The track of the tape the session writes, such as "mic" or "system". Required with a tape;
  // a track the tape does not have is added, labelled with its name. Read during create only.
  const char* tape_track;              // default NULL
} echoir_session_params;

// Kind of an echoir_event.
//   PARTIAL  provisional text of the ongoing segment. A FINAL with the same segment_id replaces it.
//   FINAL    text of a finished segment.
//   CLEARED  the transcript was cleared by echoir_session_clear(). Drop what is shown.
//   ERROR    a failure while streaming. detail holds the message.
//   STATS    speed of the recent ticks, after every 10 ticks that decode audio. detail holds a JSON
//            object: {"rtf","decode_ms","window_ms","wait_ms","backend",...}. rtf is decode time
//            over audio time; decode_ms and window_ms are the mean per tick. wait_ms is the mean
//            time per tick spent waiting for other sessions of the engine, which decode_ms and rtf
//            leave out. start_ms and end_ms are 0.
typedef enum { ECHOIR_EVENT_PARTIAL = 1, ECHOIR_EVENT_FINAL = 2, ECHOIR_EVENT_CLEARED = 3,
               ECHOIR_EVENT_ERROR = 4, ECHOIR_EVENT_STATS = 5 } echoir_event_type;

// One event from a session, delivered by callback or taken with echoir_session_poll().
typedef struct {
  echoir_event_type type;
  int64_t     segment_id;              // monotonic per session; a PARTIAL and its FINAL share it
  int64_t     start_ms, end_ms;        // milliseconds since session start or the last reset
  const char* text;                    // UTF-8. Valid only during the callback, or until the next
                                       // poll or free in queue mode.
  const char* detail;                  // ERROR message, or STATS JSON object; NULL otherwise
} echoir_event;

// Event callback, given to echoir_session_create(). It runs on the thread that drives the
// session (see echoir_session_process() and echoir_session_start()). ev is valid only during the
// call, so copy what you need. user is the pointer given at create, passed back unchanged. The
// callback must not call any function on its own session, except echoir_session_push_*.
typedef void (*echoir_event_cb)(const echoir_event* ev, void* user);

// Loaded models. Heavy to create, so create one, reuse it and free it when done. An engine serves
// any number of sessions. Their VAD runs and decodes take turns on it in arrival order, so each
// adds to the others' lag; a session's events are the same as when it runs alone.
typedef struct echoir_engine  echoir_engine;
// Streaming state over one engine. Free it before the engine it was created from.
typedef struct echoir_session echoir_session;

// Library version, for example "3.0.0". Static storage, never NULL.
ECHOIR_API const char*    echoir_version(void);

// Message of this thread's most recent failing call, in the form "<function>: <reason>". Empty
// before any failure, never NULL. The pointer stays valid until this thread's next failing call.
ECHOIR_API const char*    echoir_last_error(void);

// Status of this thread's most recent failing call, the one echoir_last_error() describes:
// ECHOIR_OK before any failure. A call that returns a pointer reports its failure only as NULL,
// so this is how a host tells, say, a missing model file (ECHOIR_ERR_MODEL_LOAD) from a bad
// argument (ECHOIR_ERR_INVALID_ARG) after echoir_engine_create() or echoir_session_create().
// Read it right after the failing call, on the same thread.
ECHOIR_API echoir_status  echoir_last_status(void);

// Sets *p to the defaults (Whisper backend, use_gpu 1, n_threads and the VAD tuning 0, all paths
// NULL) and sets struct_size. With p NULL, nothing is written and the last error is set.
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

// Creates a session on engine, or returns NULL on failure (for example when max_segment_ms is
// below 200, or stream_policy is unknown or ROLLBACK on a backend without prefix support). An
// engine may already serve other sessions. A session's run on a tape starts at the tape's end when
// the session is created, so create every session of a recording before any of them takes audio
// to give their tracks the same start. *params is read during this call only. With cb
// set (callback mode), events go to cb on the driving thread. With cb NULL (queue mode), events
// wait for echoir_session_poll(). The mode is fixed here. user is passed back to cb unchanged. The
// engine and, in callback mode, cb and user must stay valid until the session is freed.
ECHOIR_API echoir_session* echoir_session_create(echoir_engine*, const echoir_session_params*,
                                                 echoir_event_cb cb, void* user);

// Frees a session. If its worker is running, the worker is stopped first, as by
// echoir_session_stop(). Do not call it from the session's own callback: the process then aborts.
ECHOIR_API void           echoir_session_free(echoir_session*);

// Queue mode only. Copies the next queued event into *out and returns ECHOIR_OK. Returns
// ECHOIR_ERR_STATE when the queue is empty, or when the session is in callback mode, so in queue
// mode ECHOIR_ERR_STATE means only that there is nothing to take. An empty queue is not a failure:
// it leaves echoir_last_error() and echoir_last_status() as they were. out->text and
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
//
// With a tape in echoir_session_params, the audio and the envelope are kept on the tape instead,
// and these calls read through to it on the session timeline: session time t is tape time
// echoir_session_tape_offset_ms() + t on the session's track. retain_audio_ms is not used then,
// and audio older than the tape keeps is ECHOIR_ERR_STATE, as an evicted range is.

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
// bucket_ms. The envelope keeps one pair per 10 ms cell for the whole session. A bucket is
// bucket_ms of audio, a positive multiple of 10, so it spans whole cells. out_minmax holds 2
// floats per bucket, min then max, for cap buckets, and *n_buckets is set to the number written.
// end_ms is clamped to the newest whole 10 ms, and the last bucket may be shorter. With
// out_minmax NULL, only *n_buckets is set: the number the range needs. The count over
// [0, INT64_MAX) at bucket_ms 10 is the envelope's length in cells, and its newest bucket is the
// input level. Returns ECHOIR_ERR_INVALID_ARG for a bad bucket_ms, a negative start_ms, end_ms
// before start_ms, n_buckets NULL, or a cap too small for the range.
ECHOIR_API echoir_status  echoir_session_read_peaks(echoir_session*, int64_t start_ms,
                                                    int64_t end_ms, int32_t bucket_ms,
                                                    float* out_minmax, size_t cap,
                                                    size_t* n_buckets);

// muted 1: audio entering the session, pushed or captured, is replaced by silence of the same
// length, so the timeline keeps running and the VAD ends the ongoing segment. The store keeps the
// silence. muted 0 resumes. Takes effect from the next audio the session converts.
ECHOIR_API echoir_status  echoir_session_set_input_muted(echoir_session*, int32_t muted);

// How far the decoding is behind the audio the session took in, in ms on the session timeline.
// backlog_ms is head_ms - decoded_ms: the audio waiting for a tick plus the new audio of the tick
// that runs. While a session keeps up it stays near min_chunk_ms plus one decode; when it grows
// steadily, the decoder is slower than real time. Set struct_size to sizeof(echoir_lag) before the
// call. A library older than the header fills the fields it knows and zeroes the rest of
// struct_size; a newer one writes only the fields that fit. Never waits for a decode. Returns
// ECHOIR_ERR_INVALID_ARG for out NULL or a struct_size that does not reach tick_ms.
typedef struct {
  uint32_t struct_size;  // sizeof(echoir_lag), set by the caller
  int64_t  head_ms;      // audio taken in
  int64_t  decoded_ms;   // end of the window of the latest tick that finished
  int64_t  backlog_ms;   // head_ms - decoded_ms
  int64_t  tick_ms;      // how long the running tick has run; 0 between ticks
} echoir_lag;
ECHOIR_API echoir_status  echoir_session_read_lag(echoir_session*, echoir_lag* out);

// Where time 0 of the session is on its tape, in ms, into *out_ms: the start of the session's run
// on its track, which moves with echoir_session_reset(). Returns ECHOIR_ERR_STATE for a session
// without a tape, and ECHOIR_ERR_INVALID_ARG for out_ms NULL.
ECHOIR_API echoir_status  echoir_session_tape_offset_ms(echoir_session*, int64_t* out_ms);

// Session tape. A tape keeps the audio of a recording session across the sessions that write it,
// for replay and for saving. It holds named tracks, one per input (such as "mic" and "system"),
// on one timeline that starts at 0. A session given a tape (echoir_session_params.tape) writes
// its 16 kHz mono audio to one track. A session created later starts at the end of the tape (its
// longest track, rounded up to whole 10 ms), so the time between two sessions is not stored. A
// track that is behind the end of the tape when a session starts on it is padded with silence up
// to there, so all tracks share the timeline. Several sessions may write several tracks at once;
// a track takes one session at a time.
//
// Storage, per track: the newest 2 minutes as 16-bit samples in memory. With a folder, older audio
// is encoded with Opus at 24 kbit/s (about 11 MB per hour) into one-minute Ogg Opus page files in
// the folder, written as each one fills, so a crash loses at most the minute being filled.
// echoir_tape_close() joins each track's pages into <track>.opus in the folder and deletes the
// page files. In file names, a track name keeps its ASCII letters, digits, '-' and '_', and every
// other byte becomes %XX ("app:chat" is app%3Achat.opus). Without a folder, a tape keeps only the
// newest 2 minutes of each track, and the rest is dropped. The 10 ms level envelope covers the
// whole tape either way.
//
// Every tape call is thread-safe, except echoir_tape_free(), which must be the last call on the
// handle. These calls work in every build, also one without the backends.

// Creates a tape. folder is a UTF-8 directory for the page files and the joined files, created
// when it is missing; NULL or "" keeps the audio in memory only. Files of the same names in the
// folder are replaced. Returns NULL on failure, such as a folder that cannot be created. Free the
// tape with echoir_tape_free().
ECHOIR_API echoir_tape*   echoir_tape_create(const char* folder);

// Opens the files a tape left in folder (UTF-8) as a read-only tape, such as a recording saved
// earlier. Each track is <track>.opus, or its page files <track>.00001.opus and on, which a
// process that ended without echoir_tape_close() leaves, whichever holds more of the track. Page
// files are read in order from the first; a missing number or a page that does not check out
// ends the track there. Other files in the folder are not tracks. Every track is decoded once to
// build its level envelope, so run it off the UI thread. The tape writes nothing: it is closed,
// echoir_tape_add_track() and a session given it fail with ECHOIR_ERR_STATE, and
// echoir_tape_checkpoint() and echoir_tape_close() do nothing. echoir_tape_read_audio(),
// echoir_tape_read_peaks(), echoir_tape_tracks_json() and echoir_tape_duration_ms() work. Each
// track has one run, the whole track, and its label is its name. Returns NULL when folder is not
// a directory or a file of a track cannot be read. Free the tape with echoir_tape_free().
ECHOIR_API echoir_tape*   echoir_tape_open(const char* folder);

// Adds a track. name is 1 to 64 bytes of UTF-8 and must differ from every other track name when
// ASCII case is ignored. label is the name to show; NULL or "" means the name. Returns
// ECHOIR_ERR_INVALID_ARG for a bad or taken name, and ECHOIR_ERR_STATE after echoir_tape_close().
ECHOIR_API echoir_status  echoir_tape_add_track(echoir_tape*, const char* name, const char* label);

// The tracks as JSON, UTF-8:
//   {"duration_ms":N,"closed":bool,"read_only":bool,
//    "tracks":[{"name","label","file","duration_ms","writing","runs":[{"start_ms","end_ms"}],
//               "error"}]}
// duration_ms of the tape is that of its longest track. read_only is true for a tape from
// echoir_tape_open(). file is the name of the track's joined file in the folder, written by
// echoir_tape_checkpoint() and echoir_tape_close() ("" without a folder, and for a track that
// echoir_tape_open() read from page files). writing is true while
// a session writes the track. runs are the stretches that sessions wrote, in order; a seam is
// where one ends and the next starts. error, present only when a page file could not be written,
// is the reason: the audio stays in memory and the write is tried again a minute later. The
// string is owned by the library and stays valid until the next *_json call on this thread. NULL
// on failure.
ECHOIR_API const char*    echoir_tape_tracks_json(echoir_tape*);

// The length of the tape, that of its longest track, in whole ms into *out_ms. Returns
// ECHOIR_ERR_INVALID_ARG for a NULL argument.
ECHOIR_API echoir_status  echoir_tape_duration_ms(echoir_tape*, int64_t* out_ms);

// Copies the audio of [start_ms, end_ms) of a track as 16 kHz mono 16-bit samples into out, which
// holds cap samples, and sets *n_frames to the number copied. end_ms is clamped to the end of the
// track. With out NULL, only *n_frames is set: the number the range needs. Audio from the page
// files is decoded Opus, so it is close to, not equal to, the samples written; the newest 2
// minutes are exact. A padded stretch reads as silence. Returns ECHOIR_ERR_STATE when part of the
// range is no longer kept (a tape without a folder). Returns ECHOIR_ERR_INVALID_ARG for an unknown
// track, a negative start_ms, end_ms before start_ms, n_frames NULL, or a cap too small for the
// range. Returns ECHOIR_ERR_INTERNAL when a page file cannot be read.
ECHOIR_API echoir_status  echoir_tape_read_audio(echoir_tape*, const char* track,
                                                 int64_t start_ms, int64_t end_ms, int16_t* out,
                                                 size_t cap, size_t* n_frames);

// The level envelope of [start_ms, end_ms) of a track, as echoir_session_read_peaks() on the
// tape's timeline. Returns ECHOIR_ERR_INVALID_ARG for an unknown track and the argument errors of
// echoir_session_read_peaks().
ECHOIR_API echoir_status  echoir_tape_read_peaks(echoir_tape*, const char* track,
                                                 int64_t start_ms, int64_t end_ms,
                                                 int32_t bucket_ms, float* out_minmax, size_t cap,
                                                 size_t* n_buckets);

// With a folder, writes <track>.opus for every track that has audio: a complete, standard Ogg
// Opus file of everything the track holds so far, while the tape stays open and sessions keep
// writing. The page files are copied into it and the audio still in memory is encoded for it; the
// tape's own pages, memory and reads are unchanged, and audio written later continues the track
// with no gap. A later checkpoint or echoir_tape_close() replaces the file. Each call copies the
// page files and encodes at most the newest 2 minutes, so run it off the UI thread. Without a
// folder, or after echoir_tape_close(), it does nothing and returns ECHOIR_OK. Returns
// ECHOIR_ERR_INTERNAL when a file cannot be written (such as a file another program holds open);
// the other tracks are still written.
ECHOIR_API echoir_status  echoir_tape_checkpoint(echoir_tape*);

// Ends the tape. Sessions still writing it go on, but their audio is no longer kept. With a
// folder, the rest of each track is encoded and its pages are joined into <track>.opus, a
// standard Ogg Opus file; a track with no audio gets no file. Reads still work afterwards, from
// those files. Without a folder, the audio is dropped (reads return ECHOIR_ERR_STATE) and the
// envelope stays. Run it off the UI thread: it reads and writes every page file. A second call
// does nothing. Returns ECHOIR_ERR_INTERNAL when a file cannot be written; the tape is closed
// even then, and the page files of that track stay.
ECHOIR_API echoir_status  echoir_tape_close(echoir_tape*);

// Releases the handle, closing the tape first if echoir_tape_close() was not called. Sessions
// that write the tape hold their own reference, so they may outlive the handle.
ECHOIR_API void           echoir_tape_free(echoir_tape*);

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
// the time that passed, up to 5 seconds per gap. That is enough for the open segment to end. The
// rest of a longer gap, such as a quiet loopback for minutes or a system sleep, is left off the
// timeline: no silence is pushed, decoded or stored for it, and timestamps after it are that much
// earlier than the wall clock. With a tape, the audio after such a gap starts a new run of the
// session's track where the track ends (a seam in echoir_tape_tracks_json()), and
// echoir_session_tape_offset_ms() does not change, so session time still maps to tape time.
//
// If the chosen device is lost, an ERROR event with detail "device_lost" is sent, and the capture
// ends. The session keeps running until it is stopped. If the default device is lost, the capture
// reopens the new default, and when the system has no device at all it keeps trying (every second)
// until one appears or the session is stopped. No event is sent for that wait; it shows in
// echoir_session_capture_state().
ECHOIR_API echoir_status  echoir_session_start_capture(echoir_session*, echoir_source,
                                                       const char* device_id);

// State of the capture of a session, from echoir_session_capture_state().
//   OFF      the session does not capture its own audio: a push session, a session not started,
//            or one stopped.
//   LIVE     the device is open and its audio goes into the session.
//   WAITING  the default device was lost and none has opened for a second or more, such as when
//            the system has no device at all. The capture keeps trying and goes back to LIVE by
//            itself when one opens. A switch to another default device is quicker than a second,
//            so it does not show here.
//   ENDED    the capture has ended by itself: the chosen device was lost (an ERROR event with
//            detail "device_lost" was sent) or the capture failed. It stays ENDED until the
//            session is stopped.
typedef enum { ECHOIR_CAPTURE_OFF = 0, ECHOIR_CAPTURE_LIVE = 1, ECHOIR_CAPTURE_WAITING = 2,
               ECHOIR_CAPTURE_ENDED = 3 } echoir_capture_state;

// Sets *out_state to the echoir_capture_state of the session's capture. Cheap and never waits for
// a decode or a device, so a host can call it on a timer, for example to show that it waits for a
// device. Returns ECHOIR_ERR_INVALID_ARG for a NULL argument.
ECHOIR_API echoir_status  echoir_session_capture_state(echoir_session*, int32_t* out_state);

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
  float       vad_threshold;           // as in echoir_engine_params; 0 = the VAD's own value
  int32_t     vad_min_silence_ms;      // as in echoir_engine_params; 0 = the VAD's own value
} echoir_model_params;

// Sets *p to the defaults (registry_json, models_dir, model_id and vad_id NULL; use_gpu 1;
// n_threads, vad_threshold and vad_min_silence_ms 0) and sets struct_size. With p NULL, nothing is written and the last error is set.
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
//    "default_vad":{"whisper":id,"qwen":id},"default_translator":id,
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

// Text translation. A translator turns one line of text into another language. It is a separate
// object from the engine: it has its own model and its own llama.cpp context, and it never takes
// an engine's turn, so it runs in parallel with the decodes of any session. A translator is meant
// for the FINAL lines of a transcript, one call per line. It loads a model of the catalogue whose
// backend is "translator", with the same model_id and models_dir rules as an engine.
typedef struct echoir_translator echoir_translator;

// Model selection for echoir_translator_create_from_model(). Start from
// echoir_translator_params_init(). The paths are read during the call only.
typedef struct {
  uint32_t    struct_size;             // sizeof(echoir_translator_params). A smaller value is
                                       // accepted if it covers every field up to model_id; the
                                       // fields after it take their defaults. A larger value is
                                       // refused.
  const char* registry_json;           // catalogue text in the registry format; NULL = built-in
  const char* models_dir;              // UTF-8 directory with the model file (required for create)
  const char* model_id;                // registry id of a translator entry; NULL or "" = the
                                       // catalogue's default_translator
  int32_t     use_gpu;                 // 1 = GPU backend built into the library, 0 = CPU
  int32_t     n_threads;               // CPU threads; 0 = default (4)
} echoir_translator_params;

// One line to translate, for echoir_translator_translate().
typedef struct {
  uint32_t    struct_size;             // sizeof(echoir_translate_request). A smaller value is
                                       // accepted if it covers every field up to target; context
                                       // is then NULL. A larger value is refused.
  const char* text;                    // the line, UTF-8 (required, not blank)
  const char* source;                  // ISO code of its language, or "auto"; NULL or "" = "auto".
                                       // Only checked: the models find the language themselves.
  const char* target;                  // ISO code of the language to write (required), such as
                                       // "en", "vi", "ja", "zh", "zh-Hant". A code the model does
                                       // not support is ECHOIR_ERR_INVALID_ARG.
  const char* context;                 // earlier lines of the same speaker in the source language,
                                       // oldest first, separated by newlines, to help the model
                                       // with names and pronouns; NULL = none. Two lines are
                                       // enough. They are not translated.
} echoir_translate_request;

// The result of a translation. It belongs to the library, one per calling thread, and is valid
// until the next echoir_translator_translate call on the same thread, whichever translator that
// call is on. It outlives echoir_translator_free().
typedef struct {
  uint32_t    struct_size;             // sizeof(echoir_translation), set by the library
  const char* text;                    // the translation, whole UTF-8
  int32_t     capped;                  // 1 = generation stopped at its limit (5 tokens per source
                                       // token plus 32), so text may end mid-sentence
  int32_t     source_tokens;           // tokens of the line
  int32_t     prompt_tokens;           // tokens of the prompt, context and instruction included
  int32_t     output_tokens;           // tokens generated
  double      prefill_ms;              // time to read the prompt and pick the first token
  double      decode_ms;               // time to generate the rest
} echoir_translation;

// Sets *p to the defaults (registry_json, models_dir and model_id NULL; use_gpu 1; n_threads 0) and
// sets struct_size. With p NULL, nothing is written and the last error is set.
ECHOIR_API void           echoir_translator_params_init(echoir_translator_params*);

// Resolves model_id in the catalogue, checks that its file is in models_dir with the catalogue's
// size, loads it and runs a few short translations so that the first real call is not slow. Free
// the translator with echoir_translator_free(). Returns NULL on failure. An unknown id, or one that
// is not a translator, is ECHOIR_ERR_INVALID_ARG, and a missing or wrong-size file is
// ECHOIR_ERR_MODEL_LOAD. Loading takes a while, so run it off the UI thread.
ECHOIR_API echoir_translator* echoir_translator_create_from_model(const echoir_translator_params*);

// Translates one line. The call is synchronous and takes about 0.3 s for a typical line on a GPU.
// Output is greedy and each call is independent of the one before. Calls on one translator are
// thread-safe and run one at a time; they never wait for an engine. The returned result and its
// text are owned by the library, like the strings of the *_json calls: they are valid until the
// next echoir_translator_translate call on this thread, so another thread's call never changes
// them. Copy what you need to keep. Returns NULL on failure: ECHOIR_ERR_INVALID_ARG for a NULL
// argument, a blank text, a language that is not supported or a text that does not fit the model's
// context (about 1900 tokens; context lines are dropped oldest first to make room, so only a text
// that is too long alone is refused), ECHOIR_ERR_BACKEND when the model fails.
ECHOIR_API const echoir_translation* echoir_translator_translate(echoir_translator*,
                                                                 const echoir_translate_request*);

// Frees a translator and its model. No call may be running on it.
ECHOIR_API void           echoir_translator_free(echoir_translator*);
// clang-format on

#ifdef __cplusplus
}
#endif

#endif /* ECHOIR_ECHOIR_H */
