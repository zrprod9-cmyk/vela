# Source of the Vela NPU daemon and its checker

The Vela NPU system image (`vela-npu-sysext-13.raw`, release asset, sha256
`ac9672ba83e20d4e001261f6c22c7ba27f15d93fd9376c22b62bc4ba5e653deb`) ships two programs of ours as binaries:
`/usr/lib64/vela-npu/fsr4npud` (the NPU daemon) and `/usr/lib64/vela-npu/fsr4npu-check` (its checker). This folder is their
complete source and build recipe (GPL-3.0-or-later, see [Licence](#licence)). Nothing from Qualcomm is in this folder.

| file | what it is | licence |
|---|---|---|
| `fsr4npud.cpp` | the daemon: serves the socket protocol, registers dma-bufs with QNN, runs the graph on the HTP, power vote, watchdog | GPL-3.0-or-later with the section 7 permission |
| `fsr4npu_proto.h` | the wire protocol (daemon and clients) | same |
| `fsr4npu.h`, `fsr4npu.c` | the client library (used by `fsr4npu-check`) | same |
| `fsr4npu_check.c` | `fsr4npu-check`: runs the graph a few times as an ordinary user and prints timing and an output hash | same |
| `build.sh` | the build (cross-compile for aarch64) | same |
| `Dockerfile` | the toolchain the shipped binaries were built with | same |

All five source files are written for Vela; none contains third-party code. `fsr4npud.cpp` includes the Qualcomm
AI Engine Direct (QNN) headers and the Vulkan header `vulkan/vulkan.h`; neither is copied here.

## Build

You need three things.

1. **Docker** (or any machine with an aarch64 GNU cross toolchain: g++ 13.3, binutils 2.42, glibc 2.39 headers and the Vulkan
   headers 1.3.275; the `Dockerfile` shows the packages).
2. **The Qualcomm AI Engine Direct SDK, version 2.47.0.260601** (QAIRT; only its `include/QNN` headers are used). It is not in
   this repository and may not be redistributed by us. Download it from Qualcomm, after accepting the Qualcomm AI Stack
   License:
   `https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/2.47.0.260601/v2.47.0.260601.zip`
   (Qualcomm Software Center, "Qualcomm AI Runtime SDK"; the file we checked has sha256
   `d3497e110eae82c35a9152a93c0a18bbede402aaf9faa7a97c8079eb0f522b01`). Unpack it: the folder `qairt/2.47.0.260601`
   contains `include/QNN/QnnInterface.h`. That folder is `QNN_SDK_ROOT`.
3. This folder.

```sh
# from the repository root; <sdk> = the unpacked qairt/2.47.0.260601 folder, <out> = an empty output folder
docker build -t vela-npu-build npu/src
docker run --rm -v "$PWD/npu/src:/src:ro" -v <sdk>:/sdk:ro -v <out>:/out \
  -e QNN_SDK_ROOT=/sdk -e OUT=/out vela-npu-build sh /src/build.sh
```

(On Windows with Git Bash, set `MSYS_NO_PATHCONV=1` and write the host paths as `C:/...`.) On an aarch64 Linux machine
you can run `CROSS= QNN_SDK_ROOT=<sdk> sh npu/src/build.sh` with its native g++ 13 and the Vulkan headers.

`build.sh` writes `fsr4npud`, `fsr4npud.stripped`, `fsr4npu.o` and `fsr4npu-check` into `<out>`. The image builder
`../mk_public_sysext.sh` takes `fsr4npud.stripped` and `fsr4npu-check` from there (its `NPUD_BIN` folder) and sets the
library search path of `fsr4npud` with `patchelf --force-rpath --set-rpath /usr/lib64/vela-npu`.

## How the shipped binaries relate to this source

- Built from the sources of this folder with the `Dockerfile` toolchain, `fsr4npu-check` is byte-identical to the file in the
  image, and `fsr4npud.stripped` with the `patchelf` call above is byte-identical to `fsr4npud` in the image. Two builds give
  the same files.

## Licence

The five source files are GPL-3.0-or-later, plus the following additional permission (GPL-3.0 section 7), the same text
that is at the top of each file and in [`../../LICENSING.md`](../../LICENSING.md):

```
Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with the Qualcomm AI Engine Direct (QNN)
runtime libraries or the Qualcomm AI Engine Direct SDK header files (or a modified version of them), containing parts
covered by the terms of the Qualcomm AI Stack License, the licensors of this Program grant you additional permission to
convey the resulting work. This includes loading those libraries at run time (for example with dlopen()).
```

The permission is needed by `fsr4npud.cpp` only (it uses the QNN headers and loads the QNN libraries); the other files carry
the same header so that every file of this folder has one licence. The GPL-3.0 text is the `LICENSE` file at the top of the
repository.
