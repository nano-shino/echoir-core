#!/usr/bin/env python3
"""Converts a FireRedVAD checkpoint (model.pth.tar and cmvn.ark) into a GGUF file.

The GGUF file holds the hyper-parameters under firered-vad.*, the network weights under their
checkpoint names (float32, PyTorch shape order), and the CMVN statistics as cmvn.mean and
cmvn.istd. In ggml the `ne` array of a tensor is its PyTorch shape reversed: a Linear weight
[out, in] has ne = [in, out], and a depthwise conv weight [P, 1, N] has ne = [N, 1, P], so kernel
tap i of channel c is element c * N + i. The checkpoint layout and the CMVN formula follow the
FireRedVAD code (Apache-2.0).

Standard library only. Run it with -I:

    python3 -I convert-firered-vad.py <model.pth.tar> <cmvn.ark> <out.gguf>
    python3 -I convert-firered-vad.py --check <file.gguf>

A checkpoint is a pickle, so it is read by an unpickler that resolves only the globals a FireRedVAD
checkpoint uses. Any other global is an error, so a crafted file cannot run code. The output is
deterministic: the same inputs always give the same bytes.
"""

import math
import pickle
import struct
import sys
import zipfile

GGUF_MAGIC = b"GGUF"
GGUF_VERSION = 3
GGUF_ALIGNMENT = 32
GGUF_TYPE_UINT32 = 4
GGUF_TYPE_ARRAY = 9
GGUF_TYPE_STRING = 8
GGML_TYPE_F32 = 0
ARCH = "firered-vad"
UPSTREAM_URL = "https://huggingface.co/FireRedTeam/FireRedVAD"
CMVN_FLOOR = 1e-20

# Hyper-parameters stored under firered-vad.<key>, with the bounds the runtime accepts.
HPARAM_LIMITS = {
    "idim": (1, 4096),
    "R": (1, 64),
    "M": (1, 64),
    "H": (1, 4096),
    "P": (1, 4096),
    "N1": (1, 64),
    "S1": (1, 64),
    "N2": (0, 64),
    "S2": (1, 64),
    "odim": (1, 4096),
}

USAGE = """usage: convert-firered-vad.py <model.pth.tar> <cmvn.ark> <out.gguf>
       convert-firered-vad.py --check <file.gguf>
"""


class ConvertError(Exception):
    pass


# Stands in for torch.FloatStorage. Every other storage type is rejected.
_FLOAT_STORAGE = object()


class _Storage:
    def __init__(self, key, numel):
        self.key = key
        self.numel = numel


class _TensorRef:
    def __init__(self, storage, offset, size, stride):
        self.storage = storage
        self.offset = offset
        self.size = size
        self.stride = stride


def _rebuild_tensor_v2(storage, offset, size, stride, *_unused):
    return _TensorRef(storage, offset, size, stride)


class _Dict(dict):
    # Stands in for collections.OrderedDict. The checkpoint's _metadata is not needed.
    def __setstate__(self, state):
        pass


class _Namespace(dict):
    # Stands in for argparse.Namespace: its attributes become dict items.
    def __setstate__(self, state):
        self.update(state)


class _CheckpointUnpickler(pickle.Unpickler):
    def find_class(self, module, name):
        if module == "torch._utils" and name == "_rebuild_tensor_v2":
            return _rebuild_tensor_v2
        if module == "collections" and name == "OrderedDict":
            return _Dict
        if module == "argparse" and name == "Namespace":
            return _Namespace
        if module == "torch" and name.endswith("Storage"):
            if name == "FloatStorage":
                return _FLOAT_STORAGE
            raise ConvertError(f"unsupported storage torch.{name}: only float32 is accepted")
        raise ConvertError(f"checkpoint refers to {module}.{name}, which is not allowed")

    def persistent_load(self, pid):
        # A storage record is ("storage", storage type, key, location, element count).
        if not (isinstance(pid, tuple) and len(pid) == 5 and pid[0] == "storage"):
            raise ConvertError("checkpoint has an unsupported persistent id")
        _, storage_type, key, _location, numel = pid
        if storage_type is not _FLOAT_STORAGE:
            raise ConvertError("checkpoint storage is not float32")
        if not isinstance(key, str) or type(numel) is not int or numel < 0:
            raise ConvertError("checkpoint storage record is malformed")
        return _Storage(key, numel)


