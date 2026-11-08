# Echoir.Core for .NET

A .NET binding for the echoir core: live transcription over one loaded model, and the model
catalogue with downloads. It wraps the C ABI in `include/echoir/echoir.h` and needs only the
base class library.

## Projects

- `Echoir.Core/`: the library (`net10.0`). Raw P/Invoke in `Native/`, the transcriber and the
  model API on top of it.
- `Echoir.Core.Tests/`: MSTest tests. Fake-backend tests, real-model tests and shared tests.
- `Echoir.Core.sln`: both projects.

## Native libraries

The library loads `echoir.dll` and the ggml, whisper, llama and mtmd libraries from the folder
named by the `EchoirNativeDir` property. It defaults to `build/windows-cpu-x64/bin`, the output of
the `windows-cpu-x64` preset. Packing copies those libraries into `runtimes/win-x64/native/`.
`EchoirOsxNativeDir`, `EchoirLinuxNativeDir`, `EchoirAndroidNativeDir` and `EchoirIosXcframework`
(with `EchoirMobile=true`) add the other platforms; `.github/workflows/release.yml` packs them all
from the release archives.

On iOS, `Echoir.xcframework` (from `tools/package-native.sh ios`) is linked statically into the
app, and the P/Invokes resolve against `__Internal`. The `net10.0-ios` target is built as a binding
project, so the xcframework travels next to the assembly as `Echoir.Core.resources.zip` (also in
the package) and the app links it with no setup of its own. Its simulator slice is CPU only, so
`useGpu` has no effect there. The iOS workload must match the installed Xcode.

## Translation

`EchoirTranslator` loads a translation model (`hy-mt2-1.8b-q4_k_m` by default) beside the speech
engine. It has its own context and never takes the engine's turn, so it runs in parallel with the
transcribers. `TranslateAsync` translates one line; calls run one at a time, from any thread.

```csharp
await new EchoirModels().DownloadAsync("hy-mt2-1.8b-q4_k_m", modelsDir);
using EchoirTranslator translator = await EchoirTranslator.LoadAsync(null, modelsDir);
Translation result = await translator.TranslateAsync("Good morning.", "vi");
```

`TranscriptTranslator` follows the FINAL segments of one or more transcripts and translates them
through one translator, one at a time:

```csharp
using var follower = new TranscriptTranslator(translator, "vi");
follower.Translated += (_, e) => Show(e.Track, e.SegmentId, e.Text, e.Capped);
follower.Follow(micTranscriber.Transcript, "mic");
follower.Follow(systemTranscriber.Transcript, "system");
```

- Only FINAL segments are translated, each with the two FINAL segments before it as context (source
  text only), so a late or dropped translation changes nothing for the other lines.
- While one translation runs, at most `MaxWaiting` (3) segments wait, counted over all followed
  transcripts. A fourth drops the oldest waiting one of any transcript, which stays untranslated and
  is reported by `Skipped`. Context stays per transcript.
- A clear of the transcript drops what waits and the result of a translation that is running.
  Call `Reset` after `EchoirTranscriber.Reset()`. A segment id that goes back, as in a new session
  on the same transcript, does the same.
- `Translated` and `Skipped` are raised on the `SynchronizationContext` that was current at
  `Follow`, like the events of `EchoirTranscriber`. `Submit` queues a segment again, such as one
  that was decoded again.

`EchoirModels.Catalogue()` lists the translators with `Backend == "translator"`, and
`ModelCatalogue.DefaultTranslator` names the default.

## Build

```
dotnet build bindings/csharp -c Release
```

## Tests

The tests run against one native build at a time. Set `ECHOIR_NATIVE_DIR` to that build's `bin`
folder. The tests copy its libraries next to the test host.

Fake build (scripted decoder and VAD, no model files needed):

```
cmake -S . -B build/windows-fake -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DECHOIR_FAKE_BACKEND=ON
cmake --build build/windows-fake
ECHOIR_NATIVE_DIR=<repo>/build/windows-fake/bin dotnet test bindings/csharp/Echoir.Core.Tests -c Release --filter "TestCategory!=real"
```

Real build (whisper-tiny-en and the Silero VAD in `models/`):

```
ECHOIR_NATIVE_DIR=<repo>/build/windows-cpu-x64/bin dotnet test bindings/csharp/Echoir.Core.Tests -c Release --filter "TestCategory!=fake"
```

The `TestCategory` filter picks the tests of each build: `fake` tests need the fake build and
`real` tests need the real build. Tests in neither category run in both. The real-model tests are
reported as inconclusive when the model files are missing.

On Windows, the capture test sets `ECHOIR_AUDIO_BACKEND=null` through the C runtime, because the
native code reads that variable from there.
