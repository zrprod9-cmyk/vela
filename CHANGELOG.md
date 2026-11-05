# Changelog

## 0.8.21 — 2026-11-05

- **FSR 4 on the NPU at five more game resolutions:** 1600x904, 1440x810, 1280x800, 1152x648 and 960x540. 960x540 has run in a game so far, the others passed the installer's check. All five are experimental on 8 Elite: not tested there yet.
- **One NPU service instead of seven.** The NPU part (package 14) runs all its FSR 4 networks from a single service.
- **The NPU part needs one reinstall with sudo.** In Desktop mode open Konsole and run the command the panel shows:
  `curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/latest/download/install-vela-npu.sh | sudo sh`
  Until then package 13 keeps working at the resolutions it already had.
- Cyberpunk 2077: the panel no longer sits on “Loading…”.
- The panel sees the NPU part as soon as it is installed, without a restart.
- The panel no longer asks for 1920x1080 while the NPU runs the game's resolution. If the resolution only needs a newer NPU
  part, it says to update that instead.
- Non-Steam shortcuts (Heroic, Lutris…) now say they are not supported yet.
- The panel no longer asks for a report when the NPU only has no network for the game's resolution: the game runs on the GPU and the panel's hint says so.
- Known issue: PRAGMATA crashes with Vela on, seen on 8 Elite. Play it with Vela off. The cause is not found yet.

## 0.8.20 — 2026-11-04

- **Vela no longer writes into game folders.** Its files are shown to the game only while it runs (an overlay), so
  Steam's file check has nothing to repair and **Off** leaves nothing behind. Games set up by an older Vela move over by
  themselves the next time you open the panel with the game closed.
- Games that hand over their image in a compact HDR format (for example PRAGMATA) no longer crash with FSR 4.
- The panel says when a game ran in DirectX 11 (Vela needs DirectX 12) and how to switch it.

## 0.8.19 — 2026-11-03

- **Fix for games hanging at start on Snapdragon 8 Elite devices** (AYN Odin 3, KONKR Pocket FIT Elite). If a game still
  hangs on your device, please send a report.
- **Updates from the panel.** Vela checks for a new version once a day, or when you press **Check now**, and updates with
  one press. The daily check can be turned off.
- Vela stays out of games with anti-cheat and says why in the panel.

## 0.8.18 — 2026-11-03

- **Your launch options are safe.** Vela now adds and removes only its own launch arguments; whatever you had before, or add
  later, stays where it was. If an older version removed something from a game you have not edited since, it comes back.
- The NPU part installer accepts the Snapdragon 8 Elite (Odin 3) results, which are correct but not bit-identical.
- The panel's NPU install command always points to the latest release.

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
