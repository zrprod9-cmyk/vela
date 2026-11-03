"""Gamescope's scaling filter is Steam's own (0.8.15).

Up to 0.8.14 this module pushed Gamescope filters with `gamescopectl`: a whole-window profile per game (SGSR 1, FSR 1, NIS ...),
Linear for a game Vela upscales, and "stretch to screen" over a 720p / 900p "window pick". The second upscale was removed
(a game without DLSS / FSR gets the console's built-in FSR 1 upscale): a Vela game's window
is the full 1920x1080 screen, and a game without DLSS / FSR uses Steam's own FSR 1 (quick menu → Performance). So Vela
never writes a Gamescope filter any more; once, after the update, it puts Steam's own choice back (restore_steam_filter), in case
an older Vela left its filter in Gamescope (a crash skips the old unload). What stays here: the settings file with the list of
games Vela is set up in ("sgsr3", the 10 s reconcile loop) and the LL / FG migration list ("llfg_pending").

Gamescope convars are write-only from the outside: running `gamescopectl <convar>` without a value RESETS it, so Steam's choice is
read from the root window atoms (xprop) instead.
"""
import json
import os
import re
import subprocess
import threading

# GamescopeUpscaleFilter as written by Steam into GAMESCOPE_NEW_SCALING_FILTER (LINEAR=0, NEAREST, FSR, NIS, PIXEL)
STEAM_FILTERS = {0: "linear", 1: "nearest", 2: "fsr", 3: "nis", 4: "pixel", 5: "sgsr"}
GAMESCOPECTL = "/usr/bin/gamescopectl"
XPROP = "/usr/bin/xprop"
ATOMS = ("GAMESCOPE_NEW_SCALING_FILTER", "GAMESCOPE_FSR_SHARPNESS")
# keys of 0.8.14 and older that steered Gamescope: dropped once Steam's own filter is back
OLD_KEYS = ("games", "post", "enabled")


class Upscaler:
    def __init__(self, settings_dir, home, log=print):
        self.path = os.path.join(settings_dir, "settings.json")
        self.home = home
        self.log = log
        self.lock = threading.Lock()
        self.cfg = {"sgsr3": []}
        try:
            with open(self.path, encoding="utf-8") as f:
                self.cfg.update(json.load(f))
        except (OSError, ValueError):
            pass
        # an older Vela may have pushed a filter: a profile, a stretch, or Linear for a game it upscaled
        old = any(self.cfg.get(k) for k in ("games", "post")) or bool(self.cfg.get("sgsr3"))
        self.reset_pending = old and not self.cfg.get("gamescope_reset")
        self.atoms = {}

    def _save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.cfg, f, indent=2)
        os.replace(tmp, self.path)

    def set_sgsr3(self, appid, on):
        """The list of games Vela is set up in (the reconcile loop keeps their logs and DLLs); no Gamescope write."""
        appid = str(appid)
        with self.lock:
            games = [a for a in self.cfg["sgsr3"] if a != appid] + ([appid] if on else [])
            if games != self.cfg["sgsr3"]:
                self.cfg["sgsr3"] = games
                self._save()

    # ---- the one-time migration: Steam's own filter back ------------------------------------------------------------

    def _env(self):
        env = {k: v for k, v in os.environ.items() if k not in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONHOME", "PYTHONPATH")}
        uid = os.getuid()
        env["XDG_RUNTIME_DIR"] = f"/run/user/{uid}"
        primary = f"/run/user/{uid}/gamescope-primary"
        env["GAMESCOPE_WAYLAND_DISPLAY"] = "gamescope-primary" if os.path.exists(primary) else "gamescope-0"
        env["DISPLAY"] = ":0"
        return env

    def _ctl(self, convar, value):
        r = subprocess.run([GAMESCOPECTL, convar, str(value)], env=self._env(), capture_output=True, text=True, timeout=5)
        if r.returncode != 0:
            raise RuntimeError(f"gamescopectl {convar} {value}: {(r.stderr or r.stdout).strip()[:200]}")

    def _read_atoms_once(self):
        r = subprocess.run([XPROP, "-root", *ATOMS], env=self._env(), capture_output=True, text=True, timeout=5)
        self._parse(r.stdout)

    def _parse(self, text):
        for name, value in re.findall(r"^(\w+)\(CARDINAL\) = (\d+)", text or "", re.M):
            self.atoms[name] = int(value)

    def _steam_choice(self):
        return STEAM_FILTERS.get(self.atoms.get("GAMESCOPE_NEW_SCALING_FILTER", 0), "linear"), \
            max(0, min(20, self.atoms.get("GAMESCOPE_FSR_SHARPNESS", 2)))

    def restore_steam_filter(self):
        """Once after the update (reset_pending): Gamescope gets Steam's own filter and sharpness back and the old keys go.
        False while Gamescope cannot be reached (the reconcile loop tries again)."""
        if not self.reset_pending:
            return False
        try:
            self._read_atoms_once()
            filt, sharp = self._steam_choice()
            self._ctl("scaling_filter", filt)
            self._ctl("scaling_sharpness", sharp)
        except (OSError, subprocess.SubprocessError, RuntimeError) as e:
            self.log(f"upscale: Steam's filter not restored yet: {e}")
            return False
        with self.lock:
            for k in OLD_KEYS:
                self.cfg.pop(k, None)
            self.cfg["gamescope_reset"] = True
            self._save()
        self.reset_pending = False
        self.log(f"upscale: Gamescope back on Steam's own filter ({filt}, sharpness {sharp}); Vela writes no filter any more")
        return True
