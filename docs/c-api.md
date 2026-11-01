# C API

The C ABI is the interface that a host process uses to call echoir-core in process. It is declared in
`include/echoir/echoir.h`. The header is plain C99 and works from C and C++.

It is built for live transcription. A host loads a model into an engine, opens a session on the
engine, and then either pushes PCM audio into the session or lets the library capture an audio
device. The host reads the transcript as events: from a callback, or by polling a queue.

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
| `ECHOIR_ERR_STATE` | The call is not valid now: wrong driving mode, the engine already serves a session, or the event queue is empty. |
| `ECHOIR_ERR_NOT_IMPLEMENTED` | The function is declared but not available in this build. |
| `ECHOIR_ERR_INTERNAL` | An unexpected failure inside the core. |

Functions that return a pointer return NULL on failure. The `*_params_init` functions and the
`*_free` functions do not report errors.

### Error messages

Call `echoir_last_error()` after a failure to get the reason. The message has the form
`<function>: <reason>`. It is kept per thread, so it describes the most recent failing call on the
calling thread. Engine creation returns a pointer, so it has no status code. Its reason is only
available from `echoir_last_error()`.

### Strings

All strings are UTF-8, on every platform. This includes model and directory paths, device ids and
the JSON text that the library returns.

### Ownership

- Strings returned by `echoir_version()` have static storage. Do not free them.
- The string from `echoir_last_error()` stays valid until the calling thread's next failing call.
- The strings from `echoir_devices_json()` and `echoir_models_json()` are owned by the library. They
  stay valid until the next `*_json` call on the same thread. Copy them if you need them longer.
- Engines and sessions come from a create call. Free them with `echoir_engine_free()` and
  `echoir_session_free()`. Free the sessions of an engine before the engine.
- Path strings in the params structs are read during the create call only. The host can release them
  afterwards.
- Event `text` and `detail` pointers are valid only during the callback. In queue mode they stay valid
  until the next `echoir_session_poll()` or until the session is freed.
- Freeing NULL is a no-op.
- The buffers that the host passes to `echoir_session_read_audio()` and
  `echoir_session_read_peaks()` belong to the host.

### Threading

- A handle (engine or session) is used by one thread at a time.
- `echoir_session_push_f32()`, `echoir_session_push_s16()`, `echoir_session_read_audio()`,
  `echoir_session_read_peaks()` and `echoir_session_set_input_muted()` are thread-safe. They can run on
  any thread, in either driving mode.
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
  capture.
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
  knows. A smaller size is refused, and `echoir_engine_create()` returns NULL.
- **`echoir_session_params`:** a smaller size is accepted if it covers every field up to
  `emit_partials`. The fields after it take their defaults. A larger size is refused.
- **`echoir_model_params`:** a smaller size is accepted if it covers every field up to `model_id`.
  The fields after it take their defaults. A larger size is refused.

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
   `echoir_session_poll()` until it returns `ECHOIR_ERR_STATE`.
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

### Parameter defaults

```c
void echoir_engine_params_init(echoir_engine_params* p);
void echoir_session_params_init(echoir_session_params* p);
void echoir_model_params_init(echoir_model_params* p);
```

Each sets `*p` to its defaults and sets `struct_size`. With `p` NULL, nothing is written and the last
error is set. The defaults are:

- Engine: Whisper backend, `use_gpu` 1, `n_threads` 0, all paths NULL.
- Session: `language` `"auto"`, `min_chunk_ms` 500, `max_segment_ms` 10000, `emit_partials` 1,
  `stream_policy` `ECHOIR_POLICY_DEFAULT`, `retain_audio_ms` 0.
- Model: `registry_json`, `models_dir`, `model_id` and `vad_id` NULL, `use_gpu` 1, `n_threads` 0.

### Engine

```c
echoir_engine* echoir_engine_create(const echoir_engine_params* params);
```

Loads the models named in `*params` and returns a new engine, or NULL on failure. `model_path` is the
speech model, `mmproj_path` is the Qwen projector (Qwen only, otherwise NULL), and `vad_model_path` is
required. The paths are read during this call only. Free the engine with `echoir_engine_free()`.

