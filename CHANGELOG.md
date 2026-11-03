# Changelog

## 0.8.17 — 2026-11-03

NPU part only; the plugin stays `vela-0.8.16.zip`.

- **AYN Odin 3 (Snapdragon 8 Elite): FSR 4 on the NPU, experimental.** The NPU part (package 13) now carries the FSR 4
  network for both chips and picks the right one by itself.
- Snapdragon 8 Gen 2 devices: no change.

## 0.8.16 — 2026-11-02

First public release.

- **FSR 4 Fast**: the same FSR 4 network in a lighter mode, faster than FSR 4.
- **DLSS input works with FSR 4** in more games.
- **Game profiles** switch the in-game upscaler on for Control, Resident Evil 4 and Deep Rock Galactic (your own setting is kept).
- **Chip detection**: each device gets the models its chip can run.
- **Report a problem** button in the panel.
- The panel is in English.
- Godot games are shown as unsupported.
- The NPU part waits for the GPU only for a bounded time, so closing a game can no longer hang the GPU.
