"""Pick, from screenshot bursts of different runs, the moments where the (looping) title camera is at the same place.

    python -I tools/match_bursts.py --ref DIR_A --others DIR_B,DIR_C --crop x,y,w,h --out OUT.png --labels "A|B|C"

For every shot of the reference burst, finds the closest shot (grey 1/8 thumbnails, mean abs diff) in each other
burst; takes the reference shot with the best worst-case match; writes the crop of the matched shots side by side
(reference last, as passed: --labels follows ref, others order but the picture puts the ref last).
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def thumb(p):
    return np.asarray(Image.open(p).convert("L").resize((240, 135)), np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--others", required=True)
    ap.add_argument("--crop", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--labels", required=True)
    a = ap.parse_args()
    refs = sorted(glob.glob(os.path.join(a.ref, "*.png")))
    others = [sorted(glob.glob(os.path.join(d, "*.png"))) for d in a.others.split(",")]
    rt = [thumb(p) for p in refs]
    ot = [[thumb(p) for p in o] for o in others]
    best = None
    for i, r in enumerate(rt):
        picks, worst = [], 0
        for o in ot:
            d = [float(np.abs(r - t).mean()) for t in o]
            j = int(np.argmin(d))
            picks.append(j)
            worst = max(worst, d[j])
        if best is None or worst < best[0]:
            best = (worst, i, picks)
    worst, i, picks = best
    print(f"ref shot {os.path.basename(refs[i])}, matches {[os.path.basename(others[k][j]) for k, j in enumerate(picks)]}, "
          f"worst thumb diff {worst:.2f}/255")
    x, y, w, h = map(int, a.crop.split(","))
    labels = a.labels.split("|")
    paths = [others[k][j] for k, j in enumerate(picks)] + [refs[i]]
    names = labels[1:] + labels[:1]
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    tiles = []
    for p, n in zip(paths, names):
        im = Image.open(p).convert("RGB").crop((x, y, x + w, y + h))
        c = Image.new("RGB", (w, h + 26), (18, 18, 18))
        c.paste(im, (0, 26))
        ImageDraw.Draw(c).text((6, 3), n, fill=(235, 235, 235), font=font)
        tiles.append(c)
    W = sum(t.width for t in tiles) + 8 * (len(tiles) - 1)
    out = Image.new("RGB", (W, tiles[0].height), (18, 18, 18))
    xo = 0
    for t in tiles:
        out.paste(t, (xo, 0))
        xo += t.width + 8
    out.save(a.out)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
