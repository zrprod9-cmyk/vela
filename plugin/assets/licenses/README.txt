Licenses of everything in this zip (Vela 0.8.16). Every file of the zip is listed here.

upscale/plugin.json, package.json, main.py, py_modules/*.py, dist/index.js
                              the Vela plugin itself: GPL-3.0-only (../../LICENSE = Vela-plugin-GPL-3.0.txt). Started from the
                              Decky plugin template (BSD-3-Clause, notice kept in decky-plugin-template-BSD-3-Clause.txt).
upscale/assets/sgsr3/OptiScaler.dll   our fork of OptiScaler (Sloptiscaler f80e4da3, ARM64EC build; md5 3667e416...):
                              GPL-3.0 (OptiScaler-GPL-3.0.txt). Changes: the patches 0001-0008 and 0010-0020 of the Vela
                              repository (fork/patches); upstream https://github.com/justradical/Sloptiscaler,
                              https://github.com/optiscaler/OptiScaler. Inside it: fakenvapi (MIT, fakenvapi-MIT.txt),
                              LatencyFleX (Apache-2.0, LatencyFleX-Apache-2.0.txt), Snapdragon Game Super Resolution
                              (BSD-3-Clause, SGSR-BSD-3-Clause-Qualcomm.txt), FidelityFX-derived shader parts (MIT, FidelityFX-MIT.txt).
upscale/assets/sgsr3/OptiScaler.ini   the fork's settings template: GPL-3.0, as OptiScaler.
upscale/assets/sgsr3/VERSION.json     what each binary is (md5, source): part of the plugin, GPL-3.0-only.
upscale/assets/sgsr3/OptiPatcher.asi  OptiPatcher v0.42: MIT (OptiPatcher-MIT.txt).
upscale/assets/sgsr3/amd_fidelityfx_dx12.dll  AMD FidelityFX SDK 1.1.4, signed by AMD: MIT (FidelityFX-MIT.txt).
upscale/assets/sgsr3/amd_fidelityfx_upscaler_vk.dll  the Vulkan FSR 4 provider (md5 9aa4f51d...), built from
                              https://github.com/dvj5411/fsr4vk @391f62b + Vela changes: GPL-3.0 (fsr4vk/GPL-3.0.txt; the
                              provider code of fsr4vk and Vela is GPL-3.0, not MIT); inside: AMD FSR 4.1.1 INT8 and 4.1.2 Lite
                              shaders/weights (MIT, fsr4vk/AMD-FidelityFX-SDK-MIT.md; no FSR 4.0.2), FFX API headers (MIT,
                              fsr4vk/AMD-FidelityFX-SDK-2.0.0-MIT.md), Zstandard (BSD, fsr4vk/Zstandard-BSD.txt)
                              fsr4-shader-performance-overrides: GPL-2.0-or-later (fsr4-sr4-kernels-GPL-2.0-or-later.txt).
upscale/assets/staticfsr/dxgi.dll     the Resident Evil 4 FSR 2 interceptor (ours, with Microsoft Detours, MIT): GPL-3.0-only.
upscale/assets/npu/libVkLayer_VELA_npu.so, VkLayer_VELA_npu.json  our Vulkan layer that hands the FSR 4 network to the NPU:
                              GPL-3.0-only, as the plugin.
upscale/assets/licenses/*     these texts.
Not in this zip: the Vela NPU system part (install-vela-npu.sh, its own release asset vela-npu-sysext-12.raw, which carries the
Qualcomm AI Engine Direct SDK runtime 2.47.0.260601, redistributed by the Vela author, with Qualcomm's licence and Notice File
in /usr/share/licenses/armada-vela-npu/) and any Qualcomm QNN runtime file.
Detours-MIT.txt               Microsoft Detours (MIT), linked into upscale/assets/staticfsr/dxgi.dll
decky-api-1.1.3-LGPL-2.1.txt   @decky/api 1.1.3 (LGPL-2.1), bundled into upscale/dist/index.js
react-icons-5.7.0-MIT.txt      react-icons 5.7.0 (MIT), bundled into upscale/dist/index.js; its notice also lists the icon sets and their licences
GPL-2.0.txt                    the full GNU GPL version 2 text (the SR4 kernels in the FSR 4 provider are GPL-2.0-or-later)
Lucide-ISC.txt                 Lucide (ISC), the LuScanEye icon from react-icons/lu, bundled into upscale/dist/index.js