def _prod(values):
    result = 1
    for v in values:
        result *= v
    return result


def _contiguous_strides(size):
    strides = []
    acc = 1
    for d in reversed(size):
        strides.append(acc)
        acc *= d
    return tuple(reversed(strides))


def _tensor_bytes(zf, prefix, ref, raw_cache, name):
    """Returns the float32 little-endian bytes of one dense tensor."""
    storage = ref.storage
    if not isinstance(storage, _Storage) or type(ref.offset) is not int or ref.offset < 0:
        raise ConvertError(f"{name} has a malformed storage reference")
    if not isinstance(ref.size, tuple) or not isinstance(ref.stride, tuple):
        raise ConvertError(f"{name} has a malformed shape")
    size = ref.size
    if any(type(d) is not int or d < 0 for d in size):
        raise ConvertError(f"{name} has a malformed shape")
    # Size-1 dimensions may carry any stride; every other stride must be the dense one.
    expect = _contiguous_strides(size)
    if len(ref.stride) != len(size) or any(
        d != 1 and s != e for d, s, e in zip(size, ref.stride, expect)
    ):
        raise ConvertError(f"{name} is not contiguous; only dense tensors are accepted")
    numel = _prod(size)
    if ref.offset + numel > storage.numel:
        raise ConvertError(f"{name} reads past the end of its storage")
    raw = raw_cache.get(storage.key)
    if raw is None:
        try:
            raw = zf.read(prefix + "data/" + storage.key)
        except KeyError:
            raise ConvertError(f"checkpoint has no data record {storage.key}")
        except Exception as e:
            raise ConvertError(f"cannot read data record {storage.key}: {e}")
        raw_cache[storage.key] = raw
    # Checked on every reference: storages that share a key must agree on the record length.
    if len(raw) != storage.numel * 4:
        raise ConvertError(f"storage {storage.key} is not {storage.numel} float32 values")
    return raw[ref.offset * 4 : (ref.offset + numel) * 4]


def load_checkpoint(path):
    """Returns (args, tensors): args maps each hyper-parameter name to its value, and tensors maps
    each state dict name to (PyTorch shape, float32 bytes)."""
    try:
        zf = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as e:
        raise ConvertError(f"{path} is not a torch checkpoint: {e}")
    with zf:
        names = zf.namelist()
        pkl = [n for n in names if n == "data.pkl" or n.endswith("/data.pkl")]
        if len(pkl) != 1:
            raise ConvertError(f"{path} has no single data.pkl record")
        prefix = pkl[0][: -len("data.pkl")]
        if prefix + "byteorder" in names:
            try:
                byteorder = zf.read(prefix + "byteorder")
            except Exception as e:
                raise ConvertError(f"cannot read the byte order of {path}: {e}")
            if byteorder != b"little":
                raise ConvertError(f"{path} is not little-endian")
        try:
            with zf.open(pkl[0]) as f:
                top = _CheckpointUnpickler(f).load()
        except ConvertError:
            raise
        except Exception as e:
            raise ConvertError(f"cannot read the checkpoint records of {path}: {e}")
        if not isinstance(top, dict) or "args" not in top or "model_state_dict" not in top:
            raise ConvertError(f"{path} has no args or model_state_dict")
        args = top["args"]
        if not isinstance(args, _Namespace):
            raise ConvertError("checkpoint args is not a namespace")
        state = top["model_state_dict"]
        if not isinstance(state, dict):
            raise ConvertError("checkpoint model_state_dict is not a mapping")
        tensors = {}
        raw_cache = {}
        for name, ref in state.items():
            if not isinstance(ref, _TensorRef):
                raise ConvertError(f"{name} is not a tensor")
            tensors[name] = (ref.size, _tensor_bytes(zf, prefix, ref, raw_cache, name))
    return args, tensors


