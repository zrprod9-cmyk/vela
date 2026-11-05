# Vela NPU system part (package 14)

It lets Vela run FSR 4 and FSR 4 Fast on the Hexagon NPU of the Snapdragon 8 Gen 2 (SM8550, HTP v73) and, experimentally,
of the Snapdragon 8 Elite (SM8750, HTP v79: the AYN Odin 3 and other devices with this chip). Without it Vela runs them on the GPU. It is a system part, so installing it needs **sudo once** (your password).

## Install (Desktop mode → Konsole, game closed)

1. Run: `curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/download/v0.8.21/install-vela-npu.sh | sudo sh`
   Nothing else to download: the image carries the Qualcomm QNN runtime; the installer reads 3 DSP files from your device.
2. It ends with `Vela NPU: OK - all NPU graphs checked work` or `Vela NPU: FAILED - <reason>`. Start the game again; the Vela
   panel shows "on the NPU".

Remove: `curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/download/v0.8.21/uninstall-vela-npu.sh | sudo sh`

## Game resolutions on the NPU

Package 14 has FSR 4 networks for these output sizes: 1920x1080, 1600x900 and 1280x720, and the newer 1600x904, 1440x810, 1280x800,
1152x648 and 960x540. At any other size, FSR 4 runs on the GPU.

- On the Retroid Pocket 6, 960x540 has run on the NPU in a game (RE4). The other four new sizes pass the installer's fixed-input
  check on that device but have not been run in a game yet.
- On the Snapdragon 8 Elite the five new sizes are experimental: not tested there yet.
- 1366x768 is not supported on the NPU. The panel asks for 1920x1080.

## What it installs

