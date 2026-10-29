# FireRedVAD reference data

Golden outputs of the upstream FireRedVAD code and weights (Apache-2.0, FireRedTeam/FireRedVAD).
The files are written by `tools/dev/firered_reference.py`, which imports the upstream
`AudioFeat`, `DetectModel` and `VadPostprocessor` unchanged. It is a dev-only tool: it needs torch,
it is not part of the build, and ctest does not run it.

- Upstream code: FireRedTeam/FireRedVAD commit `c30ec49e8cc69642b0ee65362eba11b9d11c6e54`.
- Weights: Hugging Face `FireRedTeam/FireRedVAD` at revision
  `7990aaccc6b7aec1e527743bd30201f2c4a03b8c`. sha256:
  `VAD/model.pth.tar` 63f4fb1b00a6b8607c118dd48efc18d5e40d67d99b7bf9aa7a8d61540cf23d71,
  `Stream-VAD/model.pth.tar` ec88a8ae8ac5f004cdbd20c1eac4a9e9d12067c3d51ac5b141afa1f921fb59c9,
  `cmvn.ark` (both) c87f6f13edf0f0ec7535ddfc9cc3387d9268cb234b70182d566c5e2edf3ca473.
- Inputs: `tests/fixtures/jfk.wav` (sha256 `59dfb9a4acb36fe2a2affc14bacbee2920ff435cb13cc314a08c13f66ba7860e`)
  and `tone_noise.wav` below (sha256 `baafdb35558748f66940200fe3574b6038aaab1baf5765f4f4aec438019fda29`).

## How they were made

Set up a Python environment outside the repository with `uv`, as the script's header describes:
`uv venv --python 3.12`, then `uv pip install torch numpy kaldi_native_fbank kaldiio pyyaml soundfile`.
Check out FireRedVAD at the commit above and download the weights at the revision above into a
`MODEL_DIR` that holds `VAD/` and `Stream-VAD/`. With `PY` set to the environment's Python and
`REPO` set to the checkout, run from the repository root:

    $PY tools/dev/firered_reference.py --make-tone-noise tests/fixtures/firered/tone_noise.wav
    $PY tools/dev/firered_reference.py REPO MODEL_DIR tests/fixtures/jfk.wav tests/fixtures/firered/jfk
    $PY tools/dev/firered_reference.py --variants vad REPO MODEL_DIR \
        tests/fixtures/firered/tone_noise.wav tests/fixtures/firered/tone_noise

The `jfk` run writes both variants by default. Each run writes `cmvn.f32` from the shared `cmvn.ark`.

## Files

- `tone_noise.wav` is synthetic: 1 s silence, a 440 Hz sine at -20 dBFS, then 1 s of white noise
  at -30 dBFS (NumPy `default_rng(0)`). dBFS is RMS relative to full scale 32767, so the sine's
  peak is about -17 dBFS. It has no `.ref.txt`, since it is not speech. NumPy does not guarantee
  its random stream across releases, so regenerating the WAV may change its bytes. The committed
  WAV is the reference.
- Files are float32 little-endian. `*.fbank.f32` (before CMVN) holds the first 300 frames.
  `*.feat.f32` (after CMVN) and `*.probs.f32` hold every frame. Feature rows have 80 values.
  `cmvn.f32` is the 80 means followed by the 80 inverse standard deviations.
- `tone_noise.vad.ranges.json` is empty by design: it is a negative control, and the model's
  probabilities stay far below the threshold. On real speech, the jfk ranges run smoothing, the
  threshold, the state machine and the start fix. Merge and extend are no-ops at their default 0,
  and no golden clip reaches the 20 s split limit, so these goldens do not test those stages.
