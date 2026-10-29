"""Runs one end-to-end case from tests/e2e/cases.json.

The echoir tool transcribes the case's fixture, and the transcript is scored against the reference
text by `echoir score`. This script only starts the tools and compares their numbers, so the
normalization and the error rate are the C++ ones.

A case's mode is "offline" (`echoir transcribe --offline`, the transcript is the segment lines),
"stream" (`echoir transcribe --stream --pace none --format jsonl`, the transcript is the final
events joined by spaces), "realtime" (`--stream --pace realtime`, the same jsonl transcript, with
the audio pushed at its own speed), "stdin" (`echoir transcribe - --stream --pace none --format
jsonl`, the fixture on standard input) or "serve" (`echoir serve --stdio`: the harness sends load,
start with input push, the fixture's audio in 100 ms chunks, flush, stop and quit, then checks that
every command got its reply and that no error came back, and scores the final events joined by
spaces). A stdin case's optional "stdin" field says what standard
input holds: "wav" (the default, the fixture's own bytes) or an object such as
{"pcm": "s16le", "rate": 48000, "channels": 2}, in which case the harness converts the fixture to
that raw format and passes `--pcm`, `--rate` and `--channels`. A stream, realtime, stdin or serve
case may also name a "policy" ("agreement" or "rollback"), which is passed as `--policy` (or as the
policy of the start command in serve mode); without it echoir uses its default. A stream, realtime
or stdin case may name a "max_segment_ms": no final may last longer than it, from the start_ms and
end_ms of its jsonl event. The tool gets no --max-segment-ms, so the check covers its default cap.
Usage: run_e2e.py --echoir BIN --case ID [--cases FILE] [--vad ID] [--vad-param KEY=VALUE]...
                  [--show-text] [--source-dir DIR]

The VAD is the registry's default_vad for the case's backend unless --vad names another registry
id. Each --vad-param is passed to `echoir transcribe` as it is given. A serve case takes no
--vad-param, since the serve protocol has no VAD settings, so one is a usage error. A case's
fixture, reference and context file (its optional "context" field) are looked up next to the cases
file first, then in tests/fixtures and build/fixtures. A context file's text goes to every decode
as --context-file (in serve mode, as the context of the start command). A case's optional
"language" (such as "en") is passed as --language; serve cases take none.
A case with "compare_to" (the id of another case on the same fixture, without the context) also
runs that case, and fails unless its own rate is below the other case's rate. A case with
"must_contain_once" fails unless that text occurs exactly once in the normalized transcript, as
whole words, which catches a line that the decoder repeats.

Output holds numbers only. The transcript and the reference are printed only with --show-text, and
only for a failing case.

Exit status: 0 pass; 1 fail (rate above max_rate, more words than max_words in a case with
expect_empty, a must_contain text missing, a must_contain_once text missing or repeated, a rate that is not below its compare_to case's, or a
tool failed); 2 usage (unknown case, bad cases file, bad VAD id, or a --vad-param that is not
KEY=VALUE); 77 skip (a model file or a fixture is not on disk). An unknown VAD setting or a bad
value is reported by echoir, so it exits 1.
Only the standard library is used. Run the script with python3 -I.
"""

import argparse
import array
import base64
import json
import os
import re
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

EXIT_FAIL = 1
EXIT_USAGE = 2
EXIT_SKIP = 77  # ctest's SKIP_RETURN_CODE

# The backend names of cases, and the backend strings of models/registry.json.
REGISTRY_BACKEND = {"whisper": "whisper", "qwen": "qwen3-asr"}

REQUIRED_KEYS = ("id", "fixture", "ref", "backend", "model", "mode")

# One transcript line of `echoir transcribe`: "[mm:ss.mmm → mm:ss.mmm] text".
TRANSCRIPT_LINE = re.compile(r"^\[[0-9:.]+ → [0-9:.]+\] (.*)$")

# The modes a case can name. Each one is a way of running `echoir transcribe`.
MODES = ("offline", "stream", "realtime", "stdin", "serve")
# The modes that stream the input, and so print jsonl events.
STREAM_MODES = ("stream", "realtime", "stdin")
# The modes that take a stabilization policy: the streams, and the serve session.
POLICY_MODES = STREAM_MODES + ("serve",)
# The stabilization policies a stream case can name, as `echoir transcribe --policy` takes them.
POLICIES = ("agreement", "rollback")
# The raw sample formats a stdin case can name, as `echoir transcribe --pcm` takes them.
RAW_PCM_FORMATS = ("s16le",)
# The audio of a serve case goes to the service in chunks of this many milliseconds.
SERVE_CHUNK_MS = 100

