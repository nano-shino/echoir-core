# Test fixtures

All fixtures are 16 kHz, mono, signed 16-bit PCM WAV. The core's internal format
is also 16 kHz mono f32, so tests can feed these files without resampling.

Each WAV has a `.ref.txt` next to it with the text that was spoken. Non-speech
files have an empty `.ref.txt`. The exception is `firered/tone_noise.wav`, a
synthetic clip in a subdirectory, which has no `.ref.txt`. Reference texts are
plain text as given to the TTS, with no normalization. Compare them after
lowercasing and stripping punctuation.

## Committed (this directory)

| File | Duration | What it is |
|---|---|---|
| `jfk.wav` | 11.00 s | The JFK clip from whisper.cpp `samples/jfk.wav`, byte-identical to the copy at whisper.cpp v1.9.5 (commit `d1be6fde`). whisper.cpp is MIT licensed. |
| `jfk.ref.txt` | | "And so, my fellow Americans, ask not what your country can do for you, ask what you can do for your country." |
| `prompt_echo.wav` | 12.97 s | A recording contributed by the author. Two short lines of dialogue over background sound, which fades out from 9.5 s; nothing is said after the second line. With `--language en`, Qwen3-ASR streaming used to decode the last sound as its prompt history, repeating both lines. The converted clip still does this without the guard. |
| `prompt_echo.ref.txt` | | "Or am I wrong? Understood, sir." The last sound is not in it. |
| `firered/` | | Golden outputs of the FireRedVAD implementation, computed by the upstream Apache-2.0 code and weights. See `firered/README.md`. |
| `firered/tone_noise.wav` | 3.00 s | Synthetic: silence, a 440 Hz tone, then white noise. No `.ref.txt`, since it is not speech. |

Keep the committed fixtures small: the limit is 2 MB in total.

## Generated (not committed)

Run `tools/make-fixtures.sh` (macOS, needs `say`, `afconvert` and `python3`) to
write these into `build/fixtures/`. With the same voices, every run writes the
same bytes. Runs must not overlap: a second run exits with an error while the
first one holds the lock in `build/fixtures/.lock`. A run builds the files in
`build/fixtures/.lock/work` and moves them into place only after all of them
succeeded, so a failed run leaves the previous set untouched. A killed run (for
example `kill -9`) leaves only `build/fixtures/.lock`; the next run sees that its
owner pid is gone, removes the lock with its work directory, and carries on.

The script uses python3 only for its standard library (`wave`, `array`, `math`),
run with `-I`. It needs no pip packages.

| File | Duration | How it is made |
|---|---|---|
| `short_en.wav` | 9.51 s | One sentence, `say` (Samantha). |
| `sentences_en.wav` | 58.97 s | Five sentences, each rendered by `say` and trimmed, joined with exactly 1.0 s of silence. |
| `runon_en.wav` | 46.81 s | Five unpunctuated `say` clips, trimmed and joined with no gap. A pause longer than 120 ms inside a clip is cut down to 120 ms, which changes nothing with Samantha (her longest pause is 100 ms) and keeps the default voice under the limit. The script measures the longest low-energy run and fails if it is over 200 ms. |
| `silence_30s.wav` | 30.00 s | Digital silence. Empty ref. |
| `long_mix.wav` | 600.00 s | Exactly 10 min. Speech (`sentences_en`, `short_en`, `runon_en`), silence, and 440 Hz tone blocks (amplitude 0.3, 10 ms fades) in a fixed cycle, then silence padding. Its ref is the speech texts in order. |
| `zh_short.wav` | 6.06 s | Chinese, `say` (Tingting, zh_CN). Skipped with a warning if no zh_ voice is installed. |

Low-energy means a 10 ms frame with RMS below -40 dBFS. The run check uses the
same definition.

Voices are not part of the fixture contract. If Samantha is missing, the script
uses the system default voice and prints a warning. The fallback was checked on
this host with Samantha hidden from the voice list: the default voice's pauses
are cut as described above, and `runon_en` comes out at about 52 s, still under
the 200 ms pause limit.

Do not hand-edit generated files. Change the texts or the logic in
`tools/make-fixtures.sh` and re-run it.
