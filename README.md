# echoir-core

echoir-core turns live audio into live transcripts. It is a portable C++20 library with a C ABI
(`include/echoir/echoir.h`), a C# binding, and the `echoir` command-line tool. Audio goes in at
any sample rate and channel count, and partial and final transcript segments come out with
timestamps. The host can push PCM, or let the library capture a microphone, or loopback on
Windows, through miniaudio.

Transcription runs on whisper.cpp (Whisper models) and on llama.cpp's libmtmd (Qwen3-ASR). Both
use one shared ggml.

**Status:** early development, pre-release. Interfaces and command-line options may still change
before the first release.

## Features

- Two speech recognition backends: Whisper (whisper.cpp) and Qwen3-ASR (llama.cpp libmtmd).
- Streaming sessions that report partial and final segments with timestamps, driven by the host
  or by a live worker thread.
- Voice activity detection with Silero and FireRedVAD.
- Live capture from microphones, and from loopback devices on Windows, through miniaudio.
- A C ABI (`include/echoir/echoir.h`) and a C# binding, `Echoir.Core`.
- The `echoir` command-line tool: `transcribe`, `bench`, `score`, `devices`, `models`, and `serve`,
  a JSON-lines service for host processes.
- CPU, Metal (macOS, iOS) and Vulkan (Windows) GPU backends. On Android the CPU backend ships in one build per ARM feature set (dotprod, fp16, i8mm, SVE2, SME), and the best one the device runs is loaded.

The context prompt is not in the C ABI yet. The command line and `echoir serve` take one.

## Quick start

### Prebuilt archives

