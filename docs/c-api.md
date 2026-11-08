# C API

The C ABI is the interface that a host process uses to call echoir-core in process. It is declared in
`include/echoir/echoir.h`. The header is plain C99 and works from C and C++.

It is built for live transcription. A host loads a model into an engine, opens a session on the
engine, and then either pushes PCM audio into the session or lets the library capture an audio
device. The host reads the transcript as events: from a callback, or by polling a queue.

A host can also translate the final lines of a transcript with a translator, a separate object that
runs beside the engine (see Translator below).

The header is the reference for every declaration. This page explains how the parts fit together and
what the header's rules mean in practice. For .NET, see [bindings/csharp/README.md](../bindings/csharp/README.md).
For a process that talks over standard input and output, see [protocol.md](protocol.md).

## Linking

The library is the CMake target `echoir`, defined in `src/capi/CMakeLists.txt`. It is built as a shared
or a static library, following `BUILD_SHARED_LIBS`. The target puts `include/` on the include path, so
the header is included as `#include "echoir/echoir.h"`.

- In a shared build on Windows, the header declares the functions as `__declspec(dllimport)`.
- In a static build, the target defines `ECHOIR_STATIC` for its consumers, so no import or export
  decoration is used.
- On GCC and Clang, the functions have default visibility.

The CMake files do not contain install rules. A host that uses the library copies the header and the
built library, or uses them from the build tree.

## Conventions

### Status codes

Functions that can fail return an `echoir_status`. `ECHOIR_OK` is 0, and every other value is a
failure.

| Code | Meaning |
|---|---|
| `ECHOIR_OK` | The call succeeded. |
| `ECHOIR_ERR_INVALID_ARG` | A NULL or out-of-range argument, such as a bad sample rate. |
| `ECHOIR_ERR_MODEL_LOAD` | A model file is missing, unreadable or not the expected format. |
| `ECHOIR_ERR_BACKEND` | A backend (whisper.cpp or llama.cpp) failed to set up or decode. |
| `ECHOIR_ERR_STATE` | The call is not valid now, such as the wrong driving mode. `echoir_session_poll()` also returns it for an empty queue, which is not a failure. |
| `ECHOIR_ERR_NOT_IMPLEMENTED` | The function is declared but not available in this build. |
| `ECHOIR_ERR_INTERNAL` | An unexpected failure inside the core. |

Functions that return a pointer return NULL on failure. The `*_params_init` functions and the
`*_free` functions do not report errors.

### Error messages

Call `echoir_last_error()` after a failure to get the reason. The message has the form
`<function>: <reason>`. `echoir_last_status()` gives the status of the same call. Both are kept per
thread, so they describe the most recent failing call on the calling thread. A function that returns
a pointer reports a failure only as NULL, so read its status from `echoir_last_status()`: for
example, `echoir_engine_create_from_model()` gives `ECHOIR_ERR_MODEL_LOAD` for a missing model file
and `ECHOIR_ERR_INVALID_ARG` for an unknown id.

### Strings

All strings are UTF-8, on every platform. This includes model and directory paths, device ids and
the JSON text that the library returns.

### Ownership

- Strings returned by `echoir_version()` have static storage. Do not free them.
- The string from `echoir_last_error()` stays valid until the calling thread's next failing call.
- The strings from `echoir_devices_json()`, `echoir_models_json()` and `echoir_tape_tracks_json()`
  are owned by the library. They stay valid until the next `*_json` call on the same thread. Copy them
  if you need them longer.
- Engines, sessions, tapes and translators come from a create call. Free them with
  `echoir_engine_free()`, `echoir_session_free()`, `echoir_tape_free()` and
  `echoir_translator_free()`. Free the sessions of an engine before the engine.
  A tape may be freed before or after the sessions that write it: each session holds its own
  reference.
- Path strings in the params structs are read during the create call only. The host can release them
  afterwards.
- Event `text` and `detail` pointers are valid only during the callback. In queue mode they stay valid
  until the next `echoir_session_poll()` or until the session is freed.
- The `echoir_translation` that `echoir_translator_translate()` returns belongs to the library, one per
  calling thread, like the strings of the `*_json` calls. It and its text stay valid until the next
  `echoir_translator_translate()` call on the same thread, whichever translator that call is on, and
  another thread's call never changes them.
- Freeing NULL is a no-op.
- The buffers that the host passes to the `*_read_audio()` and `*_read_peaks()` calls belong to the
  host.

### Threading

- A handle (engine or session) is used by one thread at a time. Sessions of one engine are separate
  handles, so each may be driven on its own thread.
- `echoir_session_push_f32()`, `echoir_session_push_s16()`, `echoir_session_read_audio()`,
  `echoir_session_read_peaks()`, `echoir_session_set_input_muted()` and `echoir_session_read_lag()`
  are thread-safe. They can run on any thread, in either driving mode.
- `echoir_translator_translate()` is thread-safe: calls on one translator run one at a time. Its
  result is kept per calling thread (see Ownership above), so threads can translate on one translator and
  each reads its own result.
- Every `echoir_tape_*` call is thread-safe, except `echoir_tape_free()`, which must be the last call
  on the handle.
- Callbacks run on the thread that drives the session. In mode A, that is the thread that calls
  `echoir_session_process()`, `echoir_session_flush()`, `echoir_session_reset()` or
  `echoir_session_clear()`. In mode B, it is the session's worker thread.
- A callback must not call any function on its own session, except the push functions. Do not free
  the session from its callback. The process aborts if you do.
- In queue mode, poll from one thread, for example a UI timer.
- `echoir_engine_create()` and `echoir_engine_create_from_model()` take a while, because they load
  models. Run them off the UI thread.

