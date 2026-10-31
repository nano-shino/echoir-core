"""Measures streaming runs of echoir: final WER, real-time factor and PARTIAL rewrite rate.

Each case is streamed by `echoir transcribe --stream` once per stabilization policy and minimum
chunk. The rewrite rate is the share of PARTIAL updates that do not extend the previous PARTIAL of
their segment. One JSON record per run goes to standard output. Transcripts are never printed, so
the output can be kept when the fixtures are private.
"""

import argparse
import json
import os
import subprocess
import sys
import time
import wave
from pathlib import Path

EXIT_FAIL = 1
EXIT_USAGE = 2

POLICIES = ("agreement", "rollback")
MIN_CHUNK_MS = (500, 1000, 2000)
# The audio pushed per step, in ms.
PUSH_MS = 100
# A run that takes longer than this has hung.
TIMEOUT_S = 1800

REPO = Path(__file__).resolve().parents[1]


class Usage(Exception):
    """A bad argument, cases file or fixture."""


class Fail(Exception):
    """A tool failed, or a run reported an error."""


def normalize(text):
    """Whitespace collapsed to single spaces and trimmed, as the rewrite test compares text."""
    return " ".join(text.split())


def load_cases(path):
    try:
        with open(path, encoding="utf-8") as f:
            cases = json.load(f)
    except (OSError, ValueError) as e:
        raise Usage(f"cannot read {path}: {e}")
    if not isinstance(cases, list):
        raise Usage(f"{path} must hold a JSON list of cases")
    return [case for case in cases if isinstance(case, dict)]


def find_case(cases, case_id):
    for case in cases:
        if case.get("id") == case_id:
            missing = [key for key in ("fixture", "ref", "backend", "model") if key not in case]
            if missing:
                raise Usage(f"case {case_id!r} lacks {', '.join(missing)}")
            return case
    raise Usage(f"no case with id {case_id!r}")


def find_file(search_dirs, name):
    """A fixture, reference or context file: the first of search_dirs that holds it."""
    for directory in search_dirs:
        path = directory / name
        if path.is_file():
            return path
    raise Usage(f"{name} is not in any of {', '.join(str(d) for d in search_dirs)}")


def default_vad(models_dir, backend):
    path = models_dir / "registry.json"
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)["default_vad"][backend]
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise Usage(f"no default VAD for {backend!r} in {path}: {e}")


def audio_seconds(path):
    try:
        with wave.open(str(path), "rb") as wav:
            return wav.getnframes() / wav.getframerate()
    except (wave.Error, EOFError, OSError) as e:
        raise Usage(f"cannot read the WAV length of {path}: {e}")


def run_tool(args, what, stdin=None):
    """Runs one command and returns its standard output as text."""
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


def summarize(output):
    """Returns the joined final text, the number of finals, and the PARTIAL counts."""
    finals = []
    partials = updates = rewrites = 0
    newest = {}  # segment id -> the normalized text of its newest PARTIAL
    for line in output.splitlines():
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except ValueError:
            raise Fail("echoir transcribe printed a line that is not JSON")
        kind = event.get("type")
        if kind == "error":
            raise Fail(f"echoir reported an error: {event.get('message', '')}")
        if kind == "final":
            finals.append(event.get("text", ""))
        elif kind == "partial":
            partials += 1
            text = normalize(event.get("text", ""))
            # Only a PARTIAL that follows an earlier one of its segment is an update.
            previous = newest.get(event["id"])
            if previous is not None:
                updates += 1
                if not text.startswith(previous):
                    rewrites += 1
            newest[event["id"]] = text
    hyp = " ".join(text for text in finals if text)
    return hyp, len(finals), partials, updates, rewrites


def score(echoir, hyp, ref):
    out = run_tool([echoir, "score", "-", str(ref)], "echoir score", stdin=hyp.encode("utf-8"))
    try:
        return json.loads(out)
    except ValueError:
        raise Fail("echoir score printed no JSON")


