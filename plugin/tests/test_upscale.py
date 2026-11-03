"""py_modules/upscale.py since 0.8.15: Vela writes no Gamescope filter; once after
the update it puts Steam's own filter back (an older Vela may have left a profile / stretch / Linear in Gamescope) and drops the old
keys. gamescopectl and xprop replaced by recorders. Run: python -I tests/test_upscale.py"""
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "py_modules"))
import upscale  # noqa: E402

calls = []


class Fake(upscale.Upscaler):
    reachable = True

    def _ctl(self, convar, value):
        if not self.reachable:
            raise RuntimeError("gamescope not up")
        calls.append((convar, value))

    def _read_atoms_once(self):
        if not self.reachable:
            raise OSError("no X")
        self._parse("GAMESCOPE_NEW_SCALING_FILTER(CARDINAL) = 2\nGAMESCOPE_FSR_SHARPNESS(CARDINAL) = 3\n")


def take():
    out = list(calls)
    calls.clear()
    return out


# a settings file of 0.8.14: profiles, a stretch, games Vela upscaled (Gamescope on Linear for them)
d = tempfile.mkdtemp()
with open(os.path.join(d, "settings.json"), "w", encoding="utf-8") as f:
    json.dump({"enabled": True, "games": {"1566200": {"filter": "sgsr"}}, "post": {"2050650": ["sgsr", 5]},
               "sgsr3": ["2050650", "870780"], "llfg_pending": ["1"]}, f)
up = Fake(d, "/nonexistent", log=lambda *a: None)
assert up.reset_pending, "an old config: Steam's filter has to come back once"
up.reachable = False
assert not up.restore_steam_filter() and up.reset_pending, "Gamescope not up: the reconcile loop tries again"
up.reachable = True
assert up.restore_steam_filter()
assert take() == [("scaling_filter", "fsr"), ("scaling_sharpness", 3)], "Steam's own choice (FSR, 3) from the root atoms"
saved = json.load(open(os.path.join(d, "settings.json"), encoding="utf-8"))
assert "games" not in saved and "post" not in saved and "enabled" not in saved, saved
assert saved["sgsr3"] == ["2050650", "870780"] and saved["llfg_pending"] == ["1"] and saved["gamescope_reset"], saved
assert not up.restore_steam_filter() and take() == [], "only once"
up2 = Fake(d, "/nonexistent", log=lambda *a: None)
assert not up2.reset_pending, "after the reset: never again"

# a fresh install: nothing to put back, nothing written to Gamescope ever
fresh = Fake(tempfile.mkdtemp(), "/nonexistent", log=lambda *a: None)
assert not fresh.reset_pending and not fresh.restore_steam_filter() and take() == []
fresh.set_sgsr3("870780", True)
fresh.set_sgsr3("870780", False)
assert take() == [], "set_sgsr3 keeps the list only"
assert not hasattr(upscale.Upscaler, "set_profile") and not hasattr(upscale.Upscaler, "set_post")
print("ok")