### Units

- Times are `int64_t` milliseconds. They count from session start, or from the last
  `echoir_session_reset()`. For a captured session, they follow the wall clock from the start of the
  capture, except across a capture gap longer than 5 seconds (see Capture).
- Sample rates are `int32_t` values in hertz. Any positive rate is accepted.
- `channels` is a positive count. Samples are interleaved, so a frame holds `channels` samples.
- `n_frames` counts frames, not samples.
- Float samples are nominally in [-1, 1]. Signed 16-bit samples have full scale 32768.
- Audio is resampled internally to 16 kHz mono. The store and `echoir_session_read_audio()` use 16 kHz
  mono 16-bit samples.
- `echoir_session_read_peaks()` uses floats with full scale ±1. Its bucket size is a positive
  multiple of 10 ms.
- `n_threads` is a thread count. 0 means the default. For engine params, the default is 4.
- `use_gpu` is an `int32_t`. 1 selects the GPU backend built into the library, and 0 selects the CPU.

### Struct versioning

Each parameter struct starts with a `struct_size` field. The `*_params_init()` functions set it to
the size of the struct that the header declares. The host can change the fields after that.

The rules for an unexpected size depend on the struct:

- **`echoir_engine_params`:** a larger size is accepted, and the library reads only the fields it
  knows. A smaller size is accepted if it covers every field up to `n_threads`; the VAD tuning after
  it takes its defaults. A size that stops before `n_threads` is refused, and
  `echoir_engine_create()` returns NULL.
- **`echoir_session_params`:** a smaller size is accepted if it covers every field up to
  `emit_partials`. The fields after it take their defaults. A larger size is refused. `tape` is read
  only when the size also covers `tape_track`.
- **`echoir_model_params`:** a smaller size is accepted if it covers every field up to `model_id`.
  The fields after it take their defaults. A larger size is refused.
- **`echoir_translator_params`:** as `echoir_model_params`.
- **`echoir_translate_request`:** a smaller size is accepted if it covers every field up to
  `target`; `context` is then NULL. A larger size is refused.

## Lifecycle

The usual order of calls is:

1. Initialise an engine params struct with `echoir_engine_params_init()` or
   `echoir_model_params_init()`, and set the fields you need.
2. Create the engine with `echoir_engine_create()`, or with `echoir_engine_create_from_model()` to use
   the catalogue. Run this off the UI thread.
3. Initialise a session params struct with `echoir_session_params_init()`, and set the fields you need.
4. Create a session with `echoir_session_create()`. Pass a callback for callback mode, or NULL for queue
   mode. The mode cannot change later.
5. Start the session. Use `echoir_session_start_capture()` to let the library read a device. Or use
   `echoir_session_start()` and push audio with the push functions.
6. Read events. In callback mode they arrive on the driving thread. In queue mode, call
   `echoir_session_poll()` until it returns `ECHOIR_ERR_STATE`, which in queue mode means only that the
   queue is empty.
7. Stop with `echoir_session_stop()`, then read the remaining events.
8. Free the session with `echoir_session_free()`, and then the engine with `echoir_engine_free()`.

A complete program that captures the default microphone and prints the final segments:

```c
#include <stdio.h>
#include "echoir/echoir.h"

/* Prints every queued event. Text is valid until the next poll, so it is used right away. */
static void drain(echoir_session* session) {
  echoir_event ev;
  while (echoir_session_poll(session, &ev) == ECHOIR_OK) {
    switch (ev.type) {
      case ECHOIR_EVENT_FINAL:
        printf("[%lld-%lld ms] %s\n", (long long)ev.start_ms, (long long)ev.end_ms, ev.text);
        break;
      case ECHOIR_EVENT_ERROR:
        fprintf(stderr, "error: %s\n", ev.detail);
        break;
      default:
        break;
    }
  }
}

int main(void) {
  echoir_model_params mp;
  echoir_model_params_init(&mp);
  mp.model_id = "whisper-tiny-en";  /* a registry id */
  mp.models_dir = "models";         /* directory with the model files */

  echoir_engine* engine = echoir_engine_create_from_model(&mp);
  if (engine == NULL) {
    fprintf(stderr, "load failed: %s\n", echoir_last_error());
    return 1;
  }

  echoir_session_params sp;
  echoir_session_params_init(&sp);
  sp.language = "en";

  /* A NULL callback selects queue mode. */
  echoir_session* session = echoir_session_create(engine, &sp, NULL, NULL);
  if (session == NULL) {
    fprintf(stderr, "session failed: %s\n", echoir_last_error());
    echoir_engine_free(engine);
    return 1;
  }

  /* A NULL device_id is the default microphone. */
  if (echoir_session_start_capture(session, ECHOIR_SOURCE_MIC, NULL) != ECHOIR_OK) {
    fprintf(stderr, "capture failed: %s\n", echoir_last_error());
    echoir_session_free(session);
    echoir_engine_free(engine);
    return 1;
  }

  for (int tick = 0; tick < 200; ++tick) {
    drain(session);
    /* Wait about 50 ms here with the host's timer or sleep call. */
  }

  if (echoir_session_stop(session) != ECHOIR_OK) {
    fprintf(stderr, "stop failed: %s\n", echoir_last_error());
  }
  drain(session); /* FINAL events can arrive before stop returns. */

  echoir_session_free(session);
  echoir_engine_free(engine);
  return 0;
}
```

### Pushing audio instead of capturing

A host that has its own audio source uses mode B with the push functions. The call order is the same,
except for step 5:

```c
echoir_session* s = echoir_session_create(engine, &sp, on_event, state);
echoir_session_start(s);

/* For each block of 100 ms of 16 kHz mono 16-bit audio (1600 samples): */
echoir_session_push_s16(s, block, 1600, 16000, 1);

echoir_session_stop(s);
```

A session that captures its own audio does not accept push calls. Use one or the other on a session.

## Reference

### Version and errors

```c
const char* echoir_version(void);
```

Returns the library version, such as `"3.0.0"`. The string has static storage and is never NULL.

```c
const char* echoir_last_error(void);
```

Returns the message of the calling thread's most recent failing call, as `<function>: <reason>`. It is
empty before any failure and is never NULL. The pointer stays valid until the thread's next failing call.

```c
echoir_status echoir_last_status(void);
```

Returns the status of the calling thread's most recent failing call, the one `echoir_last_error()`
describes, or `ECHOIR_OK` before any failure. Read it right after the call, on the same thread. It is
how a host gets the status of a call that returns a pointer, such as `echoir_engine_create()` or
`echoir_session_create()`.

### Parameter defaults

```c
void echoir_engine_params_init(echoir_engine_params* p);
void echoir_session_params_init(echoir_session_params* p);
void echoir_model_params_init(echoir_model_params* p);
```

Each sets `*p` to its defaults and sets `struct_size`. With `p` NULL, nothing is written and the last
error is set. The defaults are:

- Engine: Whisper backend, `use_gpu` 1, `n_threads` 0, `vad_threshold` 0, `vad_min_silence_ms` 0,
  all paths NULL.
- Session: `language` `"auto"`, `min_chunk_ms` 500, `max_segment_ms` 10000, `emit_partials` 1,
  `stream_policy` `ECHOIR_POLICY_DEFAULT`, `retain_audio_ms` 0, `tape` and `tape_track` NULL.
- Model: `registry_json`, `models_dir`, `model_id` and `vad_id` NULL, `use_gpu` 1, `n_threads` 0,
  `vad_threshold` 0, `vad_min_silence_ms` 0.

### Engine

```c
echoir_engine* echoir_engine_create(const echoir_engine_params* params);
```

Loads the models named in `*params` and returns a new engine, or NULL on failure. `model_path` is the
speech model, `mmproj_path` is the Qwen projector (Qwen only, otherwise NULL), and `vad_model_path` is
required. The paths are read during this call only. Free the engine with `echoir_engine_free()`.

Two fields tune the VAD. Each is 0 by default, which keeps the VAD's own value:

| Field | Meaning | Silero | FireRedVAD |
|---|---|---|---|
| `vad_threshold` | Speech probability needed for speech, above 0 and at most 1. Higher hears less as speech. | 0.2 | 0.4 |
| `vad_min_silence_ms` | Shortest pause that ends a speech range, 0 to 60000 ms. Lower ends lines sooner. | 1500 ms | 500 ms |

FireRedVAD works in 10 ms frames, so the pause is rounded up to whole frames. A value out of range
fails with `ECHOIR_ERR_INVALID_ARG` before any file is read. The engine's sessions all use the same
VAD, so they share its tuning.

```c
echoir_engine* echoir_engine_create_from_model(const echoir_model_params* params);
```

Looks up `model_id` and its VAD in the catalogue, then checks that their files are in `models_dir` with
the sizes that the catalogue gives. It then creates the engine as `echoir_engine_create()` does. An
unknown or non-speech id fails with `ECHOIR_ERR_INVALID_ARG`. A missing or wrong-size file fails with
`ECHOIR_ERR_MODEL_LOAD`. The call returns NULL either way; `echoir_last_status()` gives the status
and `echoir_last_error()` the reason.

```c
void echoir_engine_free(echoir_engine* engine);
```

Frees the engine and its models. Free its sessions first.

### Session

```c
echoir_session* echoir_session_create(echoir_engine* engine, const echoir_session_params* params,
                                      echoir_event_cb cb, void* user);
```

Creates a session on `engine`, or returns NULL. It fails if `max_segment_ms` is below 200, or if
`stream_policy` is unknown or is `ECHOIR_POLICY_ROLLBACK` on a backend without decoder-prefix support.
`*params` is read during this call only.

An engine serves any number of sessions, for example one per audio source. They share its models:
each VAD run and each decode takes a turn on the engine, in the order the sessions ask, so the
sessions never decode at the same time and each one's decodes add to the others' lag. A session's
events are the same as when it runs alone. STATS reports the time a session waited for its turns as
`wait_ms`. A session's run on a tape starts at the tape's end when the session is created, so create
every session of a recording before any of them takes audio, and their tracks start together.

A non-NULL `cb` selects callback mode. A NULL `cb` selects queue mode. The mode cannot change after
creation. `user` is passed back to `cb` unchanged. The engine must stay valid until the session is
freed. In callback mode, `cb` and `user` must stay valid as well.

```c
void echoir_session_free(echoir_session* session);
```

Frees the session. If its worker is running, the worker is stopped first, as `echoir_session_stop()`
does. Do not call this from the session's own callback.

### Events

```c
echoir_status echoir_session_poll(echoir_session* session, echoir_event* out);
```

Queue mode only. Copies the next queued event into `*out` and returns `ECHOIR_OK`. It returns
`ECHOIR_ERR_STATE` when the queue is empty, or when the session is in callback mode. The mode is fixed
when the session is created, so in queue mode `ECHOIR_ERR_STATE` means only that there is nothing to
take, and any other status is a real failure. An empty queue is not a failure: it leaves
`echoir_last_error()` and `echoir_last_status()` as they were, so polling from a UI timer does not
overwrite the reason of an earlier failure on that thread.

