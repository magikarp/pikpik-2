#!/usr/bin/env python3
"""Generate the application icon.

The icon shipped with this project is an original work, produced entirely by
this script — no traced, sampled, or derived artwork, and nothing lifted from
the game. It is deliberately generic: a sprout glyph on a gradient, drawn from
primitives. Regenerate it with

    python3 release/assets/make_icon.py release/assets/icon.png

and diff the result if you ever need to show it came from here.

Requires Pillow.
"""

import sys
from PIL import Image, ImageDraw, ImageFilter

SIZE = 1024
SS = 4  # supersampling factor; the whole thing is drawn at 4x and downsampled

# Flat, unbranded palette. Nothing here is sampled from anything.
TOP = (30, 58, 88)
BOTTOM = (12, 24, 42)
LEAF = (140, 196, 240)
LEAF_DARK = (92, 146, 200)
STEM = (232, 244, 233)


def gradient(size: int) -> Image.Image:
    """Vertical two-stop gradient, drawn a row at a time."""
    img = Image.new("RGB", (1, size))
    px = img.load()
    for y in range(size):
        t = y / (size - 1)
        px[0, y] = tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM))
    return img.resize((size, size), Image.NEAREST)


def rounded_mask(size: int, radius_frac: float = 0.2237) -> Image.Image:
    """Squircle-ish mask. 0.2237 is the macOS/iOS corner-radius ratio."""
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size - 1, size - 1), radius=round(size * radius_frac), fill=255
    )
    return mask


def leaf(draw: ImageDraw.ImageDraw, cx: int, cy: int, w: int, h: int,
         mirror: bool, fill) -> None:
    """One leaf: a lens shape built from two arcs, drawn as a polygon."""
    pts_top, pts_bottom = [], []
    steps = 64
    for i in range(steps + 1):
        t = i / steps
        x = t * w
        # two parabolic edges meeting at the tip give the lens silhouette
        y_up = -h * (1 - (2 * t - 1) ** 2) * 0.5
        y_dn = h * (1 - (2 * t - 1) ** 2) * 0.16
        sx = cx - x if mirror else cx + x
        pts_top.append((sx, cy + y_up))
        pts_bottom.append((sx, cy + y_dn))
    draw.polygon(pts_top + pts_bottom[::-1], fill=fill)


def build(size: int) -> Image.Image:
    s = size * SS
    img = gradient(s)
    d = ImageDraw.Draw(img, "RGBA")

    cx, base_y = s // 2, int(s * 0.74)
    stem_w = int(s * 0.055)
    stem_top = int(s * 0.40)

    # stem: a capsule from the base up to where the leaves meet
    d.rounded_rectangle(
        (cx - stem_w // 2, stem_top, cx + stem_w // 2, base_y),
        radius=stem_w // 2, fill=STEM,
    )
    # seed: the stem's root
    r = int(s * 0.052)
    d.ellipse((cx - r, base_y - r, cx + r, base_y + r), fill=STEM)

    # two leaves, the far one darker so the pair reads at 16px
    lw, lh = int(s * 0.235), int(s * 0.30)
    leaf(d, cx, stem_top + int(s * 0.045), lw, lh, mirror=True, fill=LEAF_DARK)
    leaf(d, cx, stem_top + int(s * 0.045), lw, lh, mirror=False, fill=LEAF)

    img = img.convert("RGBA")
    img.putalpha(rounded_mask(s))
    img = img.resize((size, size), Image.LANCZOS)
    return img.filter(ImageFilter.SMOOTH)


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "icon.png"
    build(SIZE).save(out)
    print(f"wrote {out} ({SIZE}x{SIZE})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