# A tool that runs longer than this has hung, which is a failure.
TIMEOUT_S = 900


class Usage(Exception):
    """A bad cases file or an unknown case. It says nothing about the transcript."""


class Skip(Exception):
    """A model file or a fixture is not on this machine."""


class Fail(Exception):
    """The transcript is wrong, or a tool did not finish."""


def load_cases(path):
    try:
        with open(path, encoding="utf-8") as f:
            cases = json.load(f)
    except (OSError, ValueError) as e:
        raise Usage(f"cannot read {path}: {e}")
    if not isinstance(cases, list):
        raise Usage(f"{path} must hold a JSON list of cases")
    return cases


def find_case(cases, case_id):
    for case in cases:
        if isinstance(case, dict) and case.get("id") == case_id:
            return case
    raise Usage(f"no case with id {case_id!r}")


def check_stdin_spec(case_id, spec):
    """A stdin case's "stdin" field: "wav", or an object describing raw s16le samples."""
    if spec == "wav":
        return
    if not isinstance(spec, dict) or set(spec) != {"pcm", "rate", "channels"}:
        raise Usage(
            f"case {case_id!r}: stdin must be \"wav\" or an object with pcm, rate and channels"
        )
    if spec["pcm"] not in RAW_PCM_FORMATS:
        raise Usage(f"case {case_id!r}: stdin pcm must be one of {', '.join(RAW_PCM_FORMATS)}")
    for key in ("rate", "channels"):
        value = spec[key]
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise Usage(f"case {case_id!r}: stdin {key} must be a positive whole number")


def check_case(case):
    case_id = case["id"]
    missing = [key for key in REQUIRED_KEYS if key not in case]
    if missing:
        raise Usage(f"case {case_id!r} lacks {', '.join(missing)}")
    if case["mode"] not in MODES:
        raise Usage(f"case {case_id!r}: mode {case['mode']!r} is not one of {', '.join(MODES)}")
    if case["backend"] not in REGISTRY_BACKEND:
        raise Usage(f"case {case_id!r}: unknown backend {case['backend']!r}")
    if "policy" in case:
        if case["policy"] not in POLICIES:
            raise Usage(f"case {case_id!r}: policy must be one of {', '.join(POLICIES)}")
        if case["mode"] not in POLICY_MODES:
            raise Usage(
                f"case {case_id!r}: policy needs a stream, realtime, stdin or serve mode"
            )
    if "stdin" in case:
        if case["mode"] != "stdin":
            raise Usage(f"case {case_id!r}: stdin needs the stdin mode")
        check_stdin_spec(case_id, case["stdin"])
    if "max_segment_ms" in case:
        if case["mode"] not in STREAM_MODES:
            raise Usage(f"case {case_id!r}: max_segment_ms needs a stream, realtime or stdin mode")
        limit = case["max_segment_ms"]
        if isinstance(limit, bool) or not isinstance(limit, int) or limit <= 0:
            raise Usage(f"case {case_id!r}: max_segment_ms must be a positive whole number")
    if case.get("expect_empty"):
        # A silent clip is judged by its word count, so it has no rate.
        words = case.get("max_words")
        if isinstance(words, bool) or not isinstance(words, int) or words < 0:
            raise Usage(f"case {case_id!r}: max_words must be a whole number")
    else:
        rate = case.get("max_rate")
        if isinstance(rate, bool) or not isinstance(rate, (int, float)):
            raise Usage(f"case {case_id!r}: max_rate must be a number")
    for key in ("must_contain", "must_contain_once"):
        if key in case and not (isinstance(case[key], str) and case[key]):
            raise Usage(f"case {case_id!r}: {key} must be a non-empty string")
    if "context" in case and not (isinstance(case["context"], str) and case["context"]):
        raise Usage(f"case {case_id!r}: context must be a non-empty file name")
    if "language" in case:
        if not (isinstance(case["language"], str) and case["language"]):
            raise Usage(f"case {case_id!r}: language must be a non-empty code")
        if case["mode"] == "serve":
            raise Usage(f"case {case_id!r}: language needs a transcribe mode, not serve")
    if "compare_to" in case:
        if not (isinstance(case["compare_to"], str) and case["compare_to"]):
            raise Usage(f"case {case_id!r}: compare_to must be a non-empty case id")
        if case.get("expect_empty"):
            raise Usage(f"case {case_id!r}: compare_to needs a case scored by rate")