Download the archive for your platform from the
[GitHub releases](https://github.com/nano-shino/echoir-core/releases). The file names carry the
project version, shown here as `<version>`:

| Archive | For |
|---|---|
| `echoir-<version>-windows-cpu-x64.zip` | Windows x64, CPU |
| `echoir-<version>-windows-vulkan-x64.zip` | Windows x64, Vulkan GPU |
| `echoir-<version>-macos-arm64-metal.tar.gz` | macOS on Apple silicon, Metal GPU |
| `echoir-<version>-linux-x64-cpu.tar.gz` | Linux x64, CPU |
| `echoir-<version>-android-arm64.zip` | Android arm64 native libraries, for an app |
| `echoir-<version>-ios-arm64.zip` | iOS `Echoir.xcframework`, device and simulator slices |
| `Echoir.Core.<package version>.nupkg` | The C# binding, with the native libraries for the platforms above |

The desktop archives hold the `echoir` command line, the libraries it uses,
`models/registry.json`, the license notices, and the full third-party license texts in `licenses/`.
Each release also has `SHA256SUMS`. Model files
are not in the archives.

### Get a model

Fetch the model files from a clone of this repository. `tools/fetch-models.sh` writes them to the
`models/` folder of the clone. The Whisper backend also needs the Silero VAD:

```sh
git clone https://github.com/nano-shino/echoir-core.git
cd echoir-core
tools/fetch-models.sh whisper-tiny-en silero-v6.2.0
```

On Windows, run `tools\fetch-models.ps1` with the same ids.

### Transcribe a file

From the folder you unpacked the archive into, point `--models-dir` at the clone's `models/`
folder and transcribe a WAV file. Use the path to the clone in place of `/path/to/echoir-core`:

```sh
./echoir transcribe your-audio.wav --model whisper-tiny-en --offline \
  --models-dir /path/to/echoir-core/models
```

On Windows, run `echoir.exe` with the same arguments. Each output line is one speech range, with
its start and end time and its text. For Qwen3-ASR, use `--backend qwen --model qwen3-asr-0.6b-q8_0`
and fetch that model and `firered-vad-v1`. The [command-line reference](docs/cli.md) covers the
rest.

## Build from source

You need CMake 3.24 or newer, Ninja, and a C++20 compiler. The macOS presets use Apple clang 17
on Apple silicon and do not need the full Xcode.

Clone the repository with its submodules, which are whisper.cpp and llama.cpp:

```sh
git clone --recursive https://github.com/nano-shino/echoir-core.git
cd echoir-core
```

If you cloned without `--recursive`, run `git submodule update --init --recursive` in the clone.
Then build and test:

```sh
brew install cmake ninja
cmake --preset macos-cpu
cmake --build --preset macos-cpu
ctest --preset macos-cpu
```

Each preset builds into `build/<preset>/`. The first build compiles whisper.cpp and llama.cpp,
which takes several minutes.

| Preset | What it builds |
|---|---|
| `macos-cpu` | macOS arm64, CPU only |
| `macos-metal` | macOS arm64 with the ggml Metal backend. The Metal kernels compile at runtime, so the first model load is slower. |
| `macos-static` | macOS arm64, CPU only, with static libraries |
| `windows-cpu-x64` | Windows x64 with MSVC and Ninja. Built and tested in CI. |
| `windows-vulkan-x64` | Windows x64 with MSVC, Ninja and the Vulkan SDK. Built and tested locally, not in CI. |
| `linux-cpu`, `android-arm64`, `ios-arm64`, `ios-arm64-simulator` | Presets for the release archives. They have no test preset. |

`cmake --list-presets` shows only the presets for the current host.

### Windows

Run the commands from a Visual Studio 2022 x64 developer shell, or after `vcvars64.bat`. The
Vulkan preset also needs the Vulkan SDK, with `VULKAN_SDK` set.

```bat
cmake --preset windows-vulkan-x64
cmake --build --preset windows-vulkan-x64
ctest --preset windows-vulkan-x64
tools\package-windows.ps1 -Preset windows-vulkan-x64
```

The build puts `echoir.exe`, `echoir.dll` and one copy of each ggml DLL in `build\<preset>\bin`.
`tools\package-windows.ps1` zips them, with `models\registry.json` and a README, into
`dist\echoir-<version>-<preset>.zip`. The zip runs from any folder. Model files are not in it.
The e2e cases use committed speech clips and the fixtures that `tools/make-fixtures.py` makes. Cases
whose model is not in `models\` skip.

Build options:

| Option | Default | Meaning |
|---|---|---|
| `ECHOIR_WITH_WHISPER` | `ON` | Whisper backend, Silero VAD and FireRedVAD. The VAD code needs whisper.cpp. |
| `ECHOIR_WITH_QWEN` | `ON` | Qwen3-ASR through libmtmd. Requires `ECHOIR_WITH_WHISPER=ON`. |
| `GGML_METAL` | `ON` on Apple, else `OFF` | ggml Metal backend |
| `GGML_VULKAN` | `OFF` | ggml Vulkan backend |
| `GGML_CUDA` | `OFF` | ggml CUDA backend |

If a submodule is missing, CMake warns and skips the backends that need it. The unit tests still
build.

## C# binding

`bindings/csharp/Echoir.Core` is the `Echoir.Core` library (`net10.0`). It wraps the C ABI and
needs only the base class library. Its
[README](bindings/csharp/README.md) describes the projects, how the native libraries are found, and
how to pack the NuGet package.

## C API

`include/echoir/echoir.h` is the public C ABI. It documents each function and struct. For a host
that cannot link the library, `echoir serve --stdio` runs the same engine as a JSON-lines service.
[docs/protocol.md](docs/protocol.md) describes its commands and events.

## Models and fixtures

Model files go in `models/`, which git ignores. `models/registry.json` lists the models, their
files and their download URLs. Fetch the models you need by id:

```sh
tools/fetch-models.sh whisper-tiny-en silero-v6.2.0 firered-vad-v1
```

On Windows, run `tools\fetch-models.ps1` with the same ids. Integration tests whose model is not
on disk print SKIP and pass.

If a FireRedVAD file is not hosted, `fetch-models.sh` downloads the upstream checkpoints and
converts them with `tools/convert-firered-vad.py`, which needs only Python 3. The Windows script
prints the same commands instead.

`python3 -I tools/make-fixtures.py` builds the generated test audio into `build/fixtures/`. It uses
the Python standard library only and runs on any host.

## Command line

`echoir` is the command-line tool. It has these subcommands:

- `transcribe` transcribes a WAV file, standard input or a microphone, either whole (`--offline`)
  or through a streaming session (`--stream`).
- `bench` transcribes one file with several models and scores each transcript.
- `score` scores a transcript against a reference text.
- `devices` lists the capture devices, and `models` shows the model registry.
- `serve --stdio` runs a JSON-lines service for a host process.

```sh
build/macos-cpu/bin/echoir transcribe tests/fixtures/jfk.wav --model whisper-tiny-en --offline
```

The [command-line reference](docs/cli.md) describes every option and output format.

## Tests

`ctest --preset <preset>` runs these tests. The presets leave out the `long` label, except
`macos-tsan`, which runs the `unit` and `stress` labels.

- `unit` and the `cli_*` tests (label `unit`): doctest tests in `tests/unit/`, a C99 check of the
  public header, and checks of the `echoir` command line. They cover its help, its usage errors,
  `echoir score` on a text against itself, and the VAD and context options. They need no models.
- `integration` and `capi` (label `integration`): tests in `tests/integration/`. They use the
  models and fixtures when they are present. `capi` is built as its own program. It links the
  `echoir` library and the pure text helpers, but not the engine objects.
- `e2e.<case>` (label `e2e`): one test per case in `tests/e2e/cases.json`. Each case runs
  `echoir transcribe` on a fixture, scores the transcript with `echoir score`, and fails when the
  rate is above the case's `max_rate` or a `must_contain` phrase is missing. A silent case
  (`expect_empty`) has no rate: it fails when its transcript has more words than `max_words`. A
  case whose model files or fixture are not on disk is reported as skipped. The runner is
  `tests/e2e/run_e2e.py`, which needs Python 3 and only its standard library. The cases exist only
  when CMake finds Python and the command-line tool is built (`ECHOIR_WITH_WHISPER=ON`).
- `qwen_stream_golden` (label `long`): the Qwen streaming cases on real models, about 100 s. Run it by
  name, for example `ctest --preset macos-cpu -R qwen_stream_golden`.
- `stress_live` (labels `long` and `stress`): a 60 s live-mode stress run on scripted backends, with
  no models. It runs under `macos-tsan`.

To run one label, add a filter, for example `ctest --preset macos-cpu -L unit`.

## Layout

- `include/echoir/echoir.h`: the public C ABI, with each function and struct documented.
- `src/audio`, `src/text`, `src/stream`, `src/capture`, `src/models`: the core parts that load no
  model files: audio conversion, text handling, the streaming session (which takes its backend and
  VAD as interfaces), device capture, and the model catalogue.
- `src/vad`, `src/backends`: Silero and FireRedVAD voice activity detection, and the Whisper and
  Qwen3-ASR backends. They are built when whisper.cpp is. The Qwen3-ASR backend also needs
  `ECHOIR_WITH_QWEN` (on by default) and the llama.cpp submodule.
- `src/capi`: the C ABI implementation.
- `cli/`: the `echoir` command-line tool. It links the engine directly.
- `bindings/csharp/`: the C# binding and its tests.
- `tests/`: unit and integration tests, the end-to-end cases in `tests/e2e/`, and the committed
  fixtures.
- `tools/`: model download, the FireRedVAD converter and fixture generation. `tools/dev/` holds
  development-only tools, described in its README.
- `docs/`: the command-line reference and the JSON-lines protocol.
- `third_party/`: the whisper.cpp and llama.cpp submodules, and vendored single-header libraries
  (doctest, miniaudio, CLI11, nlohmann/json). Each library keeps its license file next to it.

## Conventions

Line endings are LF. C++ follows `.clang-format`: Google base, 4-space indent, 100 columns. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the build, test and style rules.

## Third-party code and models

The FireRedVAD voice activity detector is FireRedTeam's work, licensed under Apache-2.0. The code in
`src/vad/firered_*`, `src/audio/fbank.*` and `src/stream/vad_postprocess.*` ports the network,
feature front end and post-processor of the upstream project,
[FireRedTeam/FireRedVAD](https://github.com/FireRedTeam/FireRedVAD). The Kaldi feature
computation in `src/audio/fbank.*` follows the algorithm of
[kaldi-native-fbank](https://github.com/csukuangfj/kaldi-native-fbank) (Apache-2.0). Its weights
come from the upstream Hugging Face release. The FireRedVAD GGUF files that
`tools/fetch-models.sh` downloads are converted from those weights, and each file records
`apache-2.0` in its metadata.
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) lists the sources, pinned revisions and the other
third-party code.

## Contributing

Build, test and style guidance, and how to send a change, are in [CONTRIBUTING.md](CONTRIBUTING.md).

## License

echoir-core is released under the MIT License. See [LICENSE](LICENSE). Third-party code that the
library builds in or ships with, and its license, is listed in
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES). The full license texts of those libraries are in the
`licenses/` folder of each archive and package.
