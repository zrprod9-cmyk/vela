"""GPU floor (0.8.2): a minimum GPU clock while a game Vela upscales is running.

Why: the Balanced profile (armada-powerd, gpu_min=0.0) lets simple_ondemand drop the Adreno to 220 MHz; the Vela chain
then runs about 3x slower (FSR 4.1 Lite + NPU, 240 frames: 4.65 ms/frame pinned at 680 MHz, 13.2 ms unpinned).

Where Armada keeps a GPU minimum (build 20261010.c3d178c): only per power
profile, `gpu_min` in /usr/share/armada/power-profiles.conf + /etc/armada/power-profiles.conf (root, written by the
root plugin Armada Control). Per-game tweaks (/etc/armada/game-tweaks.json, root) carry no GPU key. So there is no
user-writable per-game GPU minimum in Armada; Vela keeps its own per-game value in <settings>/gpufloor.json and applies
it to /sys/class/devfreq/<gpu>/min_freq, which udev makes group-writable for wheel (60-armada-perf-acls.rules; the
user armada is in wheel). armada-powerd writes min_freq only when it applies a profile (boot, profile or GPU level
change, Reload, resume), never on a timer, so the reconcile loop puts the floor back within 10 s after that.

Rules:
- on: the game has Vela on (sgsr3 enable) and its floor is not 0; applied only while that game runs.
- never lowers a higher minimum (Performance profile gpu_min=1.0, a pinned clock); never above the profile's max_freq.
- userspace governor (Armada's manual GPU clock) is the user's choice: left alone.
- restore: when no floored game runs, min_freq goes back to the value before Vela raised it, but only if min_freq still
  holds Vela's value (powerd may have applied a profile meanwhile: then its value stays).

Access fixes (the plugin log showed "cannot write ... min_freq: [Errno 13]" 30 times; samples of min_freq flipped 220/680):
- Decky drops plugin processes to uid 1000 WITHOUT supplementary groups (/proc/<pid>/status "Groups:" empty), so the
  udev group-write for wheel never applied and the floor was never set. On EACCES the write goes through `sg <group>`
  (setuid newgrp; no password for a listed member: /etc/group wheel:x:10:armada), only when the user is a member.
- Reading min_freq returns max(user floor, the msm driver's fence-deadline boost QoS): the samples flip 220/680
  every 0.1-0.5 s with no writer. One read can therefore see "680" while the user floor is 220. The floor check and the
  "prev" to restore use the lowest of several reads (MIN_READS over READ_SPAN_S), stopping at the first low read.
"""
import glob
import json
import os
import shlex
import shutil
import subprocess
import time
try:
    import grp
    import pwd
except ImportError:  # not Linux (tests on Windows): no sg route
    grp = pwd = None

# 680 = the clock all Vela 0.8.x benchmark figures were measured at (the Adreno's top step on the Retroid Pocket 6);
# the lowest clock that keeps the Vela chain near its pinned 4.65 ms is not measured yet.
VELA_GPU_MIN_MHZ = 680

DEVFREQ_GLOB = "/sys/class/devfreq/*.gpu"
FILE = "gpufloor.json"
MIN_READS = 4          # boost-free estimate of the user floor: lowest of this many reads ...
READ_SPAN_S = 0.6      # ... spread over this long (samples: boost windows 0.1-0.3 s)


def _read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def _ints(text):
    return sorted(int(x) for x in text.split() if x.isdigit())


def at_least(freqs, hz):
    """The lowest step >= hz (the top step if none)."""
    above = [f for f in freqs if f >= hz]
    return above[0] if above else (freqs[-1] if freqs else 0)


