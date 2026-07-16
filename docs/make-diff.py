#!/usr/bin/env python3
"""Make a before/after "highlight" diff for the README screenshots.

Produces the overlay the README describes: on a white background,
  RED   = ink the fix REMOVED (dark before, light after)  -> its old position
  GREEN = ink the fix ADDED   (light before, dark after)  -> its new position
  WHITE = unchanged

The two inputs must be the same page captured before and after the fix, aligned
(same crop). If their sizes differ slightly they are cropped to the common
top-left region. A small threshold suppresses anti-aliasing / compression noise.

Usage:
    python3 docs/make-diff.py BEFORE.png AFTER.png OUT-diff.png [--threshold N]

Example (letter-spacing fix):
    python3 docs/make-diff.py \\
        docs/screenshots/letterspacing-broken.png \\
        docs/screenshots/letterspacing-correct.png \\
        docs/highlight/letterspacing-diff.png

Only dependency: Pillow (`pip install Pillow`).
"""
import argparse
from PIL import Image, ImageChops, ImageOps


def main() -> None:
    ap = argparse.ArgumentParser(description="Red=removed / green=added / white=unchanged diff.")
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("out")
    ap.add_argument("--threshold", type=int, default=16,
                    help="ink difference (0-255) below which a pixel counts as unchanged (default 16)")
    args = ap.parse_args()

    a = Image.open(args.before).convert("L")
    b = Image.open(args.after).convert("L")
    if a.size != b.size:
        w, h = min(a.width, b.width), min(a.height, b.height)
        print(f"note: sizes differ {a.size} vs {b.size}; cropping both to {(w, h)} (top-left)")
        a, b = a.crop((0, 0, w, h)), b.crop((0, 0, w, h))

    # ink = darkness (0 = white paper, 255 = black ink)
    ink_a, ink_b = ImageOps.invert(a), ImageOps.invert(b)
    removed = ImageChops.subtract(ink_a, ink_b)  # darker before -> the fix removed ink here
    added = ImageChops.subtract(ink_b, ink_a)    # darker after  -> the fix added ink here

    if args.threshold > 0:
        t = args.threshold
        cut = [0] * t + list(range(t, 256))      # values < t -> 0, else passthrough
        removed, added = removed.point(cut), added.point(cut)

    # white base; removed drains green+blue (-> red), added drains red+blue (-> green)
    r = ImageChops.invert(added)                 # 255 - added
    g = ImageChops.invert(removed)               # 255 - removed
    bl = ImageChops.invert(ImageChops.add(removed, added))
    Image.merge("RGB", (r, g, bl)).save(args.out)

    rpx = sum(removed.histogram()[1:])  # pixels with any removed ink
    gpx = sum(added.histogram()[1:])    # pixels with any added ink
    print(f"wrote {args.out}: {rpx} red (removed) + {gpx} green (added) px changed")


if __name__ == "__main__":
    main()
