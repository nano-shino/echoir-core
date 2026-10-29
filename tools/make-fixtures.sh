#!/usr/bin/env bash
# Generates the long and synthetic test fixtures into build/fixtures/.
#
# Speech comes from macOS `say` and is converted with `afconvert` to 16 kHz mono
# s16 WAV. Silence, tone and the mix are built in a small stdlib-only Python step.
# Each WAV gets a .ref.txt with the spoken text (empty for non-speech files). names_en also gets a
# .context.txt, the user context that helps the decoders spell its made-up names.
#
# Idempotent: every run rebuilds the same files and overwrites the old ones. A run
# writes into a private staging directory and moves the files into build/fixtures/
# only after everything succeeded, so a failed run leaves the previous set intact.
# A lock refuses overlapping runs, which would otherwise share the output files.
# Requires macOS (say, afconvert) and python3, which is used only for its standard
# library (wave, array, math) and runs with -I.
set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
    echo "make-fixtures.sh needs macOS (say and afconvert)." >&2
    exit 1
fi
for tool in say afconvert python3 awk; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing required tool: $tool" >&2
        exit 1
    fi
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/fixtures"
LOCK="$OUT/.lock"
JFK="$ROOT/tests/fixtures/jfk.wav"
GENERATED="short_en names_en sentences_en runon_en silence_30s long_mix zh_short"

mkdir -p "$OUT"

# Lock: mkdir is atomic, and the owner writes its pid right after taking the lock.
# A lock whose pid is alive and belongs to this script is active. Anything else is
# stale: a dead pid, a pid reused by another program, or an empty lock whose owner
# was killed before it wrote the pid. An empty lock gets about 5 s of grace first,
# because a run that starts at the same moment has not written its pid yet.
# The work directory lives inside the lock, so breaking a stale lock removes it too.
acquire_lock() {
    local waited=0 owner cmd
    until mkdir "$LOCK" 2>/dev/null; do
        owner="$(cat "$LOCK/pid" 2>/dev/null || true)"
        if [[ "$owner" =~ ^[0-9]+$ ]]; then
            cmd="$(ps -o command= -p "$owner" 2>/dev/null || true)"
            if kill -0 "$owner" 2>/dev/null && [[ "$cmd" == *make-fixtures* ]]; then
                echo "another make-fixtures.sh run is active (pid $owner); wait for it to finish" >&2
                exit 1
            fi
        elif [ -z "$owner" ] && [ "$waited" -lt 50 ]; then
            waited=$((waited + 1))
            sleep 0.1
            continue
        fi
        echo "removing stale lock (owner pid: ${owner:-none})" >&2
        rm -rf "$LOCK"
    done
    echo "$$" > "$LOCK/pid"
}
acquire_lock
trap 'rm -rf "$LOCK"' EXIT
trap 'exit 1' INT TERM HUP

# Leftovers of older runs that were killed before they could clean up.
rm -rf "$OUT"/.work.*

WORK="$LOCK/work"
PARTS="$WORK/parts"
STAGE="$WORK/out"
mkdir -p "$PARTS" "$STAGE"

VOICES="$(say -v '?')"
has_voice() {
    awk -v v="$1" 'index($0, v " ") == 1 { found = 1 } END { exit (found ? 0 : 1) }' <<<"$VOICES"
}

EN_VOICE=""
if has_voice Samantha; then
    EN_VOICE="Samantha"
else
    echo "warning: voice Samantha is not installed; using the system default voice" >&2
fi

ZH_VOICE=""
if has_voice Tingting; then
    ZH_VOICE="Tingting"
else
    ZH_VOICE="$(awk '/ zh_(CN|TW|HK)( |$)/ { sub(/ +zh_.*$/, ""); print; exit }' <<<"$VOICES")"
fi
if [ -n "$ZH_VOICE" ]; then
    echo "zh voice: $ZH_VOICE"
fi

# gen_part <name> <voice or empty> <text>
# Writes $PARTS/<name>.wav (16 kHz mono s16) and $PARTS/<name>.txt (the spoken text).
gen_part() {
    local name="$1" voice="$2" text="$3"
    if [ -n "$voice" ]; then
        say -v "$voice" -o "$PARTS/$name.aiff" "$text"
    else
        say -o "$PARTS/$name.aiff" "$text"
    fi
    afconvert -f WAVE -d LEI16@16000 -c 1 "$PARTS/$name.aiff" "$PARTS/$name.wav"
    rm -f "$PARTS/$name.aiff"
    printf '%s' "$text" > "$PARTS/$name.txt"
}