Consecutive PARTIAL events of the same segment are merged, and the newest one is kept. The queue holds
up to 1024 events. A new PARTIAL is dropped when the queue is full. A new FINAL, CLEARED or ERROR event
removes the oldest queued PARTIAL, if there is one. FINAL, CLEARED and ERROR events are never dropped,
so the queue can grow past 1024 when the host does not poll.

### Audio input

```c
echoir_status echoir_session_push_f32(echoir_session* session, const float* pcm, size_t n_frames,
                                      int32_t sample_rate, int32_t channels);
echoir_status echoir_session_push_s16(echoir_session* session, const int16_t* pcm, size_t n_frames,
                                      int32_t sample_rate, int32_t channels);
```

Appends `n_frames` frames of interleaved samples from `pcm`. `push_f32` takes floats nominally in
[-1, 1], and `push_s16` takes 16-bit signed samples with full scale 32768. The call copies the samples
and never waits for a decode.

It returns `ECHOIR_ERR_INVALID_ARG` for a `sample_rate` or `channels` that is not positive, a NULL `pcm`
with frames, or (for `push_f32` only) a NaN or infinite sample. It returns `ECHOIR_ERR_STATE` while the
session captures its own audio.

### Driving the session

These calls are for the two driving modes. In mode A the host calls `echoir_session_process()`
itself. In mode B the library runs a worker thread that decodes audio as it arrives.

```c
echoir_status echoir_session_process(echoir_session* session);
```

Mode A. Runs one decode tick on the calling thread when at least `min_chunk_ms` of audio is buffered,
and does nothing otherwise. Events go to the callback on this thread, or to the queue. It returns
`ECHOIR_ERR_STATE` in mode B. It returns `ECHOIR_ERR_BACKEND` when the VAD or the decoder fails, after
an ERROR event. The audio stays buffered, so a later call retries it. It returns
`ECHOIR_ERR_INVALID_ARG` when the decoder rejects the request, such as an unsupported language, or
when the VAD rejects the audio window as too long.

```c
echoir_status echoir_session_start(echoir_session* session);
```

Mode B. Starts the worker thread. Events are delivered on the worker thread, or queued for
`echoir_session_poll()`. It returns `ECHOIR_ERR_STATE` in mode A, or when the session is already
started.

```c
echoir_status echoir_session_stop(echoir_session* session);
```

Mode B. Stops the worker and waits for it to exit. No callback runs after this returns. The call does
not flush. Audio that was not decoded stays buffered. The exception is a session that captures its
own audio. The device is closed first, the captured audio is decoded, and the ongoing segment is
finalized, as `echoir_session_flush()` does. FINAL events can arrive before this returns. The call does
nothing if the session is not started. The session stays in mode B, so `echoir_session_start()` can
run again.

```c
echoir_status echoir_session_flush(echoir_session* session);
```

Decodes all buffered audio and finalizes the ongoing segment as a FINAL event. The events are delivered
on the calling thread, in either mode. In mode B the call waits for a decode that the worker is
running. Errors are those of `echoir_session_process()`.

```c
echoir_status echoir_session_reset(echoir_session* session);
```

Does the same as `echoir_session_flush()`, then drops all audio and text state, even when the final
decode fails. Timestamps count from this call, so the next segment starts at 0.

```c
echoir_status echoir_session_clear(echoir_session* session);
```

Drops the text of the segment in progress and emits an `ECHOIR_EVENT_CLEARED` event on the calling
thread. Timestamps keep running. Use `echoir_session_reset()` to start the timeline again.

### Audio store, level envelope and input mute

The session keeps the audio it takes in, after conversion to 16 kHz mono. The audio is placed on the
session timeline, so the milliseconds match the event times. Silence that is pushed for capture gaps is
kept too. The store restarts at 0 with `echoir_session_reset()`. It lives until the session is freed,
so it can be read after `echoir_session_stop()`. The calls in this group are thread-safe, and they can
run on any thread, in either mode.

```c
echoir_status echoir_session_read_audio(echoir_session* session, int64_t start_ms, int64_t end_ms,
                                        int16_t* out, size_t cap, size_t* n_frames);
```

Copies the kept audio for `[start_ms, end_ms)` into `out`, as 16 kHz mono 16-bit samples. `cap` is the
number of samples that `out` holds, and `*n_frames` is set to the number copied. `end_ms` is clamped to
the newest sample, so a range past it is shorter, or empty. If `out` is NULL, only `*n_frames` is set,
to the number of samples the range needs.

It returns `ECHOIR_ERR_STATE` when the session keeps no audio (`retain_audio_ms` is 0 and no tape), or
when part of the range is older than what is kept. It returns `ECHOIR_ERR_INVALID_ARG` for a negative `start_ms`,
an `end_ms` before `start_ms`, a NULL `n_frames`, or a `cap` that is too small for the range.

```c
echoir_status echoir_session_read_peaks(echoir_session* session, int64_t start_ms, int64_t end_ms,
                                        int32_t bucket_ms, float* out_minmax, size_t cap,
                                        size_t* n_buckets);
```

Writes the level envelope of `[start_ms, end_ms)`. Each bucket has a min and a max value at full
scale ±1. The envelope keeps one min/max pair per 10 ms cell for the whole session, whatever
`retain_audio_ms` is. A bucket is `bucket_ms` of audio, and `bucket_ms` must be a positive multiple of
10, so a bucket spans whole cells.

