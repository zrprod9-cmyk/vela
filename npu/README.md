# Vela NPU system part (package 18)

It lets Vela run FSR 4, FSR 4 Fast and (package 18) ANF on the Hexagon NPU of the Snapdragon 8 Gen 2 (SM8550, HTP v73), of the Snapdragon 8 Gen 3
(SM8650, HTP v75: the same v73 graphs on the v75 runtime), experimentally of the Snapdragon 8 Elite (SM8750, HTP v79: the AYN Odin 3 and other devices with this chip),
and experimentally of the Snapdragon 8 Elite Gen 5 (SM8850, HTP v81). Without it Vela runs them on the GPU. It is a system part, so installing it needs **sudo once** (your password).

## Install (Desktop mode → Konsole, game closed)

1. Run: `curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/download/v0.10.0/install-vela-npu.sh | sudo sh`
   Nothing else to download: the image carries the Qualcomm QNN runtime; the installer reads 3 DSP files from your device.
2. It ends with `Vela NPU: OK - all NPU graphs checked work` or `Vela NPU: FAILED - <reason>`. Start the game again; the Vela
   panel shows "on the NPU".

When the NPU service starts, it checks one graph with a known input (the start self-check). If the result is wrong on your unit,
it switches to the `low4/` graphs, which are planned for a smaller memory window. If those are wrong too, the NPU stays off and
Vela runs FSR 4 on the GPU. Details and the environment variables: sections "Package 17" and "Package 18" below. If the check fails on both sets, the installer says so ("Vela NPU: off on this unit ...", exit code 3) instead of "FAILED".

Remove: `curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/download/v0.10.0/uninstall-vela-npu.sh | sudo sh`

## Game resolutions on the NPU

Package 17 has the FSR 4 networks of package 14 (rebuilt in a faster form, same output), for these output sizes: 1920x1080, 1600x900 and 1280x720, and the newer 1600x904, 1440x810, 1280x800,
1152x648 and 960x540. At any other size, FSR 4 runs on the GPU.

- On the Retroid Pocket 6, 960x540 has run on the NPU in a game (RE4). The other four new sizes pass the installer's fixed-input
  check on that device but have not been run in a game yet.
- On the Snapdragon 8 Elite the five new sizes are experimental: not tested there yet.
- On the Snapdragon 8 Gen 3 (SM8650) the v73 graphs run on the HTP v75 runtime. The author of PR #6 checked all 17 graphs
  on a Lenovo Legion Tab Gen 3 (TB321FU): same output hashes as on SM8550. We have no SM8650 device to check it ourselves.
- On the Snapdragon 8 Elite Gen 5 (SM8850, HTP v81), 1920x1080 (`nf4_k1h`, `nf4up_k1h`) passed 20/20 bit-exact against the
  simulator and SM8550 v73 on a real SM8850P (Lenovo Legion Y700 Gen 5 running HyperOS 4, Android 17 / QAIRT 2.47) with E-BS `v73`, `v79`, and native `v81` graphs (with native `v81` saving ~9–14% NPU execute latency and reducing spill/fill); the other 15 sizes and the
  armadaOS sysext have not been tested on hardware yet.
- 1366x768 is not supported on the NPU. The panel asks for 1920x1080.

## What it installs

