#!/usr/bin/env python3
"""Cross-platform QAIRT 2.47 compiler & HTP simulator verifier for FSR 4.0.2 ML FG (mlfi_s1, mlfi_s3).

Supports both Linux (qnn-model-lib-generator + x86_64-linux-clang) and native Windows
(object-generator.exe + MinGW g++ -> x86_64 model DLL) with HTP v73/v79/v81 targets.
"""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import time
import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from extract_mlfi_onnx import extract as extract_onnx
from gen_calib import (
    bilinear_sample_uv,
    generate_calib_sets,
    make_natural_scene,
    make_s1_nhwc,
    make_s3_nhwc,
)

ARCH_SOC = {
    "v73": 43,  # SM8550 (Snapdragon 8 Gen 2)
    "v79": 69,  # SM8750 (Snapdragon 8 Elite)
    "v81": 87,  # SM8850 (Snapdragon 8 Elite Gen 5)
}

STAGE_SPEC = {
    "s1": ("mlfi_s1", "mlfi_stage1_1080p.onnx", "rgb01", "flow_px"),
    "s3": ("mlfi_s3", "mlfi_stage3_1080p.onnx", "feat24", "weights5"),
}


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse < 1e-12 else 10.0 * np.log10(1.0 / mse)


def build_env(qairt: pathlib.Path, py_exe: pathlib.Path) -> dict:
    env = os.environ.copy()
    env["QNN_SDK_ROOT"] = str(qairt)
    env["PYTHONPATH"] = str(qairt / "lib" / "python")
    if platform.system() == "Windows":
        parts = [
            str(py_exe.parent),
            str(qairt / "bin" / "x86_64-windows-msvc"),
            str(qairt / "lib" / "x86_64-windows-msvc"),
            str(qairt / "lib" / "python" / "qti" / "aisw" / "converters" / "common" / "windows-x86_64"),
            env.get("PATH", ""),
        ]
        env["PATH"] = ";".join(parts)
    else:
        lib_dir = str(qairt / "lib" / "x86_64-linux-clang")
        bin_dir = str(qairt / "bin" / "x86_64-linux-clang")
        env["LD_LIBRARY_PATH"] = f"{lib_dir}:{env.get('LD_LIBRARY_PATH', '')}".rstrip(":")
        env["PATH"] = f"{bin_dir}:{env.get('PATH', '')}"
    return env