`out_minmax` holds two floats per bucket, the min and then the max. It has room for `cap` buckets, and
`*n_buckets` is set to the number written. `end_ms` is clamped to the newest whole 10 ms, and the last
bucket can be shorter. If `out_minmax` is NULL, only `*n_buckets` is set. The newest bucket of the
envelope gives the current input level.

It returns `ECHOIR_ERR_INVALID_ARG` for a bad `bucket_ms`, a negative `start_ms`, an `end_ms` before
`start_ms`, a NULL `n_buckets`, or a `cap` that is too small for the range.

```c
echoir_status echoir_session_set_input_muted(echoir_session* session, int32_t muted);
```

With `muted` set to 1, audio that enters the session is replaced by silence of the same length. The
timeline keeps running, and the VAD ends the ongoing segment. The store keeps the silence. With `muted`
set to 0, the audio is passed through again. The change applies from the next audio the session
converts.

```c
typedef struct {
  uint32_t struct_size;  /* sizeof(echoir_lag), set by the caller */
  int64_t  head_ms;      /* audio taken in */
  int64_t  decoded_ms;   /* end of the window of the latest tick that finished */
  int64_t  backlog_ms;   /* head_ms - decoded_ms */
  int64_t  tick_ms;      /* how long the running tick has run; 0 between ticks */
} echoir_lag;

echoir_status echoir_session_read_lag(echoir_session* session, echoir_lag* out);
```

How far the decoding is behind the audio the session took in, in ms on the session timeline.
`backlog_ms` is the audio that waits for a tick plus the new audio of the tick that runs. While the
decoder keeps up it stays near `min_chunk_ms` plus one decode, rising during each decode and dropping
when it ends. When it grows steadily, the decoder is slower than real time. The call never waits for a
decode, so it can be polled for a display.

Set `struct_size` to `sizeof(echoir_lag)` first. A newer library writes only the fields that fit it;
an older one fills the fields it knows and zeroes the rest. It returns `ECHOIR_ERR_INVALID_ARG` for a
NULL `out` or a `struct_size` that does not reach `tick_ms`.

### Session tape

A tape keeps the audio of a whole recording session across the sessions that write it, so a host can
replay any part of it after a new run or a model switch, and save it. It holds named tracks, one per
input (for example `mic` and `system`), on one timeline that starts at 0.

- **Writing.** Set `tape` and `tape_track` in `echoir_session_params`. The session writes its 16 kHz
  mono audio, the same audio that its own store would keep, to that track. A track the tape does not
  have is added, labelled with its name. A track takes one session at a time; a second one fails
  `echoir_session_create()` with the reason in the last error.
- **Seams.** A session starts at the end of the tape, which is its longest track rounded up to whole
  10 ms. The time between two sessions is therefore not stored: their runs meet at a seam.
  `echoir_session_reset()` ends the session's run and starts a new one in the same way. A capture gap
  longer than 5 seconds also ends the run: the next one starts where the track ends, with no padding,
  so the session's offset on the tape stays the same (see Capture).
- **Several tracks.** Several sessions can write several tracks at the same time. A track that is
  behind the end of the tape when a session starts on it is padded with silence up to there, so all
  tracks keep one timeline.
- **Session reads.** `echoir_session_read_audio()` and `echoir_session_read_peaks()` read through to
  the tape, on the session timeline. Session time `t` is tape time
  `echoir_session_tape_offset_ms() + t`. `retain_audio_ms` is not used.
- **Storage.** The newest 2 minutes of each track stay in memory as 16-bit samples. With a folder,
  older audio is encoded with Opus at 24 kbit/s (about 11 MB per hour) and written to the folder as
  one-minute Ogg Opus page files, `<track>.00001.opus` and so on, as each one fills. A crash loses at
  most the minute being filled. `echoir_tape_close()` joins each track's pages into `<track>.opus`, a
  standard Ogg Opus file, and deletes the page files. Without a folder, only the newest 2 minutes of
  each track are kept. The 10 ms level envelope covers the whole tape either way.
- **File names.** A track name keeps its ASCII letters, digits, `-` and `_`. Every other byte becomes
  `%XX`, so `app:chat` is stored as `app%3Achat.opus`. Track names are 1 to 64 bytes and differ from
  each other when ASCII case is ignored, so their files never collide.
- **Reads.** Reads go by time range. Audio from the page files is decoded Opus: close to what was
  written, not equal to it. Only the pages that a range needs are read and decoded. A padded stretch
  reads as silence.

```c
echoir_tape*  echoir_tape_create(const char* folder);
void          echoir_tape_free(echoir_tape* tape);
echoir_status echoir_tape_checkpoint(echoir_tape* tape);
echoir_status echoir_tape_close(echoir_tape* tape);
```

`echoir_tape_create()` makes a tape whose files go to `folder` (UTF-8), which is created when it is
missing. NULL or `""` keeps the audio in memory only. Files of the same names in the folder are
replaced. It returns NULL on failure.

```c
echoir_tape*  echoir_tape_open(const char* folder);
```

`echoir_tape_open()` opens the files a tape left in `folder` as a read-only tape, for example to
replay a recording saved earlier. Each track is `<track>.opus`, or its page files
`<track>.00001.opus` and on, which a process that ended without `echoir_tape_close()` leaves,
whichever holds more of the track. Page files are read in order from the first; a missing number or a
page that does not check out ends the track there, so a page file cut short by a crash still gives the
audio before the cut. Other files in the folder, such as clips cut from a track, are not tracks.
Every track is decoded once to build its level envelope, so run it off the UI thread.

