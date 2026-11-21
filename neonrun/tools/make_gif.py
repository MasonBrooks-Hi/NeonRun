#!/usr/bin/env python3
"""Stitch the PNG frames produced by `neonrun --replay` into a looping GIF.

  python3 tools/make_gif.py <frames_dir> <out.gif> [--every N] [--width W] [--fps F]

Frames are decimated, downscaled and quantised to a shared 200-colour palette so
the file stays small without the neon hues banding.
"""
import argparse
import os
import sys

from PIL import Image


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("frames_dir")
    ap.add_argument("out", nargs="?", default="neonrun.gif")
    ap.add_argument("--every", type=int, default=2, help="keep one frame out of N")
    ap.add_argument("--width", type=int, default=480)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--colors", type=int, default=200)
    args = ap.parse_args()

    names = sorted(
        n for n in os.listdir(args.frames_dir)
        if n.startswith("f") and n.endswith(".png")
    )
    if not names:
        print(f"no fNNNN.png frames in {args.frames_dir}", file=sys.stderr)
        return 1

    paths = [os.path.join(args.frames_dir, n) for n in names][:: max(1, args.every)]
    if not paths:
        print("nothing left after decimation", file=sys.stderr)
        return 1

    first = Image.open(paths[0]).convert("RGB")
    scale = args.width / first.width
    size = (args.width, max(1, round(first.height * scale)))

    frames = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        if im.size != size:
            im = im.resize(size, Image.LANCZOS)
        frames.append(im)

    # One palette for the whole animation, taken from a mid-run frame that
    # contains the track, the neon rails and the HUD text.
    probe = frames[len(frames) // 2].quantize(colors=args.colors, method=Image.MEDIANCUT)
    quantised = [f.quantize(palette=probe, dither=Image.FLOYDSTEINBERG) for f in frames]

    duration = max(1, round(1000.0 / args.fps))
    quantised[0].save(
        args.out,
        save_all=True,
        append_images=quantised[1:],
        duration=duration,
        loop=0,
        optimize=True,
        disposal=1,
    )
    mb = os.path.getsize(args.out) / 1024.0 / 1024.0
    print(f"{args.out}: {len(quantised)} frames  {size[0]}x{size[1]}  "
          f"{duration}ms/frame  {len(quantised) * duration / 1000.0:.1f}s  {mb:.2f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
