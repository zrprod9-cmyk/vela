# Vela NPU Vulkan layer

`VK_LAYER_VELA_npu` is an implicit Vulkan layer for aarch64 Linux (armadaOS, Snapdragon 8 Gen 2). It sits between a game's
Vulkan driver and Vela's FSR 4 provider and hands the FSR 4.1 networks to the NPU daemon `fsr4npud` (see [`../npu`](../npu)):
it imports the daemon's dma-buf into the game's `VkDevice`, copies the network input in, waits for the NPU on the GPU
timeline and copies the outputs back. Without the daemon, or when anything goes wrong, it reports a status that makes the
provider run the network on the GPU instead. The GPU's wait for the NPU is bounded (about 50 ms at 680 MHz, up to about
160 ms at 220 MHz) so that a game killed with a job in flight cannot hang the GPU; this needs `bufferDeviceAddress`, which
vkd3d-proton and the FSR 4 provider enable.

The layer is enabled by `VELA_NPU=1` (set by the plugin; `DISABLE_VELA_NPU=1` turns it off). The plugin installs
`libVkLayer_VELA_npu.so` and `VkLayer_VELA_npu.json` and writes the implicit-layer manifest that points to them.
Licence: GPL-3.0-only (see [`../LICENSING.md`](../LICENSING.md)); nothing from Qualcomm is used here.

## Files

| file | what it is |
|---|---|
| `vela_npu_layer.cpp` | the layer (layer build 11: `VL_BUILD`, and `implementation_version` in the manifest) |
| `vela_npu_abi.h`, `vela_npu_ext.h` | the contract with the provider: the 256-byte command/ack marker recorded with `vkCmdUpdateBuffer`, and its extensions (tensors, async, zero-copy, graph choice, ping-pong) |
| `vela_npu_wait.comp`, `vela_npu_wait_spv.h` | the shader of the bounded GPU wait and its SPIR-V as a C array |
| `vela_npu.map` | linker version script: exports only the layer entry points |
| `VkLayer_VELA_npu.json` | the layer manifest (`implementation_version` 11) |
| `build.sh` | the build |
| `wl_app.c` | on-device test app: plays the provider's side of the contract against the real layer and a daemon |

The client library of the daemon protocol (`fsr4npu.c`, `fsr4npu.h`, `fsr4npu_proto.h`) is not copied here: the layer is built
against the one in [`../npu/src`](../npu/src).

## Build

You need Docker. The toolchain is the one of the NPU part: Ubuntu 24.04 with the aarch64 GNU cross compiler (g++ 13.3.0,
binutils 2.42, glibc 2.39 cross headers), the Vulkan headers 1.3.275 and the arm64 `libvulkan`. From the repository root:

```sh
docker build -t vela-npu-build npu/src
docker run --rm -v "$PWD":/r:ro -v "$PWD/out":/out -e OUT=/out vela-npu-build sh /r/npu-layer/build.sh
```

(On Windows with Git Bash, set `MSYS_NO_PATHCONV=1` and write the host paths as `C:/...`.) `build.sh` writes
`libVkLayer_VELA_npu.so` (with debug info), `VkLayer_VELA_npu.json`, `fsr4npu_pic.o` and `wl_app` into `out/` and prints the
`md5sum` of the layer. Without `OUT` it writes to `npu-layer/bin/`. On an aarch64 machine `CROSS= sh npu-layer/build.sh` uses
the native compiler.

Pinned versions of the image at the time of the check below: `g++-13-aarch64-linux-gnu 13.3.0-6ubuntu2~24.04.1cross1`,
`binutils-aarch64-linux-gnu 2.42-4ubuntu2.10`, `libc6-dev-arm64-cross 2.39-0ubuntu8cross1`, `libvulkan-dev:arm64 1.3.275.0-1build1`.
A newer Ubuntu package set can give different machine code.

### The shader header

