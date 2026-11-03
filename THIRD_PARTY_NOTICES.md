# Third-party notices: Vela 0.8.18 (`vela-0.8.18.zip`, md5 6bf61d665bde0bad7434cee4d2db677b)

Every file in the release zip, who wrote it and under which licence. Licence texts are inside the zip in
`upscale/assets/licenses/` (paths below are zip paths). "Ours" = written by the Vela developer.

## Binaries and data put into games or loaded by the system

| zip path | md5 | what | origin | licence | source / notes |
|---|---|---|---|---|---|
| `upscale/assets/sgsr3/OptiScaler.dll` | 3667e416ac97748dd27431b00e31a4cc | our OptiScaler fork, ARM64EC; installed into the game as `dxgi.dll` | OptiScaler (optiscaler/OptiScaler) via justradical/Sloptiscaler `f80e4da3` + our patches 0001–0008, 0010–0020 | **GPL-3.0** | `licenses/OptiScaler-GPL-3.0.txt`; corresponding source: `fork/` in this repository |
| — inside that DLL | | fakenvapi | fakenvapi (merged into OptiScaler) | MIT | `licenses/fakenvapi-MIT.txt` |
| — inside that DLL | | LatencyFleX header / logic | Tatsuyuki Ishi | Apache-2.0 | `licenses/LatencyFleX-Apache-2.0.txt` |
| — inside that DLL | | Snapdragon Game Super Resolution (SGSR 2) and our SGSR-derived kernels | Qualcomm | BSD-3-Clause | `licenses/SGSR-BSD-3-Clause-Qualcomm.txt` |
| — inside that DLL | | FidelityFX-derived shader parts (frame-interpolation headers, FSR 2 parts upstream OptiScaler carries) | AMD FidelityFX SDK 1.1.4 | MIT | `licenses/FidelityFX-MIT.txt` |
| — inside that DLL | | NVIDIA NGX SDK headers that upstream OptiScaler builds with | NVIDIA | NVIDIA SDK terms, as in upstream OptiScaler | not added by our patches; `fork/` holds only our patches and no NVIDIA file |
| `upscale/assets/sgsr3/amd_fidelityfx_upscaler_vk.dll` | 9aa4f51d3fcfb6c5b2c963fa2ccba038 | FSR 4 provider for Vulkan (FSR 4.1.1 INT8 network, FSR 4 Fast, FSR 4.1.2 Lite, NPU path); contains no FSR 4.0.2 material | built by us from dvj5411/fsr4vk @391f62b + our overlay | **GPL-3.0** (fsr4vk's own code and our changes; only fsr4vk's AMD parts are MIT); AMD FSR 4.1.1 / 4.1.2 Lite shaders and weights: MIT per AMD FidelityFX SDK 2.3.0 / AMD FSR SDK 2.4.0 (`licenses/fsr4vk/AMD-FidelityFX-SDK-MIT.md`, text below); FFX API headers: AMD MIT (`licenses/fsr4vk/AMD-FidelityFX-SDK-2.0.0-MIT.md`); SR4 kernels: GPL-2.0-or-later (`licenses/fsr4-sr4-kernels-GPL-2.0-or-later.txt`, full text `licenses/GPL-2.0.txt`); Zstandard: BSD (`licenses/fsr4vk/Zstandard-BSD.txt`) | corresponding source and build steps: `provider/` in this repository (README there) |
| `upscale/assets/sgsr3/amd_fidelityfx_dx12.dll` | 49230ad94ed6169933cd0457501d9cb4 | AMD FidelityFX SDK 1.1.4 DX12 runtime (file version 1.0.1.41314), AMD-signed, unmodified | AMD | MIT | `licenses/FidelityFX-MIT.txt` |
| `upscale/assets/sgsr3/OptiPatcher.asi` | 270137987e5f58b0cb371a6e38c2fedb | OptiPatcher v0.42 (44b8106, x64), unmodified | optiscaler/OptiPatcher | MIT | `licenses/OptiPatcher-MIT.txt` |
| `upscale/assets/sgsr3/OptiScaler.ini` | 0c456787… | OptiScaler config template of the fork | OptiScaler | GPL-3.0 | as the DLL |
| `upscale/assets/staticfsr/dxgi.dll` | 7b476fc53bd73441134a6c5f1d5a2b35 | Resident Evil 4 interceptor of the game's built-in FSR 2 (ARM64EC) | ours; links Microsoft Detours | ours: GPL-3.0-only (`LICENSING.md`); Detours: MIT (`licenses/Detours-MIT.txt`) | source: `interceptor/` |
| `upscale/assets/npu/libVkLayer_VELA_npu.so` | 2f438a25a73197d8eacb5b5422416286 | Vulkan layer that sends the FSR 4 network to the NPU daemon (aarch64 Linux) | ours; Khronos Vulkan headers | ours: GPL-3.0-only; Vulkan headers: Apache-2.0 / MIT | source: `npu-layer/` |
| `upscale/assets/npu/VkLayer_VELA_npu.json` | 82231f80… | layer manifest | ours | GPL-3.0-only, as the plugin | — |

