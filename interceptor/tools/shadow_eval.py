"""Noise, flicker and look of Vela vs the game's built-in FSR 2, from a multi-frame shadow capture.

    python -I tools/shadow_eval.py --dir <captures>/re4slot/s3/vela --cap 0 --out results/img/x.png
        [--sub x,y,w,h] [--frame N]

A shadow capture (VELA_FSR2=shadow, VELA_FSR2_ALGO=both) saves, for N consecutive frames, crops of: the input colour
and motion vectors (render res), FSR 2's exposure, Vela AI / Vela SR (linear, before RCAS), FSR 2's result before
its RCAS (`fsr2`) and after it (`fsr2_rcas`, what the game shows before its post-processing), plus cbRCAS.

RCAS for Vela is applied here exactly as FSR 2 does (ffx_fsr2_rcas.h + ffx_fsr1.h FsrRcasF, MIT): colour x exposure,
reversible tonemap, RCAS with denoise and the game's sharpness (cbRCAS), back. Checked against the game: RCAS of FSR's
own pre-RCAS result here vs the game's fsr2_rcas (printed). Metrics as, tone
space x/(1+x), reference = FSR 2 (no ground truth in a game):
  hf    output minus its 9x9 box mean where FSR 2 is smooth, x1e-4 (grain on flat surfaces)
  wtd   |o_t - o_t-1 warped by the MVs| where FSR 2 agrees with its own warp (< 0.05), x1e-3 (flicker without motion)
  dt    mean |o_t - o_t-1|, x1e-3
  psnr  to FSR 2 (with RCAS for the RCAS row), dB
"""
import argparse
import glob
import json
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import map_coordinates, uniform_filter


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
    if f == 10:
        return raw.view(np.float16)[: w * h * 4].reshape(h, w, 4)[..., :3].astype(np.float32)
    if f in (25, 26):
        u = raw.view(np.uint32)[: w * h].reshape(h, w)
        return np.stack([uf(u, 6), uf(u >> 11, 6), uf(u >> 22, 5)], -1)
    if f == 16:
        return raw.view(np.float32)[: w * h * 2].reshape(h, w, 2)
    if f == 34:
        return raw.view(np.float16)[: w * h * 2].reshape(h, w, 2).astype(np.float32)
    if f == 0:
        return raw
    raise SystemExit(f"{name}: unsupported dxgi {f}")


def tone(x):
    x = np.clip(x, 0, None)
    return x / (1.0 + x)


def rcas(lin, exposure, con):
    """FSR 2's RCAS on a linear frame (HDR path): x exposure, x/(1+max), FsrRcasF with denoise, back, / exposure."""
    c = np.clip(lin * exposure, 0, None)
    m = c.max(-1, keepdims=True)
    t = c / (1.0 + m)
    p = np.pad(t, ((1, 1), (1, 1), (0, 0)), mode="edge")
    b, d, e, f, h = p[:-2, 1:-1], p[1:-1, :-2], p[1:-1, 1:-1], p[1:-1, 2:], p[2:, 1:-1]
    L = lambda x: x[..., 2] * 0.5 + (x[..., 0] * 0.5 + x[..., 1])
    bL, dL, eL, fL, hL = L(b), L(d), L(e), L(f), L(h)
    nz = 0.25 * (bL + dL + fL + hL) - eL
    rng = np.maximum.reduce([bL, dL, eL, fL, hL]) - np.minimum.reduce([bL, dL, eL, fL, hL])
    nz = np.clip(np.abs(nz) / np.maximum(rng, 1e-6), 0, 1)
    nz = -0.5 * nz + 1.0
    mn4 = np.minimum.reduce([b, d, f, h])
    mx4 = np.maximum.reduce([b, d, f, h])
    hitMin = mn4 / np.maximum(4.0 * mx4, 1e-6)
    hitMax = (1.0 - mx4) / np.minimum(4.0 * mn4 - 4.0, -1e-6)
    lobe = np.maximum(-hitMin, hitMax).max(-1)
    lobe = np.maximum(-(0.25 - 1.0 / 16.0), np.minimum(lobe, 0.0)) * con
    lobe = (lobe * nz)[..., None]
    o = (lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0)
    om = np.minimum(o.max(-1, keepdims=True), 0.999)
    return o / (1.0 - om) / exposure


def warp(img, mv, dw, dh):
    """img_t-1 resampled at p + mv * display size (MVs in screen fractions, at render res, upsampled)."""
    H, W = img.shape[:2]
    mvu = np.repeat(np.repeat(mv, int(round(W / mv.shape[1])), 1), int(round(H / mv.shape[0])), 0)[:H, :W]
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    sx = xx + mvu[..., 0] * dw
    sy = yy + mvu[..., 1] * dh
    inside = (sx >= 0) & (sx <= W - 1) & (sy >= 0) & (sy <= H - 1)
    out = np.stack([map_coordinates(img[..., c], [sy, sx], order=1, mode="nearest") for c in range(3)], -1)
    return out, inside


