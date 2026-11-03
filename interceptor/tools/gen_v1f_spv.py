"""SGSR 3 v1f SPIR-V for the interceptor: writes src/SGSR3_V1f_spv.h from shaders.

    python tools/gen_v1f_spv.py --glslang <glslangValidator> --shaders <shader tree> [--wg 16x16]

Same shaders and defines as the fork's gen_v1f_spv.py (our own shaders,
sgsr3/sgsr3f_*), but emits only the variants this interceptor uses: fullscreen.vert, convert (+lock),
and the upscale sets frag / fragc / fragcl / comp for the three bit sets. No GPL, no NVIDIA source.
The shader tree (fullscreen.vert and sgsr3/sgsr3f_*) is not part of this repository; the generated
src/SGSR3_V1f_spv.h is.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SETS = [("v1a", "0x3fff"), ("v1b", "0x3dff"), ("v1c", "0x3dbf")]


def compile_(glslang, src, defs, tmp):
    out = os.path.join(tmp, "o.spv")
    r = subprocess.run([glslang, "-V", "--target-env", "vulkan1.1"] + defs + [src, "-o", out],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{src} {defs}: {r.stdout}{r.stderr}")
    data = open(out, "rb").read()
    return struct.unpack(f"<{len(data) // 4}I", data)


def arr(name, words):
    lines = [f"static const uint32_t {name}[{len(words)}] = {{"]
    for i in range(0, len(words), 12):
        lines.append("    " + ", ".join(f"0x{w:08x}" for w in words[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glslang", required=True)
    ap.add_argument("--wg", default="16x16")
    ap.add_argument("--shaders", required=True, help="shader tree holding fullscreen.vert and sgsr3/sgsr3f_* (not part of this repository)")
    a = ap.parse_args()
    sh = a.shaders
    wx, wy = (int(v) for v in a.wg.split("x"))
    fork = ["-DSGSR3_FORK_PARAMS"]
    P = ["// Generated SPIR-V (tools/gen_v1f_spv.py) of the Vela SR 3 v1f shaders; the shader sources are not part of this repository. Do not edit.",
         "#pragma once",
         "#include <cstddef>", "#include <cstdint>", "",
         "struct V1fSpv { const uint32_t* spv; size_t bytes; };",
         f"static constexpr uint32_t V1fWgX = {wx}, V1fWgY = {wy};", ""]
    conv = os.path.join(sh, "sgsr3", "sgsr3f_convert.frag")
    up = os.path.join(sh, "sgsr3", "sgsr3f_upscale.frag")
    upc = os.path.join(sh, "sgsr3", "sgsr3f_upscale.comp")
    # F_Q10 1 (same as the fork's gen_v1f_spv.py): A2B10G10R10 targets
    # are written with explicit rounding to nearest -- Turnip truncates on the render-target write and the
    # history drifts down ~1 LSB a frame (chroma shift, weak accumulation). Applied to the lock convert and
    # every compact (A2B10G10R10) upscale; the RGBA16F (frag) and compute variants do not write 10-bit.
    Q = ["-DF_Q10=1"]
    with tempfile.TemporaryDirectory() as tmp:
        P.append(arr("v1f_spv_fullscreen_vert", compile_(a.glslang, os.path.join(sh, "fullscreen.vert"), [], tmp)))
        P.append(arr("v1f_spv_convert_frag", compile_(a.glslang, conv, fork, tmp)))
        P.append(arr("v1f_spv_convert_lock_frag", compile_(a.glslang, conv, fork + ["-DF_LOCK=1"] + Q, tmp)))
        for name, fl in SETS:
            fl_nw = hex(int(fl, 16) & ~0x200)  # low-frequency lock replaces the wide box
            P.append(arr(f"v1f_spv_up_{name}_frag", compile_(a.glslang, up,
                     fork + ["-DF_SCENE_STORE=1", f"-DSGSR3_FIXED_FLAGS={fl}"], tmp)))
            P.append(arr(f"v1f_spv_up_{name}_fragc", compile_(a.glslang, up,
                     fork + ["-DF_SCENE_STORE=1", "-DF_HSPLIT=1", "-DF_WORKS=0", "-DF_LF=2", "-DF_F8=0.8",
                             f"-DSGSR3_FIXED_FLAGS={fl_nw}"] + Q, tmp)))
            P.append(arr(f"v1f_spv_up_{name}_fragcl", compile_(a.glslang, up,
                     fork + ["-DF_SCENE_STORE=1", "-DF_HSPLIT=1", "-DF_WORKS=0", "-DF_LF=2", "-DF_F8=0.8", "-DF_LOCK=1",
                             f"-DSGSR3_FIXED_FLAGS={fl_nw}"] + Q, tmp)))
            P.append(arr(f"v1f_spv_up_{name}_comp", compile_(a.glslang, upc,
                     fork + [f"-DSGSR3_FIXED_FLAGS={fl}", f"-DF_WGX={wx}", f"-DF_WGY={wy}"], tmp)))
    for kind in ("frag", "fragc", "fragcl", "comp"):
        P.append(f"static const V1fSpv v1f_upscale_{kind}[3] = {{")
        for name, _ in SETS:
            P.append(f"    {{ v1f_spv_up_{name}_{kind}, sizeof(v1f_spv_up_{name}_{kind}) }},")
        P.append("};")
    out = os.path.join(HERE, "..", "src", "SGSR3_V1f_spv.h")
    with open(os.path.normpath(out), "w", newline="\n") as f:
        f.write("\n".join(P) + "\n")
    print("wrote", os.path.normpath(out), os.path.getsize(os.path.normpath(out)), "bytes")


if __name__ == "__main__":
    main()