## The plugin itself (ours)

| zip path | licence |
|---|---|
| `upscale/main.py`, `upscale/py_modules/*.py` (anygame, cpuboost, device, gamecfg, gamecheck, gameutil, gpufloor, latency, live, npu, presets, profiles, report, sgsr3, upscale) | GPL-3.0-only (`upscale/LICENSE`) |
| `upscale/dist/index.js` | our panel code (GPL-3.0-only), bundled with `@decky/api` 1.1.3 (LGPL-2.1, SteamDeckHomebrew), the icon component of `react-icons` 5.7.0 (MIT) and one Lucide icon (`LuScanEye`, ISC). `@decky/ui` is provided by Decky Loader at run time and is not bundled. Licence texts: `licenses/decky-api-1.1.3-LGPL-2.1.txt`, `licenses/react-icons-5.7.0-MIT.txt`, `licenses/Lucide-ISC.txt` |
| `upscale/plugin.json`, `upscale/package.json`, `upscale/LICENSE` | ours, GPL-3.0-only (template origin: Steam Deck Homebrew decky-plugin-template, BSD-3-Clause, notice kept in `licenses/decky-plugin-template-BSD-3-Clause.txt`) |
| `upscale/assets/sgsr3/VERSION.json` | ours, GPL-3.0-only (build labels; some labels are older than the files, they do not change behaviour) |

## Not in the zip: the Vela NPU system part

The NPU system extension `vela-npu-sysext-13.raw` (package 13, sha256
`ac9672ba83e20d4e001261f6c22c7ba27f15d93fd9376c22b62bc4ba5e653deb`, its own release asset, installed as
`armada-vela-npu.raw`) is **not** in the release zip. It contains:

- `fsr4npud` (the NPU daemon) and `fsr4npu-check`: ours, GPL-3.0-or-later with a GPL-3.0 section 7 additional permission
  for the Qualcomm runtime (`LICENSING.md`);