def psnr(a, b):
    m = float(np.mean((tone(a) - tone(b)) ** 2))
    return 99.0 if m == 0 else 10 * np.log10(1 / m)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--cap", type=int, default=0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sub", default="")
    ap.add_argument("--frame", type=int, default=-1)
    a = ap.parse_args()
    metas = sorted(glob.glob(os.path.join(a.dir, f"s{a.cap:02d}_f*.json")))
    if len(metas) < 2:
        raise SystemExit("need >= 2 frames")
    frames = []
    con = None
    for mp in metas:
        m = json.load(open(mp))
        base = mp[:-5]
        im = m["images"]
        fr = {k: load(base, k, im[k]) for k in im if k not in ("cbrcas", "cbfsr2")}
        frames.append((m, fr))
    m0 = frames[0][0]
    dw, dh = m0["display"]
    exp_of = lambda fr: float(fr["exposure"][0, 0, 0]) if "exposure" in fr else 1.0
    # cbRCAS is not always what the capture grabbed (RE4's RCAS binds cbFSR2 first): fit the strength on the game's
    # own RCAS (FSR before -> after) when the frame has both
    _, fl = frames[-1]
    if "fsr2_rcas" in fl:
        fit = max((psnr(rcas(fl["fsr2"], exp_of(fl), c), fl["fsr2_rcas"]), c) for c in np.linspace(0, 1, 21))
        con = float(fit[1])
        print(f"RCAS strength fitted on the game's RCAS: con {con:.2f} ({fit[0]:.1f} dB)")
    if con is None:
        con = 1.0
    print(f"{len(frames)} frames, crop {m0['crop']}, wrap {m0.get('wrap')}")

    names = [("Vela AI", "vela_ai"), ("Vela SR", "vela_sr"), ("FSR 2", "fsr2")]
    names = [n for n in names if n[1] in frames[0][1]]
    # validate offline RCAS against the game's
    _, f0 = frames[-1]
    if "fsr2_rcas" in f0:
        mine = rcas(f0["fsr2"], exp_of(f0), con)
        print(f"offline RCAS check: RCAS(FSR 2) here vs the game's RCAS output: {psnr(mine, f0['fsr2_rcas']):.1f} dB")

    rows = {}
    for label, key in names:
        for rc in (False, True):
            seq = [rcas(fr[key], exp_of(fr), con) if rc else fr[key] for _, fr in frames]
            if rc and key == "fsr2" and "fsr2_rcas" in frames[0][1]:
                seq = [fr["fsr2_rcas"] for _, fr in frames]  # the game's own
            ref = [fr["fsr2_rcas"] if (rc and "fsr2_rcas" in fr) else fr["fsr2"] for _, fr in frames]
            hf, wtd, dt, ps = [], [], [], []
            for t in range(len(seq)):
                o, r = tone(seq[t]), tone(ref[t])
                smooth = np.abs(r - uniform_filter(r, size=(9, 9, 1))).max(-1) < 0.01
                hf.append(np.abs(o - uniform_filter(o, size=(9, 9, 1)))[smooth].mean() * 1e4)
                ps.append(psnr(seq[t], ref[t]))
                if t == 0:
                    continue
                mv = frames[t][1]["mv"]
                wo, ins = warp(seq[t - 1], mv, dw, dh)
                wr, _ = warp(ref[t - 1], mv, dw, dh)
                agree = ins & (np.abs(tone(wr) - r).max(-1) < 0.05)
                wtd.append(np.abs(o - tone(wo)).max(-1)[agree].mean() * 1e3)
                dt.append(np.abs(o - tone(seq[t - 1])).mean() * 1e3)
            rows[(label, rc)] = (np.mean(hf), np.mean(wtd), np.mean(dt), np.mean(ps), seq)
    print(f"{'':22} {'hf x1e-4':>9} {'wtd x1e-3':>10} {'dt x1e-3':>9} {'PSNR vs FSR 2':>13}")
    for (label, rc), v in rows.items():
        print(f"{label + (' + RCAS' if rc else ''):22} {v[0]:9.2f} {v[1]:10.2f} {v[2]:9.2f} {v[3]:13.2f}")

    # picture: one frame, one sub-crop x2, rows without / with RCAS, FSR 2 last
    k = a.frame if a.frame >= 0 else len(frames) - 1
    H, W = frames[0][1]["fsr2"].shape[:2]
    x, y, cw, ch = map(int, a.sub.split(",")) if a.sub else (W // 2 - 160, H // 2 - 90, 320, 180)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 18)
    except OSError:
        font = ImageFont.load_default()

    def tile(img, text):
        c = img[y:y + ch, x:x + cw]
        t8 = (np.clip(np.where(tone(c) <= 0.0031308, 12.92 * tone(c), 1.055 * np.power(tone(c), 1 / 2.4) - 0.055), 0, 1)
              * 255).astype(np.uint8)
        t = Image.fromarray(t8).resize((cw * 2, ch * 2), Image.NEAREST)
        canvas = Image.new("RGB", (t.width, t.height + 26), (18, 18, 18))
        canvas.paste(t, (0, 26))
        ImageDraw.Draw(canvas).text((6, 3), text, fill=(235, 235, 235), font=font)
        return canvas

    lines = []
    for rc in (False, True):
        tiles = []
        for label, key in names:
            v = rows[(label, rc)]
            txt = f"{label}{' + RCAS' if rc else ''}" + (" (reference)" if key == "fsr2" else "") + \
                  f"  hf {v[0]:.1f}  flicker {v[1]:.1f}"
            tiles.append(tile(v[4][k], txt))
        lines.append(tiles)
    gap = 8
    Wt = sum(t.width for t in lines[0]) + gap * (len(lines[0]) - 1)
    Ht = sum(r[0].height for r in lines) + gap
    out = Image.new("RGB", (Wt, Ht), (18, 18, 18))
    yo = 0
    for r in lines:
        xo = 0
        for t in r:
            out.paste(t, (xo, yo))
            xo += t.width + gap
        yo += r[0].height + gap
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    out.save(a.out)
    print(f"wrote {a.out} (frame {k}, sub-crop {x},{y} {cw}x{ch} x2)")


if __name__ == "__main__":
    main()
