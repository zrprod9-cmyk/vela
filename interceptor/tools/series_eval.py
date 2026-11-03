"""Motion analysis of a recorded series (the recorder in vela_fsr2.cpp / the fork's dumper format).

    python -I tools/series_eval.py --series DIR [--out PNG] [--replay NAME=DIR:TAG ...] [--frame N]

1) MV check (render res, whole frames, every consecutive pair): the previous colour is moved by the motion vectors in
   each candidate interpretation and compared with the current colour (tone x/(1+x), mean abs over valid pixels):
   scale x1 / x0.5 / x2 of the json's mv_scale, sign + / -, jitter compensation on / off, and no motion as baseline.
   Convention checked (FSR 2 and our contract): a pixel p of frame t was at p + mv * mv_scale (render px) in t-1;
   pixel p shows the scene at p - jitter, so the previous colour is sampled at p + mv*s + (j_{t-1} - j_t) with
   compensation. The interpretation the game uses gives the smallest error; on a camera move the gap is large.
2) Motion size per frame (median / p95 |mv| in render px) -- at ~20 FPS three times a 60 FPS step.
3) Results in motion (display crop, origin from the json): FSR 2 (the game's, reference), Vela AI as the game ran it,
   and replays (--replay NAME=DIR:TAG, sgsr_replay outputs fNNNNNN_replay_TAG.bin, display size, cropped here):
   hf (output minus 9x9 mean where FSR 2 is smooth, x1e-4), wtd (|o_t - o_t-1 moved by MV| where FSR 2 agrees with its
   own move, x1e-3), PSNR to FSR 2. Picture: one frame, the same crop x2, FSR 2 last.
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


def load_img(path, info):
    w, h = info["size"]
    raw = np.fromfile(path, np.uint8)
    f = info["dxgi"]
    if f == 10:
        return raw.view(np.float16)[: w * h * 4].reshape(h, w, 4)[..., :3].astype(np.float32)
    if f in (25, 26):
        u = raw.view(np.uint32)[: w * h].reshape(h, w)
        return np.stack([uf(u, 6), uf(u >> 11, 6), uf(u >> 22, 5)], -1)
    if f in (2,):
        return raw.view(np.float32)[: w * h * 4].reshape(h, w, 4)[..., :3].copy()
    if f == 34:
        return raw.view(np.float16)[: w * h * 2].reshape(h, w, 2).astype(np.float32)
    if f in (39, 40, 41):
        return raw.view(np.float32)[: w * h].reshape(h, w)
    raise SystemExit(f"{path}: dxgi {f} not handled")


def tone(x):
    x = np.clip(x, 0, None)
    return x / (1.0 + x)


def sample(img, sx, sy):
    return np.stack([map_coordinates(img[..., c], [sy, sx], order=1, mode="nearest") for c in range(img.shape[-1])], -1)


def mv_check(frames):
    variants = [("no motion", 0, 1, False)]
    for s in (1.0, 0.5, 2.0):
        for sign in (1, -1):
            for jc in (True, False):
                variants.append((f"MV x{s:g} {'+' if sign > 0 else '-'} {'with' if jc else 'without'} jitter", s, sign, jc))
    err = {v[0]: [] for v in variants}
    mags = []
    for (m0, f0), (m1, f1) in zip(frames, frames[1:]):
        c0, c1, mv = tone(f0["color"]), tone(f1["color"]), f1["mv"]
        H, W = c1.shape[:2]
        sx_, sy_ = m1["mv_scale"]
        yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
        dj = np.array(m0["jitter"], np.float32) - np.array(m1["jitter"], np.float32)
        mags.append(np.hypot(mv[..., 0] * sx_, mv[..., 1] * sy_))
        for name, s, sign, jc in variants:
            dx = sign * s * mv[..., 0] * sx_ + (dj[0] if jc else 0)
            dy = sign * s * mv[..., 1] * sy_ + (dj[1] if jc else 0)
            px, py = xx + dx, yy + dy
            ok = (px >= 0) & (px <= W - 1) & (py >= 0) & (py <= H - 1)
            e = np.abs(sample(c0, px, py) - c1).mean(-1)
            err[name].append(float(e[ok].mean()))
    return err, mags


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--series", required=True)
    ap.add_argument("--out", default="")
    ap.add_argument("--replay", action="append", default=[])
    ap.add_argument("--frame", type=int, default=-1)
    ap.add_argument("--sub", default="")
    ap.add_argument("--cols", type=int, default=0)
    a = ap.parse_args()
    frames = []
    for mp in sorted(glob.glob(os.path.join(a.series, "f*.json"))):
        m = json.load(open(mp))
        base = mp[:-5]
        fr = {}
        for k, info in m["images"].items():
            if k not in ("color", "mv", "depth", "fsr2", "fsr2_rcas", "vela_ai", "vela_sr"):
                continue
            p = f"{base}_{k}.bin"
            if os.path.exists(p):
                fr[k] = load_img(p, info)
        frames.append((m, fr))
    frames.sort(key=lambda x: x[0]["frame"])
    print(f"{len(frames)} frames, render {frames[0][0]['render']}, display {frames[0][0]['display']}, "
          f"mv_scale {frames[0][0]['mv_scale']}, layout {frames[0][0].get('fsr2_layout')}")

    err, mags = mv_check(frames)
    base = np.mean(err["no motion"])
    print("\n1) MV check: reprojection error of the previous colour (tone, x1e-3), all pairs:")
    for name, e in sorted(err.items(), key=lambda kv: np.mean(kv[1])):
        print(f"   {name:28s} {np.mean(e) * 1e3:8.2f}   ({np.mean(e) / base * 100:5.1f} % of no motion)")
    m = np.concatenate([x.ravel() for x in mags])
    print(f"\n2) motion: median {np.median(m):.2f} px, p95 {np.percentile(m, 95):.2f} px, max {m.max():.1f} px per frame "
          f"(render)")

    # 3) results in motion (display crop)
    have = [k for k in ("vela_ai", "vela_sr") if k in frames[0][1]]
    seqs = {}
    if "fsr2" not in frames[0][1]:
        print("no fsr2 reference in the series")
        return
    ox, oy = frames[0][0]["images"]["fsr2"]["origin"]
    ch, cw = frames[0][1]["fsr2"].shape[:2]
    ref = [fr["fsr2"] for _, fr in frames]
    for k in have:
        seqs[{"vela_ai": "Vela AI (game)", "vela_sr": "Vela SR (game)"}[k]] = [fr[k] for _, fr in frames]
    for spec in a.replay:
        name, rest = spec.split("=", 1)
        d, tag = rest.rsplit(":", 1)
        seq = []
        for mm, _ in frames:
            w, h = mm["display"]
            img = np.fromfile(os.path.join(d, f"f{mm['frame']:06d}_replay_{tag}.bin"), np.float16).astype(np.float32)
            seq.append(img.reshape(h, w, 4)[oy:oy + ch, ox:ox + cw, :3])
        seqs[name] = seq
    if "fsr2_rcas" in frames[0][1]:
        seqs["FSR 2 + RCAS (game)"] = [fr["fsr2_rcas"] for _, fr in frames]
    seqs["FSR 2 (reference)"] = ref
    dw, dh = frames[0][0]["display"]
    rw, rh = frames[0][0]["render"]
    rows = {}
    for name, seq in seqs.items():
        hf, wtd, ps = [], [], []
        for t in range(len(seq)):
            o, r = tone(seq[t]), tone(ref[t])
            smooth = np.abs(r - uniform_filter(r, size=(9, 9, 1))).max(-1) < 0.01
            hf.append(np.abs(o - uniform_filter(o, size=(9, 9, 1)))[smooth].mean() * 1e4)
            ps.append(99.0 if seq is ref else 10 * np.log10(1 / max(float(np.mean((o - r) ** 2)), 1e-12)))
            if t == 0:
                continue
            mv = frames[t][1]["mv"]
            sx_, sy_ = frames[t][0]["mv_scale"]
            yy, xx = np.mgrid[0:ch, 0:cw].astype(np.float32)
            # display px (crop) -> render px for the MV lookup; MV in render px * display / render
            rx = np.clip(((xx + ox + 0.5) * rw / dw - 0.5).astype(int), 0, rw - 1)
            ry = np.clip(((yy + oy + 0.5) * rh / dh - 0.5).astype(int), 0, rh - 1)
            dx = mv[ry, rx, 0] * sx_ * dw / rw
            dy = mv[ry, rx, 1] * sy_ * dh / rh
            px, py = xx + dx, yy + dy
            ins = (px >= 0) & (px <= cw - 1) & (py >= 0) & (py <= ch - 1)
            wo = tone(sample(seq[t - 1], px, py))
            wr = tone(sample(ref[t - 1], px, py))
            agree = ins & (np.abs(wr - r).max(-1) < 0.05)
            wtd.append(np.abs(o - wo).max(-1)[agree].mean() * 1e3)
        rows[name] = (np.mean(hf), np.mean(wtd), np.mean(ps[1:]) if len(ps) > 1 else ps[0], seq)
    print(f"\n3) in motion (display crop {cw}x{ch} at {ox},{oy}):")
    print(f"   {'':26s} {'hf x1e-4':>9s} {'flicker x1e-3':>15s} {'PSNR vs FSR 2':>13s}")
    for name, v in rows.items():
        print(f"   {name:26s} {v[0]:9.1f} {v[1]:15.2f} {v[2]:13.2f}")
    if not a.out:
        return
    k = a.frame if a.frame >= 0 else len(frames) - 1
    x, y, w, h = map(int, a.sub.split(",")) if a.sub else (cw // 2 - 160, ch // 2 - 90, 320, 180)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    tiles = []
    for name, v in rows.items():
        c = tone(v[3][k][y:y + h, x:x + w])
        t8 = (np.clip(np.where(c <= 0.0031308, 12.92 * c, 1.055 * np.power(c, 1 / 2.4) - 0.055), 0, 1) * 255).astype(np.uint8)
        im = Image.fromarray(t8).resize((w * 2, h * 2), Image.NEAREST)
        cv = Image.new("RGB", (im.width, im.height + 26), (18, 18, 18))
        cv.paste(im, (0, 26))
        ImageDraw.Draw(cv).text((6, 3), f"{name}  hf {v[0]:.0f}  flicker {v[1]:.1f}", fill=(235, 235, 235), font=font)
        tiles.append(cv)
    cols = a.cols or len(tiles)  # rows of `cols` tiles, reading order (FSR 2 stays last)
    rows_n = (len(tiles) + cols - 1) // cols
    tw, th = tiles[0].width, tiles[0].height
    out = Image.new("RGB", (cols * tw + 8 * (cols - 1), rows_n * th + 8 * (rows_n - 1)), (18, 18, 18))
    for i, t in enumerate(tiles):
        out.paste(t, ((i % cols) * (tw + 8), (i // cols) * (th + 8)))
    out.save(a.out)
    print(f"wrote {a.out} (frame {frames[k][0]['frame']}, crop {x},{y} {w}x{h} x2)")


if __name__ == "__main__":
    main()
