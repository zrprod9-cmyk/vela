"""Pack the built plugin into out/upscale.zip for Decky's "Install Plugin from ZIP File".

Run `pnpm build` first. zipfile writes forward-slash paths, which Decky on Linux needs.
The Vela NPU layer (assets/npu, built by npu-layer/build.sh of the Vela repository) is required; `--no-npu` packs
without it for testing (the panel then shows "NPU" disabled), out/upscale-nonpu.zip.
"""
import argparse
import json
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "upscale"
FILES = ["plugin.json", "package.json", "main.py", "LICENSE", "dist/index.js"]
# SGSR 3 in the game: our OptiScaler build, its ini template, OptiPatcher, FidelityFX SDK 1.1.4 for frame generation
# (assets/sgsr3/VERSION.json says which); the licenses of all of them
ASSETS = ["assets/sgsr3/OptiScaler.dll", "assets/sgsr3/OptiScaler.ini", "assets/sgsr3/OptiPatcher.asi",
          "assets/sgsr3/VERSION.json", "assets/sgsr3/amd_fidelityfx_dx12.dll",
          "assets/licenses/README.txt", "assets/licenses/OptiScaler-GPL-3.0.txt"]
# 0.8.13: the any-game layer (Vela Any, assets/layer) left the plugin; a game that still has it is turned off by the
# panel (anygame.py disable needs only its own state, not the .so)
SKIP_DIRS = ("assets/layer",)
# 0.8.0: the FSR 4 network on the NPU: our layer VK_LAYER_VELA_npu and its manifest
NPU_ASSETS = ["assets/npu/libVkLayer_VELA_npu.so", "assets/npu/VkLayer_VELA_npu.json"]
NPU_LIB = NPU_ASSETS[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--no-npu", action="store_true", help="pack without the NPU layer .so (testing only)")
    args = ap.parse_args(argv)
    if not os.path.exists(os.path.join(HERE, "dist", "index.js")):
        raise SystemExit("dist/index.js is missing, run `pnpm build` first")
    version = json.load(open(os.path.join(HERE, "package.json"), encoding="utf-8"))["version"]
    if f'"{version}"'.encode() not in open(os.path.join(HERE, "dist", "index.js"), "rb").read():
        raise SystemExit(f"dist/index.js does not carry version {version} (src/strings.ts VERSION), run `pnpm build`")
    os.makedirs(os.path.join(HERE, "out"), exist_ok=True)
    target = os.path.join(HERE, "out", f"{NAME}-nonpu.zip" if args.no_npu else f"{NAME}.zip")
    paths = list(FILES)
    for asset in ASSETS:
        if not os.path.isfile(os.path.join(HERE, asset)):
            raise SystemExit(f"{asset} is missing (assets/sgsr3/VERSION.json lists the files and their sources)")
    for asset in NPU_ASSETS:
        if not os.path.isfile(os.path.join(HERE, asset)) and not (args.no_npu and asset == NPU_LIB):
            raise SystemExit(f"{asset} is missing: build the Vela NPU layer (npu-layer/build.sh), copy it to "
                             f"assets/npu/, or pass --no-npu for a test build without it")
    for top in ("py_modules", "assets"):
        for root, dirs, files in os.walk(os.path.join(HERE, top)):
            dirs[:] = [d for d in dirs if d != "__pycache__"
                       and os.path.relpath(os.path.join(root, d), HERE).replace(os.sep, "/") not in SKIP_DIRS]
            paths += [os.path.relpath(os.path.join(root, f), HERE).replace(os.sep, "/") for f in files if not f.endswith(".pyc")]
    if args.no_npu:
        paths = [p for p in paths if p != NPU_LIB]  # a stray .so must not slip into a "without" build
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in paths:
            z.write(os.path.join(HERE, rel), f"{NAME}/{rel}")
    print(f"{target}  v{version}  {len(paths)} files  {os.path.getsize(target) // 1024} KB"
          f"{'  WITHOUT the NPU layer (test build)' if args.no_npu else ''}")


if __name__ == "__main__":
    main()
