"""Noise on smooth areas (hf, as series_eval) split by how fast the pixel moves: python -I tools/hf_by_motion.py SERIES_DIR..."""
import sys, glob, json, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import series_eval as E
from scipy.ndimage import uniform_filter
BINS = [(0, 0.25, "still"), (0.25, 2, "slow"), (2, 1e9, "fast")]
print(f"{'series':8s} {'motion':9s} {'share':>5s} " + " ".join(f"{n:>14s}" for n in ("Vela AI game", "FSR2+RCAS", "FSR 2")) + "  (hf x1e-4 on smooth areas)")
for d in sys.argv[1:]:
    s = d.rstrip('/').split('-')[-1]
    acc = {b[2]: {k: [] for k in ("vela_ai", "fsr2_rcas", "fsr2")} for b in BINS}; frac = {b[2]: [] for b in BINS}
    for mp in sorted(glob.glob(d + '/f*.json'))[4:]:
        m = json.load(open(mp)); b = mp[:-5]
        ox, oy = m['images']['fsr2']['origin']; cw, ch = m['images']['fsr2']['size']; rw, rh = m['render']; dw, dh = m['display']
        mv = E.load_img(b + '_mv.bin', m['images']['mv'])
        yy, xx = np.mgrid[0:ch, 0:cw]
        rx = np.clip(((xx + ox + .5) * rw / dw - .5).astype(int), 0, rw - 1); ry = np.clip(((yy + oy + .5) * rh / dh - .5).astype(int), 0, rh - 1)
        mag = np.hypot(mv[ry, rx, 0] * rw, mv[ry, rx, 1] * rh)
        ref = E.tone(E.load_img(b + '_fsr2.bin', m['images']['fsr2']))
        smooth = np.abs(ref - uniform_filter(ref, size=(9, 9, 1))).max(-1) < 0.01
        imgs = {k: E.tone(E.load_img(f'{b}_{k}.bin', m['images'][k])) for k in ("vela_ai", "fsr2_rcas", "fsr2")}
        hf = {k: np.abs(o - uniform_filter(o, size=(9, 9, 1))).mean(-1) for k, o in imgs.items()}
        for lo, hi, n in BINS:
            sel = smooth & (mag >= lo) & (mag < hi); frac[n].append(sel.mean())
            if sel.sum() > 500:
                for k in hf: acc[n][k].append(hf[k][sel].mean() * 1e4)
    for lo, hi, n in BINS:
        if acc[n]["fsr2"]:
            print(f"{s:8s} {n:9s} {np.mean(frac[n])*100:4.0f}% " + " ".join(f"{np.mean(acc[n][k]):14.1f}" for k in ("vela_ai", "fsr2_rcas", "fsr2")))