def read_registry(models_dir):
    """The registry entries by id, and the default VAD id of each backend."""
    path = models_dir / "registry.json"
    if not path.is_file():
        raise Skip(f"no registry.json in {models_dir}")
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        entries = {model["id"]: model for model in doc["models"]}
        default_vad = doc["default_vad"]
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise Usage(f"cannot read {path}: {e}")
    if not isinstance(default_vad, dict) or not all(
        isinstance(vad, str) for vad in default_vad.values()
    ):
        raise Usage(f"{path}: default_vad must map each backend to a registry id")
    return entries, default_vad


def check_vad(entries, vad):
    entry = entries.get(vad)
    if entry is None or entry["backend"] != "vad":
        raise Usage(f"{vad!r} is not a VAD in models/registry.json")


def check_models(models_dir, entries, case, vad):
    """Skips when a file of the case's model, projector or VAD is missing."""
    wanted = [case["model"], vad]
    if case.get("mmproj"):
        wanted.append(case["mmproj"])
    for model_id in wanted:
        entry = entries.get(model_id)
        if entry is None:
            raise Usage(f"{model_id!r} is not in models/registry.json")
        for file in entry["files"]:
            path = models_dir / file["name"]
            if not path.is_file():
                raise Skip(
                    f"{path} is missing; fetch it with tools/fetch-models.sh"
                    f" (tools/fetch-models.ps1 on Windows) {model_id}"
                )
    backend = entries[case["model"]]["backend"]
    if REGISTRY_BACKEND[case["backend"]] != backend:
        raise Usage(f"case {case['id']!r}: model {case['model']!r} is a {backend} model")


def baseline_case(cases, case):
    """The case that compare_to names. It must be another rate-scored case on the same fixture."""
    baseline = find_case(cases, case["compare_to"])
    check_case(baseline)
    if baseline["id"] == case["id"]:
        raise Usage(f"case {case['id']!r}: compare_to names the case itself")
    if baseline["fixture"] != case["fixture"] or baseline.get("expect_empty"):
        raise Usage(
            f"case {case['id']!r}: compare_to must name a rate-scored case on {case['fixture']}"
        )
    return baseline


def find_text_file(search_dirs, name):
    """A fixture or a reference text: the first of search_dirs that holds it."""
    for directory in search_dirs:
        path = directory / name
        if path.is_file():
            return path
    raise Skip(f"{name} is not in any of {', '.join(str(d) for d in search_dirs)}")


def run_tool(args, what, stdin=None):
    """Runs one echoir command and returns its standard output as text."""
    try:
        proc = subprocess.run(
            args, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT_S
        )
    except subprocess.TimeoutExpired:
        raise Fail(f"{what} ran longer than {TIMEOUT_S} s")
    except OSError as e:
        raise Usage(f"cannot run {args[0]}: {e}")
    if proc.returncode != 0:
        stderr = proc.stderr.decode("utf-8", errors="replace").strip()
        raise Fail(f"{what} exited with {proc.returncode}: {stderr}")
    try:
        return proc.stdout.decode("utf-8")
    except UnicodeDecodeError as e:
        raise Fail(f"{what} printed text that is not UTF-8: {e}")


