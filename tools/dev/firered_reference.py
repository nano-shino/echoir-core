#!/usr/bin/env python3
"""Writes golden reference data for the C++ FireRedVAD implementation, computed by the upstream
Python code.

Dev-only. It needs torch, numpy, kaldi_native_fbank, kaldiio, pyyaml and soundfile, so it is not
part of the build or the tests, and it is not run by ctest. Run it when the reference data changes,
and commit the output under tests/fixtures/firered/.

Setup with uv, outside the repository (FIRERED_WORK is any scratch directory):

    export FIRERED_WORK="$HOME/firered"
    uv venv --python 3.12 "$FIRERED_WORK/venv"
    uv pip install --python "$FIRERED_WORK/venv/bin/python" \
        torch numpy kaldi_native_fbank kaldiio pyyaml soundfile
    git clone https://github.com/FireRedTeam/FireRedVAD.git "$FIRERED_WORK/FireRedVAD"
    huggingface-cli download FireRedTeam/FireRedVAD --local-dir "$FIRERED_WORK/weights"

Usage (PY is the venv's python):

    $PY tools/dev/firered_reference.py --make-tone-noise tests/fixtures/firered/tone_noise.wav
    $PY tools/dev/firered_reference.py REPO MODEL_DIR tests/fixtures/jfk.wav \
        tests/fixtures/firered/jfk
    $PY tools/dev/firered_reference.py --variants vad REPO MODEL_DIR \
        tests/fixtures/firered/tone_noise.wav tests/fixtures/firered/tone_noise

REPO is the FireRedVAD checkout (its fireredvad/ package is imported as-is). MODEL_DIR holds the
VAD/ and Stream-VAD/ subdirectories, each with model.pth.tar and cmvn.ark.

For OUT_PREFIX = dir/clip, the script writes into dir/. Arrays are float32 little-endian, and
feature arrays are [frames][80] row-major:
    clip.fbank.f32                  first 300 frames, before CMVN
    clip.feat.f32                   every frame, after CMVN
    clip.<variant>.probs.f32        every frame
    clip.<variant>.ranges.json      VadPostprocessor.process + decision_to_segment, in seconds
    clip.json                       sample count, frame counts, shapes
    cmvn.f32                        80 means, then 80 inverse stds

Upstream code and weights are Apache-2.0. The checkpoints are loaded with weights_only=True.
"""

import argparse
import json
import math
import os
import sys

import numpy as np
import soundfile as sf
import torch

SAMPLE_RATE = 16000
FBANK_BINS = 80
# Only the fbank dump is capped to keep the committed data small. The VAD looks ahead, so a
# truncated feature sequence changes its late probabilities. feat and probs cover every frame.
FBANK_DUMP_FRAMES = 300
VARIANT_DIRS = {"vad": "VAD", "stream": "Stream-VAD"}
# Defaults from the upstream README. The streaming variant accepts shorter speech segments.
POST_DEFAULTS = {
    "smooth_window_size": 5,
    "prob_threshold": 0.4,
    "min_speech_frame": 20,
    "max_speech_frame": 2000,
    "min_silence_frame": 20,
    "merge_silence_frame": 0,
    "extend_speech_frame": 0,
}
STREAM_MIN_SPEECH_FRAME = 8


def frames_for(n_samples):
    """Kaldi snip_edges framing: 25 ms window, 10 ms shift at 16 kHz."""
    return 0 if n_samples < 400 else 1 + (n_samples - 400) // 160


def tone_noise_samples():
    """One second each of silence, a 440 Hz sine at -20 dBFS and white noise at -30 dBFS.

    dBFS levels are RMS relative to full scale (32767), so a sine's peak is RMS * sqrt(2).
    NumPy does not guarantee the Generator stream across releases, so regenerating the WAV on
    another NumPy version may change its bytes. The committed WAV is the reference.
    """
    rng = np.random.default_rng(0)
    t = np.arange(SAMPLE_RATE) / SAMPLE_RATE
    silence = np.zeros(SAMPLE_RATE)
    sine = math.sqrt(2) * 10 ** (-20 / 20) * np.sin(2 * np.pi * 440 * t)
    noise = 10 ** (-30 / 20) * rng.standard_normal(SAMPLE_RATE)
    x = np.concatenate([silence, sine, noise])
    return np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16)


def write_tone_noise(path):
    sf.write(path, tone_noise_samples(), SAMPLE_RATE, subtype="PCM_16")


def load_upstream(repo):
    sys.path.insert(0, os.path.abspath(repo))
    try:
        from fireredvad.core.audio_feat import AudioFeat
        from fireredvad.core.detect_model import DetectModel
        from fireredvad.core.vad_postprocessor import VadPostprocessor
    except ImportError as err:
        sys.exit(f"cannot import fireredvad from {repo}: {err}")
    return AudioFeat, DetectModel, VadPostprocessor


def load_detect_model(detect_model_cls, path):
    # weights_only=True loads tensors and the argparse.Namespace listed above, nothing else.
    torch.serialization.add_safe_globals([argparse.Namespace])
    package = torch.load(path, map_location="cpu", weights_only=True)
    model = detect_model_cls(package["args"])
    model.load_state_dict(package["model_state_dict"], strict=True)
    model.eval()
    return model