# short_en: about 10 s, one sentence.
gen_part short_en "$EN_VOICE" "The morning train arrived right on time, and the passengers stepped onto the platform carrying umbrellas, bags of warm bread, and newspapers that still smelled of fresh ink."

# names_en: made-up proper names that a recognizer spells wrong without a hint. The context file
# lists them, and the e2e cases decode with and without it.
gen_part names_en "$EN_VOICE" "The grant committee approved the proposal from Doctor Ysolde Vantreece of the Brannoch Institute. Her assistant Tazmerine Okonwe will send the signed files to Kestrin Hollowmere, and Vellagrin Holt will present the results at the Dorrwynn Ashgrove conference next spring."

# sentences_en: five sentences, joined with 1 s pauses (about 60 s).
gen_part sentences_1 "$EN_VOICE" "The museum opened a new wing last spring, and visitors can now walk through galleries that trace the history of river transport from the earliest canoes to the steamboats that carried grain and passengers down the valley during the nineteenth century."
gen_part sentences_2 "$EN_VOICE" "Our neighbors planted a row of apple trees along the eastern fence, and by next autumn we expect enough fruit for pies, sauce, and a few jars of cider to share with the whole street."
gen_part sentences_3 "$EN_VOICE" "When the power went out during the storm, the emergency crew restored service to most of the district within six hours, although a few streets near the harbor stayed dark until the following morning."
gen_part sentences_4 "$EN_VOICE" "The orchestra will rehearse the new symphony on Thursday evening, and the conductor has asked every section to bring pencils, metronomes, and a clear plan for the difficult tempo changes in the final movement."
gen_part sentences_5 "$EN_VOICE" "Researchers at the coastal laboratory recorded the migration of gray whales over three seasons, using underwater microphones to track their calls and to measure how the animals respond to passing ships."

# runon_en: five run-on clips with no punctuation, joined with no gap (about 45 s).
# The clips stay unpunctuated so say does not insert long phrase pauses. Some voices
# still pause inside a clip, so the Python step shortens those pauses and then checks.
gen_part runon_1 "$EN_VOICE" "the river bends slowly past the old mill where children once fished for trout and the summer light fell gold across the water every evening while farmers returned home along the dusty road"
gen_part runon_2 "$EN_VOICE" "we spent the whole afternoon repairing the fence along the northern pasture because a storm had torn three posts from the ground and the cattle kept wandering toward the highway"
gen_part runon_3 "$EN_VOICE" "after dinner my grandmother opened the old photo albums and we spent hours laughing at pictures of relatives in strange hats standing beside cars that nobody in the family had owned for decades"
gen_part runon_4 "$EN_VOICE" "the train from the capital was late again so the travelers gathered in the small station and shared sandwiches while the stationmaster told stories about winter floods and the bridge that nearly washed away"
gen_part runon_5 "$EN_VOICE" "in the morning the fog lifted slowly over the harbor and the fishing boats began to leave one by one while gulls circled above the docks waiting for scraps from the returning nets"

ZH_FLAG=0
if [ -n "$ZH_VOICE" ]; then
    gen_part zh_short "$ZH_VOICE" "你好，我叫婷婷。今天天气很好，我们一起去公园散步吧。"
    ZH_FLAG=1
else
    echo "warning: no zh_ voice installed; skipping zh_short" >&2
fi

python3 -I - "$PARTS" "$STAGE" "$JFK" "$ZH_FLAG" <<'PY'
import math
import os
import sys
import wave
from array import array

if sys.byteorder != "little":
    raise SystemExit("this script assumes a little-endian host")

RATE = 16000
FRAME = RATE // 100                 # 10 ms analysis frames
PARTS, STAGE, JFK = sys.argv[1], sys.argv[2], sys.argv[3]
HAS_ZH = sys.argv[4] == "1"
# The user context for names_en: the names as they should be spelled.
NAMES_CONTEXT = ("Vocabulary: Ysolde Vantreece, Brannoch Institute, Tazmerine Okonwe, "
                 "Kestrin Hollowmere, Vellagrin Holt, Dorrwynn Ashgrove.")

