# Licensing

## OptiScaler fork (`fork/`): GPL-3.0

The fork is a derivative of OptiScaler (GPL-3.0, the `LICENSE` of optiscaler/OptiScaler and of justradical/Sloptiscaler)
and is licensed under GPL-3.0. It is published as patches (since 0.10.0 on OptiScaler `6de775c5`, `fork/base-b/`; up to 0.9.0 on Sloptiscaler `f80e4da3`, `fork/patches/`) plus build steps (`fork/README.md`);
the full licence text is in `fork/LICENSE`. The shipped `OptiScaler.dll` (md5 `d1010f862b5617bb3a9b406840044168`) is built
from these patches.

## The plugin, the RE4 interceptor and the NPU layer: GPL-3.0-only

The plugin (`plugin/`: Python backend, TypeScript panel), the Resident Evil 4 interceptor (`interceptor/`) and the NPU
Vulkan layer (`npu-layer/`) are licensed under GPL-3.0-only. `plugin/LICENSE`, the zip's `upscale/LICENSE` and
`upscale/assets/licenses/Vela-plugin-GPL-3.0.txt` carry the GPL-3.0 text; `package.json` and `plugin.json` say
`GPL-3.0-only`. The plugin started from the Decky plugin template (BSD-3-Clause, Steam Deck Homebrew); that notice is
kept in `assets/licenses/decky-plugin-template-BSD-3-Clause.txt`.

## Vela NPU system part (`npu/`)

- `vela-npu-sysext-18.raw` (sha256 `b4f2f260be4fc2f9339411fafe480ac7ee505caa34e3e9428550a0754e700807`) contains Vela's
  files (the daemon `fsr4npud`, the checker `fsr4npu-check`, systemd units and drop-ins, a udev rule and 17 compiled FSR 4
  graphs, for HTP v73, HTP v79 and optionally HTP v81 when `GRAPHS_V81` is provided),
  quic/fastrpc (BSD-3-Clause, its text is inside the image) and the **Qualcomm AI Engine Direct SDK runtime
  2.47.0.260601**, unmodified, under the Qualcomm AI Stack License: `libQnnHtp.so`, `libQnnHtpV73Stub.so`,
  `libQnnSystem.so` (in `/usr/lib64/vela-npu`), `libQnnHtpV73Skel.so`, `libQairtHtpV73Skel.so`, `libQnnHtpV73.so`
  (in `/usr/lib/rfsa/adsp`), its HTP v79 part: `libQnnHtpV79Stub.so` (in `/usr/lib64/vela-npu`), `libQnnHtpV79Skel.so`, `libQairtHtpV79Skel.so`, `libQnnHtpV79.so` (in `/usr/lib/rfsa/adsp`), its HTP v75 part
  (Snapdragon 8 Gen 3, SM8650): `libQnnHtpV75Stub.so` (in `/usr/lib64/vela-npu`), `libQnnHtpV75Skel.so`, `libQairtHtpV75Skel.so`, `libQnnHtpV75.so` (in `/usr/lib/rfsa/adsp`), and its HTP v81 part
  (Snapdragon 8 Elite Gen 5, SM8850): `libQnnHtpV81Stub.so` (in `/usr/lib64/vela-npu`), `libQnnHtpV81Skel.so`, `libQairtHtpV81Skel.so`, `libQnnHtpV81.so` (in `/usr/lib/rfsa/adsp`).
- The 17 graphs are the AMD FSR 4.1.1 network (AMD's MIT licence, see `THIRD_PARTY_NOTICES.md`), compiled with Qualcomm's
  QAIRT tools into QNN context binaries for HTP v73 (SM8550; SM8650 runs the same files on the HTP v75 runtime and SM8850 runs them on the HTP v81 runtime by default), HTP v79
  (SM8750, in `v79/`) and optionally HTP v81 (SM8850, in `v81/` when bundled).
- The five ANF graph families (`anf*s32.bin`, HTP v73 only, since package 18) are the ANF super-resolution network of Qualcomm's
  Adreno Neural Fusion SDK (<https://github.com/SnapdragonGameStudios/adreno-neural-fusion>), rebuilt by us as integer QNN graphs and
  compiled with QAIRT; recipe and hashes in `npu/graphs/pkg18/README.md`. The code around it is ours, GPL-3.0: fork patches
  0050-0056, the daemon's family sockets, the plugin's ANF model.
- No DSP firmware file is in the image: `install-vela-npu.sh` reads `fastrpc_shell_unsigned_3`, `libc++.so.1` and
  `libc++abi.so.1` from the device's own dsp partition.
- Qualcomm's licence for the 18 runtime files, the **Qualcomm AI Stack License** (the SDK's `LICENSE.pdf`), and its Notice
  File (`QNN_NOTICE.txt`) are inside the image, unchanged, at `/usr/share/licenses/armada-vela-npu/`
  (`Qualcomm-AI-Stack-LICENSE.pdf`, `QNN_NOTICE.txt`). Section 1 of that licence allows distribution only as part of a
  software application, so the Qualcomm files are never offered as release assets of their own.

### `fsr4npud`: GPL-3.0-or-later with an additional permission

`fsr4npud` (the NPU daemon) loads the Qualcomm runtime (`dlopen` of `libQnnHtp.so` / `libQnnSystem.so`) and is compiled
against the Qualcomm SDK headers. It is licensed under GPL-3.0-or-later with the following additional permission
(GPL-3.0 section 7; the wording follows the FSF's model text,
https://www.gnu.org/licenses/gpl-faq.html#GPLIncompatibleLibs), which is also at the top of each of its source files:

```
Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with the Qualcomm AI Engine Direct (QNN)
runtime libraries or the Qualcomm AI Engine Direct SDK header files (or a modified version of them), containing parts
covered by the terms of the Qualcomm AI Stack License, the licensors of this Program grant you additional permission to
convey the resulting work. This includes loading those libraries at run time (for example with dlopen()).
```

The complete source of `fsr4npud` and of `fsr4npu-check`, with the build steps, is in [`npu/src/`](npu/src/README.md). It holds no Qualcomm file;
building `fsr4npud` needs the Qualcomm AI Engine Direct SDK headers, which you get from Qualcomm. All of these files are ours:

| file in `npu/src/` | licence |
|---|---|
| `fsr4npud.cpp` | GPL-3.0-or-later with the section 7 permission above |
| `fsr4npu_proto.h`, `fsr4npu.h`, `fsr4npu.c`, `fsr4npu_check.c` | GPL-3.0-or-later with the same permission in the header (needed only by `fsr4npud.cpp`) |
| `build.sh`, `Dockerfile`, `README.md` | GPL-3.0-or-later (build recipe and notes) |

The NPU Vulkan layer (`npu-layer/`), `fsr4npu-check` and the FSR 4 provider talk to `fsr4npud` over a socket and do not
link or load any Qualcomm file, so they need no such permission. No Qualcomm SDK header or other SDK file is in this
repository: to build `fsr4npud`, get the Qualcomm AI Engine Direct SDK from Qualcomm.
