#!/usr/bin/env python3
"""Generate deterministic synthetic calibration sets for QNN W8A16/W8A8 quantization of mlfi_s1 and mlfi_s3.

Produces 16 NHWC float32 raw inputs per stage:
  - Stage 1 (rgb01):  [1, 144, 240, 6]  covering static, small/large translations,
                      dark/bright exposure, and a hard scene cut.
  - Stage 3 (feat24): [1, 272, 480, 24] matching the pass152 feature layout across
                      ML-only, GameMV+ML, disocclusion bands, and noisy GameMV cases.
"""
import argparse
import pathlib
import numpy as np


def make_natural_scene(h=1080, w=1920, dx=0.0, dy=0.0, seed=0) -> np.ndarray:
    """Band-limited natural-style synthetic scene (no single-px checker aliasing)."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    x = xx - dx
    y = yy - dy
    p0 = float(rng.uniform(0, 6.28))
    p1 = float(rng.uniform(0, 6.28))
    p2 = float(rng.uniform(0, 6.28))
    r = 0.38 + 0.18 * np.sin(x * 0.011 + y * 0.007 + p0) * np.cos(y * 0.015 - x * 0.004 + p1)
    g = 0.42 + 0.18 * np.sin(x * 0.009 - y * 0.013 + p1) * np.cos(x * 0.019 + p2)
    b = 0.44 + 0.18 * np.cos(x * 0.008 + y * 0.011 + p2)
    d1 = 0.10 * np.sin(x * 0.055 + 0.4 * np.sin(y * 0.031 + p0)) * np.cos(y * 0.049 + p1)
    d2 = 0.07 * np.sin((x + y) * 0.037 + p2) * np.cos((x - y) * 0.029 + p0)
    r += d1 + d2
    g += d1 * 0.85 - d2 * 0.4
    b -= d1 * 0.5 + d2 * 0.7

    objs = [
        (420 + (seed * 53) % 200, 320 + (seed * 37) % 150, 130, (0.88, 0.28, 0.22)),
        (1100 + (seed * 71) % 220, 600 + (seed * 41) % 140, 165, (0.22, 0.76, 0.36)),
        (1480 - (seed * 29) % 180, 310 + (seed * 47) % 160, 110, (0.92, 0.80, 0.24)),
        (760 + (seed * 61) % 160, 810 - (seed * 31) % 120, 145, (0.26, 0.42, 0.88)),
    ]
    for (cx, cy, rad, col) in objs:
        dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
        m = np.clip((rad - dist) * 0.15, 0.0, 1.0)
        st = 0.5 + 0.5 * np.sin((x - cx) * 0.07 + (y - cy) * 0.05)
        r = r * (1 - m) + (col[0] * (0.55 + 0.45 * st)) * m
        g = g * (1 - m) + (col[1] * (0.55 + 0.45 * st)) * m
        b = b * (1 - m) + (col[2] * (0.55 + 0.45 * st)) * m

    return np.clip(np.stack([r, g, b], axis=-1), 0.0, 1.0).astype(np.float32)


def box_down8(img: np.ndarray) -> np.ndarray:
    """8x8 box average from 1920x1080 -> 240x135."""
    h, w, c = img.shape
    return img.reshape(h // 8, 8, w // 8, 8, c).mean(axis=(1, 3)).astype(np.float32)


def bilinear_sample_uv(img: np.ndarray, uv_x: np.ndarray, uv_y: np.ndarray) -> np.ndarray:
    h, w, _ = img.shape
    fx = np.clip(uv_x, 0.0, 1.0) * w - 0.5
    fy = np.clip(uv_y, 0.0, 1.0) * h - 0.5
    x0 = np.floor(fx).astype(np.int32)
    y0 = np.floor(fy).astype(np.int32)
    wx = (fx - x0)[..., None]
    wy = (fy - y0)[..., None]
    x1 = np.clip(x0 + 1, 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    x0 = np.clip(x0, 0, w - 1)
    y0 = np.clip(y0, 0, h - 1)
    c00 = img[y0, x0]
    c10 = img[y0, x1]
    c01 = img[y1, x0]
    c11 = img[y1, x1]
    return ((c00 * (1 - wx) + c10 * wx) * (1 - wy) + (c01 * (1 - wx) + c11 * wx) * wy).astype(np.float32)


def luma(c: np.ndarray) -> np.ndarray:
    return (0.299 * c[..., 0] + 0.587 * c[..., 1] + 0.114 * c[..., 2]).astype(np.float32)


def make_s1_nhwc(i0: np.ndarray, i1: np.ndarray) -> np.ndarray:
    """Return [1, 144, 240, 6] float32 (NHWC)."""
    arr = np.zeros((1, 144, 240, 6), dtype=np.float32)
    arr[0, :135, :, 0:3] = box_down8(i0)
    arr[0, :135, :, 3:6] = box_down8(i1)
    return arr


def make_s3_nhwc(i0: np.ndarray, i1: np.ndarray, flow0_uv: np.ndarray, flow1_uv: np.ndarray,
                 mv0_uv: np.ndarray, mv1_uv: np.ndarray, val0: np.ndarray, val1: np.ndarray) -> np.ndarray:
    """Return [1, 272, 480, 24] float32 (NHWC) matching pass152 feature packing."""
    w4, h4, h4_pad = 480, 270, 272
    u4 = (np.arange(w4, dtype=np.float32) + 0.5) / w4
    v4 = (np.arange(h4, dtype=np.float32) + 0.5) / h4
    uu4, vv4 = np.meshgrid(u4, v4)

    fl0_q = bilinear_sample_uv(flow0_uv, uu4, vv4)
    fl1_q = bilinear_sample_uv(flow1_uv, uu4, vv4)
    rgb0_q = bilinear_sample_uv(i0, uu4, vv4)
    rgb1_q = bilinear_sample_uv(i1, uu4, vv4)
    ml0_q = bilinear_sample_uv(i0, uu4 + fl0_q[..., 0], vv4 + fl0_q[..., 1])
    ml1_q = bilinear_sample_uv(i1, uu4 + fl1_q[..., 0], vv4 + fl1_q[..., 1])
    gw0_q = bilinear_sample_uv(i0, uu4 - mv0_uv[..., 0], vv4 - mv0_uv[..., 1])
    gw1_q = bilinear_sample_uv(i1, uu4 - mv1_uv[..., 0], vv4 - mv1_uv[..., 1])

    arr = np.zeros((1, h4_pad, w4, 24), dtype=np.float32)
    arr[0, :h4, :, 0:3] = gw0_q
    arr[0, :h4, :, 3:6] = gw1_q
    arr[0, :h4, :, 6:9] = ml0_q
    arr[0, :h4, :, 9:12] = ml1_q
    arr[0, :h4, :, 12] = val0
    arr[0, :h4, :, 13] = val1
    arr[0, :h4, :, 14:17] = rgb0_q
    arr[0, :h4, :, 17:20] = rgb1_q
    arr[0, :h4, :, 20] = np.abs(luma(rgb0_q) - luma(rgb1_q))
    arr[0, :h4, :, 21] = np.abs(luma(gw0_q) - luma(gw1_q))
    arr[0, :h4, :, 22] = np.abs(luma(gw0_q) - luma(ml0_q))
    arr[0, :h4, :, 23] = np.abs(luma(ml0_q) - luma(ml1_q))
    return arr


def generate_calib_sets(calib_dir: pathlib.Path, stages=("s1", "s3")):
    s1_dir = calib_dir / "s1"
    s3_dir = calib_dir / "s3"
    if "s1" in stages:
        s1_dir.mkdir(parents=True, exist_ok=True)
    if "s3" in stages:
        s3_dir.mkdir(parents=True, exist_ok=True)

    h, w = 1080, 1920
    h4, w4 = 270, 480
    motions = [
        (0.0, 0.0),
        (4.0, 2.0),
        (-8.0, 4.0),
        (16.0, -8.0),
        (-24.0, 12.0),
        (32.0, -16.0),
        (-40.0, -20.0),
        (48.0, 24.0),
        (0.0, 28.0),
        (-36.0, 0.0),
        (12.0, 18.0),
        (-14.0, -22.0),
        (26.0, 10.0),
        (-18.0, 30.0),
        (6.0, -12.0),
        (-6.0, 6.0),
    ]

    s1_lines = []
    s3_lines = []
    rng = np.random.default_rng(1234)
    for k, (dx, dy) in enumerate(motions):
        gain = 0.25 if k == 14 else (1.15 if k == 15 else 1.0)
        i0 = np.clip(make_natural_scene(h, w, 0.0, 0.0, seed=100 + k) * gain, 0.0, 1.0).astype(np.float32)
        if k == 13:
            i1 = make_natural_scene(h, w, 0.0, 0.0, seed=999)
        else:
            i1 = np.clip(make_natural_scene(h, w, dx, dy, seed=100 + k) * gain, 0.0, 1.0).astype(np.float32)

        if "s1" in stages:
            s1_arr = make_s1_nhwc(i0, i1)
            p1 = s1_dir / f"s1_{k:02d}.raw"
            p1.write_bytes(np.ascontiguousarray(s1_arr, dtype=np.float32).tobytes())
            s1_lines.append(f"rgb01:={p1.resolve().as_posix()}")

        noise = rng.normal(0.0, 0.0008, size=(135, 240, 2)).astype(np.float32)
        if "s3" in stages:
            fl0 = np.zeros((135, 240, 2), dtype=np.float32)
            fl1 = np.zeros((135, 240, 2), dtype=np.float32)
            fl0[..., 0] = -dx * 0.5 / w + noise[..., 0]
            fl0[..., 1] = -dy * 0.5 / h + noise[..., 1]
            fl1[..., 0] = +dx * 0.5 / w - noise[..., 0]
            fl1[..., 1] = +dy * 0.5 / h - noise[..., 1]

            mv0 = np.zeros((h4, w4, 2), dtype=np.float32)
            mv1 = np.zeros((h4, w4, 2), dtype=np.float32)
            if k % 4 == 0:
                val0 = np.zeros((h4, w4), dtype=np.float32)
                val1 = np.zeros((h4, w4), dtype=np.float32)
            elif k % 4 == 1:
                mv0[..., 0], mv0[..., 1] = +dx * 0.5 / w, +dy * 0.5 / h
                mv1[..., 0], mv1[..., 1] = -dx * 0.5 / w, -dy * 0.5 / h
                val0 = np.ones((h4, w4), dtype=np.float32)
                val1 = np.ones((h4, w4), dtype=np.float32)
            elif k % 4 == 2:
                mv0[..., 0], mv0[..., 1] = +dx * 0.5 / w, +dy * 0.5 / h
                mv1[..., 0], mv1[..., 1] = -dx * 0.5 / w, -dy * 0.5 / h
                val0 = np.ones((h4, w4), dtype=np.float32)
                val1 = np.ones((h4, w4), dtype=np.float32)
                val0[:, :120] = 0.0
                val1[:, 360:] = 0.0
            else:
                mv0[..., 0], mv0[..., 1] = +dx * 0.35 / w, +dy * 0.35 / h
                mv1[..., 0], mv1[..., 1] = -dx * 0.5 / w, -dy * 0.5 / h
                val0 = np.ones((h4, w4), dtype=np.float32)
                val1 = np.ones((h4, w4), dtype=np.float32)

            s3_arr = make_s3_nhwc(i0, i1, fl0, fl1, mv0, mv1, val0, val1)
            p3 = s3_dir / f"s3_{k:02d}.raw"
            p3.write_bytes(np.ascontiguousarray(s3_arr, dtype=np.float32).tobytes())
            s3_lines.append(f"feat24:={p3.resolve().as_posix()}")

    out = {}
    if "s1" in stages:
        s1_list = calib_dir / "s1_list.txt"
        s1_list.write_text("\n".join(s1_lines) + "\n", encoding="utf-8")
        out["s1"] = s1_list
    if "s3" in stages:
        s3_list = calib_dir / "s3_list.txt"
        s3_list.write_text("\n".join(s3_lines) + "\n", encoding="utf-8")
        out["s3"] = s3_list
    print(f"CALIB generated s1={len(s1_lines)} s3={len(s3_lines)} in {calib_dir}", flush=True)
    return out


def main():
    ap = argparse.ArgumentParser(description="Generate synthetic calibration inputs for mlfi_s1 and mlfi_s3.")
    ap.add_argument("--out", required=True, type=pathlib.Path, help="Calibration output directory")
    ap.add_argument("--stage", choices=["all", "s1", "s3"], default="all")
    args = ap.parse_args()
    stages = ("s1", "s3") if args.stage == "all" else (args.stage,)
    generate_calib_sets(args.out, stages=stages)


if __name__ == "__main__":
    main()