def resample_linear(plane, src_rate, dst_rate):
    """One channel's samples at dst_rate, by linear interpolation between the source samples."""
    n_src = len(plane)
    out = []
    for j in range(n_src * dst_rate // src_rate):
        i, rem = divmod(j * src_rate, dst_rate)
        if i + 1 < n_src:
            out.append(plane[i] + (plane[i + 1] - plane[i]) * rem / dst_rate)
        else:
            out.append(plane[i])
    return out


def fixture_as_raw_pcm(path, spec):
    """The fixture as little-endian s16le at the rate and channel count of a stdin spec.

    The fixture must be 16-bit. Each channel is resampled, and when the channel count changes, the
    channels are mixed to mono and copied to every output channel.
    """
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise Usage(f"{path.name} is not 16-bit, so it cannot be made into raw s16le")
        src_channels = w.getnchannels()
        src_rate = w.getframerate()
        frames = w.readframes(w.getnframes())
    samples = array.array("h")
    samples.frombytes(frames)
    if sys.byteorder == "big":
        samples.byteswap()
    planes = [
        resample_linear(samples[c::src_channels], src_rate, spec["rate"])
        for c in range(src_channels)
    ]
    if src_channels != spec["channels"]:
        mono = [sum(frame) / src_channels for frame in zip(*planes)]
        planes = [mono] * spec["channels"]
    out = array.array(
        "h",
        (min(32767, max(-32768, round(v))) for frame in zip(*planes) for v in frame),
    )
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


def stdin_input(case, fixture):
    """The bytes that standard input gets in a stdin case, and the options that describe them."""
    spec = case.get("stdin", "wav")
    if spec == "wav":
        return fixture.read_bytes(), []
    options = [
        "--pcm", spec["pcm"], "--rate", str(spec["rate"]), "--channels", str(spec["channels"]),
    ]
    return fixture_as_raw_pcm(fixture, spec), options


def transcribe(echoir, case, fixture, models_dir, vad, vad_params, context_file):
    """The transcript of the case's mode, as one line of text."""
    if case["mode"] == "serve":
        return serve_transcript(echoir, case, fixture, models_dir, vad, vad_params, context_file)
    stdin = None
    if case["mode"] == "stdin":
        stdin, input_options = stdin_input(case, fixture)
        source = ["-"] + input_options
    else:
        source = [str(fixture)]
    args = [
        echoir, "transcribe", *source, "--backend", case["backend"],
        "--model", case["model"], "--vad", vad, "--models-dir", str(models_dir),
    ]
    if context_file is not None:
        args += ["--context-file", str(context_file)]
    if "language" in case:
        args += ["--language", case["language"]]
    if case["mode"] in STREAM_MODES:
        pace = "realtime" if case["mode"] == "realtime" else "none"
        args += ["--stream", "--pace", pace, "--format", "jsonl"]
        if "policy" in case:
            args += ["--policy", case["policy"]]
    else:
        args += ["--offline"]
    if case.get("mmproj"):
        args += ["--mmproj", case["mmproj"]]
    if case.get("gpu") is False:
        args.append("--no-gpu")
    for assignment in vad_params:
        args += ["--vad-param", assignment]
    output = run_tool(args, "echoir transcribe", stdin=stdin)
    if case["mode"] in STREAM_MODES:
        return stream_transcript(output, case.get("max_segment_ms"))
    texts = []
    for line in output.splitlines():
        match = TRANSCRIPT_LINE.match(line)
        if match is None:
            raise Fail(f"unexpected line from echoir transcribe: {line!r}")
        texts.append(match.group(1))
    return " ".join(texts)


def stream_transcript(output, max_segment_ms=None):
    """The finals of a jsonl stream, joined. An error event fails the case, and so does a final
    longer than max_segment_ms when that is given."""
    texts = []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            raise Fail(f"unexpected line from echoir transcribe: {line!r}")
        kind = event.get("type")
        if kind == "final":
            length = event["end_ms"] - event["start_ms"]
            if max_segment_ms is not None and length > max_segment_ms:
                raise Fail(f"a final lasts {length} ms, longer than the {max_segment_ms} ms cap")
            texts.append(event["text"])
        elif kind == "error":
            raise Fail(f"echoir reported an error: {event.get('message', '')}")
    return " ".join(text for text in texts if text)


def serve_chunks(path):
    """The fixture's rate, channel count and its frames as 16-bit chunks of SERVE_CHUNK_MS each,
    except that the last one may be shorter."""
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise Usage(f"{path.name} is not 16-bit, so it cannot be sent to serve")
        rate, channels = w.getframerate(), w.getnchannels()
        per_chunk = rate * SERVE_CHUNK_MS // 1000
        chunks = []
        while True:
            frames = w.readframes(per_chunk)
            if not frames:
                break
            chunks.append(frames)
    return rate, channels, chunks


def serve_commands(case, fixture, vad, context_file):
    """The commands of a serve case: load, start with push, the audio, flush, stop and quit."""
    load = {"cmd": "load", "model": case["model"], "backend": case["backend"], "vad": vad}
    if case.get("mmproj"):
        load["mmproj"] = case["mmproj"]
    if case.get("gpu") is False:
        load["gpu"] = False
    start = {"cmd": "start", "input": "push"}
    if "policy" in case:
        start["policy"] = case["policy"]
    if context_file is not None:
        start["context"] = context_file.read_text(encoding="utf-8-sig").strip()
    rate, channels, chunks = serve_chunks(fixture)
    commands = [load, start]
    for chunk in chunks:
        commands.append({
            "cmd": "audio",
            "pcm_s16le_b64": base64.b64encode(chunk).decode("ascii"),
            "rate": rate,
            "channels": channels,
        })
    commands += [{"cmd": "flush"}, {"cmd": "stop"}, {"cmd": "quit"}]
    return commands


def serve_transcript(echoir, case, fixture, models_dir, vad, vad_params, context_file):
    """Runs `echoir serve --stdio` with the case's commands, and returns the transcript."""
    if vad_params:
        raise Usage(f"case {case['id']!r}: serve takes no --vad-param")
    commands = serve_commands(case, fixture, vad, context_file)
    script = "".join(json.dumps(command) + "\n" for command in commands).encode("utf-8")
    output = run_tool(
        [echoir, "serve", "--stdio", "--models-dir", str(models_dir)],
        "echoir serve", stdin=script,
    )
    return serve_events(output, [command["cmd"] for command in commands])


def serve_events(output, sent):
    """The final events of a serve session, joined. An error reply or an error event fails the
    case, and so do replies that do not match the commands sent, in number or in order. `sent`
    lists the cmd of each command, so its last one, quit, must be the last reply too."""
    texts, replies = [], []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            raise Fail(f"unexpected line from echoir serve: {line!r}")
        if not isinstance(event, dict):
            raise Fail(f"unexpected line from echoir serve: {line!r}")
        kind = event.get("type")
        if kind == "final":
            texts.append(event["text"])
        elif kind in ("ok", "error") and "cmd" in event:
            if kind == "error":
                raise Fail(
                    f"echoir serve reported an error for {event['cmd']!r}: "
                    f"{event.get('message', '')}"
                )
            replies.append(event["cmd"])
        elif kind == "error":
            raise Fail(f"echoir reported an error: {event.get('message', '')}")
    if len(replies) != len(sent):
        raise Fail(f"echoir serve sent {len(replies)} replies for {len(sent)} commands")
    if replies != sent:
        raise Fail("echoir serve sent its replies out of order")
    return " ".join(text for text in texts if text)


def score(echoir, hyp_path, ref_path):
    out = run_tool([echoir, "score", str(hyp_path), str(ref_path)], "echoir score")
    try:
        return json.loads(out)
    except ValueError as e:
        raise Fail(f"echoir score printed no JSON: {e}")


def normalize(echoir, path=None, text=None):
    """The normalized form of a file, or of text given on standard input."""
    if path is not None:
        out = run_tool([echoir, "score", "--normalize", str(path)], "echoir score --normalize")
    else:
        out = run_tool(
            [echoir, "score", "--normalize", "-"], "echoir score --normalize -",
            stdin=text.encode("utf-8"),
        )
    return out.strip()


def limit_text(case):
    """The case's allowance, as the run prints it."""
    if case.get("expect_empty"):
        return f"max_words {case['max_words']}"
    return f"max_rate {float(case['max_rate']):g}"


def problems_for(echoir, case, result, norm_hyp):
    """The reasons the case fails. An empty list means it passes."""
    problems = []
    if case.get("expect_empty"):
        # The reference is empty, so every word of the transcript counts as an insertion.
        if result["ref_len"] != 0:
            raise Usage(f"case {case['id']!r}: expect_empty needs an empty reference")
        allowed = case["max_words"]
        if result["ins"] > allowed:
            problems.append(
                f"expected an empty transcript; it has {result['ins']} words, "
                f"at most {allowed} allowed"
            )
    else:
        max_rate = float(case["max_rate"])
        if result["rate"] > max_rate:
            problems.append(f"rate {result['rate']:.4f} is above max_rate {max_rate:g}")
    if "must_contain" in case:
        needle = normalize(echoir, text=case["must_contain"])
        if needle == "":
            raise Usage(f"case {case['id']!r}: must_contain has no text after normalization")
        if needle not in norm_hyp:
            problems.append("the must_contain text is missing")
    if "must_contain_once" in case:
        needle = normalize(echoir, text=case["must_contain_once"])
        if needle == "":
            raise Usage(f"case {case['id']!r}: must_contain_once has no text after normalization")
        # Whole words only, so "sir" does not match inside "sire".
        count = f" {norm_hyp} ".count(f" {needle} ")
        if count != 1:
            problems.append(f"the must_contain_once text occurs {count} times")
    return problems


def measure(echoir, case, search_dirs, models_dir, vad_id, vad_params):
    """Runs a case's tool and scores its transcript. Returns the score, the normalized transcript,
    the transcript itself and the reference path."""
    check_case(case)
    entries, default_vads = read_registry(models_dir)
    if vad_id is None:
        if case["backend"] not in default_vads:
            raise Usage(f"{models_dir / 'registry.json'}: no default_vad for {case['backend']!r}")
        vad = default_vads[case["backend"]]
    else:
        vad = vad_id
    check_vad(entries, vad)
    check_models(models_dir, entries, case, vad)
    fixture = find_text_file(search_dirs, case["fixture"])
    ref_path = find_text_file(search_dirs, case["ref"])
    context_file = find_text_file(search_dirs, case["context"]) if "context" in case else None

    hyp = transcribe(echoir, case, fixture, models_dir, vad, vad_params, context_file)
    with tempfile.TemporaryDirectory() as tmp:
        hyp_path = Path(tmp) / "hyp.txt"
        hyp_path.write_bytes(hyp.encode("utf-8"))
        result = score(echoir, hyp_path, ref_path)
        norm_hyp = normalize(echoir, path=hyp_path)
    return result, norm_hyp, hyp, ref_path


def run_case(echoir, case, cases, search_dirs, models_dir, vad_id, vad_params, show_text):
    result, norm_hyp, hyp, ref_path = measure(
        echoir, case, search_dirs, models_dir, vad_id, vad_params
    )
    problems = problems_for(echoir, case, result, norm_hyp)
    rate_line = (
        f"rate {result['rate']:.4f} ({limit_text(case)}; "
        f"subs {result['subs']} dels {result['dels']} ins {result['ins']} "
        f"ref_len {result['ref_len']})"
    )
    if "compare_to" in case:
        baseline = baseline_case(cases, case)
        base_result, _, _, _ = measure(
            echoir, baseline, search_dirs, models_dir, vad_id, vad_params
        )
        rate_line += f"; {baseline['id']} rate {base_result['rate']:.4f}"
        if result["rate"] >= base_result["rate"]:
            problems.append(
                f"rate {result['rate']:.4f} is not below the rate {base_result['rate']:.4f} "
                f"of {baseline['id']!r}"
            )
    if problems:
        print(f"FAIL e2e.{case['id']}: " + "; ".join(problems))
        print(f"  {rate_line}")
        if show_text:
            print(f"  hyp: {hyp}")
            print(f"  ref: {ref_path.read_text(encoding='utf-8').strip()}")
        return EXIT_FAIL
    print(f"PASS e2e.{case['id']}: {rate_line}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run one end-to-end case: transcribe a fixture and score the transcript."
    )
    parser.add_argument("--echoir", required=True, help="the echoir binary")
    parser.add_argument("--case", required=True, help="the case id")
    parser.add_argument("--cases", help="cases file (default: tests/e2e/cases.json)")
    parser.add_argument(
        "--vad",
        help="VAD registry id (default: the registry's default_vad for the case's backend)",
    )
    parser.add_argument(
        "--vad-param", action="append", default=[], metavar="KEY=VALUE",
        help="a VAD setting for echoir transcribe; repeatable",
    )
    parser.add_argument(
        "--show-text", action="store_true",
        help="on a failure, also print the transcript and the reference",
    )
    parser.add_argument("--source-dir", help="repository root (default: this script's)")
    args = parser.parse_args(argv)

    if args.source_dir:
        source = Path(args.source_dir).resolve()
    else:
        source = Path(__file__).resolve().parents[2]
    cases_path = Path(args.cases) if args.cases else source / "tests" / "e2e" / "cases.json"
    models_env = os.environ.get("ECHOIR_MODELS")
    models_dir = Path(models_env) if models_env else source / "models"

    for assignment in args.vad_param:
        if "=" not in assignment or assignment.startswith("="):
            print(f"error: --vad-param expects KEY=VALUE, got {assignment!r}", file=sys.stderr)
            return EXIT_USAGE
    search_dirs = [
        cases_path.resolve().parent,
        source / "tests" / "fixtures",
        source / "build" / "fixtures",
    ]

    try:
        cases = load_cases(cases_path)
        case = find_case(cases, args.case)
        return run_case(
            args.echoir, case, cases, search_dirs, models_dir, args.vad, args.vad_param,
            args.show_text,
        )
    except Usage as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_USAGE
    except Skip as e:
        print(f"SKIP e2e.{args.case}: {e}")
        return EXIT_SKIP
    except Fail as e:
        print(f"FAIL e2e.{args.case}: {e}")
        return EXIT_FAIL


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