The tape writes nothing. It is closed, `echoir_tape_add_track()` and `echoir_session_create()` with
it return `ECHOIR_ERR_STATE`, and `echoir_tape_checkpoint()` and `echoir_tape_close()` do nothing.
The reads, `echoir_tape_tracks_json()` and `echoir_tape_duration_ms()` work as on any tape. Each
track has one run, the whole track, and its label is its name; `read_only` is true in the JSON. It
returns NULL, with `ECHOIR_ERR_INVALID_ARG` for a NULL or empty folder, when the folder is not a
directory or a file of a track cannot be read. An empty folder gives a tape with no tracks.

`echoir_tape_checkpoint()` writes `<track>.opus` for every track that has audio while the tape stays
open: a complete, standard Ogg Opus file of everything the track holds so far. The page files are
copied into it and the audio still in memory is encoded for it, on a copy of the track's encoder, so
the tape's own pages, memory and reads do not change and audio written later continues the track with
no gap. A later checkpoint or the close replaces the file. Call it when the audio so far should be safe
on disk as one file, such as when recording stops. Each call copies the page files and encodes at most
the newest 2 minutes, so run it off the UI thread. Without a folder, or after the close, it does
nothing. It returns `ECHOIR_ERR_INTERNAL` when a file cannot be written, such as one another program
holds open; the other tracks are still written.

`echoir_tape_close()` ends the tape. Sessions that still write it go on, but their audio is no longer
kept. With a folder, the rest of each track is encoded and the pages are joined into `<track>.opus`; a
track with no audio gets no file. Reads still work after the close, from those files. Without a folder
the audio is dropped, reads return `ECHOIR_ERR_STATE`, and the envelope stays. A second call does
nothing. It reads and writes every page file, so run it off the UI thread. It returns
`ECHOIR_ERR_INTERNAL` when a file cannot be written; the tape is closed even then, and the page files
of that track stay.

`echoir_tape_free()` releases the handle, and closes the tape first if `echoir_tape_close()` was not
called.

```c
echoir_status echoir_tape_add_track(echoir_tape* tape, const char* name, const char* label);
const char*   echoir_tape_tracks_json(echoir_tape* tape);
echoir_status echoir_tape_duration_ms(echoir_tape* tape, int64_t* out_ms);
```

`echoir_tape_add_track()` adds a track with a display label (NULL or `""` uses the name). It returns
`ECHOIR_ERR_INVALID_ARG` for a bad or taken name and `ECHOIR_ERR_STATE` after a close.

`echoir_tape_tracks_json()` returns the tracks:

```json
{"duration_ms": 185000, "closed": false, "read_only": false,
 "tracks": [{"name": "mic", "label": "Microphone", "file": "mic.opus", "duration_ms": 185000,
             "writing": true, "runs": [{"start_ms": 0, "end_ms": 61000},
                                       {"start_ms": 61010, "end_ms": 185000}]}]}
```

`read_only` is true for a tape from `echoir_tape_open()`. `file` is the name of the track's joined
file in the folder (`""` without a folder, and for a track that `echoir_tape_open()` read from page
files); it exists after `echoir_tape_checkpoint()` or `echoir_tape_close()`. `writing` is true while a session writes the track. `runs` are the stretches
that sessions wrote, in order, so the seams are between them. A track has an `error` field only when a
page file could not be written: the audio then stays in memory and the write is tried again when the
next minute has filled.

`echoir_tape_duration_ms()` gives the length of the longest track in whole ms.

```c
echoir_status echoir_tape_read_audio(echoir_tape* tape, const char* track, int64_t start_ms,
                                     int64_t end_ms, int16_t* out, size_t cap, size_t* n_frames);
echoir_status echoir_tape_read_peaks(echoir_tape* tape, const char* track, int64_t start_ms,
                                     int64_t end_ms, int32_t bucket_ms, float* out_minmax,
                                     size_t cap, size_t* n_buckets);
```

These work as `echoir_session_read_audio()` and `echoir_session_read_peaks()`, on a track of the tape
and on the tape's timeline. `echoir_tape_read_audio()` returns `ECHOIR_ERR_STATE` when part of the
range is no longer kept (a tape without a folder), `ECHOIR_ERR_INVALID_ARG` for an unknown track and the
argument errors of the session call, and `ECHOIR_ERR_INTERNAL` when a page file cannot be read.

```c
echoir_status echoir_session_tape_offset_ms(echoir_session* session, int64_t* out_ms);
```

Sets `*out_ms` to where time 0 of the session is on its tape: the start of the session's current
run. It returns `ECHOIR_ERR_STATE` for a session without a tape.

### Capture

```c
typedef enum { ECHOIR_SOURCE_MIC = 1, ECHOIR_SOURCE_LOOPBACK = 2 } echoir_source;
```

`ECHOIR_SOURCE_MIC` reads a capture device. `ECHOIR_SOURCE_LOOPBACK` reads what the system plays. The
library reads the device on its own thread, so the host only starts and stops the capture.

```c
const char* echoir_devices_json(void);
```

Returns the audio devices as JSON:

```json
{"capture":[{"id":"…","name":"…","default":true}, …],"loopback":[…]}
```

The `id` is stable across runs. Pass it as `device_id` to `echoir_session_start_capture()`. `loopback`
is an empty array when the audio backend cannot record playback devices. Only WASAPI can on Windows.
With `ECHOIR_AUDIO_BACKEND=null` in the environment, the list holds one silent device, which is used
for tests. The string is owned by the library. It returns NULL on failure.

```c
echoir_status echoir_session_start_capture(echoir_session* session, echoir_source source,
                                           const char* device_id);
```

Starts the session in live mode, as `echoir_session_start()` does, with the device open and its audio
pushed into the session. Works in callback mode and in queue mode.