RUNON_MAX_PAUSE_MS = 200            # runon_en has no pause over 200 ms
RUNON_KEEP_PAUSE_MS = 120           # a longer pause inside a runon clip is cut down to this
LOW_ENERGY_DB = -40.0               # a 10 ms frame below this RMS counts as low energy
TRIM_DB = -45.0                     # speech edge threshold when trimming say's padding
EDGE_PAD_MS = 30
TOTAL_MS = 600000                   # long_mix is exactly 10 min
TONE_HZ = 440
TONE_AMP = 0.3


def read_wav(path):
    with wave.open(path, "rb") as w:
        fmt = (w.getnchannels(), w.getsampwidth(), w.getframerate())
        if fmt != (1, 2, RATE):
            raise SystemExit("%s: expected mono s16 at %d Hz, got %r" % (path, RATE, fmt))
        raw = w.readframes(w.getnframes())
    samples = array("h")
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


def frame_levels_db(samples):
    levels = []
    for start in range(0, len(samples), FRAME):
        seg = samples[start:start + FRAME]
        rms = math.sqrt(sum(v * v for v in seg) / len(seg)) / 32768.0
        levels.append(20.0 * math.log10(rms) if rms > 0 else -200.0)
    return levels


def trim_silence(samples):
    voiced = [i for i, db in enumerate(frame_levels_db(samples)) if db >= TRIM_DB]
    if not voiced:
        raise SystemExit("a say clip has no audible speech")
    pad = RATE * EDGE_PAD_MS // 1000
    start = max(0, voiced[0] * FRAME - pad)
    end = min(len(samples), (voiced[-1] + 1) * FRAME + pad)
    return samples[start:end]


def longest_low_run_ms(samples):
    best = cur = 0
    for db in frame_levels_db(samples):
        cur = cur + 1 if db < LOW_ENERGY_DB else 0
        best = max(best, cur)
    return best * 10


def shorten_pauses(samples, keep_ms):
    """Cuts every low-energy run longer than keep_ms down to keep_ms.

    Only silent frames are removed, from the middle of each long run, so the speech
    on both sides stays as it was. Returns (samples, number of samples removed).
    """
    keep = RATE * keep_ms // 1000
    levels = frame_levels_db(samples)
    out = array("h")
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


