<div align="center">

<img src="docs/banner.png" alt="Vela: FSR 4 on your handheld's NPU" width="100%">

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)
[![Platform: armadaOS](https://img.shields.io/badge/platform-armadaOS-16324a)](#supported-devices)
[![Chip: Snapdragon handhelds](https://img.shields.io/badge/chip-Snapdragon%20handhelds-6fb7e6)](#supported-devices)
[![Release](https://img.shields.io/github/v/release/zrprod9-cmyk/vela?color=2ea043)](../../releases/latest)
[![Decky plugin](https://img.shields.io/badge/Decky-plugin-8a5cf6)](https://github.com/SteamDeckHomebrew/decky-loader)

<a href="https://buymeacoffee.com/zrprod9"><img src="https://img.shields.io/badge/Buy%20me%20a%20coffee-zrprod9-FFDD00?style=for-the-badge&logo=buymeacoffee&logoColor=black" alt="Buy me a coffee"></a>

[Install](#install) · [Games](#games) · [How it works](#how-it-works) · [FAQ](#faq) · [Report a problem](#report-a-problem)

</div>

**Vela** is a [Decky Loader](https://github.com/SteamDeckHomebrew/decky-loader) plugin for **armadaOS** on Snapdragon handhelds.
It replaces a game's own upscaler (DLSS, FSR 2/3 or XeSS) with **FSR 4**, which runs on the handheld's **Hexagon NPU**, or with
**FSR 4 Fast**, **FSR 3** or **SGSR 2**. You pick the model per game in the Quick Access panel.

<p align="center">
  <a href="docs/compare.png"><img src="docs/compare.png" alt="Bright Memory: Infinite on Retroid Pocket 6, five upscalers at the same spot" width="100%"></a>
  <br><sub>Bright Memory: Infinite on Retroid Pocket 6, 640×360 → 1920×1080 (click for full size).</sub>
</p>

## Features

| | |
|---|---|
| 🧠 **FSR 4 on the NPU** — AMD's FSR 4 network runs on the Hexagon NPU, so the GPU keeps its time for the game | ⚡ **FSR 4 Fast** — the same network with lighter GPU passes, for more FPS |
| 🎮 **Works with DLSS, FSR 2/3 and XeSS** — whatever upscaler the game offers | 🛠 **Game profiles** — Control, Resident Evil 4 and Deep Rock Galactic are set up for you |
| 🔁 **Switch while playing** — change the model in the panel; **Off** puts the game back as it was | 🩺 **Report a problem** — one button sends an anonymised report, only when you press Send |

## Models

| model | what it is |
|---|---|
| **FSR 4** | AMD's FSR 4 network. Runs on the NPU when the Vela NPU part is installed, otherwise on the GPU |
| **FSR 4 Fast** | the same network in a lighter mode, faster |
| **FSR 3** | AMD FSR 3.1 |
| **SGSR 2** | Qualcomm Snapdragon Game Super Resolution 2, the lightest |
| **Off** | the game as it was |

## Supported devices

| device | chip | status |
|---|---|---|
| Retroid Pocket 6 | Snapdragon 8 Gen 2 | ✅ everything, FSR 4 on the NPU |
| AYN Odin 2 / Odin 2 Mini / Odin 2 Portal / Thor | Snapdragon 8 Gen 2 | ✅ same chip as the Retroid Pocket 6 |
| AYN Odin 3 and other Snapdragon 8 Elite devices | Snapdragon 8 Elite | 🧪 FSR 4 on the NPU, experimental |
| Retroid Pocket 5 / Retroid Pocket Mini | Snapdragon 865 | FSR 3 and SGSR 2 (no FSR 4) |

You need **armadaOS** with **Decky Loader**, and a game that offers **DLSS, FSR 2/3 or XeSS** in its settings.

## Games

| works | does not work |
|---|---|
| Resident Evil 4, Control, Bright Memory: Infinite, Into the Dead: Our Darkest Days, Deep Rock Galactic | games without DLSS / FSR / XeSS, online games with anti-cheat |

Other games with DLSS, FSR 2/3 or XeSS should work too. Tell us how it goes: [Report a problem](#report-a-problem).

## Install

> [!NOTE]
> When Vela is in the **Armada Store**, install it from there. Until then, install the zip by hand.

**1. The plugin**

1. Download the `vela-<version>.zip` from the [latest release](../../releases/latest).
2. In Decky: gear icon → **Developer** (developer mode on) → **Install Plugin from ZIP File** → pick the zip.
3. Vela appears in the Quick Access menu.

**2. The NPU part** (for FSR 4 on the NPU). In Desktop mode, open Konsole, close the game and run:

```sh
curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/latest/download/install-vela-npu.sh | sudo sh
```

It asks for your password once and ends with `Vela NPU: OK`. Without it, FSR 4 runs on the GPU. Details and removal:
[npu/README.md](npu/README.md).

FSR 4 runs on the NPU at 1920x1080, 1600x900 and 1280x720, and at 1600x904, 1440x810, 1280x800, 1152x648 and 960x540
(these five are experimental on Snapdragon 8 Elite). 1366x768 is not supported on the NPU.

<details>
<summary><b>Update and removal</b></summary>

- **Update:** when there is a new version, the Vela panel shows it (Vela checks once a day, or press **Check now**). Press
  **Update** and confirm in Decky. Installing the new zip the same way also works.
- **Take Vela out of a game:** Vela panel → **Off**. Vela does not change the game's folder: it shows its files to the
  game only while the game runs. Where the system cannot do that, Vela copies them in and **Off** puts the game's own
  files back.
- **Remove the plugin:** set every game to **Off** first, then uninstall Vela in Decky.

</details>

## Usage

1. Select the game in the Steam library and open Quick Access → **Vela**.
2. **Pick a model.** The first time you turn Vela on for a running game, restart the game once; after that the model
   switches while you play.
3. **In the game's settings, choose the upscaler** the panel names (for example "choose FSR 3.1 or DLSS"). For Control,
   Resident Evil 4 and Deep Rock Galactic Vela does this for you.
4. If Steam asks DX11 or DX12, choose **DX12**.

## How it works

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/how-it-works-dark.svg">
  <source media="(prefers-color-scheme: light)" srcset="docs/how-it-works-light.svg">
  <img alt="Game, then Vela's OptiScaler fork, then the FSR 4 provider; each frame the network runs on the Hexagon NPU" src="docs/how-it-works-light.svg" width="100%">
</picture>

Vela uses its own fork of [OptiScaler](https://github.com/optiscaler/OptiScaler) (based on
[justradical/Sloptiscaler](https://github.com/justradical/Sloptiscaler)) to take the game's upscaler input, a Vulkan FSR 4
provider, and a small Vulkan layer that runs the FSR 4 network on the NPU. If the NPU is late, that frame runs on the GPU, so
the game never waits on it.

Vela's files stay in its own folder. When a game starts, a small launcher lays them over the game's folder (a bubblewrap
overlay) for that game only, so the installed game is never modified and Steam's file check has nothing to repair.

## Report a problem

Vela panel → **Report a problem** → pick what went wrong → **Collect report** → you see the files → **Send**. You get a number
**`VELA-XXXXXX`**; quote it when you [open an issue](../../issues/new/choose).

## FAQ

<details>
<summary><b>Why the NPU?</b></summary>

The network is the heaviest part of FSR 4. Snapdragon handhelds have a Hexagon NPU built for exactly this kind of work, so Vela
runs the network there and leaves the GPU to the game.
</details>

<details>
<summary><b>Is it AMD's FSR 4?</b></summary>

The **FSR 4** model runs AMD's FSR 4.1.1 network through a Vulkan provider built from
[dvj5411/fsr4vk](https://github.com/dvj5411/fsr4vk). **FSR 4 Fast** uses the same network with lighter passes around it. Vela is
not an official AMD product.
</details>

<details>
<summary><b>Does it work on the AYN Odin 3 and other Snapdragon 8 Elite devices?</b></summary>

Yes, experimentally: install the NPU part as above. We have not had a Snapdragon 8 Elite device to try it on, so please send a report and tell us
how it runs.
</details>

## Known issues

- Do not use the Decky **OptiScaler** plugin and Vela on the same game.
- Before uninstalling Vela, set your games to **Off**.

## Privacy

Once a day Vela asks GitHub whether there is a new version; the request carries only Vela's version number, and you can
turn it off in the panel. Apart from that, Vela never sends anything on its own. **Report a problem** collects the run logs, Vela's settings for the game and the system
versions, removes your user and device names, IDs, addresses and anything that looks like a password, and shows you the files
before sending. Reports go to the developers only and are used only to fix Vela. A copy stays in
`~/homebrew/settings/upscale/reports`. To have a report deleted, send us its `VELA-…` number.

## Source and licences

Everything is GPL-3.0 open source: the plugin (`plugin/`), our OptiScaler fork as patches (`fork/`), the FSR 4 provider
(`provider/`), the NPU layer (`npu-layer/`) and the NPU part (`npu/`). The NPU part includes Qualcomm's AI Engine Direct
runtime under Qualcomm's licence, which ships inside it. Details: [LICENSING.md](LICENSING.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Contributing

Bug reports with a `VELA-XXXXXX` number, games that work or don't, and pull requests are welcome. Build the plugin with
`pnpm install && pnpm build`, then `build_zip.py`.

## Support

If Vela makes your games look better, you can [buy me a coffee](https://buymeacoffee.com/zrprod9) ☕

## Credits

- **OptiScaler** team: https://github.com/optiscaler/OptiScaler
- **justradical / Sloptiscaler**, the ARM64EC OptiScaler build our fork starts from: https://github.com/justradical/Sloptiscaler
- **AMD** for FSR 4, FSR 3 and the FidelityFX SDK; **dvj5411/fsr4vk** for the Vulkan FSR 4 provider.
- **Qualcomm** for Snapdragon Game Super Resolution.
- **OptiPatcher**, **fakenvapi**, **LatencyFleX** (see the notices).
- **armadaOS / Armada developers** and **Decky Loader**.

<sub>Vela is an independent project. FSR is a trademark of AMD, Snapdragon and Hexagon are trademarks of Qualcomm, DLSS is a
trademark of NVIDIA; Vela is not affiliated with or endorsed by them.</sub>