| where | what | from |
|---|---|---|
| `/var/lib/extensions/armada-vela-npu.raw` | release asset `vela-npu-sysext-18.raw` (squashfs systemd system extension, 64 061 440 bytes, sha256 `b4f2f260be4fc2f9339411fafe480ac7ee505caa34e3e9428550a0754e700807`, list in `vela-npu-sysext-18.contents.txt`) | `mk_public_sysext.sh`, byte-reproducible (package 18: two builds, the same sha256; package 17: five of six, see below) |
| inside: `/usr/lib64/vela-npu/fsr4npud`, `fsr4npu-check` | the NPU daemon (one process for all FSR 4 graphs) and its checker | ours, GPL-3.0-or-later; source and build steps: [`src/`](src/README.md) |
| inside: `/usr/lib64/vela-npu/nf4*.bin` (17) | the FSR 4.1.1 network compiled for HTP v73 (also used on SM8650 with the HTP v75 runtime, and by default on SM8850 with the HTP v81 runtime when `v81/` is omitted): 1920x1080 / 1600x900 / 1280x720 / 1600x904 / 1440x810 / 1280x800 / 1152x648 / 960x540 output × Performance / Ultra Performance, + exact | ours (from AMD's FSR 4 network) |
| inside: `/usr/lib64/vela-npu/low4/nf4*.bin` (17) + `low4/SELFCHECK` | the same 17 graphs planned for a 4 MB memory window, for units whose NPU gives wrong results with the main set; used only if the start self-check fails | ours; recipe and hashes: [`graphs/pkg17/`](graphs/pkg17/README.md) |
| inside: `/usr/lib64/vela-npu/v79/nf4*.bin` (17) | the same 17 graphs compiled for HTP v79 (SM8750), same file names | ours (from AMD's FSR 4 network) |
| inside: `/usr/lib64/vela-npu/v81/nf4*.bin` (17, optional) | optional native build of the 17 graphs for HTP v81 (SM8850, bundled when built with `GRAPHS_V81`; when omitted SM8850 uses the HTP v73 graphs on the HTP v81 runtime) | ours (from AMD's FSR 4 network) |
| inside: `/usr/lib64/vela-npu/anf{360,480,540,648,720}s32.bin` (5) | the ANF super-resolution network as five graph families for HTP v73 (Snapdragon 8 Gen 2), 32 phase graphs per family, for the render canvases 640x360, 854x480, 960x540, 1152x648 and 1280x720 | compiled by us; recipe and hashes: [`graphs/pkg18/`](graphs/pkg18/README.md) |
| inside: `cdsprpcd`, `libcdsprpc.so`, `libcdsp_default_listener.so` | the FastRPC client and the cDSP listener | quic/fastrpc a56e9d4, BSD-3-Clause |
| inside: `/usr/lib64/vela-npu/libQnnHtp.so`, `libQnnHtpV73Stub.so`, `libQnnSystem.so`; `/usr/lib/rfsa/adsp/libQnnHtpV73Skel.so`, `libQairtHtpV73Skel.so`, `libQnnHtpV73.so` | the Qualcomm AI Engine Direct (QNN) runtime 2.47.0.260601 (host part + HTP v73 part) | Qualcomm AI Engine Direct SDK runtime, unmodified, Qualcomm AI Stack License (sha256 of each file in `mk_public_sysext.sh`) |
| inside: `/usr/lib64/vela-npu/libQnnHtpV79Stub.so`; `/usr/lib/rfsa/adsp/libQnnHtpV79Skel.so`, `libQairtHtpV79Skel.so`, `libQnnHtpV79.so` | the same runtime's HTP v79 part (SM8750) | same |
| inside: `/usr/lib64/vela-npu/libQnnHtpV75Stub.so`; `/usr/lib/rfsa/adsp/libQnnHtpV75Skel.so`, `libQairtHtpV75Skel.so`, `libQnnHtpV75.so` | the same runtime's HTP v75 part (SM8650) | same |
| inside: `/usr/lib64/vela-npu/libQnnHtpV81Stub.so`; `/usr/lib/rfsa/adsp/libQnnHtpV81Skel.so`, `libQairtHtpV81Skel.so`, `libQnnHtpV81.so` | the same runtime's HTP v81 part (SM8850) | same |
| inside: `/usr/share/licenses/armada-vela-npu/Qualcomm-AI-Stack-LICENSE.pdf`, `QNN_NOTICE.txt` | Qualcomm's licence for that runtime (the SDK's `LICENSE.pdf`) and its third-party Notice File, both unchanged (sha256 in `mk_public_sysext.sh`) | Qualcomm AI Engine Direct SDK 2.47.0.260601 |
| inside: `vela-npu.service`, `vela-npu-listener.service`, `vela-npu-setup.service` (source: [`units/`](units/)) | the daemon runs as a dynamic user in group `render`, no capabilities, read-only system; the listener runs as root with 2 capabilities (it needs the root-only `/dev/fastrpc-cdsp-secure`) | ours |
| inside: `vela-npu-setup.service.d/50-vela-npu-chip.conf`, `vela-npu.service.d/50-vela-npu-chip.conf` (source: [`units/`](units/)) | chip selection: on `qcom,sm8550` and `qcom,sm8650` nothing changes; on `qcom,sm8750` (`v79/`) or `qcom,kaanapali` / `qcom,sm8850` / `qcom,canoe` (when `v81/` is bundled) the setup unit links `/run/vela-npu/graphs` and each graph is mounted read-only over its v73 path (if `v81/` is omitted, SM8850 keeps the v73 graphs on HTP v81) | ours |
| inside: `70-vela-npu.rules`, `modules-load.d/vela-npu.conf` | `/dev/fastrpc-cdsp` and `/dev/dma_heap/system` → group `render`, mode 0660: this is how the daemon reaches the NPU without root; loads `system_heap` | ours |
| `/var/lib/extensions/armada-vela-npu-qnn/` (root:root) | directory extension (name kept from the first installer): DSP shell `fastrpc_shell_unsigned_3`, `libc++.so.1`, `libc++abi.so.1` read from the device's own `dsp` partition | the device |
| `/etc/systemd/system/vela-npu-boot.service` | starts the NPU service `vela-npu.service` after the extension merge at every boot | written by the installer |

How the graph files are built and how to rebuild them byte for byte: [docs/npu-graphs.md](../docs/npu-graphs.md) (scripts in [`graphs/`](graphs/)).
What is new in the package 17 graphs (the E-BS form of the main set, the low4 set, md5 and sha256 of every file): [`graphs/pkg17/README.md`](graphs/pkg17/README.md).

The numbers in the main README were measured with an earlier NPU package. Package 14 measured the same on the Retroid Pocket 6
in the games checked (Control 8.32 ms on the NPU). The extension says `ID=armada VERSION_ID=44`; after an armadaOS update with another VERSION_ID,
systemd-sysext stops loading it and the installer must be run again (it refuses other versions unless `--force`).

Package 15 adds the HTP v75 runtime (SM8650) and changes the daemon's power vote: while a game uses the NPU, the daemon
asks for the NPU's turbo voltage level instead of the top one (max). On the Retroid Pocket 6 this left more power to the
GPU when the chip was hot: Deep Rock Galactic at 1920x1080 with FSR 4 on the NPU ran about 6.5 % faster, 1280x720 games the
same. The level is a daemon option: `--power-corner <max|turbo_plus|turbo|nom_plus|nom|svs_plus|svs|svs2|dcvs>` or the
environment variable `FSR4NPUD_POWER_CORNER` (the option wins; a wrong name stops the daemon). `max` gives the vote of
package 14. The other files, the 34 graphs included, are the same as in package 14.

## Package 17 (Vela 0.8.25)

Package 16 (not released on its own) and package 17 add three things to package 15:

- **Start self-check.** At start, `fsr4npud` runs the graph `nf4_k1h_360` three times on a fixed input and compares the output hash
  (`2606832826ab4c91`). Right: the main graphs are used. Wrong: the same check runs on `low4/nf4_k1h_360`; if that is right, every graph
  is loaded from `low4/`; if it is wrong too, the service stops and Vela runs FSR 4 on the GPU instead of showing a broken picture.
  If the check cannot run, the main graphs are used. Some Retroid Pocket 6 and Odin 2 Portal units give wrong NPU results with the
  main graphs (issue #2); the 4 MB plans of the earlier graph form gave right results on the two units we tested.
  Option `--graph-set auto|released|low` or the environment variable `FSR4NPUD_GRAPH_SET` (`released`: no check, as in package 15;
  `low`: always `low4/`, for tests).
- **Faster graphs, same output.** The 17 v73 graphs of the main set are rebuilt in a form that takes the layers' bias through the NPU's
  own bias input (E-BS): bit for bit the same output, 2 to 31 % less NPU time per frame on the Retroid Pocket 6, depending on the size.
  The two 1440x810 graphs keep their earlier files (the new form was not faster there). Details: [`graphs/pkg17/README.md`](graphs/pkg17/README.md).
  The `low4/` graphs use the same form.
- **Power governor on by default.** The daemon lowers the NPU's core power level step by step while the game's frames have time to
  spare, and raises it again when the NPU starts to hold the frame back. `FSR4NPUD_POWER_GOV=0` turns it off (the fixed `turbo`
  vote of package 15). `--power-corner` / `FSR4NPUD_POWER_CORNER` still set the level it starts from.

The image is the same as package 17 apart from the daemon, the unit, the five ANF graph files and the text files that carry the package number (list of differences: package 18 section below). Package 17 was the same as package 15 apart from these graph files, the daemon, and the two text files that carry the package number.
The package 17 image build is byte-reproducible in five of the six builds we made (the same sha256); one build gave another sha256 and
could not be examined afterwards (the build output was lost). The file contents and the listing of the five equal builds are the
shipped ones.

## Security
- Library search paths: only `/usr/lib64/vela-npu` (`fsr4npud` RPATH, `libcdsp_default_listener` RUNPATH), plus `$ORIGIN`
  on the three unmodified Qualcomm host libraries, which resolves to the same root-owned, read-only folder. The builder
  fails on any search path outside `/usr/lib64/vela-npu` and on any home or `/tmp` path string (the Qualcomm runtime's mkstemp template `/tmp/tmp.XXXXXX` excepted; the daemon runs with `PrivateTmp=yes`).
- All files root:root, no setuid/setgid, nothing group- or world-writable (checked by the builder).
- Device access: `/dev/fastrpc-cdsp` and `/dev/dma_heap/system` get group `render`, mode 0660. On armadaOS the default user
  is in `render`, so the user's own programs can open them too (not only the daemon); `/dev/fastrpc-cdsp-secure` (the
  root PD) stays root-only.

## Qualcomm runtime: conditions
- The 18 Qualcomm files are distributed only inside this image (and the armadaOS `vela-npu` package made from it), as part
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

## Package 18 (Vela 0.9.0)

Package 18 is package 17 plus ANF. Everything FSR 4 uses is unchanged: the 17 + 17 + 17 FSR 4 graphs, the self-check and the `low4/` set.

- **ANF graphs.** Five files `anf360s32.bin`, `anf480s32.bin`, `anf540s32.bin`, `anf648s32.bin`, `anf720s32.bin` (HTP v73 only; the
  Snapdragon 8 Elite and the Snapdragon 8 Gen 3 do not get ANF). Each holds 32 graphs, one per jitter phase. What they are, how
  they were built and md5/sha256 of each file: [`graphs/pkg18/README.md`](graphs/pkg18/README.md).
- **The daemon** (`fsr4npud`, md5 `97a022306f16e5cf364c822e6afe82e6` in the image, source in [`src/`](src/README.md)) opens one
  socket per graph name, the 17 FSR 4 names and the five `fsr4npud_anf<canvas>s32` ones. A game reaches the family of its own
  render size only. A family loads when its first client connects (about 0.2 to 0.4 s; meanwhile the daemon answers "busy" and the
  upscaler shows SGSR 2), idle families unload, least recently used first.
- **The unit** (`units/vela-npu.service`, md5 `7871586a2e343ce3a4f3488c3860ba49`): the five family sockets are added to `--graphs`,
  and `--max-loaded 4 --max-loaded-graphs 48` replaces `--max-loaded 3` (a family counts 32 graphs, an FSR 4 graph 1; the DSP
  session holds about 52). Known gap: a family requested while every loaded context has a client and the total would pass 48 is
  loaded anyway.
- **The installer** checks the output hash of phase 0 of each family on a Snapdragon 8 Gen 2 (other chips: "not checked"), and says
  "Vela NPU: off on this unit - the NPU gives wrong results here (checked at start); Vela runs FSR 4 on the GPU" when the daemon
  stops with exit code 4 (both FSR 4 graph sets wrong on the start self-check, issue #2), with exit code 3.
- Differences to package 17 (md5 of the files that changed): `diff_17_18` is in the build log of the release; the changed files are
  the daemon, the unit, `MANIFEST.md5`, `extension-release.armada-vela-npu`, the licence `README.txt` and the five ANF files.
- The package 18 image build is byte-reproducible: two builds gave the same sha256.