```c
echoir_engine* echoir_engine_create_from_model(const echoir_model_params* params);
```

Looks up `model_id` and its VAD in the catalogue, then checks that their files are in `models_dir` with
the sizes that the catalogue gives. It then creates the engine as `echoir_engine_create()` does. An
unknown or non-speech id fails with `ECHOIR_ERR_INVALID_ARG`. A missing or wrong-size file fails with
`ECHOIR_ERR_MODEL_LOAD`. Both are reported through `echoir_last_error()`.

```c
void echoir_engine_free(echoir_engine* engine);
```

Frees the engine and its models. Free its sessions first.

### Session

```c
echoir_session* echoir_session_create(echoir_engine* engine, const echoir_session_params* params,
                                      echoir_event_cb cb, void* user);
```

Creates a session on `engine`, or returns NULL. It fails if the engine already serves a session, if
`max_segment_ms` is below 200, or if `stream_policy` is unknown or is `ECHOIR_POLICY_ROLLBACK` on a
backend without decoder-prefix support. `*params` is read during this call only.

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
`ECHOIR_ERR_STATE` when the queue is empty, or when the session is in callback mode.

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

It returns `ECHOIR_ERR_STATE` when the session keeps no audio (`retain_audio_ms` is 0), or when part of
the range is older than what is kept. It returns `ECHOIR_ERR_INVALID_ARG` for a negative `start_ms`,
an `end_ms` before `start_ms`, a NULL `n_frames`, or a `cap` that is too small for the range.

```c
echoir_status echoir_session_read_peaks(echoir_session* session, int64_t start_ms, int64_t end_ms,
                                        int32_t bucket_ms, float* out_minmax, size_t cap,
                                        size_t* n_buckets);
```

Writes the level envelope of `[start_ms, end_ms)`. Each bucket is `bucket_ms` long, and it has a
min and a max value at full scale ±1. `bucket_ms` must be a positive multiple of 10. The envelope keeps
one pair per 10 ms for the whole session, whatever `retain_audio_ms` is.

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

If the chosen device is lost, an ERROR event with `detail` set to `"device_lost"` is sent, and the
capture ends. The session keeps running until it is stopped. If the default device is lost, the capture
reopens the new default. If the system has no device at all, it retries every second until one appears
or the session is stopped. No event is sent during that wait.

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
model's backend.

```c
const char* echoir_models_json(const char* registry_json, const char* models_dir);
```

Returns the catalogue and the machine as JSON:

```json
{"devices":[{"name","type":"gpu"|"cpu","description","integrated","memory_total_mb","memory_free_mb"}],
 "default_vad":{"whisper":id,"qwen":id},
 "models":[{<entry>,"files":[{<file>,"present":bool}],"present":bool,"fits":bool}]}
```

`registry_json` NULL selects the built-in catalogue. `models_dir` NULL or empty means no file is present.
`name` is ggml's device id. `description` is its readable name and may be empty. `integrated` marks a GPU
that shares system memory, so its memory size says little about its speed.

`fits` is true when `min_vram_mb` is at most the free memory of the best device. The best device is the
GPU with the most free memory, or the CPU when there is no GPU. An entry without `min_vram_mb` always
fits. `devices` is empty in a build without the backends. The string is owned by the library. It returns
NULL on failure, for example when `registry_json` does not parse.

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
{"rtf":0.12,"decode_ms":180,"window_ms":2600,"backend":"qwen3-asr"}
```

`rtf` is the decode time over the audio time. `decode_ms` and `window_ms` are the mean decode time and
the mean length of the audio decoded per tick, in ms. `backend` names the backend.

`detail` is NULL for every event type except ERROR and STATS.

## Related documents

- [protocol.md](protocol.md) describes the JSON-lines service that `echoir serve --stdio` runs.
- [cli.md](cli.md) describes the command-line tool, which uses the same engine.
- [bindings/csharp/README.md](../bindings/csharp/README.md) describes the .NET binding.
