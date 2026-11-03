"""Upscale by a Vulkan layer, for any game (also without DLSS / FSR / XeSS), path "S-any".

The game draws at a lower resolution (Steam's resolution override, as the plugin's "game resolution"; or the
game's own setting in a 1080p window), our layer VK_LAYER_UPSCALE_any (assets/layer)
creates the real 1920x1080 swapchain, hands the game proxy images of its size and on every
present scales the frame with S-any "lut" (a learned 16-tap filter per edge class, ~0.8-0.9 ms on Adreno 740 at
720p -> 1080p) straight into the swapchain image. Gamescope then gets 1080p and stays on Linear (scale 1).

Nothing is installed anywhere: the layer is found through VK_ADD_IMPLICIT_LAYER_PATH=<plugin>/assets/layer and is
active only with UPSCALE_ANY=1 -- both in the game's Steam launch options, like the rest of the variables:
  TUP_OUTPUT=1920x1080 TUP_UPSCALE_SPV/LUT=<table for the scale> TUP_UPSCALE_TIMING=1 TUP_LOG_FILE=<settings>/…
  TUP_UPFILTER_FILE=<settings>/anygame/filter-<appid> ("linear" in it for a few seconds = the panel's "compare")
Off puts back the launch options and the resolution override exactly; for a Unity game also the screen values in
the prefix's user.reg (Unity remembers the reduced resolution there, found with TUNIC), once the game has quit.
"""
import json
import os
import re
import time

import gameutil
import launchopts

LAYER_NAME = "VK_LAYER_UPSCALE_any"
ENABLE_ENV = "UPSCALE_ANY=1"
# render resolution -> S-any table (assets/layer/sany/<x>); "game": no override, the game is set to 720p by itself
RENDERS = {"1280x720": "x1.5", "960x540": "x2", "640x360": "x3", "game": "x1.5"}
FILTERS = ["sany", "linear"]
DEFAULT_OPTS = {"render": "960x540", "filter": "sany", "mode": "balance", "res": "auto"}
MODES = ["quality", "balance", "speed", "custom"]
OUTPUT = "1920x1080"
LOG_TAIL = 256 * 1024

_RX_SWAP = re.compile(r"upscale: game swapchain (\d+)x(\d+) -> real (\d+)x(\d+)")
_RX_GPU = re.compile(r"upscale: \d+ presents, (.+?) \d+x\d+ -> \d+x\d+, avg GPU ([\d.]+) ms")


def log_summary(path, since=0.0):
    try:
        st = os.stat(path)
    except OSError:
        return None
    if st.st_mtime < since:
        return None
    with open(path, "rb") as f:
        if st.st_size > LOG_TAIL:
            f.seek(st.st_size - LOG_TAIL)
        text = f.read().decode("utf-8", "replace")
    sw = _RX_SWAP.findall(text)
    gpu = _RX_GPU.findall(text)
    off = re.search(r"shader off|pipeline creation failed|not 8-bit RGBA/BGRA|no TRANSFER_DST", text)
    return {"time": st.st_mtime, "loaded": "instance created" in text or bool(sw),
            "render": f"{sw[-1][0]}x{sw[-1][1]}" if sw else None, "output": f"{sw[-1][2]}x{sw[-1][3]}" if sw else None,
            "path": gpu[-1][0] if gpu else None, "gpu_ms": float(gpu[-1][1]) if gpu else None,
            "fallback": off.group(0) if off else None}


