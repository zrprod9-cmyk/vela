# Changelog

## 0.8.17 (draft, not released)

NPU part only; the plugin zip stays `vela-0.8.16.zip`.

### New
- **NPU package 13** (`vela-npu-sysext-13.raw`, sha256 `ac9672ba83e20d4e001261f6c22c7ba27f15d93fd9376c22b62bc4ba5e653deb`): one image for both chips.
  - **Snapdragon 8 Elite (SM8750, AYN Odin 3): FSR 4 on the NPU (experimental, untested on a device).** The 7 FSR 4 graphs
    built for HTP v79 (`/usr/lib64/vela-npu/v79/`), the QNN 2.47.0.260601 HTP v79 runtime (4 files, unmodified) and two
    systemd drop-ins: on `qcom,sm8750` each daemon loads the v79 build of its graph; on other chips nothing changes.
    On Qualcomm's x86 HTP simulator the v79 graphs give the same output as the v73 graphs, byte for byte.
  - **SM8550: unchanged.** Every program, graph and library of package 12 is in package 13 byte for byte. The systemd
    units, the udev rule and the modules-load file differ only in their comments (now built from `npu/units/`), and the
    three files that describe the image (`extension-release`, `MANIFEST.md5`, the licence folder's `README.txt`) are new.
- `install-vela-npu.sh` accepts `qcom,sm8750` (it prints "SM8750 (Odin 3): not tested on a device yet") and checks every
  daemon against the v79 output hashes there; another device's own DSP shell files are used with a warning, as before.

## 0.8.16 — 2026-11-02

Release asset `vela-0.8.16.zip`, md5 `11d684e137ea0e0f4d8777ffe5731253`.

### New
- **FSR 4 Fast** model: the same FSR 4.1.1 network in a cheaper mode, about 1.9× faster than exact FSR 4 on the GPU
  (no-game bench: RE4 720p Performance 1.744 → 0.915 ms; 1080p Ultra Performance 3.67 → 1.93 ms). Uses the NPU like FSR 4.
- **FSR 4 Fast 1.5× mode by default** when the game asks for output = 3 × render (1080p Ultra Performance): Fast Performance to
  2 × render plus one cubic 1.5× pass; bench 1080p UP ~2.09 → ~1.51 ms. Hidden off-switch: live key `fastup15=0` or
  `VELA_FAST_UP15=0`.
- **DLSS input with screen-size or jittered motion vectors now works with FSR 4** (MV pass in the fork's FSR 4 bridge);
  checked in Bright Memory: Infinite with DLSS (853×480 → 1280×720). The pass costs 0.087 ms (screen-size MV) / 0.062 ms
  (jittered render MV) at 720p.
- **Game profiles write the in-game upscaler setting** (only when it is switched off; the player's own value is kept and
  restored on Off): Control (output 1920×1080, render 960×540), Resident Evil 4 (FSR 2 on, Performance), Ghostrunner
  (DLSS on, Performance), Deep Rock Galactic (UpscalingType 0 → FSR). Config files in the game folder and in the Proton prefix.
- **Chip detection** from the device tree: SM8550 unchanged; SM8750 (Odin 3) gets FSR 4 on the GPU, FSR 3, SGSR 2 (no NPU,
  no FSR 4 Fast) until an HTP v79 NPU package exists; unknown chips get no NPU.
- **Report a problem** button in the panel: collects an anonymised report (≤ 5 MB), shows the file list,
  sends it on your click; one question after a run that went wrong. See Privacy in the README.
- **English only**: the panel, its hints and messages are in English; it no longer follows Steam's language.
- **Godot games** are recognised and shown as unsupported (was a wrong "FSR 4" verdict).
- NPU hint points to NPU system part package 12 (faster graphs: 1080p Ultra Performance 10.86 → 7.45 ms on the NPU).

### Fixed
- Ghostrunner: the profile's DLSS switch was never written (file mixes CRLF and LF).
- Control: renders 960×540 also when the game had saved a 720p render size from an old window setting.

### Changed
- NPU layer build 11: an NPU job slower than 20 ms sends that one frame to the GPU network and keeps the daemon
  connection; the connection is dropped only when the daemon gives no answer for 400 ms (was 100 ms), so a slow job
  no longer switches the NPU off for about a second.
- Cyberpunk 2077 profile: `VKD3D_CONFIG=no_upload_hvv` and `Spoofing.Dxgi=false`. They do **not** fix the game's GPU hang
  (it happens without Vela too); Cyberpunk is listed as "do not use".

### Known issues
See README → Known issues (new NPU part installer, Control HDR on SDR, Ghostrunner, Cyberpunk).