def hparams_from_args(args):
    hp = {}
    for key, (low, high) in HPARAM_LIMITS.items():
        value = args.get(key)
        if type(value) is not int or not low <= value <= high:
            raise ConvertError(f"checkpoint args.{key} is {value!r}, expected {low}..{high}")
        hp[key] = value
    return hp


def expected_weights(hp):
    """The weight tensors of the network, in order, as (checkpoint name, PyTorch shape)."""
    out = []

    def linear(name, n_in, n_out, bias=True):
        out.append((name + ".weight", (n_out, n_in)))
        if bias:
            out.append((name + ".bias", (n_out,)))

    def fsmn(name):
        out.append((name + ".lookback_filter.weight", (hp["P"], 1, hp["N1"])))
        if hp["N2"] > 0:
            out.append((name + ".lookahead_filter.weight", (hp["P"], 1, hp["N2"])))

    linear("dfsmn.fc1.0", hp["idim"], hp["H"])
    linear("dfsmn.fc2.0", hp["H"], hp["P"])
    fsmn("dfsmn.fsmn1")
    for i in range(hp["R"] - 1):
        base = f"dfsmn.fsmns.{i}"
        linear(base + ".fc1.0", hp["P"], hp["H"])
        linear(base + ".fc2", hp["H"], hp["P"], bias=False)
        fsmn(base + ".fsmn")
    # Each DNN Linear is at every third index of dnns, between parameterless ReLU and Dropout.
    for k in range(hp["M"]):
        linear(f"dfsmn.dnns.{3 * k}", hp["P"] if k == 0 else hp["H"], hp["H"])
    linear("out", hp["H"], hp["odim"])
    return out


def check_layout(hp, shapes):
    """Raises unless shapes (name -> PyTorch shape) are exactly the weights hp describes."""
    wanted = expected_weights(hp)
    by_name = dict(wanted)
    missing = [n for n, _ in wanted if n not in shapes]
    extra = sorted(n for n in shapes if n not in by_name)
    wrong = [(n, shapes[n], s) for n, s in wanted if n in shapes and shapes[n] != s]
    if missing or extra or wrong:
        parts = []
        if missing:
            parts.append(f"missing {missing[:3]}")
        if extra:
            parts.append(f"unexpected {extra[:3]}")
        if wrong:
            parts.append(f"wrong shape {wrong[:3]}")
        raise ConvertError("weights do not match the hyper-parameters: " + "; ".join(parts))


def read_cmvn(path, idim):
    """Reads a Kaldi binary statistics matrix (row 0: sums with the frame count last, row 1: sums
    of squares). Returns the per-dimension mean and inverse standard deviation."""
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        raise ConvertError(f"cannot read {path}: {e}")
    if raw[:2] != b"\x00B" or raw[2:5] not in (b"DM ", b"FM "):
        raise ConvertError(f"{path} is not a Kaldi binary double or float matrix")
    code, width = ("d", 8) if raw[2:5] == b"DM " else ("f", 4)
    rows, pos = _kaldi_int(raw, 5, path)
    cols, pos = _kaldi_int(raw, pos, path)
    if rows != 2 or cols != idim + 1 or len(raw) - pos != rows * cols * width:
        raise ConvertError(f"{path} is not a 2 x {idim + 1} statistics matrix")
    stats = struct.unpack_from("<%d%s" % (rows * cols, code), raw, pos)
    count = stats[idim]
    if count < 1:
        raise ConvertError(f"{path} has a frame count below 1")
    means = []
    istds = []
    for d in range(idim):
        mean = stats[d] / count
        variance = stats[cols + d] / count - mean * mean
        if variance < CMVN_FLOOR:
            variance = CMVN_FLOOR
        means.append(mean)
        istds.append(1.0 / math.sqrt(variance))
    return means, istds


def _kaldi_int(raw, pos, path):
    if len(raw) < pos + 5 or raw[pos : pos + 1] != b"\x04":
        raise ConvertError(f"{path} has a malformed Kaldi header")
    return struct.unpack_from("<i", raw, pos + 1)[0], pos + 5


def _pad(n, align=GGUF_ALIGNMENT):
    return (-n) % align


