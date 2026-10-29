# Stdio protocol

`echoir serve --stdio` runs a transcription service that a host process drives through standard
input and standard output. The host sends commands, one JSON object per line. The service writes
transcript events and one reply per command, one JSON object per line.

```
echoir serve --stdio [--models-dir DIR]
```

`--models-dir` names a directory with `registry.json` and the model files. It is searched first,
before `$ECHOIR_MODELS`, `./models` and the models directory beside the executable.

## Transport

- **stdin:** one JSON object per line, UTF-8. Blank lines are ignored, and a trailing carriage
  return is allowed.
- **stdout:** only protocol lines: events and replies. Each line is one complete JSON object with
  no line breaks inside it, and each is flushed when it is written. A reader can read line by line.
- **stderr:** logs and diagnostics. Nothing on stderr is part of the protocol.
- **Exit status:** `0` after `quit` or at the end of input. `2` for a usage error, such as a missing
  `--stdio`. `1` for a failure outside command handling.

The service handles one command at a time, in the order the lines arrive. A session runs on its
own thread, so events can arrive at any moment while it runs (see [Ordering](#ordering)).

## Commands

Every command is an object with a string field `cmd`. Other fields are listed below. Unknown
fields are ignored. An optional field may be omitted, or set to `null`, to take its default. A
required field must be present.

| `cmd` | Fields | Effect | Reply |
|---|---|---|---|
| `load` | `model`, `backend`, `mmproj`, `vad`, `gpu`, `threads` | Loads the models | `ok` |
| `start` | `input`, `device`, `language`, `policy`, `context`, `min_chunk_ms` | Starts a session | `ok` |
| `audio` | `pcm_s16le_b64`, `rate`, `channels` | Pushes audio into a `push` session | `ok` |
| `flush` | none | Decodes all pending audio and ends the segment in progress | `ok` |
| `reset` | none | Flushes, then drops all audio and text, and restarts the timeline at 0 | `ok` |
| `clear` | none | Drops the text on screen and in flight | `ok` |
| `stop` | none | Ends the session, with its final events | `ok` |
| `devices` | none | Lists the capture and loopback devices | `devices` |
| `quit` | none | Ends the session, if one runs, and exits | `ok` |

### `load`

Loads the speech model and its VAD. It replaces any models already loaded, and it is refused while a
session runs.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `model` | string | required | A registry id, such as `whisper-tiny-en` or `qwen3-asr-0.6b-q8_0`, or the path of a model file. |
| `backend` | string | from the registry | `whisper` or `qwen`. Required when `model` is a file path. When both are given, they must agree. |
| `mmproj` | string | from the registry | The Qwen projector: a registry id or a file. Only for `qwen`. |
| `vad` | string | the backend's default VAD | A registry id or a file. The default is `silero-v6.2.0` for `whisper` and `firered-vad-v1` for `qwen`. |
| `gpu` | boolean | `true` | Use the GPU when this build has one. |
| `threads` | integer | `0` | Threads for decoding. `0` takes the default. |

The held models are released before the new ones load, so a failed load leaves no model loaded.
Reply: `ok`. Errors: a failed load, an unknown registry id, a `backend` that is not `whisper` or
`qwen`, or a session that is still running (`send stop first`).

### `start`

Starts a session. Its events are written as they are produced, until `stop`, `quit` or the end of
input.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `input` | string | required | `push`: the host sends audio with `audio` commands. `mic`: the default microphone, or `device`. `loopback`: system audio (Windows only). |
| `device` | string | the default device | For `mic` and `loopback`: a device index from `devices`, or part of a device name, not case-sensitive. |
| `language` | string | `auto` | `auto` to detect the language, or an ISO code such as `en`. |
| `policy` | string | the backend default | `agreement` or `rollback`. The backend default is `rollback` for `qwen` and `agreement` for `whisper`. `rollback` needs a backend with decoder-prefix support, so it fails on `whisper`. |
| `context` | string | none | Text sent with every decode, such as proper names. |
| `min_chunk_ms` | integer | `500` | Audio that must be pending before a decode runs, in milliseconds. `0` or more. |

Reply: `ok`, once the session is running. Errors: no model loaded, a session already running, an
unknown `input`, an unsupported `policy`, a `language` the model does not have, or a device that
cannot open. With `mic` or `loopback`, the service captures the audio itself, so `audio` is
refused.

With the default device (no `device`), the capture follows the system default. When the system has
no device at all, it waits, sending nothing, and resumes when one appears. A chosen `device` that
is lost gives an `error` event (`device_lost`); no more audio arrives, and the session stays until
`stop`.

### `audio`

Pushes audio into a session started with `input` set to `push`.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `pcm_s16le_b64` | string | required | Standard base64 (RFC 4648, with `=` padding, no line breaks) of 16-bit signed little-endian PCM. Samples are interleaved by channel. |
| `rate` | integer | required | Sample rate in Hz. Any positive rate. |
| `channels` | integer | required | Channels per frame. Any positive count. |

The decoded byte count must be even, and the number of samples must be a whole number of frames.
The audio is resampled to 16 kHz mono inside the service. A chunk of 100 ms at 16 kHz mono is 3200
bytes, or 4268 base64 characters, and this size works well. Reply: `ok` once the audio is accepted.
Decoding happens later, and its events follow on their own.

Errors: no session, a session that is not `push`, a missing or non-positive `rate` or `channels`,
bad base64, an odd byte count, or a count of samples that is not a whole number of frames.

### `flush`, `reset`, `clear`

These need a running session. Their events are written before their reply.

- `flush` decodes all audio that is still pending, then ends the segment in progress as a `final`.
  Timestamps keep running.
- `reset` does a `flush`, then drops all audio and text state. The timeline restarts at 0. Segment
  ids keep counting up.
- `clear` drops the text in flight and sends `cleared`. Audio of a segment still in progress is kept,
  so that segment's text can show again in a later `partial`.

### `stop`

Ends the session. The service stops capturing, decodes the audio it still has, finalizes the segment
in progress, and stops the session. All of its `final` events are written before the reply `ok`.
After that the session is gone, and the next `start` begins a new one. Error: no session running.

### `devices`

Lists the capture devices and the loopback devices. The reply has no `ok`:

```json
{"type":"devices","capture":[{"index":0,"id":"0a1b2c","name":"MacBook Pro Microphone","default":true}],"loopback":[]}
```

`index` is the position that `device` accepts, and `id` is the stable device id the C ABI takes.
`loopback` is empty except on Windows.
Errors: an audio system that cannot be opened, as an `error` reply with `cmd` `devices`.

### `quit`

Ends the session, if one runs, and then exits with status `0`. The reply is `ok`. If the final flush
of the session fails, the reply is an `error` with `cmd` `quit`, and the process still exits with
`0`.

### End of input

When stdin ends, the service behaves as `quit` does, with no reply, and exits with status `0`. If
the final flush fails, the message goes to standard error instead.

## Events

Each event is one line. It has a `type`, and the other fields depend on the type.

**`partial`** is the text of the segment in progress, and it changes as more audio arrives:

```json
{"type":"partial","id":7,"start_ms":12340,"end_ms":15020,"text":"what is your"}
```

**`final`** is the text of a segment that is complete. It replaces the `partial` with the same `id`.
An empty `text` means the segment has no speech: remove its `partial` and show nothing:

```json
{"type":"final","id":7,"start_ms":12340,"end_ms":15900,"text":"What is your name?"}
```

**`cleared`** is sent by `clear`. Drop the text on screen, including the current `partial`:

```json
{"type":"cleared"}
```

**`error`** reports a decode or capture failure. It has no `cmd`. The session keeps running: the
failed audio is dropped and the next audio is decoded. A lost device sends an `error` too, and no more
audio arrives after that. Send `stop` to end the session:

```json
{"type":"error","message":"device_lost"}
```

Times are milliseconds from the start of the session, or from the last `reset`. Segment ids start at
1 in a session and never repeat, so a `partial` and its `final` share an id.

`stats` is reserved for latency measurements. The service never sends it.

## Replies

A reply names the command it answers in `cmd`:

```json
{"type":"ok","cmd":"start"}
{"type":"error","cmd":"audio","message":"audio needs a session started with input push"}
```

The `message` of an error is for people. Its wording may change, so match on `type` and `cmd`.

## Ordering

- Every command gets exactly one reply, in the order the commands arrive. The reply to `devices` is
  the device list, which has no `ok`.
- Events are never split across lines, and they never interleave inside a line.
- Events of a running session may appear between replies, including between the `ok` of an `audio`
  command and the `ok` of the next one.
- The events of `flush`, `reset`, `clear` and `stop` are written before that command's reply.
- After the `ok` of `stop`, the session sends no more events.

## Errors

- A line that is not a JSON object, or has no string `cmd`, gets an error reply with `"cmd":""`.
- An unknown `cmd` gets an error reply with that `cmd`.
- A command that fails gets an error reply with its `cmd`. The service keeps reading the next line.
- A session error is an `error` event without a `cmd`, as described above.

## Example

The host sends these lines (the `audio` lines are elided; this session sends about 110 of them,
100 ms each, for 11 seconds of speech from the JFK sample):

```json
{"cmd":"load","model":"whisper-tiny-en"}
{"cmd":"start","input":"push"}
{"cmd":"audio","pcm_s16le_b64":"<base64 of 3200 bytes>","rate":16000,"channels":1}
{"cmd":"flush"}
{"cmd":"stop"}
{"cmd":"quit"}
```

The service writes these lines (from a run with the `whisper-tiny-en` model):

```json
{"type":"ok","cmd":"load"}
{"type":"ok","cmd":"start"}
{"type":"partial","id":1,"start_ms":0,"end_ms":600,"text":"And"}
{"type":"partial","id":1,"start_ms":0,"end_ms":11000,"text":"And so my fellow Americans ask not what your country can do for you ask what you can do for your country."}
{"type":"final","id":1,"start_ms":0,"end_ms":11000,"text":"And so my fellow Americans ask not what your country can do for you ask what you can do for your country."}
{"type":"ok","cmd":"flush"}
{"type":"ok","cmd":"stop"}
{"type":"ok","cmd":"quit"}
```

The `partial` lines came while the audio was still arriving, so their place among the `audio`
replies is not fixed. The `final` came from the flush, and it is written before the `ok` of `stop`.
