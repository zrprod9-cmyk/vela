"""0.8.11, 0.8.13: Steam's per-game window (SteamClient.Apps.GetResolutionOverrideForApp) in the panel's game
state, the plan following it, the steps when it is not the window picked in "window pick", game_res naming what the game really
gives (OptiScaler.log), and the launch-option cleanup keeping foreign tokens (~/lsfg of Decky LSFG-VK).
Case (Into the Dead 2239710): "Balanced" set the override 1280x720, "Manual" kept it, the panel did not show
it and the game offered max 1280x720. Run: python -I tests/test_window.py
The fake library stays in a fresh temp folder (vela_wn_*) and is not deleted."""
import os
import shutil
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import presets  # noqa: E402

# ---- window_size: Steam's override as a size
W = presets.window_size
assert W("1280x720") == "1280x720" and W("1280×720") == "1280x720" and W(" 1600x900 ") == "1600x900"
assert W("Default") == W("default") == W("Native") == W("") == "1920x1080"
assert W(None) is None and W("abc") is None and W("0x0") is None

# ---- 0.8.15: no "window pick"; the game's window is always the full screen (test_presets.py: window_fix, apply_model)
assert presets.SCREEN == "1920x1080"
# ---- ~/lsfg is not Vela's (Decky LSFG-VK's wrapper): every cleanup keeps it
import sgsr3  # noqa: E402
FOREIGN = "~/lsfg /usr/libexec/armada/armada-game-launch /var/home/armada/.local/share/optiscaler/optiscaler-launch.sh %command%"
assert sgsr3.strip_ours("VELA_NPU=1 OPTI_SGSR3_STUDENT=1 OPTI_SGSRFG=1 " + FOREIGN) == FOREIGN
assert sgsr3.drop_api_flags(FOREIGN + " -dx12") == FOREIGN
print("test_window: ok")