def measure(echoir, case, fixture, ref, context, models_dir, job):
    """Runs one streaming transcription and returns its metrics as one record."""
    model, vad, policy, min_chunk_ms, audio_s = job
    args = [
        echoir,
        "transcribe",
        str(fixture),
        "--backend",
        case["backend"],
        "--model",
        model,
        "--vad",
        vad,
        "--models-dir",
        str(models_dir),
        "--stream",
        "--pace",
        "none",
        "--format",
        "jsonl",
        "--policy",
        policy,
        "--min-chunk-ms",
        str(min_chunk_ms),
        "--chunk-ms",
        str(PUSH_MS),
    ]
    if context is not None:
        args += ["--context-file", str(context)]
    started = time.monotonic()
    output = run_tool(args, "echoir transcribe")
    wall_s = time.monotonic() - started
    hyp, finals, partials, updates, rewrites = summarize(output)
    result = score(echoir, hyp, ref)
    return {
        "case": case["id"],
        "model": model,
        "vad": vad,
        "policy": policy,
        "min_chunk_ms": min_chunk_ms,
        "push_ms": PUSH_MS,
        "audio_s": round(audio_s, 3),
        "wall_s": round(wall_s, 3),
        "rtf": wall_s / audio_s,
        "wer": result["rate"],
        "subs": result["subs"],
        "dels": result["dels"],
        "ins": result["ins"],
        "ref_len": result["ref_len"],
        "finals": finals,
        "partials": partials,
        "updates": updates,
        "rewrites": rewrites,
        "rewrite_rate": rewrites / updates if updates else None,
    }


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Measure streaming runs: WER, real-time factor and rewrite rate.",
        epilog="Exit status: 0 when every run finished, 1 when a tool failed or reported an error, "
        "2 for a usage error. Run with python3 -I; only the standard library is used.",
    )
    parser.add_argument("--echoir", required=True, help="the echoir binary")
    parser.add_argument(
        "--case",
        action="append",
        required=True,
        metavar="ID",
        help="a case id from the cases file; repeatable",
    )
    parser.add_argument("--cases", help="cases file (default: tests/e2e/cases.json)")
    parser.add_argument("--model", help="registry id that replaces the model of every case")
    parser.add_argument(
        "--policy",
        action="append",
        choices=POLICIES,
        help="stabilization policy; repeatable (default: both)",
    )
    parser.add_argument(
        "--min-chunk-ms",
        action="append",
        type=int,
        metavar="MS",
        help="minimum pending audio before a decode, in ms; repeatable (default: 500, 1000, 2000)",
    )
    parser.add_argument(
        "--models-dir", help="registry.json and the model files (default: models/ or ECHOIR_MODELS)"
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    policies = args.policy or list(POLICIES)
    min_chunks = args.min_chunk_ms or list(MIN_CHUNK_MS)
    if any(ms < 0 for ms in min_chunks):
        print("error: --min-chunk-ms must not be negative", file=sys.stderr)
        return EXIT_USAGE

    try:
        cases_path = Path(args.cases) if args.cases else REPO / "tests" / "e2e" / "cases.json"
        models_dir = Path(args.models_dir or os.environ.get("ECHOIR_MODELS") or REPO / "models")
        search_dirs = [
            cases_path.resolve().parent,
            REPO / "tests" / "fixtures",
            REPO / "build" / "fixtures",
        ]
        cases = load_cases(cases_path)

        # Each case is resolved once, so a missing file fails before the first run.
        resolved = []
        for case_id in args.case:
            case = find_case(cases, case_id)
            fixture = find_file(search_dirs, case["fixture"])
            ref = find_file(search_dirs, case["ref"])
            context = find_file(search_dirs, case["context"]) if case.get("context") else None
            audio_s = audio_seconds(fixture)
            if audio_s <= 0:
                raise Usage(f"case {case_id!r}: the fixture has no audio")
            model = args.model or case["model"]
            vad = default_vad(models_dir, case["backend"])
            resolved.append((case, fixture, ref, context, model, vad, audio_s))

        total = len(resolved) * len(policies) * len(min_chunks)
        done = 0
        for case, fixture, ref, context, model, vad, audio_s in resolved:
            for policy in policies:
                for min_chunk_ms in min_chunks:
                    job = (model, vad, policy, min_chunk_ms, audio_s)
                    record = measure(args.echoir, case, fixture, ref, context, models_dir, job)
                    done += 1
                    print(json.dumps(record), flush=True)
                    print(
                        f"[{done}/{total}] {case['id']} {policy} min_chunk_ms={min_chunk_ms}: "
                        f"wer {record['wer']:.4f}, rtf {record['rtf']:.3f}, "
                        f"rewrites {record['rewrites']}/{record['updates']}, "
                        f"partials {record['partials']}, {record['wall_s']:.1f} s",
                        file=sys.stderr,
                        flush=True,
                    )
    except Usage as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_USAGE
    except Fail as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return EXIT_FAIL
    return 0


if __name__ == "__main__":
    sys.exit(main())
