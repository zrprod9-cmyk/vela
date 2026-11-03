# SPIR-V headers of the provider overlay

The provider embeds compute kernels as C++ headers of SPIR-V words (`overlay/ov`, `overlay/ov2`, `overlay/gen`). This folder holds the sources and
the build steps of all of them except the SR4 kernels, which have their own folder (`../sr4-kernels/`).

| header | what it holds | source here | build |
|---|---|---|---|
| `overlay/ov/fv_spv.hpp` | NPU tensor transfer and the per-frame GPU/NPU select | `fv/*.comp` (ours, GPL-3.0) | `build.sh` |
| `overlay/gen/fsr412lite-npu-spv.hpp` | pack / unpack / select / reference kernels of the FSR 4.1.2 Lite NPU path | `npu/*.comp` (ours, GPL-3.0) | `build.sh` |
| `overlay/gen/fsr4sr4-spv.hpp` | skipped-frame kernels | `../sr4-kernels/` (GPL-2.0-or-later) | `../sr4-kernels/build.sh` |
| `overlay/ov/vela-fast-spv.hpp` | FSR 4 Fast prepass and K2 variants, fused 1.5x K2, separate 1.5x passes | `fast/*.comp`, `fast/*.spvasm` | `build_amd.sh` |
| `overlay/gen/fsr4fu-spv.hpp` | fused prepass + K1 kernels and the select kernel of the Lite NPU path | `fused/npu_select.comp`, `lite/pre_lds.comp`, `lite/b1_0009.comp`, generator `tools/gen_fu.py` | `build_amd.sh` |
| `overlay/ov2/fsr412lite-blobs.hpp` | the FSR 4.1.2 Lite passes (SPIR-V), the Lite weights and the pass table | AMD DLL (fetched), `lite/`, `tools/` | `build_amd.sh` |

## Licences of the sources
- `fv/`, `npu/`, `fused/npu_select.comp`, `tools/`, `amd/*.sh`, `amd/*.py`: ours, GPL-3.0.
- `fast/`, `lite/*.comp`, `lite/converted/`: GLSL and SPIR-V assembly derived from AMD's FSR 4.1.1 INT8 and FSR 4.1.2 Lite compute shaders, which are MIT
  licensed (FidelityFX SDK). Our changes are GPL-3.0. The MIT notice travels with them: `amd/NOTICE.md` (full text in
  `../../plugin/assets/licenses/fsr4vk/AMD-FidelityFX-SDK-MIT.md`). Each `.comp` file carries a pointer to `amd/NOTICE.md` in its first lines.
- AMD's own files are not copied into this repository. `amd/fetch.sh` downloads the one input that is needed (below).

## Build and check: our own kernels (`build.sh`)
`sh build.sh` (Docker and network needed) compiles `fv/*.comp` and `npu/*.comp` in `ubuntu:24.04` with the pinned Ubuntu packages
`glslang-tools 15.1.0-2~ubuntu0.24.04.2` and `spirv-tools 2025.1~rc1-1~ubuntu0.24.04.2`, writes both headers with `tools/gen_hpp.py` to `out/`
and compares them with the copies in `../overlay`. The last lines of `out/build.log` must read `fv_spv.hpp IDENTICAL` and
`fsr412lite-npu-spv.hpp IDENTICAL`.

## Build and check: the AMD-derived kernels (`build_amd.sh`)
`sh build_amd.sh [input dir]` (Docker and network needed; same pinned packages, `ubuntu:24.04`) does this:

1. `amd/fetch.sh` downloads AMD's signed Lite provider, **`amd_fidelityfx_upscaler_vk.dll` from the AMD FidelityFX SDK release `v2.4.0`**
   (https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, path `Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll`, 36 135 696 bytes,
   SHA-256 `4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7`) and checks the hash. This is the only AMD file the build needs.
2. `amd/extract_lite.py` pulls the 13 SPIR-V modules of the Lite passes and the 27 392-byte Lite weight block out of the DLL (modules and weights
   are named by SHA-256 in `amd/lite_modules.tsv`).
3. FSR 4 Fast kernels: `glslangValidator -V --target-env vulkan1.1` of `fast/*.comp`; `spirv-as --target-env vulkan1.1` of `fast/plk_*.spvasm`.
4. Lite modules, as in the pass table of `lite/pass_table.inc`:
   - 0001, 0003, 0004, 0006, 0008, 0010: AMD's modules after `tools/xf.sh` (signed dot product to mixed-sign dot product, constant Z).
   - 0000 and the pre-pass of the skip path: `tools/p0.sh` on `lite/pre_lds.comp`, `lite/pre_r2c.comp`.
   - 0009 and 0013: `tools/p.sh` (glslang, `sudot.py`, `zconst.py`, then `tools/wl3.sh`: unroll, constant folding, weights loads replaced by literals with
     `wlit.py`, `cres.py`, `kfold.py`) on `lite/b1_0009.comp`, `lite/k2cl.comp`.
   - 0002, 0005, 0007: `tools/gen_conv_p1.py` / `tools/gen_conv_px.py` generate GLSL from the converted AMD modules in `lite/converted/` and the weights;
     `tools/conv.sh` compiles them.
5. Fused kernels: `tools/gen_fu.py` edits `lite/pre_lds.comp`, `lite/b1_0009.comp` and `fused/npu_select.comp` into the fused kernels, which go through the same
   `p0.sh` / `p.sh` steps.
6. `tools/gen_amd_hpp.py` writes the three headers; the script compares them with the copies in `../overlay`.

The last lines of `out/build_amd.log` must read `IDENTICAL` three times.

### What the sources are
- `fast/*.comp` and `lite/*.comp` are the GLSL that was compiled; they are the form in which the kernels are modified. They started as decompilations
  (SPIRV-Cross) of AMD's SPIR-V modules and were then edited by hand and by generator scripts. The decompiled starting points of 0002, 0005 and 0007 are in
  `lite/converted/` (generated from AMD's modules after `xf.sh`); the other starting points are not kept, only their results.
- The FSR 4.1.1 Fast kernels derive from the pass-01 prepass and K2 modules of the FSR 4.1.1 INT8 bundles in `../vela_assets/fsr411/`. Those bundles are AMD's
  FSR 4.1.1 shaders (SDK 2.3.0, `amd_fidelityfx_upscaler_dx12.dll`) translated to SPIR-V by vkd3d-proton; they are in this repository already, so
  no download is needed for the Fast kernels.
- `lite/pass_table.inc` is the pass table (entry points, descriptor layouts, dispatch sizes, per-pass constants) of the Lite provider as recorded from a
  run of AMD's DLL with a Vulkan capture layer on a GPU. It is data of ours (GPL-3.0); recording it again needs a GPU and the layer, which are not part of this repository.