`vela_npu_wait_spv.h` is the SPIR-V of `vela_npu_wait.comp`. To regenerate it, compile the shader with
`glslangValidator -V --target-env vulkan1.2 vela_npu_wait.comp -o vela_npu_wait.spv` and write the 331 32-bit words as the array
`kWaitSpv` (glslang 15.2.0 gives exactly the words in the header; `spirv-val --target-env vulkan1.2` accepts it).

## How the shipped binary relates to this source

The release zip (`upscale/assets/npu/libVkLayer_VELA_npu.so`) carries the layer build 11, built from this folder with the
toolchain and the command above: md5 `2f438a25a73197d8eacb5b5422416286`, 4,143,056 bytes (manifest `VkLayer_VELA_npu.json` md5 `82231f80d6c34e09c492fcf8ca2b8f44`).
Building again gives the same file byte for byte. The repository holds no binary of the layer: the shipped one is in the
release zip.

## Slow and hung NPU jobs

The HTP normally runs a frame in about 4 ms. Rarely a single execute takes much longer (100-300 ms have been seen) and then
finishes normally. The worker handles three cases:

- the execute takes longer than `VELA_NPU_SLOW_MS`: that frame gets a non-OK status at once (the provider runs the GPU
  network for it, so the GPU does not wait), every job already queued is released the same way, and new markers get `BUSY`
  until the execute ends. The connection stays: when the execute ends, the next frame runs on the NPU again;
- no answer within `VELA_NPU_NPU_TIMEOUT_MS`, an error from the daemon, or the daemon gone: the connection is closed and the
  worker reconnects every `VELA_NPU_RECONNECT_MS` (frames use the GPU network meanwhile, without waiting);
- the bounded GPU wait stays as the last guard for a frame whose worker cannot answer at all (the process is stopped or killed).

## Settings

Environment variables read by the layer (all optional):

| variable | default | meaning |
|---|---|---|
| `VELA_NPU` | set by the manifest | `0` switches every hook off |
| `VELA_NPUD_NAME` | `fsr4npud` | abstract socket name of the daemon (used when the provider asks for no graph) |
| `VELA_NPUD_GRAPH_NAME` | — | overrides the daemon name the provider asks for (`fsr4npud_<graph>`) |
| `VELA_NPU_LOG_FILE` | stderr | log file (appended); `VELA_NPU_LOG` is accepted as an alias |
| `VELA_NPU_CONNECT_MS` | 100 | connect time limit at the first probe |
| `VELA_NPU_RECONNECT_MS` | 150 | pause between reconnects after the daemon is lost |
| `VELA_NPU_E1_TIMEOUT_MS` | 200 | how long the worker waits for the GPU to reach the copy-in event |
| `VELA_NPU_SLOW_MS` | 20 | an execute that takes longer sends its frame (and the frames until it ends) to the GPU network |
| `VELA_NPU_NPU_TIMEOUT_MS` | 400 | no answer from the NPU for this long: the HTP counts as hung, fresh connection |
| `VELA_NPU_BOUNDED` | on | `0` waits with `vkCmdWaitEvents` instead of the bounded wait |
| `VELA_NPU_ZEROCOPY`, `VELA_NPU_PINGPONG` | on | `0` switches the feature off |
| `VELA_NPU_WORKER_CPUS` | — | CPU list for the worker thread, e.g. `0-2,4` |

## Test app

`wl_app` runs on the device without a game. It creates the tensor images, records the provider's markers and checks the
outputs: `wl_app exact|async|plain <frames> [width height]`. Start `fsr4npud` (from the NPU part, `/usr/lib64/vela-npu/fsr4npud`)
with its stub backend, which copies the input to every output, and put the layer where the loader finds it:

```sh
fsr4npud --name wl_big --backend stub --stub-bytes 8294400 --stub-nout 2 &
VK_ADD_IMPLICIT_LAYER_PATH=<folder with the .so and the .json> VELA_NPU=1 VELA_NPUD_NAME=wl_big ./wl_app exact 600
```

It prints `RESULT` and `TIMING` lines and a `TEST` line with the pass/fail verdict. Further switches are described at the top
of `wl_app.c`.
