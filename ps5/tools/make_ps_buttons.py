#!/usr/bin/env python3
"""Draw the PlayStation button pictures for the PS5 port's "playstation" texture pack.

    python3 ps5/tools/make_ps_buttons.py OUT_DIR

Every picture is drawn here from numbers in ps_buttons.json (beside this
script): which game texture it replaces (its pack id), its size, where the
button sits in it and where its callout bars run. No game pixels are read or
copied. The output is OUT_DIR/manifest.ini and OUT_DIR/textures/<id>.png, at
four times the original size.

Mapping, by position on the pad (the DualSense is read the same way):
A -> Cross, B -> Circle, X -> Square, Y -> Triangle, left trigger -> L2,
right trigger -> R2. Style: white button, black symbol, thin dark edge.
"""
import json
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SCALE = 4          # output size against the original
SS = 4             # supersampling while drawing
INK = (18, 18, 18, 255)
EDGE = (20, 20, 20, 255)

FONTS = ("/usr/share/fonts/opentype/inter/Inter-Bold.otf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
         "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf")


def font(px):
    for path in FONTS:
        if os.path.exists(path):
            return ImageFont.truetype(path, px)
    return ImageFont.load_default()


def white_fill(size, box, rounded=None):
    """A white shape with a soft top-to-bottom shade, as an RGBA layer."""
    w, h = size
    shade = Image.new("L", (1, 256))
    for y in range(256):
        shade.putpixel((0, y), 255 - int(28 * y / 255))
    x0, y0, x1, y1 = box
    grad = shade.resize((max(1, int(x1 - x0)), max(1, int(y1 - y0))))
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    color = Image.merge("RGBA", (grad, grad, grad, Image.new("L", grad.size, 255)))
    mask = Image.new("L", size, 0)
    md = ImageDraw.Draw(mask)
    if rounded is None:
        md.ellipse(box, fill=255)
    else:
        md.rounded_rectangle(box, radius=rounded, fill=255)
    layer.paste(color, (int(x0), int(y0)))
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.paste(layer, (0, 0), mask)
    return out


def line(d, a, b, width):
    d.line([a, b], fill=INK, width=int(round(width)))
    r = width / 2
    for p in (a, b):
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=INK)


def symbol(d, kind, cx, cy, R):
    sw = 0.16 * R
    if kind == "A":                                      # Cross
        k = 0.39 * R
        line(d, (cx - k, cy - k), (cx + k, cy + k), sw)
        line(d, (cx - k, cy + k), (cx + k, cy - k), sw)
    elif kind == "B":                                    # Circle
        r = 0.47 * R
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=INK, width=int(round(sw)))
    elif kind == "X":                                    # Square
        h = 0.41 * R
        d.rectangle([cx - h, cy - h, cx + h, cy + h], outline=INK, width=int(round(sw)))
    elif kind == "Y":                                    # Triangle
        r = 0.52 * R
        oy = cy + 0.09 * R
        pts = [(cx + r * math.cos(math.radians(a)), oy + r * math.sin(math.radians(a))) for a in (-90, 30, 150)]
        for i in range(3):
            line(d, pts[i], pts[(i + 1) % 3], sw)


def bar(d, box, disc, gray, f):
    """A callout bar from the button outwards, light or dark like the original's."""
    x0, y0, x1, y1 = box
    cx, cy, r = disc
    beside = y0 < cy + r * 0.5 and y1 > cy - r * 0.5      # level with the button: it runs sideways
    if beside:                                           # run it in to the button
        if x0 > cx:
            x0 = cx
        else:
            x1 = cx
    else:
        if y0 > cy:
            y0 = cy
        else:
            y1 = cy
    body = (240, 240, 240, 255) if gray > 0.5 else (34, 34, 34, 255)
    rim = (150, 150, 150, 255) if gray > 0.5 else (95, 95, 95, 255)
    b = [x0 * f, y0 * f, x1 * f, y1 * f]
    thick = min(b[2] - b[0], b[3] - b[1])
    d.rounded_rectangle(b, radius=thick * 0.25, fill=rim)
    inset = thick * 0.18
    d.rounded_rectangle([b[0] + inset, b[1] + inset, b[2] - inset, b[3] - inset], radius=thick * 0.2, fill=body)


def face(e):
    f = SCALE * SS
    size = (e["w"] * f, e["h"] * f)
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for b in e["bars"]:
        bar(d, b[:4], e["disc"], b[4], f)
    cx, cy, r = e["disc"]
    R = (r + 0.7) * f
    cx *= f
    cy *= f
    if e["ring"] == "light":                             # the original's highlighted look: a light outer ring
        d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=(214, 214, 214, 255))
        R *= 0.88
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=EDGE)
    inner = R * 0.88
    img.alpha_composite(white_fill(size, [cx - inner, cy - inner, cx + inner, cy + inner]))
    symbol(ImageDraw.Draw(img), e["button"], cx, cy, inner)
    return img