A NULL or empty `device_id` selects the system default device, and the capture follows the default when
it changes. Any other `device_id` must come from `echoir_devices_json()`.

It returns `ECHOIR_ERR_INVALID_ARG` for a `source` value that is not declared in the enum, and
`ECHOIR_ERR_STATE` when the session already captures its own audio. The push functions return
`ECHOIR_ERR_STATE` while the session captures. Loopback on a backend without loopback returns
`ECHOIR_ERR_NOT_IMPLEMENTED`. An unknown `device_id` returns `ECHOIR_ERR_INVALID_ARG`.

Timestamps follow the wall clock from the start of the capture. When the device sends nothing for a
moment (loopback while no sound plays, or a lost default device), silence is pushed for the time that
passed, up to 5 seconds per gap: enough for the VAD to end the open segment. The rest of a longer gap,
such as a quiet loopback for minutes or a system sleep, is left off the timeline. No silence is pushed,
decoded or stored for it, so a wake from sleep costs at most 5 seconds of work, and later timestamps
are that much earlier than the wall clock. With a tape, the audio after such a gap starts a new run of
the session's track where the track ends, so the gap shows as a seam in `echoir_tape_tracks_json()`.
`echoir_session_tape_offset_ms()` does not change, so session time `t` is still tape time
`offset + t`. The track is not padded, so tracks that other sessions write at the same time no longer
line up after such a gap until a session attaches again.

If the chosen device is lost, an ERROR event with `detail` set to `"device_lost"` is sent, and the
capture ends. The session keeps running until it is stopped. If the default device is lost, the capture
reopens the new default. If the system has no device at all, it retries every second until one appears
or the session is stopped. No event is sent during that wait; `echoir_session_capture_state()`
reports it.

```c
typedef enum { ECHOIR_CAPTURE_OFF = 0, ECHOIR_CAPTURE_LIVE = 1, ECHOIR_CAPTURE_WAITING = 2,
               ECHOIR_CAPTURE_ENDED = 3 } echoir_capture_state;

echoir_status echoir_session_capture_state(echoir_session* session, int32_t* out_state);
```

Sets `*out_state` to the state of the session's capture:

| State | Meaning |
|---|---|
| `ECHOIR_CAPTURE_OFF` | The session does not capture its own audio: a push session, a session not started, or one stopped. |
| `ECHOIR_CAPTURE_LIVE` | The device is open and its audio goes into the session. |
| `ECHOIR_CAPTURE_WAITING` | The default device was lost and none has opened for a second or more, such as when the system has no device at all. The capture keeps trying and returns to `LIVE` by itself. A switch to another default device takes less than a second, so it does not show here. |
| `ECHOIR_CAPTURE_ENDED` | The capture ended by itself: the chosen device was lost (the `"device_lost"` ERROR event was sent) or the capture failed. It stays so until the session is stopped. |

The call is cheap and never waits for a decode or a device, so a host can read it on a timer, for
example to show that it waits for a device. It returns `ECHOIR_ERR_INVALID_ARG` for a NULL argument.

### Model catalogue and devices

The catalogue is the list in `models/registry.json`. The library builds it in. A host can pass its own
list. Each file of an entry is at `<models_dir>/<file name>`. A file is present when it exists there with
the size that the catalogue gives.

```c
void echoir_model_params_init(echoir_model_params* p);
```

Sets `*p` to its defaults and sets `struct_size`. With `p` NULL, nothing is written and the last error is
set. Set `model_id` and `models_dir` before creating an engine with them. `registry_json` replaces the
built-in catalogue. `vad_id` selects the VAD. NULL or an empty string selects the default VAD of the
model's backend. `vad_threshold` and `vad_min_silence_ms` tune it as in `echoir_engine_params`.

```c
const char* echoir_models_json(const char* registry_json, const char* models_dir);
```

Returns the catalogue and the machine as JSON:

```json
{"devices":[{"name","type":"gpu"|"cpu","description","integrated","memory_total_mb","memory_free_mb"}],
 "default_vad":{"whisper":id,"qwen":id},"default_translator":id,
 "models":[{<entry>,"files":[{<file>,"present":bool}],"present":bool,"fits":bool}]}
```

`default_translator` is the id of the translator that is used when none is named; it is left out when
the catalogue names none. An entry whose `backend` is `"translator"` is a translation model, and
has a `prompt_style` field. It is not a speech model, so `echoir_engine_create_from_model()` refuses
its id.

`registry_json` NULL selects the built-in catalogue. `models_dir` NULL or empty means no file is present.
`name` is ggml's device id. `description` is its readable name and may be empty. `integrated` marks a GPU
that shares system memory, so its memory size says little about its speed.

`fits` is true when `min_vram_mb` is at most the free memory of the best device. The best device is the
GPU with the most free memory, or the CPU when there is no GPU. An entry without `min_vram_mb` always
fits. `devices` is empty in a build without the backends. The string is owned by the library. It returns
NULL on failure, for example when `registry_json` does not parse.

### Translator

A translator turns one line of text into another language. It is meant for the FINAL lines of a
transcript: one call per line, a few hundred milliseconds each on a GPU. It is a separate object
from the engine. It loads its own model into its own llama.cpp context and never takes the
engine's turn, so translating runs in parallel with the decodes of every session. It costs memory
on top of the speech model: about 1.5 GB for the default model and 5 GB for the large one
(`min_vram_mb` in the catalogue).

The catalogue lists two translators, both Hy-MT2 models under the Apache-2.0 licence:

| Id | Size | Use |
|---|---|---|
| `hy-mt2-1.8b-q4_k_m` | 1.06 GiB | The default. About 0.3 s for a typical line on a GPU. |
| `hy-mt2-7b-q4_k_m` | 4.31 GiB | Better quality, especially into Japanese. About 1 s a line, and it can make several sources at once fall behind. |

```c
void echoir_translator_params_init(echoir_translator_params* p);
echoir_translator* echoir_translator_create_from_model(const echoir_translator_params* params);
```

Set `models_dir`, and `model_id` (NULL or an empty string selects the catalogue's
`default_translator`). `registry_json`, `use_gpu` and `n_threads` are as in `echoir_model_params`.
The create call checks that the id is a translator and that its file is in `models_dir` with the
catalogue's size, loads it, and runs a few short translations so that the first real line is not
slow. An unknown id, or one that is not a translator, is `ECHOIR_ERR_INVALID_ARG`; a missing or
wrong-size file is `ECHOIR_ERR_MODEL_LOAD`. Run it off the UI thread.

```c
const echoir_translation* echoir_translator_translate(echoir_translator* t,
                                                      const echoir_translate_request* request);
```

Translates `request->text` into the language `request->target`, an ISO code: `en`, `zh`, `zh-Hant`,
`yue`, `ja`, `ko`, `vi`, `th`, `id`, `ms`, `tl`, `fr`, `de`, `es`, `pt`, `it`, `ru`, `uk`, `pl`, `cs`,
`nl`, `tr`, `ar`, `fa`, `he`, `hi`, `bn`, `ta`, `te`, `mr`, `gu`, `ur`, `km`, `my`, `bo`, `kk`, `mn`
or `ug`. `request->source` is optional: the models find the language themselves, so it is only
checked. `request->context` holds the earlier lines of the same speaker in the source language,
oldest first, separated by newlines; two lines are enough. They help the model with names and
pronouns, and they are not translated. Pass source text, not translations, so that a translation
that was late or dropped changes nothing for the lines after it.

The call is synchronous. Calls on one translator run one at a time. The output is greedy, and each
call is independent of the one before. The generation stops at 5 tokens per source token plus 32:
`capped` is then 1, and the text may end mid-sentence. The text is always whole UTF-8.

The result has the translation in `text`, and the numbers `source_tokens`, `prompt_tokens`,
`output_tokens`, `prefill_ms` and `decode_ms`. It belongs to the library and is valid until the next translate call on
the same thread; copy what you need to keep. A call returns NULL on
failure: `ECHOIR_ERR_INVALID_ARG` for a NULL argument, a blank text, a language that is not
supported or a text that does not fit the model's context (about 1900 tokens; when the context lines do not fit with it, the oldest are dropped first and only
a text that is too long alone is refused),
and `ECHOIR_ERR_BACKEND` when the model fails.

```c
void echoir_translator_free(echoir_translator* t);
```

Frees the translator and its model. No call may be running on it.

## Event types

Each event is an `echoir_event`:

```c
typedef struct {
  echoir_event_type type;
  int64_t     segment_id;   /* monotonic per session; a PARTIAL and its FINAL share it */
  int64_t     start_ms, end_ms;
  const char* text;         /* UTF-8, valid as described under Ownership */
  const char* detail;       /* ERROR message or STATS JSON; NULL otherwise */
} echoir_event;
```

| Type | Meaning |
|---|---|
| `ECHOIR_EVENT_PARTIAL` | Provisional text of the segment in progress. A FINAL with the same `segment_id` replaces it. |
| `ECHOIR_EVENT_FINAL` | Text of a finished segment. |
| `ECHOIR_EVENT_CLEARED` | The transcript was cleared by `echoir_session_clear()`. Drop what is shown. |
| `ECHOIR_EVENT_ERROR` | A failure while streaming. `detail` holds the message. A lost capture device gives `"device_lost"`. |
| `ECHOIR_EVENT_STATS` | Decoding speed. Sent after every 10 ticks that decode audio. `start_ms` and `end_ms` are 0. |

The `detail` of a STATS event is a JSON object:

```json
{"rtf":0.12,"decode_ms":180,"window_ms":2600,"ticks":10,"new_ms":1000,"speech_ms":800,
 "decode_max_ms":400,"load":0.18,"speech_rtf":0.2,"wait_ms":0,"backend":"qwen3-asr"}
```

`rtf` is the decode time over the audio time of the windows, carried audio included. `decode_ms` and
`window_ms` are the mean decode time and the mean length of the audio decoded per tick, in ms. `ticks`
is the number of ticks covered, `new_ms` the mean audio a tick took in, `speech_ms` the mean length of
the speech it decoded, and `decode_max_ms` the slowest tick. `load` is the decode time over the new
audio, the share of real time the decoder is busy. Once a decode takes longer than `min_chunk_ms`,
ticks run back to back and `load` stays near 1 even while the session keeps up; use
`echoir_session_read_lag()` to tell whether the backlog grows. `speech_rtf` is the `rtf` over the
ticks that decoded speech, and 0 when none did. `wait_ms` is the mean time per tick the session
waited while other sessions of the engine decoded; it is 0 for a session alone on its engine, and
it is not part of `decode_ms`, `decode_max_ms`, `rtf`, `load` or `speech_rtf`, which stay the
model's speed. `backend` names the backend.

`detail` is NULL for every event type except ERROR and STATS.

## Related documents

- [protocol.md](protocol.md) describes the JSON-lines service that `echoir serve --stdio` runs.
- [cli.md](cli.md) describes the command-line tool, which uses the same engine.
- [bindings/csharp/README.md](../bindings/csharp/README.md) describes the .NET binding.