def _gguf_str(s):
    raw = s.encode("utf-8")
    return struct.pack("<Q", len(raw)) + raw


def build_gguf(hp, name, tensors):
    """tensors: a list of (name, PyTorch shape, float32 bytes). Returns the file bytes."""
    kvs = [
        ("general.architecture", GGUF_TYPE_STRING, ARCH),
        ("general.name", GGUF_TYPE_STRING, name),
        ("general.license", GGUF_TYPE_STRING, "apache-2.0"),
        ("general.source.url", GGUF_TYPE_STRING, UPSTREAM_URL),
        ("general.alignment", GGUF_TYPE_UINT32, GGUF_ALIGNMENT),
    ]
    kvs += [(f"{ARCH}.{key}", GGUF_TYPE_UINT32, hp[key]) for key in HPARAM_LIMITS]
    out = bytearray(GGUF_MAGIC)
    out += struct.pack("<IQQ", GGUF_VERSION, len(tensors), len(kvs))
    for key, vtype, value in kvs:
        out += _gguf_str(key) + struct.pack("<I", vtype)
        if vtype == GGUF_TYPE_STRING:
            out += _gguf_str(value)
        else:
            out += struct.pack("<I", value)
    # ggml's ne[] is the PyTorch shape reversed, innermost dimension first.
    offset = 0
    for tname, shape, raw in tensors:
        ne = tuple(reversed(shape))
        out += _gguf_str(tname) + struct.pack("<I", len(ne)) + struct.pack("<%dQ" % len(ne), *ne)
        out += struct.pack("<IQ", GGML_TYPE_F32, offset)
        offset += len(raw) + _pad(len(raw))
    out += b"\0" * _pad(len(out))
    for _, _, raw in tensors:
        out += raw + b"\0" * _pad(len(raw))
    return bytes(out)


class _Reader:
    _SCALARS = {
        0: "<B",
        1: "<b",
        2: "<H",
        3: "<h",
        4: "<I",
        5: "<i",
        6: "<f",
        7: "<?",
        10: "<Q",
        11: "<q",
        12: "<d",
    }

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        if n < 0 or self.pos + n > len(self.data):
            raise ConvertError("GGUF file is truncated")
        chunk = self.data[self.pos : self.pos + n]
        self.pos += n
        return chunk

    def unpack(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))[0]

    def string(self):
        raw = self.take(self.unpack("<Q"))
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            raise ConvertError("GGUF string is not UTF-8")

    def value(self, vtype):
        if vtype == GGUF_TYPE_STRING:
            return self.string()
        if vtype == GGUF_TYPE_ARRAY:
            etype = self.unpack("<I")
            # ggml does not read nested arrays, so a file that has them is not a GGUF it can load.
            if etype == GGUF_TYPE_ARRAY:
                raise ConvertError("GGUF file has a nested array, which is not supported")
            count = self.unpack("<Q")
            if count > len(self.data):
                raise ConvertError("GGUF array is truncated")
            return [self.value(etype) for _ in range(count)]
        fmt = self._SCALARS.get(vtype)
        if fmt is None:
            raise ConvertError(f"unknown GGUF value type {vtype}")
        return self.unpack(fmt)


