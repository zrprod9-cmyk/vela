# RE4 interceptor: replacing a game's built-in FSR 2 at the graphics-API level

Resident Evil 4 (RE Engine, Steam AppId 2050650) has FSR 2 (FidelityFX) **compiled into the exe**: there is no
`ffx_fsr2_api_*.dll` or `amd_fidelityfx_*.dll` for OptiScaler to replace. This interceptor finds the game's FSR 2 passes
through D3D12 and draws the frame with a Vela model instead (FSR 4.1, FSR 3.1 or SGSR 2). Nothing on disk is patched,
there is no DRM / anti-tamper work: only graphics-API hooks inside the game process, a single-player game without
anti-cheat. The Vela plugin installs it for RE4 (profile "static FSR 2") as `dxgi.dll` next to the game's exe.

## Why a D3D12 hook and not a Vulkan layer

1. The bridge to our upscalers already lives in D3D12: inputs come as `ID3D12Resource`, the compute work runs through the
   vkd3d-proton interop (`ID3D12DXVKInteropDevice1`). A Vulkan layer would see `VkImage`s of vkd3d's device and need a
   separate path.
2. A stable signature: a shader is identified by the hash of its DXIL container, which is baked into the game build. The
   SPIR-V hash after vkd3d changes with the vkd3d version.
3. The injection is the same one the plugin already uses (`dxgi.dll` in the game folder). No adapter / NVAPI spoofing is
   needed: the game offers FSR on Adreno as it is.

## How FSR 2 is recognised without reading the exe

- Hooks on `D3D12CreateDevice` and the device vtable (`CreateComputePipelineState`, descriptor heap and view creation,
  `CreateCommittedResource` / `CreatePlacedResource`) and on the command list vtable (`SetPipelineState`,
  `SetDescriptorHeaps`, `SetComputeRoot*`, `Dispatch`).
- **By content:** at `CreateComputePipelineState` the DXIL hash is computed (`src/dxil.h`) and its ASCII strings are read;
  when reflection is kept they are FSR 2's own resource names (`r_input_color_jittered`, `r_input_depth`,
  `r_input_motion_vectors`, …).
- **By the dispatch shape:** the upscale pass writes a UAV at the output size while reading SRVs at the render size.
- RE4 prepares FSR 2's constant buffer on the GPU, so its bytes (jitter, pre-exposure) are read by a GPU copy into a
  readback buffer; `JitterTrack` matches the jitter against FSR 2's Halton(2,3) table and keeps the phase. Until the phase
  is locked (2 matching frames), and after 2 misses in a row, the frame is drawn by the game's own FSR 2.

## Models (Vela 0.8.16)

The same live file as the games that go through OptiScaler (`/tmp/vela-live/<appid>.ini`, written by the panel) or
`VELA_FSR2_ALGO=fsr4|fsr3`:

- **FSR 4.1** — the fsr4vk provider (`amd_fidelityfx_upscaler_vk.dll` next to our DLL) on vkd3d-proton's VkDevice through
  the same interop; the NPU path (`Vela NPU: ...` log lines) comes from the provider.
- **FSR 3.1** — `amd_fidelityfx_dx12.dll` (FidelityFX SDK) with its D3D12 backend in the game's command list; its own
  dispatches bypass our hooks.
- **SGSR 2** — Qualcomm's shaders (`shaders/sgsr2_*.comp`, `src/sgsr2_vk.cpp`, licence `shaders/SGSR2_LICENSE`).
- Input: a reversible tone map (`src/hdr_wrap.cpp`, `shaders/hdr_wrap.comp`), output written into the game's target in its
  own format.

Log lines the panel reads (`sgsr3intercept.log` next to the exe): `vela: live (...): model ...`, `vela: jitter locked ...`,
`vela: model <m> runs RxR -> DxD`, `vela: FSR 3.1 context ...`, and on refusal `vela: model <m> is not available here`.

## Using it

- In the game, FSR 2 must be on (RE4: "FidelityFX Super Resolution 2", any mode). The first seconds after loading show the
  game's FSR 2, then the Vela model.
- Debug switches (launch options): `RECON_LEVEL=1` log only; `RECON_LEVEL=2` (default) also allows a one-frame dump on
  request; `RECON_BARRIER=1` state transitions around the copies; `RECON_NOPSV=1` no PSV0 binding filter;
  `RECON_VACHECK=1` address check.
