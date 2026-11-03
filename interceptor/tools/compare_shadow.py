"""Same-frame comparison from a shadow capture: Vela SR | Vela AI | built-in FSR 2 (reference, last).

    python -I tools/compare_shadow.py --cap <captures>/re4slot/s2/vela/s00.json --out results/img/x.png
        [--crop x,y,w,h] [--zoom 2]

In shadow mode (VELA_FSR2=shadow) FSR 2 runs as the game set it up, and Vela renders the same inputs into its own
textures; a capture saves, from one frame: the input colour, FSR's exposure, Vela AI / Vela SR results and FSR's own
upscaled history (plain linear colour: FSR multiplies its input by the exposure and divides the history back). All
linear; shown with the same
x/(1+x) tonemap + sRGB, before RE4's post-processing and before FSR's RCAS sharpening (none of the three has it).
Also prints PSNR of each Vela panel against FSR inside the crop (only to say how close, not which is better).
"""
import argparse
import json
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def uf(bits, mant):
    e = (bits >> mant) & 0x1F
    m = (bits & ((1 << mant) - 1)).astype(np.float32)
    norm = np.ldexp(1.0 + m / (1 << mant), e.astype(np.int32) - 15)
    den = np.ldexp(m / (1 << mant), -14)
    return np.where(e == 0, den, np.where(e == 31, 0.0, norm)).astype(np.float32)


def load(base, name, info):
    w, h = info["size"]
    raw = np.fromfile(f"{base}_{name}.bin", np.uint8)
    f = info["dxgi"]
    if f in (10,):
        return raw.view(np.float16)[: w * h * 4].reshape(h, w, 4)[..., :3].astype(np.float32)
    if f in (25, 26):
        u = raw.view(np.uint32)[: w * h].reshape(h, w)
        return np.stack([uf(u, 6), uf(u >> 11, 6), uf(u >> 22, 5)], -1)
    if f == 16:
        return raw.view(np.float32)[:2]
    raise SystemExit(f"{name}: unsupported dxgi {f}")


def tm(x):
    x = np.clip(x, 0, None)
    y = x / (1.0 + x)
    return (np.clip(np.where(y <= 0.0031308, 12.92 * y, 1.055 * np.power(y, 1 / 2.4) - 0.055), 0, 1) * 255).astype(np.uint8)


def psnr(a, b):
    m = float(np.mean((tm(a).astype(np.float32) / 255 - tm(b).astype(np.float32) / 255) ** 2))
    return 99.0 if m == 0 else 10 * np.log10(1 / m)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cap", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--crop", default="")
    ap.add_argument("--zoom", type=int, default=2)
    a = ap.parse_args()
    m = json.load(open(a.cap))
    base = a.cap[:-5]
    im = m["images"]
    exp = load(base, "exposure", im["exposure"])
    panels = []
    if "vela_sr" in im:
        panels.append(("Vela SR", load(base, "vela_sr", im["vela_sr"])))
    if "vela_ai" in im:
        panels.append(("Vela AI", load(base, "vela_ai", im["vela_ai"])))
    fsr = load(base, "fsr2_history", im["fsr2_history"])  # FSR history = plain linear colour
    panels.append(("built-in FSR 2 (reference)", fsr))
    h, w = fsr.shape[:2]
    x, y, cw, ch = map(int, a.crop.split(",")) if a.crop else (w // 2 - 240, h // 2 - 135, 480, 270)
    tiles = []
    for name, img in panels:
        c = img[y:y + ch, x:x + cw]
        t = Image.fromarray(tm(c)).resize((cw * a.zoom, ch * a.zoom), Image.NEAREST)
        canvas = Image.new("RGB", (t.width, t.height + 26), (18, 18, 18))
        canvas.paste(t, (0, 26))
        label = name if name.startswith("built-in") else f"{name}  (PSNR vs FSR 2: {psnr(c, fsr[y:y + ch, x:x + cw]):.1f} dB)"
        try:
            font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 18)  # Cyrillic labels
        except OSError:
            font = ImageFont.load_default()
        ImageDraw.Draw(canvas).text((6, 3), label, fill=(235, 235, 235), font=font)
        tiles.append(canvas)
    gap = 8
    W = sum(t.width for t in tiles) + gap * (len(tiles) - 1)
    out = Image.new("RGB", (W, tiles[0].height), (18, 18, 18))
    xo = 0
    for t in tiles:
        out.paste(t, (xo, 0))
        xo += t.width + gap
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    out.save(a.out)
    print(f"wrote {a.out}: crop {x},{y} {cw}x{ch} x{a.zoom}, exposure {float(exp[0]):.4f}, panels {[p[0] for p in panels]}")


if __name__ == "__main__":
    main()
