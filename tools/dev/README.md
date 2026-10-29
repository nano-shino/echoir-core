# Development tools

The scripts in this directory are for development only. The build, the unit and integration
tests, the end-to-end runner and the `echoir` tool do not use them. The one exception is the
committed reference data in `tests/fixtures/firered/`, which `firered_reference.py` wrote once.

## firered_reference.py

Writes the golden reference data for the FireRedVAD implementation. It runs the upstream
Python code on a clip and writes the features, frame probabilities and speech ranges that the
C++ tests compare against. It needs:

- A checkout of the upstream FireRedVAD code, https://github.com/FireRedTeam/FireRedVAD. The
  script imports its `fireredvad` package as it is.
- The upstream weights, from https://huggingface.co/FireRedTeam/FireRedVAD: the `VAD/` and
  `Stream-VAD/` directories, each with `model.pth.tar` and `cmvn.ark`.
- A Python environment with torch, numpy, kaldi_native_fbank, kaldiio, pyyaml and soundfile.
  Make it with uv, outside the repository.

```sh
uv venv --python 3.12 "$HOME/firered/venv"
uv pip install --python "$HOME/firered/venv/bin/python" \
    torch numpy kaldi_native_fbank kaldiio pyyaml soundfile
```

The script's header has the full commands and the usage. The checkpoints are loaded with
`weights_only=True`.

## Not development tools

`tools/convert-firered-vad.py` is not in this directory. `tools/fetch-models.sh` runs it to build
the FireRedVAD model files when they are not hosted. It needs only Python 3.