def silence(ms):
    return array("h", [0]) * (RATE * ms // 1000)


def tone(freq, ms, amp):
    n = RATE * ms // 1000
    fade = RATE * 10 // 1000        # 10 ms fades avoid clicks at the block edges
    out = array("h", [0]) * n
    for i in range(n):
        env = min(1.0, i / fade, (n - 1 - i) / fade)
        out[i] = int(round(32767 * amp * env * math.sin(2 * math.pi * freq * i / RATE)))
    return out


def concat(clips, gap_ms):
    out = array("h")
    gap = silence(gap_ms)
    for k, clip in enumerate(clips):
        if k:
            out.extend(gap)
        out.extend(clip)
    return out


def load_part(name):
    samples = read_wav(os.path.join(PARTS, name + ".wav"))
    text = read_text(os.path.join(PARTS, name + ".txt"))
    return trim_silence(samples), text


def emit(name, samples, ref):
    write_wav(os.path.join(STAGE, name + ".wav"), samples)
    write_ref(os.path.join(STAGE, name + ".ref.txt"), ref)


clips = {}  # name -> (samples, ref), the speech sources for long_mix

short, short_ref = load_part("short_en")
emit("short_en", short, short_ref)
clips["short_en"] = (short, short_ref)

names, names_ref = load_part("names_en")
emit("names_en", names, names_ref)
write_ref(os.path.join(STAGE, "names_en.context.txt"), NAMES_CONTEXT)

sent = [load_part("sentences_%d" % k) for k in range(1, 6)]
sentences = concat([s for s, _ in sent], gap_ms=1000)
sentences_ref = " ".join(t for _, t in sent)
emit("sentences_en", sentences, sentences_ref)
clips["sentences_en"] = (sentences, sentences_ref)

runon_parts = [load_part("runon_%d" % k) for k in range(1, 6)]
runon_before = longest_low_run_ms(concat([s for s, _ in runon_parts], gap_ms=0))
runon_clips, runon_cut = [], 0
for s, _ in runon_parts:
    cut, removed = shorten_pauses(s, RUNON_KEEP_PAUSE_MS)
    runon_clips.append(cut)
    runon_cut += removed
runon_samples = concat(runon_clips, gap_ms=0)
runon_ref = " ".join(t for _, t in runon_parts)
worst = longest_low_run_ms(runon_samples)
print("runon_en: longest low-energy run %d ms before pause shortening, %d ms after (limit %d ms); %d ms of silence cut"
      % (runon_before, worst, RUNON_MAX_PAUSE_MS, runon_cut * 1000 // RATE))
if worst > RUNON_MAX_PAUSE_MS:
    raise SystemExit("runon_en has a pause over %d ms; reword its clips" % RUNON_MAX_PAUSE_MS)
emit("runon_en", runon_samples, runon_ref)
clips["runon_en"] = (runon_samples, runon_ref)

emit("silence_30s", silence(30000), "")

if HAS_ZH:
    zh, zh_ref = load_part("zh_short")
    emit("zh_short", zh, zh_ref)

# long_mix: cycle through speech, silence and tone blocks until the next block
# would overrun 10 min, then pad with silence to exactly 10 min. Only whole speech
# clips are used, so the reference is the clips' texts in order.
total_samples = RATE * TOTAL_MS // 1000
tone_block = {10000: tone(TONE_HZ, 10000, TONE_AMP), 20000: tone(TONE_HZ, 20000, TONE_AMP)}
cycle = [
    ("speech", "sentences_en"), ("silence", 15000), ("tone", 20000),
    ("speech", "short_en"), ("silence", 10000),
    ("speech", "runon_en"), ("silence", 20000), ("tone", 10000),
]
blocks, mix_refs, used = [], [], 0
done = False
while not done:
    for kind, arg in cycle:
        if kind == "speech":
            block, ref = clips[arg]
        elif kind == "silence":
            block, ref = silence(arg), None
        else:
            block, ref = tone_block[arg], None
        if used + len(block) > total_samples:
            done = True
            break
        blocks.append(block)
        used += len(block)
        if ref is not None:
            mix_refs.append(ref)
blocks.append(array("h", [0]) * (total_samples - used))
mix = concat(blocks, gap_ms=0)
assert len(mix) == total_samples, (len(mix), total_samples)
emit("long_mix", mix, " ".join(mix_refs))

# Summary table: generated files, plus the committed jfk sample.
def wav_info(path):
    with wave.open(path, "rb") as w:
        return w.getnframes() / float(w.getframerate()), w.getnchannels(), w.getframerate(), w.getsampwidth() * 8


names = ["short_en", "names_en", "sentences_en", "runon_en", "silence_30s", "long_mix"]
if HAS_ZH:
    names.append("zh_short")
rows = [(name + ".wav", os.path.join(STAGE, name + ".wav"), "generated") for name in names]
rows.append(("jfk.wav", JFK, "committed (tests/fixtures)"))

print()
print("%-18s %10s  %-20s  %s" % ("file", "duration", "format", "notes"))
print("%-18s %10s  %-20s  %s" % ("----", "--------", "------", "-----"))
for label, path, note in rows:
    seconds, channels, rate, bits = wav_info(path)
    fmt = "%d Hz, %s, %d-bit" % (rate, "mono" if channels == 1 else "%d ch" % channels, bits)
    if label == "runon_en.wav":
        note = "longest low-energy run %d ms" % worst
    print("%-18s %9.2fs  %-20s  %s" % (label, seconds, fmt, note))
PY

# Everything succeeded: move the staged files into place, and remove the outputs of
# names this run did not produce (such as zh_short when no zh voice is installed).
for name in $GENERATED; do
    if [ -f "$STAGE/$name.wav" ]; then
        mv -f "$STAGE/$name.wav" "$OUT/$name.wav"
        mv -f "$STAGE/$name.ref.txt" "$OUT/$name.ref.txt"
    else
        rm -f "$OUT/$name.wav" "$OUT/$name.ref.txt"
    fi
    if [ -f "$STAGE/$name.context.txt" ]; then
        mv -f "$STAGE/$name.context.txt" "$OUT/$name.context.txt"
    else
        rm -f "$OUT/$name.context.txt"
    fi
done

echo
echo "done: $OUT"
