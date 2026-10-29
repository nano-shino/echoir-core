# echoir CLI

`echoir transcribe` transcribes a WAV file at any sample rate and channel count, a WAV or raw samples
on standard input (the file `-`), or live audio from a capture device. It needs one of two modes:

- `--offline` transcribes the whole file at once.
- `--stream` pushes the file through a streaming session in steps of `--chunk-ms` (500 by
  default), the way live audio arrives. `--format` picks `text` (the final segments, with the
  partial line redrawn in place on a terminal), `jsonl` (every event, one JSON object per line) or
  `srt`. `--pace none`, the default, pushes the audio as fast as it is decoded, so the output is
  deterministic. `--pace realtime` pushes it at the speed of the audio, while the session's worker
  decodes. `--min-chunk-ms` (500 by default) is how much new audio a decode waits for; 1000 halves
  the decode work at the cost of slower partials. `--max-segment-ms` (10000 by default) is the
  length at which a segment is cut at its latest pause (or, with none, at a quiet point in the 3 s
  before that length), so no final is longer than it; a shorter length gives shorter finals at the
  cost of more cuts. Streaming VADs do not split long speech, so this cut is the only one there
  (offline, the VAD splits it).
- `--policy` picks how the partial line is stabilized. `rollback`, the default on Qwen3-ASR, gives
  the decoder its previous output minus the last few tokens as a fixed start, so it only rewrites
  the end. `agreement`, the default on Whisper and available on both, keeps the part that the
  latest decodes agree on.

Live audio comes from a capture device: `--input mic` is the default microphone, and
`--input mic:<device>` picks another, by index or part of its name from `echoir devices`. Live
capture needs `--stream` and runs until Ctrl-C, or for `--duration` seconds. `--input loopback`
records what the system plays, through WASAPI, and works on Windows only. On macOS, route the audio
to a loopback device such as BlackHole and use it as `mic:<device>`.

Raw samples on standard input have no header. `--pcm s16le` (16-bit signed) or `--pcm f32le`
(32-bit float), both little-endian, says what they are, and `--rate` and `--channels` give their
format. This pipe feeds the bundled JFK fixture to `transcribe` as raw samples:

```sh
python3 -c 'import sys, wave; w = wave.open("tests/fixtures/jfk.wav"); sys.stdout.buffer.write(w.readframes(w.getnframes()))' \
  | build/macos-cpu/bin/echoir transcribe - --pcm s16le --rate 16000 --channels 1 --offline --model whisper-tiny-en
# needs a microphone:
build/macos-cpu/bin/echoir transcribe --input mic --stream --duration 30 --model whisper-tiny-en
```

```sh
build/macos-cpu/bin/echoir transcribe tests/fixtures/jfk.wav --model whisper-tiny-en --offline
build/macos-cpu/bin/echoir transcribe build/fixtures/zh_short.wav \
  --backend qwen --model qwen3-asr-0.6b-q8_0 --offline
build/macos-cpu/bin/echoir transcribe build/fixtures/sentences_en.wav --stream --format jsonl \
  --backend qwen --model qwen3-asr-0.6b-q8_0
```

Each output line is one speech range, with its start and end time and its text. A range longer
than 20 s is cut at quiet points, so it takes more than one line. `--model` takes a registry id or
a model file. A model file needs `--backend`. Qwen also needs a projector: a
registry id brings its own, and `--mmproj` gives one for a model file or overrides the id's. `--vad`
takes a VAD id or file. Without it, the VAD is the backend's `default_vad` in the registry:
`silero-v6.2.0` for Whisper and `firered-vad-v1` for Qwen. The GPU is used by default; `--no-gpu`
runs on the CPU.

`--context TEXT`, or `--context-file PATH` for a UTF-8 file, sends text with every decode, such as
a list of proper names the speaker uses. Line breaks become spaces. The context is limited to 223
tokens with Whisper and 512 with Qwen. A longer context is an error, not cut short. With Whisper,
the oldest history is dropped first to make room for the context.

Set `ECHOIR_LOG=debug` to see the whisper.cpp and llama.cpp logs on stderr.

The tool looks for these directories, in order: the `--models-dir` option, the `ECHOIR_MODELS`
environment variable, `./models`, `models/` beside the executable, and the repository's `models/`
when the executable is in a build tree (`build/<preset>/bin`). It reads `registry.json` from the
first of them that has one. Without one, it uses the catalogue built into the library, with model
files from the first of them that exists. Exit status is 0 on success, 1 when a model, the input or a decode fails,
and 2 for a usage error.

`echoir bench` transcribes one file with several models and scores each transcript against a
reference text. Without `--model`, it runs the default Whisper model (`whisper-tiny-en`) and the
default Qwen model (`qwen3-asr-0.6b-q8_0`), and skips any whose model or default VAD is not on
disk. `--backend` picks
one backend, and `--model` picks one registry id. The table shows the error rate (word error rate,
or character error rate for CJK text), the edit counts, the audio and wall times, the real-time
factor (wall time over audio time) and the model load time, which the factor leaves out.

```sh
build/macos-cpu/bin/echoir bench build/fixtures/sentences_en.wav \
  --ref build/fixtures/sentences_en.ref.txt --no-gpu
```

`echoir score` compares a transcript with a reference text. Both are UTF-8 files, and `-` reads
standard input. It prints one JSON line, `{"rate":…,"subs":…,"dels":…,"ins":…,"ref_len":…}`, where
the rate is `(subs + dels + ins) / ref_len`. `--normalize` prints the normalized text of one file
instead, which is the form the comparison uses.

```sh
build/macos-cpu/bin/echoir score transcript.txt reference.txt
```

`echoir devices` lists the capture devices and the loopback devices, with the indexes that
`--input mic:<index>` takes. `echoir models list` shows each model in the registry with its
backend, whether its files are on disk, and its size. `echoir models path <id>` prints the path of
each file of a model, or the command that fetches it when a file is missing.

```sh
build/macos-cpu/bin/echoir devices
build/macos-cpu/bin/echoir models list
build/macos-cpu/bin/echoir models path whisper-tiny-en
```

`echoir serve --stdio` runs a JSON-lines service for a host process. The host writes one command per
line to standard input, such as `load`, `start`, `audio`, `flush`, `stop` and `quit`. The service
writes the transcript events, and one reply per command, to standard output. The commands, their
fields and the events are described in [protocol.md](protocol.md).

```sh
printf '{"cmd":"load","model":"whisper-tiny-en"}\n{"cmd":"quit"}\n' | build/macos-cpu/bin/echoir serve --stdio
```

