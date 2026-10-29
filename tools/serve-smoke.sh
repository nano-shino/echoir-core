#!/usr/bin/env bash
# Runs a push session through `echoir serve --stdio` with real models, and prints the lines the
# service writes. The reply of each audio command is left out, since there is one per chunk.
#
# Usage: tools/serve-smoke.sh [model-id] [wav]
#
# The model id defaults to whisper-tiny-en and the WAV to tests/fixtures/jfk.wav. The session is
# load, start with input push, the WAV in 100 ms chunks, flush, stop and quit. The script exits 1
# when echoir fails or any error line appears. Set ECHOIR to use another binary (default:
# build/macos-metal/bin/echoir under the repo root).
# Requires python3, which is used only for its standard library (wave, base64, json) and runs
# with -I.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ECHOIR="${ECHOIR:-$ROOT/build/macos-metal/bin/echoir}"
MODEL="${1:-whisper-tiny-en}"
WAV="${2:-$ROOT/tests/fixtures/jfk.wav}"
OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT

if [ ! -x "$ECHOIR" ]; then
    echo "echoir is not an executable file: $ECHOIR (build it, or set ECHOIR)" >&2
    exit 1
fi
if [ ! -f "$WAV" ]; then
    echo "no such WAV file: $WAV" >&2
    exit 1
fi

status=0
python3 -I - "$MODEL" "$WAV" <<'PY' | "$ECHOIR" serve --stdio --models-dir "$ROOT/models" >"$OUT" || status=$?
import base64
import json
import sys
import wave

model, path = sys.argv[1], sys.argv[2]


def send(command):
    sys.stdout.write(json.dumps(command) + "\n")


send({"cmd": "load", "model": model})
send({"cmd": "start", "input": "push"})
with wave.open(path, "rb") as w:
    if w.getsampwidth() != 2:
        sys.exit(f"{path} is not 16-bit PCM")
    rate, channels = w.getframerate(), w.getnchannels()
    chunk_frames = rate // 10  # 100 ms
    while True:
        frames = w.readframes(chunk_frames)
        if not frames:
            break
        send({
            "cmd": "audio",
            "pcm_s16le_b64": base64.b64encode(frames).decode("ascii"),
            "rate": rate,
            "channels": channels,
        })
send({"cmd": "flush"})
send({"cmd": "stop"})
send({"cmd": "quit"})
PY

grep -v -x -F '{"type":"ok","cmd":"audio"}' "$OUT" || true

if grep -q '"type":"error"' "$OUT"; then
    echo "error: echoir reported an error (see the lines above)" >&2
    status=1
fi
exit "$status"