def run_step(cmd, cwd: pathlib.Path, env: dict, log_path: pathlib.Path, label: str) -> str:
    t0 = time.time()
    p = subprocess.run(
        cmd,
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    log_path.write_text(p.stdout, encoding="utf-8")
    dt = time.time() - t0
    if p.returncode != 0:
        tail = "\n".join(p.stdout.splitlines()[-40:])
        raise RuntimeError(f"{label} failed (rc={p.returncode}, {dt:.1f}s):\n{tail}")
    print(f"  [ok] {label} ({dt:.1f}s)", flush=True)
    return p.stdout


def compile_model_lib(qairt: pathlib.Path, w: pathlib.Path, m: str, env: dict) -> pathlib.Path:
    if platform.system() != "Windows":
        run_step(
            [
                str(qairt / "bin" / "x86_64-linux-clang" / "qnn-model-lib-generator"),
                "-c", str(w / f"{m}.cpp"),
                "-b", str(w / f"{m}.bin"),
                "-l", m,
                "-t", "x86_64-linux-clang",
                "-o", str(w / "libs"),
            ],
            cwd=w,
            env=env,
            log_path=w / "libgen.log",
            label=f"{m} qnn-model-lib-generator",
        )
        return w / "libs" / "x86_64-linux-clang" / f"lib{m}.so"

    # Native Windows fallback using QAIRT object-generator.exe + MinGW g++
    bld = w / "dll_bld"
    if bld.exists():
        shutil.rmtree(bld)
    raw_dir = bld / "obj" / "binary"
    obj_out = bld / "obj" / "windows-x86_64"
    raw_dir.mkdir(parents=True, exist_ok=True)
    obj_out.mkdir(parents=True, exist_ok=True)

    with tarfile.open(w / f"{m}.bin") as tar:
        tar.extractall(raw_dir)

    obj_gen = qairt / "bin" / "x86_64-windows-msvc" / "object-generator.exe"
    run_step(
        [str(obj_gen), r"obj\binary", "windows-x86_64", r"obj\windows-x86_64"],
        cwd=bld,
        env=env,
        log_path=w / "objgen.log",
        label=f"{m} object-generator",
    )

    conv_jni = qairt / "share" / "QNN" / "converter" / "jni"
    for f in [
        "QnnModel.cpp",
        "QnnModel.hpp",
        "QnnModelPal.hpp",
        "QnnTypeMacros.hpp",
        "QnnWrapperUtils.cpp",
        "QnnWrapperUtils.hpp",
    ]:
        shutil.copy2(conv_jni / f, bld / f)
    shutil.copy2(conv_jni / "windows" / "QnnModelPal.cpp", bld / "QnnModelPal.cpp")
    shutil.copy2(w / f"{m}.cpp", bld / "QnnNetworkModel.cpp")

    objs = sorted(str(p) for p in obj_out.glob("*.o"))
    out_dir = w / "libs" / "x64"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_dll = out_dir / f"{m}.dll"

    run_step(
        [
            "g++", "-shared", "-O2", "-std=gnu++17",
            "-DQNN_API=__declspec(dllexport)",
            f"-I{qairt / 'include' / 'QNN'}",
            f"-I{bld}",
            str(bld / "QnnModel.cpp"),
            str(bld / "QnnModelPal.cpp"),
            str(bld / "QnnWrapperUtils.cpp"),
            str(bld / "QnnNetworkModel.cpp"),
            *objs,
            "-lpsapi", "-static", "-static-libgcc", "-static-libstdc++",
            "-o", str(out_dll),
        ],
        cwd=bld,
        env=env,
        log_path=w / "libgen.log",
        label=f"{m} g++ model DLL",
    )
    return out_dll


def parse_ctx_metrics(log_text: str) -> dict:
    out = {}
    for k in ("spill_bytes", "fill_bytes", "write_total_bytes", "read_total_bytes"):
        m = re.search(rf"^{k}\s*[:=]?\s*(\d+)", log_text, re.M)
        if m:
            out[k] = int(m.group(1))
    return out


def build_stage(qairt: pathlib.Path, py_exe: pathlib.Path, stage_key: str,
                onnx_path: pathlib.Path, calib_list: pathlib.Path, work_root: pathlib.Path,
                out_dir: pathlib.Path, mode: str, archs, vtcm_mb: int) -> dict:
    bin_stem, _, in_name, out_name = STAGE_SPEC[stage_key]
    graph_name = f"{bin_stem}_{mode}"
    w = work_root / graph_name
    if w.exists():
        shutil.rmtree(w)
    w.mkdir(parents=True, exist_ok=True)

    env = build_env(qairt, py_exe)
    is_win = platform.system() == "Windows"
    bin_sub = "x86_64-windows-msvc" if is_win else "x86_64-linux-clang"
    lib_sub = bin_sub
    htp_lib = qairt / "lib" / lib_sub / ("QnnHtp.dll" if is_win else "libQnnHtp.so")
    ext_lib = qairt / "lib" / lib_sub / ("QnnHtpNetRunExtensions.dll" if is_win else "libQnnHtpNetRunExtensions.so")
    ctx_gen = qairt / "bin" / bin_sub / ("qnn-context-binary-generator.exe" if is_win else "qnn-context-binary-generator")

    io_yaml = w / "io.yaml"
    io_yaml.write_text(
        f"- IOName: {in_name}\n  Datatype: float16\n"
        f"- IOName: {out_name}\n  Datatype: float16\n",
        encoding="utf-8",
    )

    conv_cmd = [
        str(py_exe),
        str(qairt / "bin" / bin_sub / "qnn-onnx-converter"),
        "-i", str(onnx_path),
        "--custom_io", str(io_yaml),
        "-o", str(w / f"{graph_name}.cpp"),
    ]
    if mode == "fp16":
        conv_cmd += ["--float_bitwidth", "16"]
    elif mode == "w8a16":
        conv_cmd += [
            "--input_list", str(calib_list),
            "--act_bitwidth", "16",
            "--weights_bitwidth", "8",
            "--use_per_channel_quantization",
        ]
    elif mode == "w8a8":
        conv_cmd += [
            "--input_list", str(calib_list),
            "--act_bitwidth", "8",
            "--weights_bitwidth", "8",
            "--use_per_channel_quantization",
        ]
    else:
        raise ValueError(mode)

    run_step(conv_cmd, cwd=w, env=env, log_path=w / "convert.log", label=f"{graph_name} qnn-onnx-converter")
    model_lib = compile_model_lib(qairt, w, graph_name, env)

    built = {}
    for arch in archs:
        soc = ARCH_SOC[arch]
        arch_out = out_dir / arch
        arch_out.mkdir(parents=True, exist_ok=True)

        htp_cfg = w / f"htp_{arch}.json"
        ctx_cfg = w / f"ctxcfg_{arch}.json"
        htp_cfg.write_text(
            json.dumps({
                "graphs": [{"graph_names": [graph_name], "vtcm_mb": vtcm_mb, "O": 3}],
                "devices": [{"soc_model": soc, "dsp_arch": arch}],
            }),
            encoding="utf-8",
        )
        ctx_cfg.write_text(
            json.dumps({
                "backend_extensions": {
                    "shared_library_path": str(ext_lib),
                    "config_file_path": str(htp_cfg),
                }
            }),
            encoding="utf-8",
        )

        ctx_log = run_step(
            [
                str(ctx_gen),
                "--backend", str(htp_lib),
                "--model", str(model_lib),
                "--binary_file", bin_stem,
                "--output_dir", str(arch_out),
                "--config_file", str(ctx_cfg),
                "--log_level", "info",
            ],
            cwd=w,
            env=env,
            log_path=w / f"ctx_{arch}.log",
            label=f"{bin_stem} ctx ({arch})",
        )
        ctx_bin = arch_out / f"{bin_stem}.bin"
        metrics = parse_ctx_metrics(ctx_log)
        h = sha256_file(ctx_bin)
        print(
            f"CTX {arch}/{bin_stem}.bin sha256={h} size={ctx_bin.stat().st_size} "
            f"spill={metrics.get('spill_bytes', -1)} fill={metrics.get('fill_bytes', -1)}",
            flush=True,
        )
        built[arch] = {"bin": ctx_bin, "cfg": ctx_cfg, "sha256": h, "metrics": metrics}
    return built


def run_htp_sim(qairt: pathlib.Path, py_exe: pathlib.Path, ctx_bin: pathlib.Path,
                ctx_cfg: pathlib.Path, in_name: str, input_arr: np.ndarray,
                sim_dir: pathlib.Path) -> np.ndarray:
    env = build_env(qairt, py_exe)
    is_win = platform.system() == "Windows"
    bin_sub = "x86_64-windows-msvc" if is_win else "x86_64-linux-clang"
    htp_lib = qairt / "lib" / bin_sub / ("QnnHtp.dll" if is_win else "libQnnHtp.so")
    net_run = qairt / "bin" / bin_sub / ("qnn-net-run.exe" if is_win else "qnn-net-run")

    if sim_dir.exists():
        shutil.rmtree(sim_dir)
    sim_dir.mkdir(parents=True, exist_ok=True)
    in_raw = sim_dir / "in.raw"
    in_raw.write_bytes(np.ascontiguousarray(input_arr.astype(np.float16)).tobytes())
    list_txt = sim_dir / "list.txt"
    list_txt.write_text(f"{in_name}:={in_raw.resolve().as_posix()}\n", encoding="utf-8")
    out_dir = sim_dir / "out"
    out_dir.mkdir(parents=True, exist_ok=True)

    run_step(
        [
            str(net_run),
            "--backend", str(htp_lib),
            "--retrieve_context", str(ctx_bin),
            "--input_list", str(list_txt),
            "--use_native_input_files", "--use_native_output_files",
            "--output_dir", str(out_dir),
            "--config_file", str(ctx_cfg),
            "--log_level", "warn",
        ],
        cwd=sim_dir,
        env=env,
        log_path=sim_dir / "sim.log",
        label=f"sim {ctx_bin.stem}",
    )
    raws = sorted(out_dir.rglob("*.raw"))
    if not raws:
        raise RuntimeError(f"No output .raw from qnn-net-run for {ctx_bin}")
    return np.frombuffer(raws[0].read_bytes(), dtype=np.float16).astype(np.float32)


def eval_pair(qairt: pathlib.Path, py_exe: pathlib.Path, s1_info: dict, s3_info: dict,
              work_root: pathlib.Path, arch: str):
    h, w = 1080, 1920
    dx, dy = 32.0, -16.0
    sl = (slice(64, h - 64), slice(64, w - 64))
    u_full = (np.arange(w, dtype=np.float32) + 0.5) / w
    v_full = (np.arange(h, dtype=np.float32) + 0.5) / h
    uu, vv = np.meshgrid(u_full, v_full)

    i0 = make_natural_scene(h, w, 0.0, 0.0, seed=10)
    imid = make_natural_scene(h, w, dx * 0.5, dy * 0.5, seed=10)
    i1 = make_natural_scene(h, w, dx, dy, seed=10)

    s1_out = run_htp_sim(
        qairt, py_exe, s1_info["bin"], s1_info["cfg"], "rgb01",
        make_s1_nhwc(i0, i1), work_root / f"eval_{arch}_s1",
    ).reshape(1, 144, 240, 4)[0, :135, :, :]

    fl0_uv = np.stack([s1_out[..., 0] / 240.0, s1_out[..., 1] / 135.0], axis=-1)
    fl1_uv = np.stack([s1_out[..., 2] / 240.0, s1_out[..., 3] / 135.0], axis=-1)
    fl0_f = bilinear_sample_uv(fl0_uv, uu, vv)
    fl1_f = bilinear_sample_uv(fl1_uv, uu, vv)
    ml0_f = bilinear_sample_uv(i0, uu + fl0_f[..., 0], vv + fl0_f[..., 1])
    ml1_f = bilinear_sample_uv(i1, uu + fl1_f[..., 0], vv + fl1_f[..., 1])
    p_ml0 = psnr(ml0_f[sl], imid[sl])
    p_ml1 = psnr(ml1_f[sl], imid[sl])

    res = {}
    for mtag, has_mv in [("ml_only", False), ("gamemv_ml", True)]:
        h4, w4 = 270, 480
        mv0_q = np.zeros((h4, w4, 2), dtype=np.float32)
        mv1_q = np.zeros((h4, w4, 2), dtype=np.float32)
        if has_mv:
            mv0_q[..., 0], mv0_q[..., 1] = +dx * 0.5 / w, +dy * 0.5 / h
            mv1_q[..., 0], mv1_q[..., 1] = -dx * 0.5 / w, -dy * 0.5 / h
            val0_q = np.ones((h4, w4), dtype=np.float32)
            val1_q = np.ones((h4, w4), dtype=np.float32)
            val0_f = np.ones((h, w, 1), dtype=np.float32)
            val1_f = np.ones((h, w, 1), dtype=np.float32)
            gw0_f = bilinear_sample_uv(i0, uu - (+dx * 0.5 / w), vv - (+dy * 0.5 / h))
            gw1_f = bilinear_sample_uv(i1, uu - (-dx * 0.5 / w), vv - (-dy * 0.5 / h))
        else:
            val0_q = np.zeros((h4, w4), dtype=np.float32)
            val1_q = np.zeros((h4, w4), dtype=np.float32)
            val0_f = np.zeros((h, w, 1), dtype=np.float32)
            val1_f = np.zeros((h, w, 1), dtype=np.float32)
            gw0_f, gw1_f = i0, i1

        s3_in = make_s3_nhwc(i0, i1, fl0_uv, fl1_uv, mv0_q, mv1_q, val0_q, val1_q)
        w5 = run_htp_sim(
            qairt, py_exe, s3_info["bin"], s3_info["cfg"], "feat24",
            s3_in, work_root / f"eval_{arch}_s3_{mtag}",
        ).reshape(1, 272, 480, 5)[0, :270, :, :]
        w5_f = bilinear_sample_uv(w5, uu, vv)
        cand0 = gw0_f * (1.0 - val1_f) + gw1_f * val1_f
        cand1 = gw1_f * (1.0 - val0_f) + gw0_f * val0_f
        pred = (
            w5_f[..., 0:1] * cand0
            + w5_f[..., 1:2] * cand1
            + w5_f[..., 2:3] * ml0_f
            + w5_f[..., 3:4] * ml1_f
            + 0.5 * w5_f[..., 4:5] * (i0 + i1)
        )
        res[mtag] = psnr(pred[sl], imid[sl])

    print(
        f"EVAL {arch} (dx=+32,dy=-16): MLwarp={p_ml0:.2f}/{p_ml1:.2f} dB | "
        f"ML-only={res['ml_only']:.2f} dB | GameMV+ML={res['gamemv_ml']:.2f} dB",
        flush=True,
    )


def main():
    ap = argparse.ArgumentParser(description="Compile FSR 4.0.2 ML FG (mlfi_s1, mlfi_s3) QNN HTP context binaries.")
    ap.add_argument("--qairt", required=True, type=pathlib.Path, help="QAIRT 2.47.0.260601 SDK root")
    ap.add_argument("--dll", required=True, type=pathlib.Path, help="FidelityFX SDK v2.4.0 amd_fidelityfx_framegeneration_vk.dll")
    ap.add_argument("--out", required=True, type=pathlib.Path, help="Output root (receives v73/, v79/, v81/)")
    ap.add_argument("--work", type=pathlib.Path, default=None, help="Intermediate work directory (default: <out>/work)")
    ap.add_argument("--mode", choices=["w8a16", "fp16", "w8a8"], default="w8a16")
    ap.add_argument("--archs", default="v73,v79,v81", help="Comma-separated HTP targets (v73,v79,v81)")
    ap.add_argument("--stages", default="s1,s3", help="Comma-separated stages (s1,s3)")
    ap.add_argument("--vtcm-mb", type=int, default=8)
    ap.add_argument("--python", type=pathlib.Path, default=pathlib.Path(sys.executable), help="Python 3.10 executable for qnn-onnx-converter")
    ap.add_argument("--eval", action="store_true", help="Run HTP simulator end-to-end PSNR verification")
    args = ap.parse_args()

    work = args.work or (args.out / "work")
    work.mkdir(parents=True, exist_ok=True)
    archs = [a.strip() for a in args.archs.split(",") if a.strip()]
    stages = [s.strip() for s in args.stages.split(",") if s.strip()]
    for a in archs:
        if a not in ARCH_SOC:
            raise ValueError(f"Unsupported HTP arch: {a} (expected one of {list(ARCH_SOC)})")

    onnx_map = extract_onnx(args.dll, work / "onnx", verify_sha256=True, stages=stages)
    calib_map = {}
    if args.mode != "fp16":
        calib_map = generate_calib_sets(work / "calib", stages=stages)

    results = {}
    for st in stages:
        results[st] = build_stage(
            args.qairt, args.python, st, onnx_map[st], calib_map.get(st),
            work, args.out, args.mode, archs, args.vtcm_mb,
        )

    if args.eval and "s1" in results and "s3" in results:
        for arch in archs:
            eval_pair(args.qairt, args.python, results["s1"][arch], results["s3"][arch], work, arch)


if __name__ == "__main__":
    main()
