# Vela NPU system part (package 12)

It lets Vela run FSR 4 and FSR 4 Fast on the Hexagon NPU of the Snapdragon 8 Gen 2 (SM8550, HTP v73). Without it Vela runs
them on the GPU. It is a system part, so installing it needs **sudo once** (your password).

## Install (Desktop mode → Konsole, game closed)

1. Run: `curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/download/v0.8.16/install-vela-npu.sh | sudo sh`
   Nothing else to download: the image carries the Qualcomm QNN runtime; the installer reads 3 DSP files from your device.
2. It ends with `Vela NPU: OK - 7 of 7 NPU daemons work` or `Vela NPU: FAILED - <reason>`. Start the game again; the Vela
   panel shows "on the NPU".

Remove: `curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/download/v0.8.16/uninstall-vela-npu.sh | sudo sh`

## What it installs

| where | what | from |
|---|---|---|
| `/var/lib/extensions/armada-vela-npu.raw` | release asset `vela-npu-sysext-12.raw` (squashfs systemd system extension, sha256 `392ba27e276d985221d1d695b782628cffa68ae4165bcb2ec0e7676cfd49a52a`, list in `vela-npu-sysext-12.contents.txt`) | `mk_public_sysext.sh`, byte-reproducible (built twice, same sha256) |
| inside: `/usr/lib64/vela-npu/fsr4npud`, `fsr4npu-check` | the NPU daemon (one per FSR 4 graph) and its checker | ours, GPL-3.0-or-later; source and build steps: [`src/`](src/README.md) |
| inside: `/usr/lib64/vela-npu/nf4*.bin` (7) | the FSR 4.1.1 network compiled for HTP v73: 1080p / 900p / 720p output × Performance / Ultra Performance, + exact | ours (from AMD's FSR 4 network) |
| inside: `cdsprpcd`, `libcdsprpc.so`, `libcdsp_default_listener.so` | the FastRPC client and the cDSP listener | quic/fastrpc a56e9d4, BSD-3-Clause |
| inside: `/usr/lib64/vela-npu/libQnnHtp.so`, `libQnnHtpV73Stub.so`, `libQnnSystem.so`; `/usr/lib/rfsa/adsp/libQnnHtpV73Skel.so`, `libQairtHtpV73Skel.so`, `libQnnHtpV73.so` | the Qualcomm AI Engine Direct (QNN) runtime 2.47.0.260601 (host part + HTP v73 part) | Qualcomm AI Engine Direct SDK runtime, unmodified, Qualcomm AI Stack License (sha256 of each file in `mk_public_sysext.sh`) |
| inside: `/usr/share/licenses/armada-vela-npu/Qualcomm-AI-Stack-LICENSE.pdf`, `QNN_NOTICE.txt` | Qualcomm's licence for that runtime (the SDK's `LICENSE.pdf`) and its third-party Notice File, both unchanged (sha256 in `mk_public_sysext.sh`) | Qualcomm AI Engine Direct SDK 2.47.0.260601 |
| inside: `vela-npu@.service`, `vela-npu-listener.service`, `vela-npu-setup.service` | daemons run as a dynamic user in group `render`, no capabilities, read-only system; the listener runs as root with 2 capabilities (it needs the root-only `/dev/fastrpc-cdsp-secure`) | ours |
| inside: `70-vela-npu.rules`, `modules-load.d/vela-npu.conf` | `/dev/fastrpc-cdsp` and `/dev/dma_heap/system` → group `render`, mode 0660: this is how the daemons reach the NPU without root; loads `system_heap` | ours |
| `/var/lib/extensions/armada-vela-npu-qnn/` (root:root) | directory extension (name kept from the first installer): DSP shell `fastrpc_shell_unsigned_3`, `libc++.so.1`, `libc++abi.so.1` read from the device's own `dsp` partition | the device |
| `/etc/systemd/system/vela-npu-boot.service` | starts the 7 daemons after the extension merge at every boot | written by the installer |

Apart from the library search paths of `fsr4npud` and `libcdsp_default_listener`, the merged result is the same set of files
as the NPU package all numbers in the main README were measured with. The extension says `ID=armada VERSION_ID=44`; after an armadaOS update with another VERSION_ID,
systemd-sysext stops loading it and the installer must be run again (it refuses other versions unless `--force`).

## Security
- Library search paths: only `/usr/lib64/vela-npu` (`fsr4npud` RPATH, `libcdsp_default_listener` RUNPATH), plus `$ORIGIN`
  on the three unmodified Qualcomm host libraries, which resolves to the same root-owned, read-only folder. The builder
  fails on any search path outside `/usr/lib64/vela-npu` and on any home or `/tmp` path string (the Qualcomm runtime's mkstemp template `/tmp/tmp.XXXXXX` excepted; the daemons run with `PrivateTmp=yes`).
- All files root:root, no setuid/setgid, nothing group- or world-writable (checked by the builder).
- Device access: `/dev/fastrpc-cdsp` and `/dev/dma_heap/system` get group `render`, mode 0660. On armadaOS the default user
  is in `render`, so the user's own programs can open them too (not only the daemons); `/dev/fastrpc-cdsp-secure` (the
  root PD) stays root-only.

## Qualcomm runtime: conditions
- The 6 Qualcomm files are distributed only inside this image (and the armadaOS `vela-npu` package made from it), as part
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

## Testing
The public installer was run on a Retroid Pocket 6 with a local copy of the image (`--image`): 7 of 7 NPU daemons passed
their fixed-input check. The download step (`curl` from the release) has not been run yet. Its sha256 / dsp-partition
steps are also covered by `dry_test.sh` in a container. The daemons accept clients of uid 1000 only (the default armadaOS user).