- Undo: the plugin's «Off» restores the game's backup; by hand, remove `dxgi.dll` and restore the launch options.

## Files

| file | what |
|---|---|
| `src/dxil.h` | DXIL container: hash (the shader's signature), resources from PSV0, ASCII names |
| `src/d3d12_recon.cpp` | device / resource / command list hooks, descriptor mirror, root signatures, FSR 2 sequence recognition, frame and constant buffer capture |
| `src/dxgi_proxy.cpp`, `src/dllmain.cpp`, `src/dxgi_exports.def` | the `dxgi.dll` proxy to the system DXVK; hooks are set before the device is created |
| `src/vela_fsr2.{h,cpp}` | which FSR passes to skip, input collection, calibration, running the model |
| `src/ext_upscalers.{h,cpp}`, `src/fsr4_warm.h` | FSR 4.1 (fsr4vk provider) and FSR 3.1 (FidelityFX DX12) |
| `src/sgsr2_vk.cpp`, `src/sgsr2_spv.h`, `shaders/sgsr2_*.comp` | SGSR 2 |
| `src/student_vk.*`, `src/sgsr3_fast_vk.*`, `src/contract.h`, `src/sgsr3_constants.h` | the earlier Vela AI / Vela SR models (no longer offered by the panel) |
| `src/gpu_patch.h` | copies the changing fields (jitter, preExposure) from the game's GPU constant buffer into ours |
| `src/SGSR3_V1f_spv.h`, `src/SGSR3_Student_spv*.h` | generated SPIR-V of the Vela SR / Vela AI models (see "Building") |
| `src/hdr_wrap.*`, `src/exposure_fix.*`, `shaders/exposure_div.comp`, `src/misc_spv.h` | reversible tone map; exposure division (off, `VELA_EXPDIV=1`) |
| `src/interop_vkd3d.h`, `src/log.h`, `src/vela_stub.cpp` | vkd3d interop, log, a stub for the PC build |
| `build.sh`, `Dockerfile` | ARM64EC build for the device (Docker or Ubuntu 22.04; downloads the pinned toolchain and sources, see "Building") |
| `build_pc.ps1` | x64 PC build with `tools/fsr2_harness.cpp` (a D3D12 test app with FSR 2.1.2 linked in); not checked for 0.8.16 |
| `tools/` | capture, comparison and SPIR-V generation scripts (`gen_*_spv.py`, `label_dump.py`, `show_inputs.py`, `shadow_eval.py`, `series_eval.py`, `compare_shadow.py`, `hf_by_motion.py`, `match_bursts.py`, `fault_run.sh`, …) |

## Building

The ARM64EC `dxgi.dll` is built with clang from llvm-mingw. `build.sh` downloads its inputs at pinned versions and checks
them: llvm-mingw 20260908 (sha256 `2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092`), Microsoft Detours
(MIT) at commit `1dbd4ea281d73d478aa8d09a8b83fbd265f715f7`, Khronos Vulkan-Headers (Apache-2.0) tag `v1.4.310`
(commit `cacef3039d277c448c89336290ec3937270b0996`). From the root of the repository:

    docker build -t vela-interceptor interceptor
    docker run --rm -v "$PWD/interceptor:/interceptor:ro" -v "$PWD/interceptor-out:/out" -v vela-interceptor-deps:/deps vela-interceptor

The result is `interceptor-out/arm64ec/dxgi.dll` (on Windows with Git Bash put `MSYS_NO_PATHCONV=1` in front of
`docker run`). Without Docker, on an x86-64 Linux with bash, curl, git, xz and tar: `DEPS=$HOME/vela-deps OUT=$PWD/out bash interceptor/build.sh`.
Two builds of the same sources differ only in the PE and debug-directory timestamps. The release zip carries
`upscale/assets/staticfsr/dxgi.dll` md5 `7b476fc53bd73441134a6c5f1d5a2b35` (3,448,320 bytes), built this way from this folder.

The SGSR 3 / Vela AI headers (`src/SGSR3_V1f_spv.h`, `src/SGSR3_Student_spv.h`, `src/SGSR3_Student_spv_fallback.h`) are added as finished files because the shader sources and the Student recipe they are generated from are not public.

Dependencies: Microsoft Detours (MIT, `LICENSE-Detours-MIT.txt`) and the system dxguid / ole32 / uuid. The interceptor's own
code is GPL-3.0-only, like the rest of Vela (see the repository's LICENSING.md).
