# Default border for the PS5 port: dark, a red halftone growing toward the screen edge, and a red and gold rule
# next to the game's 4:3 picture. Original art; 3840x2160 with the middle 2880 px transparent.
import numpy as np
from PIL import Image
import sys

W, H = 3840, 2160
SIDE = 480                      # (3840 - 2160*4/3) / 2
out = sys.argv[1]

def strip():
    y, x = np.mgrid[0:H, 0:SIDE].astype(np.float32)
    # x = 0 at the screen edge, SIDE at the picture's edge
    t = x / SIDE                                    # 0 outer .. 1 inner
    v = (y - H / 2) / (H / 2)                       # -1 top .. 1 bottom
    base = np.array([16, 14, 18], np.float32)
    lift = np.array([10, 8, 11], np.float32)
    col = base + lift * (1 - v * v)[..., None] * (0.55 + 0.45 * t)[..., None]
    # halftone on a 45-degree grid, dots bigger toward the outer edge and toward top and bottom
    s = 22.0
    u = (x + y) / np.sqrt(2) / s
    w = (x - y) / np.sqrt(2) / s
    du = (u - np.round(u)) * s
    dw = (w - np.round(w)) * s
    d = np.sqrt(du * du + dw * dw)
    r = 1.0 + 7.0 * (1 - t) ** 1.6 * (0.75 + 0.25 * np.abs(v))
    a = np.clip(r - d + 0.5, 0, 1) * 0.85
    red = np.array([62, 12, 18], np.float32)
    col = col * (1 - a[..., None]) + red * a[..., None]
    # top and bottom darker
    col *= (1 - 0.35 * np.abs(v) ** 3)[..., None]
    # the rules next to the picture: red 4 px, a gap, gold 2 px
    def rule(x0, x1, c, k=1.0):
        m = np.clip(np.minimum(x - x0 + 1, x1 - x), 0, 1)[..., None] * k
        return col * (1 - m) + np.array(c, np.float32) * m
    col = rule(SIDE - 30, SIDE - 26, [200, 16, 46])
    col = rule(SIDE - 20, SIDE - 18, [150, 120, 66], 0.9)
    # a soft shadow falling away from the picture
    sh = np.clip(1 - (SIDE - x) / 60.0, 0, 1) ** 2 * 0.6
    col *= (1 - sh)[..., None]
    return np.clip(col + 0.5, 0, 255).astype(np.uint8)

left = strip()
img = np.zeros((H, W, 4), np.uint8)
img[:, :SIDE, :3] = left
img[:, :SIDE, 3] = 255
img[:, W - SIDE:, :3] = left[:, ::-1]
img[:, W - SIDE:, 3] = 255
Image.fromarray(img, "RGBA").save(out, optimize=True)
