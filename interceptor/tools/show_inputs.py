"""One recon frame of RE4's built-in FSR 2 inputs, side by side.

    python -I tools/show_inputs.py --dump <captures>/fsr2dump --frame N --out results/img/re4_inputs.png

Reads the bench-format dump the recon DLL wrote (fNNNNNN.json + fNNNNNN_<name>.bin) and lays out, left to
right: input colour (render res), depth, motion vectors, and the game's own FSR output (display res) --
the reference, shown last. One frame, one crop (whole image), labelled. Colour/depth/mv decoders match (DXGI formats).
"""
import argparse
import glob
import json
import os

import numpy as np
from PIL import Image, ImageDraw


def read_bin(path, info):
    w, h = info["size"]
    raw = np.fromfile(path, dtype=np.uint8)
    n = w * h * info["texel_bytes"]
    return raw[:n], w, h


def _srgb(c):
    return np.where(c <= 0.0031308, 12.92 * c, 1.055 * np.power(np.clip(c, 0, None), 1 / 2.4) - 0.055)


def _lin_from_srgb(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _uf(bits, mant):
    """Unsigned small float channel (R11G11B10): 5-bit exponent, bias 15."""
    e = (bits >> mant) & 0x1F
    m = (bits & ((1 << mant) - 1)).astype(np.float32)
    norm = np.ldexp(1.0 + m / (1 << mant), e.astype(np.int32) - 15)
    den = np.ldexp(m / (1 << mant), -14)
    return np.where(e == 0, den, np.where(e == 31, 0.0, norm)).astype(np.float32)


def decode_color(raw, w, h, dxgi):
    if dxgi in (9, 10):
        return raw.view(np.float16).reshape(h, w, 4)[..., :3].astype(np.float32)
    if dxgi in (1, 2):
        return raw.view(np.float32).reshape(h, w, 4)[..., :3].copy()
    u = raw.view(np.uint32).reshape(h, w)
    if dxgi in (25, 26):  # R11G11B10_FLOAT (RE4's colour and output)
        return np.stack([_uf(u, 6), _uf(u >> 11, 6), _uf(u >> 22, 5)], -1)
    if dxgi in (23, 24):  # R10G10B10A2_UNORM
        return np.stack([(u >> s) & 0x3FF for s in (0, 10, 20)], -1).astype(np.float32) / 1023.0
    b = raw.reshape(h, w, 4).astype(np.float32) / 255.0
    if dxgi in (27, 28):
        return b[..., :3]
    if dxgi == 29:
        return _lin_from_srgb(b[..., :3])
    if dxgi in (87, 90):
        return b[..., 2::-1].copy()
    if dxgi == 91:
        return _lin_from_srgb(b[..., 2::-1])
    return b[..., :3]


def tm_to_img(rgb):
    return (np.clip(_srgb(rgb / (1.0 + rgb)), 0, 1) * 255).astype(np.uint8)


def decode_depth(raw, w, h, info):
    f, t = info["dxgi"], info["texel_bytes"]
    if 44 <= f <= 47 or 44 <= info.get("plane_dxgi", f) <= 47:
        d = (raw.view(np.uint32).reshape(h, w) & 0xFFFFFF).astype(np.float32) / 16777215.0
    elif t == 2:
        d = raw.view(np.uint16).reshape(h, w).astype(np.float32) / 65535.0
    else:
        d = raw.view(np.float32).reshape(h, w)
    d = np.clip(d, 0, 1)
    g = (np.power(d, 0.25) * 255).astype(np.uint8)  # gamma to see near-plane detail
    return np.stack([g, g, g], -1)


def decode_mv(raw, w, h, info):
    v = raw.view(np.float16 if info["texel_bytes"] == 4 else np.float32).reshape(h, w, 2).astype(np.float32)
    mag = np.sqrt((v ** 2).sum(-1))
    s = np.percentile(mag, 99) or 1.0
    r = np.clip(0.5 + v[..., 0] / (2 * s), 0, 1)
    g = np.clip(0.5 + v[..., 1] / (2 * s), 0, 1)
    return (np.stack([r, g, np.full_like(r, 0.5)], -1) * 255).astype(np.uint8)


def panel(img, label, H):
    h, w = img.shape[:2]
    W = max(1, round(w * H / h))
    im = Image.fromarray(img).resize((W, H), Image.NEAREST if label.startswith(("depth", "mv")) else Image.BILINEAR)
    out = Image.new("RGB", (W, H + 22), (18, 18, 18))
    out.paste(im, (0, 22))
    ImageDraw.Draw(out).text((4, 5), label, fill=(235, 235, 235))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True)
    ap.add_argument("--frame", type=int, default=-1)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    metas = sorted(glob.glob(os.path.join(a.dump, "f*.json")))
    if not metas:
        raise SystemExit(f"no dumps in {a.dump}")
    mp = metas[-1] if a.frame < 0 else os.path.join(a.dump, f"f{a.frame:06d}.json")
    m = json.load(open(mp))
    base = mp[:-5]
    rw, rh = m["render"]
    dw, dh = m["display"]
    panels = []

    def load(name):
        info = m["images"].get(name)
        p = base + f"_{name}.bin"
        if not info or not os.path.exists(p):
            return None, None
        raw, w, h = read_bin(p, info)
        return raw, (w, h, info)

    raw, ci = load("color")
    if raw is not None:
        w, h, info = ci
        panels.append(panel(tm_to_img(decode_color(raw, w, h, info["dxgi"])), f"color {w}x{h}", 360))
    raw, di = load("depth")
    if raw is not None:
        w, h, info = di
        panels.append(panel(decode_depth(raw, w, h, info), f"depth {w}x{h}", 360))
    raw, vi = load("mv")
    if raw is not None:
        w, h, info = vi
        panels.append(panel(decode_mv(raw, w, h, info), f"motion {w}x{h}", 360))
    # any extra SRVs
    for name in sorted(m["images"]):
        if name in ("color", "depth", "mv", "output"):
            continue
        raw, ii = load(name)
        if raw is not None:
            w, h, info = ii
            panels.append(panel(tm_to_img(decode_color(raw, w, h, info["dxgi"])), f"{name} {w}x{h}", 360))
    # reference LAST: the game's own FSR output
    raw, oi = load("output")
    if raw is not None:
        w, h, info = oi
        panels.append(panel(tm_to_img(decode_color(raw, w, h, info["dxgi"])), f"built-in FSR out {w}x{h} (ref)", 360))

    if not panels:
        raise SystemExit("nothing decoded")
    gap = 8
    W = sum(p.width for p in panels) + gap * (len(panels) - 1)
    H = max(p.height for p in panels)
    canvas = Image.new("RGB", (W, H), (18, 18, 18))
    x = 0
    for p in panels:
        canvas.paste(p, (x, 0))
        x += p.width + gap
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    canvas.save(a.out)
    print(f"wrote {a.out}  ({W}x{H}, {len(panels)} panels, render {rw}x{rh} display {dw}x{dh})")


if __name__ == "__main__":
    main()
