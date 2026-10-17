#!/usr/bin/env python3
"""Make a 4x HD texture pack of the menus and HUD from the player's own game files.

    python3 ps5/tools/hd_pack.py WEIGHTS.pth GAMEDATA OUT_DIR [--skip ID_LIST.json ...]

GAMEDATA is the extracted disc (the folder with default.xbe). Every texture in
screens/screens.viv and assets/misc.viv is decoded (scripts/hd_assets.py),
given colour under its transparent pixels so no dark fringe appears, enlarged
4x by Real-ESRGAN's compact model (hd_upscale.py) or, below 32 pixels, by
Lanczos, and written to OUT_DIR/textures/<id>.png with OUT_DIR/manifest.ini.
Pictures already there are kept, so a stopped run carries on. Textures named
in a --skip list (the PlayStation buttons, the owner's own art) are left out.
Order: HUD and pause, then the front end, then everything else, shop
thumbnails last. Local use only: the output is made from game data.
"""
import json
import os
import struct
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts"))
import hd_assets as hd          # noqa: E402
import hd_upscale as up         # noqa: E402

ARCHIVES = ("assets/misc.viv", "screens/screens.viv")
FIRST = ("misc.viv", "intMain", "_art", "memoryCard", "options", "pageHead", "xHead", "loader", "main")
LAST = ("shopThumbs",)


def priority(label):
    for i, key in enumerate(FIRST):
        if key in label:
            return i
    for key in LAST:
        if key in label:
            return 100
    return 50


def bleed(rgba):
    """Colour for fully transparent pixels from their nearest visible neighbour."""
    from scipy import ndimage
    alpha = rgba[..., 3]
    hidden = alpha < 8
    if not hidden.any() or hidden.all():
        return rgba
    _, (iy, ix) = ndimage.distance_transform_edt(hidden, return_indices=True)
    out = rgba.copy()
    out[..., :3] = rgba[iy, ix, :3]
    return out


def main():
    args = sys.argv[1:]
    skip = set()
    while "--skip" in args:
        i = args.index("--skip")
        data = json.load(open(args[i + 1]))
        skip |= {e["id"] if isinstance(e, dict) else e for e in data}
        del args[i:i + 2]
    weights, game, out = args
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)

    # Every texture once, with where it lives.
    work = {}
    for arch in ARCHIVES:
        blob = open(os.path.join(game, arch), "rb").read()
        for label, data in hd.walk(blob, arch):
            if data[:4] != b"SHPX":
                continue
            for rec, _ in hd.shpx_images(data, decode=False):
                sid = rec.get("source_id")
                if not sid or sid in skip or sid in work:
                    continue
                work[sid] = (priority(label), label, rec["index"], rec["width"], rec["height"])
    order = sorted(work, key=lambda s: (work[s][0], work[s][1]))
    print("%d textures" % len(order), flush=True)

    import onnxruntime as ort
    opts = ort.SessionOptions()
    sess = ort.InferenceSession(up.build_model(up.load_state(weights)), opts, providers=["CPUExecutionProvider"])

    blobs = {}
    done = 0
    t0 = time.time()
    for n, sid in enumerate(order):
        dst = os.path.join(out, "textures", sid + ".png")
        if os.path.exists(dst):
            done += 1
            continue
        _, label, index, w, h = work[sid]
        arch, _, rest = label.partition("::")
        if arch not in blobs:
            blobs.clear()
            blobs[arch] = {lab: d for lab, d in hd.walk(open(os.path.join(game, arch), "rb").read(), arch)}
        data = blobs[arch].get(label)
        img = None
        for rec, im in hd.shpx_images(data, indices={index}):
            if rec.get("source_id") == sid:
                img = im
        if img is None:
            continue
        a = bleed(np.asarray(img.convert("RGBA")))
        if min(w, h) < 32:
            big = Image.fromarray(a, "RGBA").resize((w * 4, h * 4), Image.LANCZOS)
        else:
            rgb = up.upscale(sess, a[..., :3].astype(np.float32) / 255)
            alpha = np.asarray(Image.fromarray(a[..., 3]).resize((w * 4, h * 4), Image.LANCZOS))
            big = Image.fromarray(np.dstack([(rgb * 255 + 0.5).astype(np.uint8), alpha]), "RGBA")
        big.save(dst + ".tmp", "PNG", compress_level=6)
        os.replace(dst + ".tmp", dst)
        done += 1
        if done % 200 == 0:
            print("%d/%d (%.0f s)" % (done, len(order), time.time() - t0), flush=True)

    with open(os.path.join(out, "manifest.ini"), "w") as m:
        m.write("[pack]\nschema=1\nname=HD menus and HUD\n[textures]\n")
        for sid in order:
            if os.path.exists(os.path.join(out, "textures", sid + ".png")):
                m.write("%s=textures/%s.png\n" % (sid, sid))
    print("done: %d pictures" % done, flush=True)


if __name__ == "__main__":
    main()
