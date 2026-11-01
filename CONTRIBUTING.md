# Contributing to echoir-core

Thanks for your interest. Small fixes can go straight to a pull request. For a larger change, such
as a new backend, a change to the C ABI in `include/echoir/echoir.h`, or a new dependency, open an
issue first so we can agree on the approach.

## Build

Build with a preset from `CMakePresets.json`. `README.md` lists them with their requirements. The
examples below use `macos-metal`, the preset CI builds on macOS. On Linux, use `linux-cpu`. On
Windows, use `windows-cpu-x64` from a Visual Studio x64 developer shell.

```sh
git clone --recursive https://github.com/nano-shino/echoir-core.git
cmake --preset macos-metal
cmake --build --preset macos-metal
```

## Tests

Build the preset first, then run one of its test presets. `macos-metal-quick` is a test preset for
the build preset `macos-metal`:

```sh
ctest --preset macos-metal-quick        # unit, C ABI, CLI and integration tests, about a minute; CI runs this
ctest --preset macos-metal              # every test except the long label
ctest --preset macos-metal -L e2e       # end-to-end cases
ctest --preset macos-metal -R qwen_stream_golden  # the Qwen streaming cases, about 100 s
```

The integration and e2e tests need models. They skip, and do not fail, when a model is missing.
Fetch the ones you need:

```sh
tools/fetch-models.sh whisper-tiny-en silero-v6.2.0 firered-vad-v1
```

On Windows, run `tools\fetch-models.ps1` with the same ids.

The generated test audio is built with `python3 -I tools/make-fixtures.py`, which runs on any host.

## Lint

```sh
tools/lint.sh          # the linters on every file, the formatters on the files you changed
tools/lint.sh --all    # the formatters on every file too; some older files do not pass yet
```

`tools/lint.sh` runs shellcheck, actionlint, ruff check, clang-format and ruff format. Install them
at the versions pinned in `tools/lint-versions.env`: the script refuses to run with other versions.
CI installs the same pinned versions.

## Continuous integration

The workflows in `.github/workflows/` run on pushes to `main` and on pull requests. The build
workflows skip changes that touch only docs, `LICENSE`, `THIRD_PARTY_NOTICES` or the packaging
scripts.

| Workflow | Runs on | What it checks |
|---|---|---|
| `lint.yml` | Pushes to `main`, pull requests | `tools/lint.sh origin/main`: shellcheck, actionlint and ruff check on every file, and clang-format and ruff format on the files the change touches. |
| `macos.yml` | macOS 26 | Builds `macos-metal`, then runs `macos-metal-quick`: the unit, C ABI, CLI and integration tests. No models are fetched. |
| `linux.yml` | Ubuntu 24.04 | Builds `linux-cpu` with GCC, fetches models, and runs its tests, including the e2e cases. |
| `windows.yml` | Windows | Builds `windows-cpu-x64`, fetches models, and runs its tests, including the e2e cases. Builds `windows-vulkan-x64` without running it, because the runners have no GPU. |
| `dotnet.yml` | Ubuntu 24.04 | Builds the fake backend (`ECHOIR_FAKE_BACKEND=ON`) and runs the C# tests that are not marked real. No models are needed. |
| `android.yml` | Ubuntu 24.04 | Cross-builds `android-arm64` with the NDK. No tests. |
| `release.yml` | Tags `v*`, pushes to `ci/release`, manual runs | Builds and packages the release archives and the `Echoir.Core` NuGet package. A tag must match the version in `CMakeLists.txt`. A tag without a suffix publishes a release; a tag with one, such as `v3.0.0-alpha.1`, publishes a pre-release. |

A case whose model is missing skips, so a run without the model files still passes.

## Style

- Format C++ with the `.clang-format` at the root: Google base, 4-space indent, 100 columns.
- Use LF line endings.
- Name functions and variables in `snake_case`, and types in `PascalCase`. Code lives in the
  `echoir` namespace. Headers do not contain `using namespace`.
- Write comments that explain the behaviour and the reason for it. Keep them short and make each
  one understandable on its own.

## Rules

- No OS API calls and no OS `#ifdef` in `src/`. Platform differences come from miniaudio and ggml,
  which report what the running system supports, such as `ma_is_loopback_supported`. The one
  exception is the module folder lookup in `src/backends/backend_loader.cpp`, which calls `dladdr`
  in `GGML_BACKEND_DL` builds (Android), because ggml's default search does not find modules inside
  an app. Three small `#ifdef`s are allowed where the standard library differs: wide-path file
  open in `src/audio/wav.cpp` and environment access in `src/text/env.cpp` on Windows, and
  `snprintf` in place of floating-point `std::to_chars` in `src/stream/vad_postprocess.cpp` on Apple.
- No new third-party dependency without a discussion in an issue. The current ones are vendored in
  `third_party/single/` with their licenses, plus the whisper.cpp, llama.cpp and libopus
  submodules.
- Comments and documentation must make sense on their own. Do not refer to internal planning
  documents, private notes, issue tracker entries that are not public, task identifiers or local
  file paths.
- Keep the committed fixtures in `tests/fixtures/` small: under 10 MB in total.
- A new input path or output mode needs an end-to-end case in `tests/e2e/cases.json`.

## Pull requests

- Keep `ctest` green for the preset you built. Say which preset you ran.
- Describe the behaviour change and why it is needed.
- Keep a pull request to one change. Split unrelated fixes into separate pull requests.

Contributions are accepted under the MIT License in `LICENSE`.