def parse_gguf(data):
    """Returns (kvs, tensors): kvs maps keys to values, and tensors is a list of
    (name, PyTorch shape, float32 bytes). Raises ConvertError on anything malformed."""
    r = _Reader(data)
    if r.take(4) != GGUF_MAGIC:
        raise ConvertError("not a GGUF file")
    version = r.unpack("<I")
    if version != GGUF_VERSION:
        raise ConvertError(f"GGUF version {version} is not {GGUF_VERSION}")
    n_tensors = r.unpack("<Q")
    n_kv = r.unpack("<Q")
    if n_tensors > len(data) or n_kv > len(data):
        raise ConvertError("GGUF counts are larger than the file")
    kvs = {}
    for _ in range(n_kv):
        key = r.string()
        kvs[key] = r.value(r.unpack("<I"))
    alignment = kvs.get("general.alignment", 32)
    if type(alignment) is not int or alignment <= 0:
        raise ConvertError("general.alignment is not a positive integer")
    infos = []
    for _ in range(n_tensors):
        name = r.string()
        n_dims = r.unpack("<I")
        if not 1 <= n_dims <= 4:
            raise ConvertError(f"{name} has {n_dims} dimensions")
        ne = [r.unpack("<Q") for _ in range(n_dims)]
        ttype = r.unpack("<I")
        offset = r.unpack("<Q")
        infos.append((name, tuple(reversed(ne)), ttype, offset))
    data_start = r.pos + _pad(r.pos, alignment)
    tensors = []
    for name, shape, ttype, offset in infos:
        if ttype != GGML_TYPE_F32:
            raise ConvertError(f"{name} is not F32")
        if offset % alignment:
            raise ConvertError(f"{name} is not aligned")
        start = data_start + offset
        end = start + _prod(shape) * 4
        if end > len(data):
            raise ConvertError(f"{name} runs past the end of the file")
        tensors.append((name, shape, data[start:end]))
    return kvs, tensors


def convert(model_path, cmvn_path, out_path):
    args, tensors = load_checkpoint(model_path)
    hp = hparams_from_args(args)
    check_layout(hp, {name: shape for name, (shape, _) in tensors.items()})
    weights = [(name, shape, tensors[name][1]) for name, shape in expected_weights(hp)]
    means, istds = read_cmvn(cmvn_path, hp["idim"])
    cmvn = [
        ("cmvn.mean", (hp["idim"],), struct.pack("<%df" % hp["idim"], *means)),
        ("cmvn.istd", (hp["idim"],), struct.pack("<%df" % hp["idim"], *istds)),
    ]
    name = "FireRedVAD Stream-VAD" if hp["N2"] == 0 else "FireRedVAD VAD"
    blob = build_gguf(hp, name, weights + cmvn)
    parse_gguf(blob)  # the bytes must read back before anything is written
    try:
        with open(out_path, "wb") as f:
            f.write(blob)
    except OSError as e:
        raise ConvertError(f"cannot write {out_path}: {e}")
    print(f"wrote {out_path}: {len(weights) + len(cmvn)} tensors, {len(blob)} bytes")


def check_file(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise ConvertError(f"cannot read {path}: {e}")
    kvs, tensors = parse_gguf(data)
    if kvs.get("general.architecture") != ARCH:
        raise ConvertError(f"general.architecture is not {ARCH!r}")
    hp = {}
    for key in HPARAM_LIMITS:
        value = kvs.get(f"{ARCH}.{key}")
        if type(value) is not int:
            raise ConvertError(f"missing hyper-parameter {ARCH}.{key}")
        hp[key] = value
    shapes = {name: shape for name, shape, _ in tensors if not name.startswith("cmvn.")}
    check_layout(hp, shapes)
    cmvn = {name: shape for name, shape, _ in tensors if name.startswith("cmvn.")}
    if cmvn != {"cmvn.mean": (hp["idim"],), "cmvn.istd": (hp["idim"],)}:
        raise ConvertError(f"cmvn tensors are not two [{hp['idim']}] vectors")

    print(f"GGUF v3, {len(tensors)} tensors, {len(kvs)} keys")
    for key, value in kvs.items():
        print(f"  {key} = {value!r}")
    for name, shape, _ in tensors:
        print(f"  tensor {name} F32 {list(shape)}")
    lookahead = sum(1 for name, _, _ in tensors if name.endswith("lookahead_filter.weight"))
    weight_params = sum(_prod(shape) for name, shape, _ in tensors if not name.startswith("cmvn."))
    print(f"lookahead filters: {lookahead}")
    print(f"weight parameters: {weight_params}")
    print(f"cmvn values: {_prod(cmvn['cmvn.mean']) + _prod(cmvn['cmvn.istd'])}")
    print("check: ok")


def main(argv):
    if len(argv) == 2 and argv[0] == "--check":
        check_file(argv[1])
        return 0
    if len(argv) == 3:
        convert(argv[0], argv[1], argv[2])
        return 0
    sys.stderr.write(USAGE)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except ConvertError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