def read_audio(path):
    samples, rate = sf.read(path, dtype="int16")
    if rate != SAMPLE_RATE or samples.ndim != 1:
        sys.exit(f"{path}: need 16 kHz single-channel audio, got {rate} Hz, shape {samples.shape}")
    return samples


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def write_f32(path, array):
    np.ascontiguousarray(array, dtype="<f4").tofile(path)


def write_json(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")


def parse_variants(text):
    variants = [v.strip() for v in text.split(",") if v.strip()]
    unknown = [v for v in variants if v not in VARIANT_DIRS]
    if not variants or unknown:
        sys.exit(f"--variants takes a comma-separated list of {sorted(VARIANT_DIRS)}")
    return variants


def reference(args):
    variants = parse_variants(args.variants)
    AudioFeat, DetectModel, VadPostprocessor = load_upstream(args.repo)

    # Features are computed once, so every variant must use the same CMVN statistics.
    cmvn_paths = [os.path.join(args.model_dir, VARIANT_DIRS[v], "cmvn.ark") for v in variants]
    cmvn_bytes = [read_bytes(p) for p in cmvn_paths]
    if any(b != cmvn_bytes[0] for b in cmvn_bytes):
        sys.exit("cmvn.ark differs between the requested variants")

    out_prefix = args.out_prefix
    out_dir = os.path.dirname(out_prefix) or "."
    clip = os.path.basename(out_prefix)
    os.makedirs(out_dir, exist_ok=True)

    samples = read_audio(args.wav)
    audio_feat = AudioFeat(cmvn_paths[0])
    fbank = audio_feat.fbank((SAMPLE_RATE, samples))
    feat = audio_feat.cmvn(fbank)
    n_frames = fbank.shape[0]
    if n_frames != frames_for(samples.shape[0]):
        sys.exit(f"frame count {n_frames} does not match 1 + (n - 400) // 160")
    if n_frames == 0:
        sys.exit("clip is shorter than one 25 ms frame")
    if not np.isfinite(fbank).all():
        sys.exit("non-finite fbank values")

    # The same call upstream makes for a file. It must reproduce the steps above exactly.
    feat_upstream, dur = audio_feat.extract(args.wav)
    if not np.array_equal(feat.astype(np.float32), feat_upstream.numpy()):
        sys.exit("manual feature path differs from AudioFeat.extract")
    feat_f32 = feat.astype(np.float32)

    fbank_dump = min(n_frames, FBANK_DUMP_FRAMES)
    write_f32(f"{out_prefix}.fbank.f32", fbank[:fbank_dump])
    write_f32(f"{out_prefix}.feat.f32", feat_f32)
    write_f32(os.path.join(out_dir, "cmvn.f32"),
              np.concatenate([audio_feat.cmvn.means, audio_feat.cmvn.inverse_std_variances]))

    summary = {
        "clip": clip,
        "sample_rate": SAMPLE_RATE,
        "samples": int(samples.shape[0]),
        "duration_s": dur,
        "frames": n_frames,
        "dumped_frames": {"fbank": fbank_dump, "feat": n_frames},
        "shapes": {"fbank": [fbank_dump, FBANK_BINS], "feat": [n_frames, FBANK_BINS]},
        "segments": {},
    }
    for v in variants:
        model = load_detect_model(DetectModel, os.path.join(args.model_dir, VARIANT_DIRS[v],
                                                            "model.pth.tar"))
        with torch.no_grad():
            probs, _ = model(torch.from_numpy(feat_f32).unsqueeze(0))
        probs_list = probs.cpu().reshape(-1).tolist()
        if len(probs_list) != n_frames:
            sys.exit(f"{v}: got {len(probs_list)} probabilities for {n_frames} frames")

        params = dict(POST_DEFAULTS)
        if v == "stream":
            params["min_speech_frame"] = STREAM_MIN_SPEECH_FRAME
        post = VadPostprocessor(**params)
        ranges = post.decision_to_segment(post.process(probs_list), dur)

        write_f32(f"{out_prefix}.{v}.probs.f32", probs_list)
        write_json(f"{out_prefix}.{v}.ranges.json", {
            "model": VARIANT_DIRS[v],
            "params": params,
            "frames": n_frames,
            "duration_s": dur,
            "ranges": [[s, e] for s, e in ranges],
        })
        summary["shapes"][f"{v}_probs"] = [n_frames]
        summary["segments"][v] = len(ranges)
        print(f"{clip} {v}: {n_frames} frames, {len(ranges)} ranges")

    write_json(f"{out_prefix}.json", summary)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--make-tone-noise", metavar="WAV",
                        help="write the synthetic tone_noise clip and exit")
    parser.add_argument("--variants", default="vad,stream",
                        help="comma-separated subset of vad, stream (default: both)")
    parser.add_argument("repo", nargs="?", help="FireRedVAD checkout")
    parser.add_argument("model_dir", nargs="?", help="directory with VAD/ and Stream-VAD/")
    parser.add_argument("wav", nargs="?", help="16 kHz single-channel 16-bit WAV")
    parser.add_argument("out_prefix", nargs="?", help="output prefix, e.g. dir/clip")
    args = parser.parse_args(argv)
    if args.make_tone_noise is None and None in (args.repo, args.model_dir, args.wav,
                                                  args.out_prefix):
        parser.error("give REPO MODEL_DIR WAV OUT_PREFIX, or --make-tone-noise WAV")
    return args


def main(argv=None):
    args = parse_args(argv)
    if args.make_tone_noise is not None:
        write_tone_noise(args.make_tone_noise)
        return 0
    reference(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