- the 7 FSR 4 graphs for HTP v73 and the same 7 for HTP v79: the AMD FSR 4.1.1 network (AMD's MIT licence, notice below), compiled by us with
  Qualcomm's QAIRT tools;
- the FastRPC client and DSP listener (`cdsprpcd`, `libcdsprpc`, `libcdsp_default_listener`): quic/fastrpc `a56e9d4`,
  built from source, BSD-3-Clause, text inside the image;
- the systemd units and drop-ins, the udev rule and the modules-load file: ours;
- the Qualcomm AI Engine Direct (QNN) runtime, see the next section.

Not in the image: `fastrpc_shell_unsigned_3` and the DSP-side `libc++.so.1` / `libc++abi.so.1`; `install-vela-npu.sh`
reads them from the device's own dsp partition.

## Qualcomm AI Engine Direct (QNN) runtime

- **Included** (only inside `vela-npu-sysext-13.raw`, never as a separate download): `libQnnHtp.so`,
  `libQnnHtpV73Stub.so`, `libQnnSystem.so`, `libQnnHtpV73Skel.so`, `libQairtHtpV73Skel.so`, `libQnnHtpV73.so`,
  `libQnnHtpV79Stub.so`, `libQnnHtpV79Skel.so`, `libQairtHtpV79Skel.so`, `libQnnHtpV79.so` from the
  Qualcomm AI Engine Direct SDK (QAIRT) 2.47.0.260601, unmodified.
- **Licence:** Qualcomm AI Stack License, shipped unchanged in the image as
  `/usr/share/licenses/armada-vela-npu/Qualcomm-AI-Stack-LICENSE.pdf`, with Qualcomm's third-party Notice File
  `/usr/share/licenses/armada-vela-npu/QNN_NOTICE.txt` (unchanged).
- **Export control:** the Qualcomm software is subject to export-control and sanctions laws; see section 10(f) of
  `Qualcomm-AI-Stack-LICENSE.pdf`.
- Not included: any Qualcomm SDK header or other SDK file. `fsr4npud` loads the runtime under a GPL-3.0 section 7
  additional permission (`LICENSING.md`).

## AMD FSR 4 network and shaders

The FSR 4.1.1 INT8 network comes from the signed `amd_fidelityfx_upscaler_dx12.dll` of AMD FidelityFX SDK 2.3.0, and FSR
4.1.2 Lite from `amd_fidelityfx_upscaler_vk.dll` of AMD FSR SDK 2.4.0; AMD's `docs/license.md` lists both DLLs under the
MIT licence. The provider and the NPU graphs contain no FSR 4.0.2 material. The notice shipped as
`licenses/fsr4vk/AMD-FidelityFX-SDK-MIT.md`:

```
AMD FSR 4 model and shader material in amd_fidelityfx_upscaler_vk.dll

This provider embeds compute shaders and network weights derived from AMD software:
- FSR 4.1.1 INT8: translated (vkd3d-proton SPIR-V) from amd_fidelityfx_upscaler_dx12.dll,
  AMD FidelityFX SDK 2.3.0, Kits/FidelityFX/signedbin/ (commit 60f4ea81909200d8542eca14dccb2628b763a9a3),
  and modified by the Vela and fsr4vk projects.
- FSR 4.1.2 Lite: captured from amd_fidelityfx_upscaler_vk.dll, AMD FSR SDK 2.4.0,
  Kits/FidelityFX/signedbin/ (commit 7539ae8b6568ad29ed78a753404351daf3b5cca7), and modified by the Vela project.
This build contains no FSR 4.0.2 material (Vela release provider, md5 9aa4f51d3fcfb6c5b2c963fa2ccba038).
AMD's SDK license (docs/license.md) lists these signed DLLs under the following license:

Copyright (C) Advanced Micro Devices, Inc.
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. This project is independent and is not
provided, sponsored or endorsed by AMD. The file name amd_fidelityfx_upscaler_vk.dll is required by the FFX loader.

The FFX API headers compiled into the provider (ffx_api.h, ffx_api_types.h, ffx_upscale.h) carry AMD's MIT notice of
AMD FidelityFX SDK 2.0.0: AMD-FidelityFX-SDK-2.0.0-MIT.md next to this file.
```

## Source

Corresponding source of every GPL binary above: the provider in `provider/` (fsr4vk @391f62b without its asset bundles
and validation data, Vela's overlay in `overlay/ov`, `overlay/ov2`, `overlay/gen`, the FSR 4.1.1 bundles it embeds in
`vela_assets/fsr411`, build scripts and README with the build steps), the OptiScaler fork in `fork/`, the RE4 interceptor in
`interceptor/` and the NPU layer in `npu-layer/`. `upscale/assets/licenses/README.txt` in the zip gives the same overview
in short form.