| where | what | from |
|---|---|---|
| `/var/lib/extensions/armada-vela-npu.raw` | release asset `vela-npu-sysext-14.raw` (squashfs systemd system extension, sha256 `10acba9196a7a55f712d14451ed64a20385cc5eab045744be6a63dd264392ac1`, list in `vela-npu-sysext-14.contents.txt`) | `mk_public_sysext.sh`, byte-reproducible (built twice, same sha256) |
| inside: `/usr/lib64/vela-npu/fsr4npud`, `fsr4npu-check` | the NPU daemon (one process for all FSR 4 graphs) and its checker | ours, GPL-3.0-or-later; source and build steps: [`src/`](src/README.md) |
| inside: `/usr/lib64/vela-npu/nf4*.bin` (17) | the FSR 4.1.1 network compiled for HTP v73: 1920x1080 / 1600x900 / 1280x720 / 1600x904 / 1440x810 / 1280x800 / 1152x648 / 960x540 output × Performance / Ultra Performance, + exact | ours (from AMD's FSR 4 network) |
| inside: `/usr/lib64/vela-npu/v79/nf4*.bin` (17) | the same 17 graphs compiled for HTP v79 (SM8750), same file names | ours (from AMD's FSR 4 network) |
| inside: `/usr/lib64/vela-npu/v79/nf4*.bin` (17) | the same 17 graphs compiled for HTP v79 (SM8750), same file names | ours (from AMD's FSR 4 network) |
| inside: `cdsprpcd`, `libcdsprpc.so`, `libcdsp_default_listener.so` | the FastRPC client and the cDSP listener | quic/fastrpc a56e9d4, BSD-3-Clause |
| inside: `/usr/lib64/vela-npu/libQnnHtp.so`, `libQnnHtpV73Stub.so`, `libQnnSystem.so`; `/usr/lib/rfsa/adsp/libQnnHtpV73Skel.so`, `libQairtHtpV73Skel.so`, `libQnnHtpV73.so` | the Qualcomm AI Engine Direct (QNN) runtime 2.47.0.260601 (host part + HTP v73 part) | Qualcomm AI Engine Direct SDK runtime, unmodified, Qualcomm AI Stack License (sha256 of each file in `mk_public_sysext.sh`) |
| inside: `/usr/lib64/vela-npu/libQnnHtpV79Stub.so`; `/usr/lib/rfsa/adsp/libQnnHtpV79Skel.so`, `libQairtHtpV79Skel.so`, `libQnnHtpV79.so` | the same runtime's HTP v79 part (SM8750) | same |
| inside: `/usr/lib64/vela-npu/libQnnHtpV79Stub.so`; `/usr/lib/rfsa/adsp/libQnnHtpV79Skel.so`, `libQairtHtpV79Skel.so`, `libQnnHtpV79.so` | the same runtime's HTP v79 part (SM8750) | same |
| inside: `/usr/share/licenses/armada-vela-npu/Qualcomm-AI-Stack-LICENSE.pdf`, `QNN_NOTICE.txt` | Qualcomm's licence for that runtime (the SDK's `LICENSE.pdf`) and its third-party Notice File, both unchanged (sha256 in `mk_public_sysext.sh`) | Qualcomm AI Engine Direct SDK 2.47.0.260601 |
| inside: `vela-npu.service`, `vela-npu-listener.service`, `vela-npu-setup.service` (source: [`units/`](units/)) | the daemon runs as a dynamic user in group `render`, no capabilities, read-only system; the listener runs as root with 2 capabilities (it needs the root-only `/dev/fastrpc-cdsp-secure`) | ours |
| inside: `vela-npu-setup.service.d/50-vela-npu-chip.conf`, `vela-npu.service.d/50-vela-npu-chip.conf` (source: [`units/`](units/)) | chip selection: on `qcom,sm8550` nothing changes; on `qcom,sm8750` the setup unit links `/run/vela-npu/graphs` to `v79/` and each graph's v79 file is mounted read-only over its v73 path | ours |
| inside: `vela-npu-setup.service.d/50-vela-npu-chip.conf`, `vela-npu.service.d/50-vela-npu-chip.conf` (source: [`units/`](units/)) | chip selection: on `qcom,sm8550` nothing changes; on `qcom,sm8750` the setup unit links `/run/vela-npu/graphs` to `v79/` and each graph's v79 file is mounted read-only over its v73 path | ours |
| inside: `70-vela-npu.rules`, `modules-load.d/vela-npu.conf` | `/dev/fastrpc-cdsp` and `/dev/dma_heap/system` → group `render`, mode 0660: this is how the daemon reaches the NPU without root; loads `system_heap` | ours |
| `/var/lib/extensions/armada-vela-npu-qnn/` (root:root) | directory extension (name kept from the first installer): DSP shell `fastrpc_shell_unsigned_3`, `libc++.so.1`, `libc++abi.so.1` read from the device's own `dsp` partition | the device |
| `/etc/systemd/system/vela-npu-boot.service` | starts the NPU service `vela-npu.service` after the extension merge at every boot | written by the installer |

The numbers in the main README were measured with an earlier NPU package. Package 14 measured the same on the Retroid Pocket 6
in the games checked (Control 8.32 ms on the NPU). The extension says `ID=armada VERSION_ID=44`; after an armadaOS update with another VERSION_ID,
systemd-sysext stops loading it and the installer must be run again (it refuses other versions unless `--force`).

## Security
- Library search paths: only `/usr/lib64/vela-npu` (`fsr4npud` RPATH, `libcdsp_default_listener` RUNPATH), plus `$ORIGIN`
  on the three unmodified Qualcomm host libraries, which resolves to the same root-owned, read-only folder. The builder
  fails on any search path outside `/usr/lib64/vela-npu` and on any home or `/tmp` path string (the Qualcomm runtime's mkstemp template `/tmp/tmp.XXXXXX` excepted; the daemon runs with `PrivateTmp=yes`).
- All files root:root, no setuid/setgid, nothing group- or world-writable (checked by the builder).
- Device access: `/dev/fastrpc-cdsp` and `/dev/dma_heap/system` get group `render`, mode 0660. On armadaOS the default user
  is in `render`, so the user's own programs can open them too (not only the daemon); `/dev/fastrpc-cdsp-secure` (the
  root PD) stays root-only.

## Qualcomm runtime: conditions
- The 10 Qualcomm files are distributed only inside this image (and the armadaOS `vela-npu` package made from it), as part
  of Vela. They are never offered as separate downloads: the Qualcomm AI Stack License allows distribution "as incorporated
  in Your software application", not on a standalone basis (section 1).
- They are unmodified (no strip, no patchelf); the licence and the Notice File ship next to them, unchanged.
- Export control: see section 10(f) of `Qualcomm-AI-Stack-LICENSE.pdf`.
- Building `fsr4npud` (`src/build.sh`) needs the Qualcomm AI Engine Direct SDK (QAIRT 2.47.0.260601) headers: get the SDK from Qualcomm
  (https://softwarecenter.qualcomm.com; exact download in `src/README.md`); no SDK header is in this repository.
- `fsr4npud` is GPL-3.0-or-later with a GPL-3.0 section 7 additional permission to link with and load the Qualcomm runtime
  (`LICENSING.md`).
- The source of `fsr4npud` and `fsr4npu-check` is in [`src/`](src/README.md) (file table, licences, build steps, and how the image's
  binaries relate to it).