def shoulder(e):
    f = SCALE * SS
    size = (e["w"] * f, e["h"] * f)
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1 = [v * f for v in e["box"]]
    h = y1 - y0
    d.rounded_rectangle([x0, y0, x1, y1], radius=h * 0.24, fill=EDGE)
    t = h * 0.08
    img.alpha_composite(white_fill(size, [x0 + t, y0 + t, x1 - t, y1 - t], rounded=h * 0.19))
    d = ImageDraw.Draw(img)
    ft = font(int(h * 0.56))
    l, top, r, bottom = d.textbbox((0, 0), e["label"], font=ft)
    d.text(((x0 + x1) / 2 - (l + r) / 2, (y0 + y1) / 2 - (top + bottom) / 2), e["label"], font=ft, fill=INK)
    return img


def rotate(e):
    """The wardrobe's turn-the-fighter icon: a curved arrow and an L2/R2 label (red edge when lit)."""
    f = SCALE * SS
    size = (e["w"] * f, e["h"] * f)
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # The arrow, drawn for the left icon and mirrored for the right one.
    cx, cy, r, t, o = 17 * f, 21 * f, 10.5 * f, 6 * f, 1.2 * f
    a0, a1 = 88, 318
    outline, body = EDGE, (205, 205, 205, 255)
    # PIL draws an arc's width inwards from its box: the box is the outer edge.
    for width, edge_r, colour, ends in ((t + 2 * o, r + t / 2 + o, outline, 4), (t, r + t / 2, body, 0)):
        d.arc([cx - edge_r, cy - edge_r, cx + edge_r, cy + edge_r], a0 - ends, a1, fill=colour, width=int(width))
    end = math.radians(a1)
    ex, ey = cx + r * math.cos(end), cy + r * math.sin(end)
    tx, ty = -math.sin(end), math.cos(end)                 # direction of travel at the end
    nx, ny = math.cos(end), math.sin(end)
    head, half = 7 * f, 6.5 * f
    tip = (ex + tx * head, ey + ty * head)
    left = (ex + nx * (half + t * 0.15), ey + ny * (half + t * 0.15))
    right = (ex - nx * (half + t * 0.15), ey - ny * (half + t * 0.15))
    d.polygon([tip, left, right], fill=outline)
    shrink = 1.3 * f
    def towards(p, q, k):
        return (p[0] + (q[0] - p[0]) * k, p[1] + (q[1] - p[1]) * k)
    centre = ((tip[0] + left[0] + right[0]) / 3, (tip[1] + left[1] + right[1]) / 3)
    k = 1 - shrink / max(1.0, math.dist(tip, centre))
    d.polygon([towards(centre, tip, k), towards(centre, left, k), towards(centre, right, k)], fill=body)
    if e.get("mirror"):
        # Mirrored about the middle of the two labels (x 28 and x 11), not the picture's middle.
        flipped = img.transpose(Image.FLIP_LEFT_RIGHT)
        img = Image.new("RGBA", size, (0, 0, 0, 0))
        img.paste(flipped, (-(e["w"] - 39) * f, 0))
    # The label.
    x0, y0, x1, y1 = [v * f for v in e["box"]]
    h = y1 - y0
    d = ImageDraw.Draw(img)
    edge = (205, 22, 40, 255) if e.get("red") else EDGE
    d.rounded_rectangle([x0, y0, x1, y1], radius=h * 0.24, fill=edge)
    tt = h * (0.14 if e.get("red") else 0.09)
    img.alpha_composite(white_fill(size, [x0 + tt, y0 + tt, x1 - tt, y1 - tt], rounded=h * 0.18))
    d = ImageDraw.Draw(img)
    ft = font(int(h * 0.58))
    l, top, rr, bottom = d.textbbox((0, 0), e["label"], font=ft)
    d.text(((x0 + x1) / 2 - (l + rr) / 2, (y0 + y1) / 2 - (top + bottom) / 2), e["label"], font=ft, fill=INK)
    return img


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    spec = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ps_buttons.json")))
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)
    lines = ["[pack]", "schema=1", "name=PlayStation buttons", "[textures]"]
    for e in spec:
        img = face(e) if e["kind"] == "face" else rotate(e) if e["kind"] == "rotate" else shoulder(e)
        img = img.resize((e["w"] * SCALE, e["h"] * SCALE), Image.LANCZOS)
        img.save(os.path.join(out, "textures", e["id"] + ".png"), optimize=True)
        lines.append("%s=textures/%s.png" % (e["id"], e["id"]))
    with open(os.path.join(out, "manifest.ini"), "w") as m:
        m.write("\n".join(lines) + "\n")
    print("%d pictures in %s" % (len(spec), out))


if __name__ == "__main__":
    main()
