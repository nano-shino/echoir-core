# Contributing to echoir-core

Thanks for your interest. Small fixes can go straight to a pull request. For a larger change, such
as a new backend, a change to the C ABI in `include/echoir/echoir.h`, or a new dependency, open an
issue first so we can agree on the approach.

## Build

Build with a preset from `CMakePresets.json`. `README.md` lists them with their requirements.

```sh
git clone --recursive https://github.com/nano-shino/echoir-core.git
cmake --preset macos-cpu
cmake --build --preset macos-cpu
```

## Tests

```sh
ctest --preset macos-metal-quick        # unit, C ABI, CLI and integration tests, about a minute
ctest --preset macos-cpu                # every test except the long and stress labels
ctest --preset macos-cpu -L e2e         # end-to-end cases
ctest --preset macos-cpu -R long_session  # the long-session cases, about ten minutes of audio
```

The integration and e2e tests need models. They skip, and do not fail, when a model is missing.
Fetch the ones you need:

```sh
tools/fetch-models.sh whisper-tiny-en silero-v6.2.0 firered-vad-v1
```

The longer test audio is generated on macOS by `tools/make-fixtures.sh`.

## Style

- Format C++ with the `.clang-format` at the root: Google base, 4-space indent, 100 columns.
- Use LF line endings.
- Name functions and variables in `snake_case`, and types in `PascalCase`. Code lives in the
  `echoir` namespace. Headers do not contain `using namespace`.
- Write comments that explain the behaviour and the reason for it. Keep them short and make each
  one understandable on its own.

## Rules

- No OS API calls and no OS `#ifdef` in `src/`. Platform differences come from miniaudio and ggml,
  which report what the running system supports, such as `ma_is_loopback_supported`.
- No new third-party dependency without a discussion in an issue. The current ones are vendored in
  `third_party/single/` with their licenses, plus the whisper.cpp and llama.cpp submodules.
- Comments and documentation must make sense on their own. Do not refer to internal planning
  documents, private notes, issue tracker entries that are not public, task identifiers or local
  file paths.
- Keep the committed fixtures in `tests/fixtures/` small: under 2 MB in total.
- A new input path or output mode needs an end-to-end case in `tests/e2e/cases.json`.

## Pull requests

- Keep `ctest` green for the preset you built. Say which preset you ran.
- Describe the behaviour change and why it is needed.
- Keep a pull request to one change. Split unrelated fixes into separate pull requests.

Contributions are accepted under the MIT License in `LICENSE`.