class AnyGame:
    def __init__(self, assets_dir, home, settings_dir, log=print):
        self.assets = assets_dir                  # <plugin>/assets/layer
        self.home = home
        self.dir = os.path.join(settings_dir, "anygame")
        self.log = log

    # -- files

    def available(self):
        return os.path.isfile(os.path.join(self.assets, "libVkLayer_UPSCALE_any.so")) and \
            os.path.isfile(os.path.join(self.assets, "VkLayer_UPSCALE_any.json"))

    def _paths(self, appid, render):
        x = RENDERS[render]
        return {"spv": os.path.join(self.assets, "sany", x, "lut_layer.spv"),
                "lut": os.path.join(self.assets, "sany", x, "lut_f16.bin"),
                "log": os.path.join(self.dir, f"layer-{appid}.log"),
                "filter": os.path.join(self.dir, f"filter-{appid}")}

    def _state_path(self, appid):
        return os.path.join(self.dir, f"{appid}.json")

    def read(self, appid):
        try:
            with open(self._state_path(appid), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def _write(self, appid, st):
        os.makedirs(self.dir, exist_ok=True)
        tmp = self._state_path(appid) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(st, f, indent=2)
        os.replace(tmp, self._state_path(appid))

    # -- launch options

    def tokens(self, appid, opts):
        p = self._paths(appid, opts["render"])
        t = [f"VK_ADD_IMPLICIT_LAYER_PATH={self.assets}", ENABLE_ENV, "TUP_LOG_EVERY=0", f"TUP_LOG_FILE={p['log']}",
             f"TUP_OUTPUT={OUTPUT}", "TUP_UPSCALE_TIMING=1", f"TUP_UPFILTER_FILE={p['filter']}"]
        if opts["filter"] == "sany":
            t += [f"TUP_UPSCALE_SPV={p['spv']}", f"TUP_UPSCALE_LUT={p['lut']}"]
        return t

    @staticmethod
    def strip(options, tokens):
        out = options or ""
        for tok in tokens or []:
            out = re.sub(rf"(?<!\S){re.escape(tok)}(?!\S)", " ", out)
        return re.sub(r"\s+", " ", out).strip()

    def launch_on(self, options, tokens, old_tokens=()):
        base = self.strip(options, old_tokens)
        if "%command%" not in base:
            base = f"%command% {base}".strip()
        return " ".join(list(tokens) + [base])

    # -- status

    def status(self, appid, scan, running=False):
        st = self.read(appid)
        out = {"available": self.available() and bool(scan.get("install_dir")) and not scan.get("anticheat"),
               "layer_found": self.available(), "installed": bool(st and st.get("on")),
               "opts": dict(DEFAULT_OPTS), "log": None, "unity": scan.get("engine") == "Unity",
               "pending_restore": bool(st and not st.get("on") and st.get("unity_saved") is not None)}
        if st and st.get("on"):
            out["opts"] = {**DEFAULT_OPTS, **(st.get("opts") or {})}
            out["log"] = log_summary(self._paths(appid, out["opts"]["render"])["log"], st.get("installed_at", 0))
        return out

    # -- on / off

    def enable(self, appid, opts, launch_options, resolution, scan):
        """Returns the launch options and the resolution override to set (None: leave the override)."""
        clean = dict(DEFAULT_OPTS)
        st = self.read(appid) or {}
        clean.update(st.get("opts") or {})
        clean.update({k: (opts or {})[k] for k in DEFAULT_OPTS if k in (opts or {})})
        if clean["render"] not in RENDERS:
            raise ValueError(f"unknown render resolution {clean['render']}")
        if clean["filter"] not in FILTERS:
            raise ValueError(f"unknown filter {clean['filter']}")
        if clean["mode"] not in MODES:
            raise ValueError(f"unknown mode {clean['mode']}")
        if scan.get("anticheat"):
            raise RuntimeError(f"anti-cheat in the game ({', '.join(scan['anticheat'])}), the layer is not used")
        if not self.available():
            raise RuntimeError("plugin files assets/layer are missing")
        p = self._paths(appid, clean["render"])
        if clean["filter"] == "sany" and not (os.path.isfile(p["spv"]) and os.path.isfile(p["lut"])):
            raise RuntimeError(f"S-any table {RENDERS[clean['render']]} is missing in assets/layer/sany")
        first = not st.get("on")
        if first:
            st = {"on": True, "launch_prev": launch_options, "resolution_prev": resolution,
                  "installed_at": time.time(), "unity_saved": st.get("unity_saved")}
            if scan.get("engine") == "Unity" and st["unity_saved"] is None:
                st["unity_saved"] = self._unity_read(appid)
        tokens = self.tokens(appid, clean)
        current = launch_options if launch_options is not None else st.get("launch_set")
        new_launch = self.launch_on(current, tokens, st.get("tokens") or ())
        st.update({"opts": clean, "tokens": tokens, "launch_set": new_launch, "updated_at": time.time()})
        os.makedirs(self.dir, exist_ok=True)
        with open(p["filter"], "w") as f:
            f.write("shader\n" if clean["filter"] == "sany" else "linear\n")
        if first:
            try:
                os.remove(p["log"])  # the layer appends: one log per "on"
            except OSError:
                pass
        self._write(appid, st)
        self.log(f"anygame: {appid} on: {clean}, launch: {new_launch}")
        res = None if clean["render"] == "game" else clean["render"]
        return {"launch_options": new_launch, "resolution": res, "status": self.status(appid, scan)}

    def disable(self, appid, launch_options, scan, running=False):
        st = self.read(appid)
        if not st or not st.get("on"):
            raise RuntimeError("the layer upscale is not on for this game")
        cur = launch_options
        if cur is None or cur == st.get("launch_set"):
            restored = st.get("launch_prev") or ""
        else:
            # 0.8.17: only the layer's own tokens go (launchopts), the player's stay as they are
            mine = set(st.get("tokens") or ())
            restored = launchopts.remove(cur, launchopts.derive(st.get("launch_prev"), st.get("launch_set"),
                                                                lambda t: t in mine))
        st["on"] = False
        self._write(appid, st)
        if st.get("unity_saved") is not None and not running                 and not gameutil.prefix_busy(gameutil.prefix_dir(self.home, appid)):
            self.unity_restore(appid)
        elif st.get("unity_saved") is None:
            self._cleanup(appid)
        self.log(f"anygame: {appid} off, launch: {restored}")
        return {"launch_options": restored, "resolution": st.get("resolution_prev"),
                "status": self.status(appid, scan, running)}

    def _cleanup(self, appid):
        st = self.read(appid) or {}
        for f in (self._state_path(appid), os.path.join(self.dir, f"filter-{appid}")):
            try:
                os.remove(f)
            except OSError:
                pass
        return st

    def compare(self, appid, seconds):
        """Linear blit for a few seconds (the layer reads the filter file every 30 presents)."""
        st = self.read(appid)
        if not st or not st.get("on") or st["opts"].get("filter") != "sany":
            return False
        p = self._paths(appid, st["opts"]["render"])["filter"]
        with open(p, "w") as f:
            f.write("linear\n")
        import threading

        def back():
            s2 = self.read(appid)
            if s2 and s2.get("on"):
                with open(p, "w") as f:
                    f.write("shader\n")
        threading.Timer(float(seconds), back).start()
        return True

    # -- Unity's remembered resolution

    def _user_reg(self, appid):
        pfx = gameutil.prefix_dir(self.home, appid)
        return os.path.join(pfx, "user.reg") if pfx else None

    def _unity_read(self, appid):
        path = self._user_reg(appid)
        if not path or not os.path.isfile(path):
            return {}
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            return gameutil.unity_screen_values(f.read())

    def unity_restore(self, appid):
        st = self.read(appid)
        if not st or st.get("on") or st.get("unity_saved") is None:
            return False
        path = self._user_reg(appid)
        if path and os.path.isfile(path):
            with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
                text = f.read()
            new = gameutil.unity_restore(text, st["unity_saved"])
            if new != text:
                tmp = path + ".upscale-tmp"
                with open(tmp, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                    f.write(new)
                os.replace(tmp, path)
                self.log(f"anygame: {appid}: Unity screen values restored in user.reg")
        self._cleanup(appid)
        return True

    def has_pending(self):
        try:
            names = [f[:-5] for f in os.listdir(self.dir) if f.endswith(".json")]
        except OSError:
            return False
        for appid in names:
            st = self.read(appid)
            if st and not st.get("on") and st.get("unity_saved") is not None:
                return True
        return False

    def reconcile(self, running):
        """Unity values wait for the game (and its wineserver) to quit."""
        try:
            names = [f[:-5] for f in os.listdir(self.dir) if f.endswith(".json")]
        except OSError:
            return
        for appid in names:
            st = self.read(appid)
            if st and not st.get("on") and st.get("unity_saved") is not None and appid not in running                     and not gameutil.prefix_busy(gameutil.prefix_dir(self.home, appid)):
                try:
                    self.unity_restore(appid)
                except OSError as e:
                    self.log(f"anygame: {appid}: user.reg restore failed: {e}")
