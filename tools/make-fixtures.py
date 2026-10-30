#!/usr/bin/env python3
# Builds the generated test fixtures into build/fixtures/ from the committed speech clips in
# tests/fixtures/. Only the standard library is used, so it runs on any host:
#
#     python3 -I tools/make-fixtures.py
#
# Outputs, each with a .ref.txt (empty for non-speech files):
#   runon_en.wav     pd_nasa_ep407 with every pause over 120 ms cut down to 120 ms. The speech has
#                    no pause the session would cut at, so a forced cut is searched for instead.
#   silence_30s.wav  digital silence.
#
# The clips are 16 kHz mono s16 WAV. A run writes into a staging directory and moves the files
# into build/fixtures/ only after everything succeeded.
import array
import math
import os
import shutil
import sys
import tempfile
import wave

RATE = 16000
FRAME = RATE // 100                 # 10 ms analysis frames
RUNON_KEEP_PAUSE_MS = 120           # a longer pause inside runon_en is cut down to this
RUNON_MAX_PAUSE_MS = 200            # runon_en must have no pause over this after shortening
LOW_ENERGY_DB = -40.0               # a 10 ms frame below this RMS counts as low energy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CLIPS = os.path.join(ROOT, "tests", "fixtures")
OUT = os.path.join(ROOT, "build", "fixtures")


def read_wav(path):
    with wave.open(path, "rb") as w:
        fmt = (w.getnchannels(), w.getsampwidth(), w.getframerate())
        if fmt != (1, 2, RATE):
            raise SystemExit("%s: expected mono s16 at %d Hz, got %r" % (path, RATE, fmt))
        raw = w.readframes(w.getnframes())
    samples = array.array("h")
    samples.frombytes(raw)
    return samples


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(samples.tobytes())


def write_ref(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text + "\n" if text else "")


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read().strip()


def load_clip(name):
    # A clip is a committed speech WAV with its reference text next to it.
    samples = read_wav(os.path.join(CLIPS, name + ".wav"))
    text = read_text(os.path.join(CLIPS, name + ".ref.txt"))
    return samples, text


def frame_levels_db(samples):
    levels = []
    for start in range(0, len(samples), FRAME):
        seg = samples[start:start + FRAME]
        rms = math.sqrt(sum(v * v for v in seg) / len(seg)) / 32768.0
        levels.append(20.0 * math.log10(rms) if rms > 0 else -200.0)
    return levels


def longest_low_run_ms(samples):
    best = cur = 0
    for db in frame_levels_db(samples):
        cur = cur + 1 if db < LOW_ENERGY_DB else 0
        best = max(best, cur)
    return best * 10


def shorten_pauses(samples, keep_ms):
    # Cuts every low-energy run longer than keep_ms down to keep_ms. Only silent frames are
    # removed, from the middle of each long run, so the speech on both sides stays as it was.
    keep = RATE * keep_ms // 1000
    levels = frame_levels_db(samples)
    out = array.array("h")
    removed = 0
    i = 0
    while i < len(levels):
        start = i * FRAME
        if levels[i] >= LOW_ENERGY_DB:
            out.extend(samples[start:min(len(samples), (i + 1) * FRAME)])
            i += 1
            continue
        j = i
        while j < len(levels) and levels[j] < LOW_ENERGY_DB:
            j += 1
        end = min(len(samples), j * FRAME)
        run = end - start
        if run > keep:
            head = keep // 2
            out.extend(samples[start:start + head])
            out.extend(samples[end - (keep - head):end])
            removed += run - keep
        else:
            out.extend(samples[start:end])
        i = j
    return out, removed


def build(stage):
    # runon_en: the ep407 clip with its long pauses shortened.
    ep407, ep407_ref = load_clip("pd_nasa_ep407")
    runon, removed = shorten_pauses(ep407, RUNON_KEEP_PAUSE_MS)
    worst = longest_low_run_ms(runon)
    print("runon_en: longest low-energy run %d ms before shortening, %d ms after (limit %d ms);"
          " %d ms of silence cut" % (longest_low_run_ms(ep407), worst, RUNON_MAX_PAUSE_MS,
                                     removed * 1000 // RATE))
    if worst > RUNON_MAX_PAUSE_MS:
        raise SystemExit("runon_en has a pause over %d ms" % RUNON_MAX_PAUSE_MS)
    write_wav(os.path.join(stage, "runon_en.wav"), runon)
    write_ref(os.path.join(stage, "runon_en.ref.txt"), ep407_ref)

    write_wav(os.path.join(stage, "silence_30s.wav"), array.array("h", [0]) * (RATE * 30))
    write_ref(os.path.join(stage, "silence_30s.ref.txt"), "")


def main():
    os.makedirs(OUT, exist_ok=True)
    stage = tempfile.mkdtemp(prefix=".work.", dir=OUT)
    try:
        build(stage)
        for name in sorted(os.listdir(stage)):
            os.replace(os.path.join(stage, name), os.path.join(OUT, name))
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    for name in ["runon_en", "silence_30s"]:
        with wave.open(os.path.join(OUT, name + ".wav"), "rb") as w:
            print("%-14s %8.2f s" % (name + ".wav", w.getnframes() / float(w.getframerate())))


if __name__ == "__main__":
    sys.exit(main())
