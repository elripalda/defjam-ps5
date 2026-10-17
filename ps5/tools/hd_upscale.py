#!/usr/bin/env python3
"""Upscale textures 4x with Real-ESRGAN's compact general model, on the CPU, no PyTorch.

    python3 ps5/tools/hd_upscale.py WEIGHTS.pth IN.png OUT.png [IN2.png OUT2.png ...]

WEIGHTS is realesr-general-x4v3.pth from github.com/xinntao/Real-ESRGAN
(BSD-3-Clause). The weights are read straight out of the PyTorch zip, turned
into an ONNX graph (SRVGGNetCompact: 34 convolutions, PReLU, pixel shuffle,
plus the nearest-upscaled input) and run with onnxruntime, in tiles so a big
texture fits in memory. Colour goes through the network; alpha is enlarged
with Lanczos. Run it on the user's own textures only; nothing here ships them.
"""
import io
import pickle
import sys
import zipfile

import numpy as np
import onnx
import onnxruntime as ort
from onnx import helper, numpy_helper, TensorProto
from PIL import Image

TILE = 192          # input pixels per tile side
PAD = 12            # overlap, input pixels


def load_state(path):
    z = zipfile.ZipFile(path)
    root = z.namelist()[0].split("/")[0]
    dtypes = {"FloatStorage": np.float32, "HalfStorage": np.float16, "DoubleStorage": np.float64}

    class Storage:
        def __init__(self, kind, key):
            self.array = np.frombuffer(z.read("%s/data/%s" % (root, key)), dtype=dtypes[kind])

    def rebuild(storage, offset, size, stride, *rest):
        a = storage.array
        if not size:
            return a[offset:offset + 1].reshape(()).copy()
        itemsize = a.itemsize
        return np.lib.stride_tricks.as_strided(a[offset:], shape=size,
                                               strides=[s * itemsize for s in stride]).copy()

    class Unpickler(pickle.Unpickler):
        def find_class(self, module, name):
            if name == "_rebuild_tensor_v2":
                return rebuild
            if module == "collections" and name == "OrderedDict":
                import collections
                return collections.OrderedDict
            if name.endswith("Storage"):
                return name
            if module == "torch._utils" and name == "_rebuild_parameter":
                return lambda data, *a: data
            raise pickle.UnpicklingError("unexpected %s.%s" % (module, name))

        def persistent_load(self, pid):
            _, kind, key, _, _ = pid
            return Storage(kind if isinstance(kind, str) else kind, key)

    state = Unpickler(io.BytesIO(z.read("%s/data.pkl" % root))).load()
    for k in ("params_ema", "params"):
        if isinstance(state, dict) and k in state:
            return state[k]
    return state


def build_model(state):
    nodes, inits = [], []
    convs = sorted({int(k.split(".")[1]) for k in state if k.endswith(".weight") and state[k].ndim == 4})
    x = "input"
    for i, idx in enumerate(convs):
        w, b = "w%d" % idx, "b%d" % idx
        inits += [numpy_helper.from_array(state["body.%d.weight" % idx].astype(np.float32), w),
                  numpy_helper.from_array(state["body.%d.bias" % idx].astype(np.float32), b)]
        y = "c%d" % idx
        nodes.append(helper.make_node("Conv", [x, w, b], [y], pads=[1, 1, 1, 1]))
        x = y
        act = "body.%d.weight" % (idx + 1)
        if act in state and state[act].ndim == 1:
            slope = "s%d" % idx
            inits.append(numpy_helper.from_array(state[act].astype(np.float32).reshape(-1, 1, 1), slope))
            y = "a%d" % idx
            nodes.append(helper.make_node("PRelu", [x, slope], [y]))
            x = y
    nodes.append(helper.make_node("DepthToSpace", [x], ["up"], blocksize=4, mode="CRD"))
    inits.append(numpy_helper.from_array(np.array([1, 1, 4, 4], np.float32), "scales"))
    nodes.append(helper.make_node("Resize", ["input", "", "scales"], ["base"], mode="nearest"))
    nodes.append(helper.make_node("Add", ["up", "base"], ["output"]))
    graph = helper.make_graph(nodes, "srvgg", [helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, None, None])],
                              [helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 3, None, None])], inits)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    return model.SerializeToString()


def upscale(sess, rgb):
    """rgb: float32 HxWx3 in 0..1. Returns 4H x 4W x 3."""
    h, w, _ = rgb.shape
    out = np.zeros((h * 4, w * 4, 3), np.float32)
    padded = np.pad(rgb, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="edge")
    for y in range(0, h, TILE):
        for x in range(0, w, TILE):
            th, tw = min(TILE, h - y), min(TILE, w - x)
            tile = padded[y:y + th + 2 * PAD, x:x + tw + 2 * PAD]
            res = sess.run(None, {"input": tile.transpose(2, 0, 1)[None]})[0][0].transpose(1, 2, 0)
            out[y * 4:(y + th) * 4, x * 4:(x + tw) * 4] = res[PAD * 4:(PAD + th) * 4, PAD * 4:(PAD + tw) * 4]
    return np.clip(out, 0, 1)


def main():
    if len(sys.argv) < 4 or len(sys.argv) % 2:
        raise SystemExit(__doc__)
    opts = ort.SessionOptions()
    opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    sess = ort.InferenceSession(build_model(load_state(sys.argv[1])), opts, providers=["CPUExecutionProvider"])
    for src, dst in zip(sys.argv[2::2], sys.argv[3::2]):
        im = Image.open(src).convert("RGBA")
        a = np.asarray(im).astype(np.float32) / 255
        rgb = upscale(sess, a[..., :3])
        alpha = np.asarray(im.getchannel("A").resize((im.width * 4, im.height * 4), Image.LANCZOS))
        out = np.dstack([(rgb * 255 + 0.5).astype(np.uint8), alpha])
        Image.fromarray(out, "RGBA").save(dst, optimize=False, compress_level=6)
        print(dst, flush=True)


if __name__ == "__main__":
    main()
