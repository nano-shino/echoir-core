# Test fixtures

All fixtures are 16 kHz, mono, signed 16-bit PCM WAV. The core's internal format
is also 16 kHz mono f32, so tests can feed these files without resampling.

Each WAV has a `.ref.txt` next to it with the text that was spoken. Non-speech
files have an empty `.ref.txt`. The exception is `firered/tone_noise.wav`, a
synthetic clip in a subdirectory, which has no `.ref.txt`. A speech reference
is a person's transcript of the clip, checked against the audio, with the
spoken fillers kept. Compare references after lowercasing and stripping
punctuation, as the scorer does.

## Committed (this directory)

| File | Duration | What it is |
|---|---|---|
| `jfk.wav` | 11.00 s | The JFK clip from whisper.cpp `samples/jfk.wav`, byte-identical to the copy at whisper.cpp v1.9.5 (commit `d1be6fde`). whisper.cpp is MIT licensed. |
| `jfk.ref.txt` | | "And so, my fellow Americans, ask not what your country can do for you, ask what you can do for your country." |
| `prompt_echo.wav` | 12.97 s | A recording contributed by the author. Two short lines of dialogue over background sound, which fades out from 9.5 s; nothing is said after the second line. With `--language en`, Qwen3-ASR streaming used to decode the last sound as its prompt history, repeating both lines. The converted clip still does this without the guard. |
| `prompt_echo.ref.txt` | | "Or am I wrong? Understood, sir." The last sound is not in it. |
| `pd_nasa_ep407.wav` | 33.31 s | An excerpt of NASA's "Houston We Have a Podcast" episode 407. American English. |
| `pd_nasa_ep345.wav` | 27.72 s | An excerpt of "Houston We Have a Podcast" episode 345, "The Lost Universe". American English. |
| `pd_nasa_ep180.wav` | 25.64 s | An excerpt of "Houston We Have a Podcast" episode 180, about Artemis flight tests. American English. |
| `pd_voa.wav` | 31.54 s | An excerpt of a Voice of America interview with the singer Alessia Cara at Jingle Ball 2016. American English, emotional and conversational, with background noise from the event. |
| `pd_*.ref.txt` | | The transcript of each clip above. |
| `firered/` | | Golden outputs of the FireRedVAD implementation, computed by the upstream Apache-2.0 code and weights. See `firered/README.md`. |
| `firered/tone_noise.wav` | 3.00 s | Synthetic: silence, a 440 Hz tone, then white noise. No `.ref.txt`, since it is not speech. |

The `pd_*` clips are public domain: NASA works and Voice of America recordings are
U.S. government works. Each clip is a 25 to 33 second excerpt, cut at a segment
boundary and converted to 16 kHz mono. Each transcript was drafted with
`whisper-large-v3-turbo-q5_0` and then checked by a person against the audio,
with `qwen3-asr-1.7b-q8_0` used to find the words the two models disagree on.

References keep the speaker's stutters and repeated words. Whisper's offline
decoding drops most of them, so `whisper_voa_offline` scores higher than the
other cases on the same kind of speech, and its `max_rate` allows for that.

Keep the committed fixtures small: the limit is 10 MB in total.

## Generated (not committed)

Run `python3 -I tools/make-fixtures.py` to write these into `build/fixtures/`.
It needs only the Python standard library (`wave`, `array`, `math`) and runs on
any host. It reads the committed `pd_*` clips, so run it after a fresh checkout.
Every run writes the same bytes. A run builds the files in a temporary
directory inside `build/fixtures/` and moves them into place only after all of
them succeeded.

| File | Duration | How it is made |
|---|---|---|
| `runon_en.wav` | 32.04 s | `pd_nasa_ep407` with every pause longer than 120 ms cut down to 120 ms. The session cuts at a pause of at least 250 ms, so this clip has none and exercises the forced cut. The script fails if a pause over 200 ms remains. Ref is the ep407 transcript. |
| `silence_30s.wav` | 30.00 s | Digital silence. Empty ref. |

Low-energy means a 10 ms frame with RMS below -40 dBFS. The pause check uses the
same definition.

Do not hand-edit generated files. Change `tools/make-fixtures.py` and re-run it.