class GpuFloor:
    def __init__(self, settings_dir, log=print, devfreq_glob=DEVFREQ_GLOB, runner=subprocess.run, sg=None, sleep=time.sleep):
        self.path = os.path.join(settings_dir, FILE)
        self.log = log
        self._glob = devfreq_glob
        self._run = runner
        self._sg = sg if sg is not None else (shutil.which("sg") or "")
        self._sleep = sleep
        self._last_err = None
        self.data = self._load()

    # -- storage: {"games": {appid: {"on": bool, "mhz": int}}, "applied": {"hz": int, "prev": int, "appid": str} | null}

    def _load(self):
        try:
            with open(self.path, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError):
            d = {}
        games = d.get("games") if isinstance(d.get("games"), dict) else {}
        games = {str(a): {"on": bool(g.get("on")), "mhz": int(g.get("mhz", VELA_GPU_MIN_MHZ))}
                 for a, g in games.items() if str(a).isdigit() and isinstance(g, dict)}
        applied = d.get("applied") if isinstance(d.get("applied"), dict) else None
        return {"games": games, "applied": applied}

    def _save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.data, f, indent=1, sort_keys=True)
        os.replace(tmp, self.path)

    # -- the GPU's devfreq node

    def gpu(self):
        nodes = sorted(glob.glob(self._glob))
        return nodes[0] if nodes else None

    def freqs(self, node=None):
        node = node or self.gpu()
        return _ints(_read(os.path.join(node, "available_frequencies"))) if node else []

    def writable(self, node=None):
        node = node or self.gpu()
        if not node:
            return False
        mf = os.path.join(node, "min_freq")
        return os.access(mf, os.W_OK) or bool(self._sg_group(mf))

    def _sg_group(self, path):
        """The file's group when it is group-writable and this user is listed in it (sg needs no password then), else ''."""
        if not self._sg or grp is None:
            return ""
        try:
            st = os.stat(path)
            if not st.st_mode & 0o020:
                return ""
            g = grp.getgrgid(st.st_gid)
            me = pwd.getpwuid(os.getuid())
            return g.gr_name if (me.pw_name in g.gr_mem or me.pw_gid == st.st_gid) else ""
        except (OSError, KeyError):
            return ""

    def _min_read(self, path, n=MIN_READS, span=READ_SPAN_S, stop_below=None):
        """Lowest of n reads (the boost QoS only ever raises what is read); stops early once a read is below stop_below."""
        low = None
        for i in range(max(1, n)):
            v = _read(path)
            if v.isdigit():
                low = int(v) if low is None else min(low, int(v))
            if stop_below is not None and low is not None and low < stop_below:
                break
            if i + 1 < n:
                self._sleep(span / max(1, n - 1))
        return low or 0

    # -- per game

    def known(self, appid):
        return str(appid) in self.data["games"]

    def enable(self, appid):
        """Vela turned on for the game: the floor follows (the game's own value kept, default VELA_GPU_MIN_MHZ)."""
        g = self.data["games"].setdefault(str(appid), {"on": True, "mhz": VELA_GPU_MIN_MHZ})
        g["on"] = True
        self._save()

    def disable(self, appid, running=()):
        """Vela turned off for the game: no floor for it any more (its value is kept for the next enable)."""
        g = self.data["games"].get(str(appid))
        if g and g["on"]:
            g["on"] = False
            self._save()
        self.reconcile(running)

    def set_mhz(self, appid, mhz, running=()):
        """The panel's slider: 0 = no floor for this game, else MHz (snapped to a GPU step when applied)."""
        mhz = max(0, int(mhz))
        g = self.data["games"].setdefault(str(appid), {"on": True, "mhz": VELA_GPU_MIN_MHZ})
        g["mhz"] = mhz
        self._save()
        self.reconcile(running)
        return self.status(appid)

    def want_hz(self, running):
        """The floor the running games ask for (the highest), 0 = none."""
        mhz = [g["mhz"] for a, g in self.data["games"].items() if a in running and g["on"] and g["mhz"] > 0]
        return max(mhz) * 1_000_000 if mhz else 0

    # -- apply / restore

    def reconcile(self, running):
        running = {str(a) for a in running}
        node = self.gpu()
        if not node:
            return
        mf = os.path.join(node, "min_freq")
        applied = self.data["applied"]
        want = self.want_hz(running)
        # boost-free floor estimate (one read can show the driver's boost); when floored, stop at the first low read
        cur = self._min_read(mf, stop_below=at_least(self.freqs(node), want) if want else None) if (want or applied) else 0
        gov = _read(os.path.join(node, "governor"))
        if want and gov != "userspace":
            freqs = self.freqs(node)
            top = _read(os.path.join(node, "max_freq"))
            hz = at_least(freqs, want)
            if top.isdigit():
                hz = min(hz, int(top))   # the profile's cap wins (battery saver)
            if cur < hz:
                if self._write(mf, hz):
                    prev = applied["prev"] if applied and applied.get("hz") == cur else cur
                    self.data["applied"] = {"hz": hz, "prev": prev}
                    self._save()
                    self.log(f"gpufloor: min_freq {cur} -> {hz}")
            return
        if applied:
            if cur == applied.get("hz") and applied.get("prev") is not None:
                if self._write(mf, int(applied["prev"])):
                    self.log(f"gpufloor: min_freq {cur} -> {applied['prev']} (restored)")
            self.data["applied"] = None
            self._save()

    def release(self):
        """Plugin unload: give the clock back."""
        self.reconcile(())

    def _write(self, path, hz):
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(f"{int(hz)}\n")
            return True
        except PermissionError as e:
            group = self._sg_group(path)
            if group:   # the plugin process has no supplementary groups (Decky); newgrp/sg gives it the file's group
                cmd = [self._sg, group, "-c", f"printf '%s\\n' {int(hz)} > {shlex.quote(path)}"]
                try:
                    r = self._run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=5)
                    if r.returncode == 0:
                        if self._last_err:
                            self.log(f"gpufloor: {path} written through sg {group}")
                        self._last_err = None
                        return True
                    err = f"sg {group}: exit {r.returncode} {(r.stderr or b'').decode('utf-8', 'replace').strip()[:200]}"
                except (OSError, subprocess.SubprocessError) as e2:
                    err = f"sg {group}: {e2}"
            else:
                err = f"{e} (no group-write for a group this user is in, or no sg)"
        except OSError as e:
            err = str(e)
        if err != self._last_err:   # once per distinct error (the 10 s loop logged the same EACCES 30 times in 3 min)
            self.log(f"gpufloor: cannot write {path}: {err}")
            self._last_err = err
        return False

    # -- the panel

    def status(self, appid, running=None):
        node = self.gpu()
        g = self.data["games"].get(str(appid)) or {"on": False, "mhz": VELA_GPU_MIN_MHZ}
        cur = _read(os.path.join(node, "min_freq")) if node else ""
        now = _read(os.path.join(node, "cur_freq")) if node else ""
        applied = self.data["applied"]
        return {
            "on": g["on"], "mhz": g["mhz"], "default_mhz": VELA_GPU_MIN_MHZ,
            "steps_mhz": [f // 1_000_000 for f in self.freqs(node)],
            "min_mhz": int(cur) // 1_000_000 if cur.isdigit() else None,
            "cur_mhz": int(now) // 1_000_000 if now.isdigit() else None,
            "applied": bool(applied) and cur.isdigit() and int(cur) == applied.get("hz"),
            "governor": _read(os.path.join(node, "governor")) if node else None,
            "writable": self.writable(node),
        }
