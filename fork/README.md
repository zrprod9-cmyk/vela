# Vela's OptiScaler fork (GPL-3.0)

Base: [justradical/Sloptiscaler](https://github.com/justradical/Sloptiscaler) commit `f80e4da339cd4624212fd0546922571fe01205c1`
= ARM64EC build of [OptiScaler](https://github.com/optiscaler/OptiScaler) 10.0.0.1 with an SGSR 2 backend.
Our changes are the patches in `patches/`, applied in number order. License: GPL-3.0 (`LICENSE`, OptiScaler's).

The `OptiScaler.dll` in Vela 0.8.19 (md5 `3667e416ac97748dd27431b00e31a4cc`, 18 450 432 bytes) was built from the base +
**0001–0008 and 0010–0020** (patch 0009 and the experimental upstream-sync patch 0000 are not part of this build and are not
included). Checksums of the patch files: `patches.md5`.

| patch | what |
|---|---|
| 0001 | SGSR 2 input dumper; motion-vector scale and sampling fixes for full-resolution MV |
| 0002–0005 | SGSR 3 upscaler (D3D12), pixel-shader path, A/B timing; Vulkan interop through vkd3d-proton (student network, fp16 v1f) |
| 0006 | SGSR-FG frame generation passes (uses OptiScaler's FFX swapchain) |
| 0007–0008 | plugin build: temporal box, history rounding fixes (large: embedded shader bytecode) |
| 0010 | Vela live settings (model switch while the game runs) |
| 0011–0016 | FSR 4 bridge to the Vulkan FSR 4 provider (`Fsr4VkBridge.h`), live sub-rect / SGSR 2, NPU keys, FSR 3 through FFX, any render size |
| 0017 | FSR 4: depth flip, heaps, SGSR 2 fallback |
| 0018 | model `fsr4fast` (FSR 4 Fast) |
| 0019 | MV pass: converts display-size / jittered motion vectors (DLSS input) to the render-size, non-jittered vectors FSR 4 needs |
| 0020 | GPU identification without a held lock: fixes the start hang on Snapdragon 8 Elite; `VELA_OPTI_GPUTHREAD=0` turns the early identification off, `VELA_OPTI_LOGLEVEL=<n>` sets the log level |

## Build

Needs Docker on an x86-64 host and network access (GitHub, Docker Hub, Ubuntu mirrors). From the repository root:

    sh fork/build/build.sh out          # -> out/OptiScaler.dll (IMAGE_FILE_MACHINE_ARM64EC), out/build.log

`build.sh` starts `fork/build/build_in_container.sh` in a throw-away Docker volume. The script checks `patches.md5`,
clones Sloptiscaler at the base commit with its submodules, applies the patches with `git apply --ignore-whitespace`
(upstream files use CRLF) and runs upstream's `arm64ec/build.sh`, which cross-compiles with llvm-mingw.

Pinned inputs:

| input | pin |
|---|---|
| container image | `ubuntu:22.04@sha256:0a9887931b0e38af2e3487ed39e29436b0535c4f3f52e57745baaf79d1898956` (host tools there: cmake 3.22.1, ninja 1.10.1, git 2.34.1, Python 3.10.12; they do not produce code) |
| compiler | llvm-mingw 20260908 (ucrt, ubuntu-22.04-x86_64), tarball sha256 `2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092` |
| sources | Sloptiscaler `f80e4da3` plus its submodules; DirectXMath `f5d82574b03417d905fdd2f85bfa9bf7a2f1deac`; Detours `1dbd4ea281d73d478aa8d09a8b83fbd265f715f7`; freetype tag `VER-2-13-3` |
| source path | the tree is built at `/work/src`; the compiler stores source paths in the objects, so another path changes the DLL |
| build date | OptiScaler stores it in its version resource: today's date, or `FORK_BUILD_DATE=YYYY-MM-DD` |

### Reproducing the release DLL

The `OptiScaler.dll` of Vela 0.8.19 (md5 `3667e416ac97748dd27431b00e31a4cc`) is this build with

    FORK_BUILD_DATE=2026-11-02 sh fork/build/build.sh out

A rebuild from a clean clone is byte-identical to the release DLL except two timestamps: the linker's PE timestamp
(`TimeDateStamp` in the COFF header) and the timestamp of the debug directory entry. With those two fields set to the
release values the md5 is the same.

By hand: clone Sloptiscaler, `git checkout f80e4da3`, `git submodule update --init --recursive`, `git apply --ignore-whitespace`
each patch in order, run `./arm64ec/build.sh <deps-dir>` from a checkout located at `/work/src`.
