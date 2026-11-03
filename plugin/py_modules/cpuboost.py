"""CPU for the game (0.8.3): a higher cap on the prime core (cpu7, Cortex-X3) while a chosen game runs, inside the
Balanced profile. The Performance profile is avoided on purpose ("heats a lot without a big gain").

Why (console probes): Balanced caps policy7 at 2092.8 MHz
(cpu_underclock=medium; hw max 2956.8) and MangoHud shows the CPU sitting AT that cap in Into the Dead: the cap, not
the governor, limits CPU-bound frames. Levers kept here (no root):
- mild = policy7 scaling_max_freq 2476.8 MHz (the "small" underclock cap), max = 2956.8 MHz (hw max). Only policy7's
  scaling_max_freq changes; governor, policy0/3, GPU, fan, IRQs are never touched; powerd's write methods are never
  called. scaling_max_freq is root:wheel 0664 (udev 60-armada-perf-acls.rules); Decky runs the plugin as uid 1000
  without supplementary groups, so a refused write goes through `sg wheel` (the same helper as gpufloor).
- thread placement: Xwayland + gamescope-wl to cpu3-6 and mangoapp to cpu0-2 (sched_setaffinity on every thread),
  processes whose real/effective uid is 1000 only, so the game's main thread keeps cpu7 for itself. Undone on exit.

Safety:
- the original cap is saved in <settings>/cpuboost.json BEFORE the first write; restore writes it back only while the
  file still holds Vela's value (powerd applied a profile / resumed meanwhile: its value stays). The same file restores
  after a crash at the next start (the reconcile loop's first pass: no boosted game runs -> restore).
- guard every 2 s on max(cpu7-top/middle/bottom, cpuss0..3) by zone type (zone numbers are not fixed):
  >= 85 C one frequency step down, >= 88 C the original cap; one step up after 20 s below 78 C (CPU passive trips are
  90/95 C, so Vela backs off before the kernel throttles).
- battery < 20 % and discharging -> original cap until charging or >= 20 %.
- powerd re-writes the cap on a profile change, Reload or resume: seen as "the file no longer holds Vela's value";
  Vela applies again only if the game still runs and /var/lib/armada/powerd.state says profile=balanced.
- scaling_max_freq reads back the policy's effective max (the kernel's thermal cooling device cpufreq-cpu7 lowers it at
  the 90 C trip): while that device is active a lower read is the kernel, not powerd, and Vela's value still counts.
"""
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

POLICY = "/sys/devices/system/cpu/cpufreq/policy7"
THERMAL = "/sys/class/thermal"
BATTERY = "/sys/class/power_supply/battery"
POWERD_STATE = "/var/lib/armada/powerd.state"
FILE = "cpuboost.json"
MODES = {"off": 0, "mild": 2476800, "max": 2956800}   # kHz (scaling_available_frequencies of policy7)
ZONE_PREFIXES = ("cpu7-", "cpuss")
HOT_C, CRIT_C, COOL_C, COOL_S = 85.0, 88.0, 78.0, 20.0
BATT_LOW = 20
OWNER_UID = 1000
HELPERS = {"Xwayland": {3, 4, 5, 6}, "gamescope-wl": {3, 4, 5, 6}, "mangoapp": {0, 1, 2}}


def _read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def _int(text):
    return int(text) if text.isdigit() else 0


class CpuBoost:
    def __init__(self, settings_dir, log=print, policy=POLICY, thermal=THERMAL, battery=BATTERY,
                 powerd_state=POWERD_STATE, proc="/proc", runner=subprocess.run, sg=None, sg_group=None,
                 clock=time.monotonic, getaff=None, setaff=None):
        self.path = os.path.join(settings_dir, FILE)
        self.log = log
        self.policy, self.thermal, self.battery, self.powerd_state, self.proc = policy, thermal, battery, powerd_state, proc
        self._run = runner
        self._sg = sg if sg is not None else (shutil.which("sg") or "")
        self._sg_group = sg_group or self._file_group
        self._clock = clock
        self._getaff = getaff or getattr(os, "sched_getaffinity", None)
        self._setaff = setaff or getattr(os, "sched_setaffinity", None)
        self._last_err = None
        self._zones = None
        self._cdev = None
        self._cool_since = None
        self.data = self._load()

    # -- storage: {"games": {appid: mode}, "applied": {"appid", "orig", "hz", "target"} | null,
    #              "threads": {pid: {"comm", "orig": [cpus], "set": [cpus]}}}

    def _load(self):
        try:
            with open(self.path, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError):
            d = {}
        games = d.get("games") if isinstance(d.get("games"), dict) else {}
        games = {str(a): m for a, m in games.items() if str(a).isdigit() and m in MODES and m != "off"}
        applied = d.get("applied") if isinstance(d.get("applied"), dict) else None
        threads = d.get("threads") if isinstance(d.get("threads"), dict) else {}
        return {"games": games, "applied": applied, "threads": threads}

    def _save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.data, f, indent=1)
        os.replace(tmp, self.path)

    # -- sysfs

    def freqs(self):
        return sorted(int(x) for x in _read(os.path.join(self.policy, "scaling_available_frequencies")).split()
                      if x.isdigit())

    def cap(self):
        return _int(_read(os.path.join(self.policy, "scaling_max_freq")))

    def temp_c(self):
        """max of the cpu7-* and cpuss* zones (by type), in C; None when none is readable."""
        if self._zones is None:
            self._zones = []
            try:
                for z in sorted(os.listdir(self.thermal)):
                    if z.startswith("thermal_zone") and _read(os.path.join(self.thermal, z, "type")).startswith(ZONE_PREFIXES):
                        self._zones.append(os.path.join(self.thermal, z, "temp"))
            except OSError:
                pass
        ts = [int(v) for v in (_read(p) for p in self._zones) if v.lstrip("-").isdigit()]
        return max(ts) / 1000.0 if ts else None

    def throttled(self):
        """The kernel's cpufreq-cpu7 cooling device is active (cur_state > 0)."""
        if self._cdev is None:
            self._cdev = ""
            try:
                for c in sorted(os.listdir(self.thermal)):
                    if c.startswith("cooling_device") and _read(os.path.join(self.thermal, c, "type")) == "cpufreq-cpu7":
                        self._cdev = os.path.join(self.thermal, c, "cur_state")
                        break
            except OSError:
                pass
        return bool(self._cdev) and _int(_read(self._cdev)) > 0

    def profile(self):
        for line in _read(self.powerd_state).splitlines():
            k, _, v = line.partition("=")
            if k.strip() == "profile":
                return v.strip().lower()
        return ""

    def battery_low(self):
        cap = _read(os.path.join(self.battery, "capacity"))
        return cap.isdigit() and int(cap) < BATT_LOW and _read(os.path.join(self.battery, "status")) == "Discharging"

    def _file_group(self, path):
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

    def writable(self):
        p = os.path.join(self.policy, "scaling_max_freq")
        return os.path.exists(p) and (os.access(p, os.W_OK) or bool(self._sg and self._sg_group(p)))

    def _write(self, khz):
        """policy7 scaling_max_freq <- khz; through `sg <group>` when the plain write is refused (the gpufloor helper)."""
        path = os.path.join(self.policy, "scaling_max_freq")
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(f"{int(khz)}\n")
            self._last_err = None
            return True
        except PermissionError as e:
            group = self._sg_group(path) if self._sg else ""
            if group:
                cmd = [self._sg, group, "-c", f"printf '%s\\n' {int(khz)} > {shlex.quote(path)}"]
                try:
                    r = self._run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=5)
                    if r.returncode == 0:
                        self._last_err = None
                        return True
                    err = f"sg {group}: exit {r.returncode} {(r.stderr or b'').decode('utf-8', 'replace').strip()[:200]}"
                except (OSError, subprocess.SubprocessError) as e2:
                    err = f"sg {group}: {e2}"
            else:
                err = f"{e} (no group-write for a group this user is in, or no sg)"
        except OSError as e:
            err = str(e)
        if err != self._last_err:
            self.log(f"cpuboost: cannot write {path}: {err}")
            self._last_err = err
        return False

    def _step(self, khz, up):
        fs = self.freqs()
        if up:
            above = [f for f in fs if f > khz]
            return above[0] if above else khz
        below = [f for f in fs if f < khz]
        return below[-1] if below else khz

    def _at_most(self, khz):
        fs = [f for f in self.freqs() if f <= khz]
        return fs[-1] if fs else 0

    # -- per game

    def mode(self, appid):
        return self.data["games"].get(str(appid), "off")

    def set_mode(self, appid, mode, running):
        if mode not in MODES:
            raise ValueError(f"cpu boost mode {mode!r}")
        if mode == "off":
            self.data["games"].pop(str(appid), None)
        else:
            self.data["games"][str(appid)] = mode
        self._save()
        self.reconcile(running)
        return self.status(appid)

    def _want(self, running):
        """(appid, target kHz) of the running game with the highest mode, or (None, 0)."""
        best = (None, 0)
        for a in running or ():
            hz = MODES.get(self.mode(a), 0)
            if hz > best[1]:
                best = (str(a), hz)
        return best

    # -- the 10 s loop: apply / restore, helpers' placement

    def reconcile(self, running):
        appid, target = self._want(running)
        if not appid:
            self.release()
            return
        self._place_helpers()
        a = self.data["applied"]
        if a and a.get("appid") == appid and a.get("target") == target:
            return
        if a:
            self._restore_cap()
        self._apply(appid, target)

    def _apply(self, appid, target):
        if self.profile() != "balanced" or self.battery_low():
            return
        orig = self.cap()
        hz = self._at_most(target)
        if not orig or hz <= orig:
            return   # the cap is already as high (another profile or underclock level): nothing to raise
        self.data["applied"] = {"appid": appid, "orig": orig, "hz": orig, "target": hz}
        self._save()   # original value on disk before the first write
        self._cool_since = None
        t = self.temp_c()
        start = hz if t is None or t < HOT_C else orig
        if start > orig and self._write(start):
            self.data["applied"]["hz"] = start
            self._save()
            self.log(f"cpuboost: {appid} policy7 cap {orig // 1000} -> {start // 1000} MHz")

    def _restore_cap(self):
        a = self.data["applied"]
        if not a:
            return
        cur = self.cap()
        # only Vela's own value, only inside Balanced (Performance writes hw max = what "max" wrote: that one stays)
        if a.get("hz") != a.get("orig") and (cur == a.get("hz") or self.throttled()) and self.profile() == "balanced":
            if self._write(a["orig"]):
                self.log(f"cpuboost: policy7 cap back to {a['orig'] // 1000} MHz")
            else:
                return   # keep the record: the next pass tries again
        self.data["applied"] = None
        self._save()

    def release(self):
        """Game exit, Vela off, plugin unload, the first pass after a crash: original cap and thread placement back."""
        self._restore_cap()
        self._unplace_helpers()

    # -- the 2 s guard (only while a cap is applied; no /proc scan unless powerd re-wrote the cap)

    def guard(self, running_fn):
        a = self.data["applied"]
        if not a:
            return
        cur = self.cap()
        if self.profile() != "balanced":
            self.data["applied"] = None   # another profile now owns the cap (its value may equal ours): never write back
            self._save()
            self.log("cpuboost: power profile is no longer Balanced, cap left to powerd")
            return
        if cur != a["hz"] and self.throttled():
            return   # the kernel's thermal limit shows through the read; Vela's request is unchanged
        if cur != a["hz"]:
            # powerd applied a profile / reloaded / resumed: its value is the new original
            self.data["applied"] = None
            self._save()
            if cur:
                self.log(f"cpuboost: policy7 cap changed by someone else ({a['hz'] // 1000} -> {cur // 1000} MHz)")
            running = running_fn()
            appid, target = self._want(running)
            if appid == a["appid"]:
                self._apply(appid, target)
            return
        if self.battery_low():
            if a["hz"] != a["orig"] and self._write(a["orig"]):
                a["hz"] = a["orig"]
                self._save()
                self.log("cpuboost: battery < 20 %, cap back to the original")
            self._cool_since = None
            return
        t = self.temp_c()
        if t is None:
            return
        new = a["hz"]
        if t >= CRIT_C:
            new = a["orig"]
            self._cool_since = None
        elif t >= HOT_C:
            new = max(a["orig"], self._step(a["hz"], up=False))
            self._cool_since = None
        elif t < COOL_C:
            now = self._clock()
            if a["hz"] >= a["target"]:
                self._cool_since = None
            elif self._cool_since is None:
                self._cool_since = now
            elif now - self._cool_since >= COOL_S:
                new = min(a["target"], self._step(a["hz"], up=True))
                self._cool_since = now
        else:
            self._cool_since = None
        if new != a["hz"] and self._write(new):
            self.log(f"cpuboost: {t:.1f} C, policy7 cap {a['hz'] // 1000} -> {new // 1000} MHz")
            a["hz"] = new
            self._save()

    # -- helpers' placement (own uid 1000 only, no root)

    def _helper_pids(self):
        out = []
        try:
            names = os.listdir(self.proc)
        except OSError:
            return out
        for p in names:
            if not p.isdigit():
                continue
            comm = _read(os.path.join(self.proc, p, "comm"))
            if comm in HELPERS and self._owned(p):
                out.append((p, comm))
        return out

    def _owned(self, pid):
        for line in _read(os.path.join(self.proc, str(pid), "status")).splitlines():
            if line.startswith("Uid:"):
                ids = line.split()[1:]
                return bool(ids) and all(i == str(OWNER_UID) for i in ids)
        return False

    def _tids(self, pid):
        try:
            return [int(t) for t in os.listdir(os.path.join(self.proc, str(pid), "task")) if t.isdigit()]
        except OSError:
            return [int(pid)]

    def _place_helpers(self):
        if not self._setaff or not self._getaff:
            return
        threads, changed = self.data["threads"], False
        for pid, comm in self._helper_pids():
            want = HELPERS[comm]
            rec = threads.get(pid)
            if rec and rec.get("comm") != comm:
                rec = None
            for tid in self._tids(pid):
                try:
                    cur = set(self._getaff(tid))
                    if cur == want:
                        continue
                    if rec is None:
                        rec = {"comm": comm, "orig": sorted(cur), "set": sorted(want)}
                        threads[pid] = rec
                        self._save()   # original before the first change
                    self._setaff(tid, want)
                    changed = True
                except OSError:
                    continue   # thread gone, or not ours
        if changed:
            self.log("cpuboost: Xwayland / gamescope-wl -> cpu3-6, mangoapp -> cpu0-2")

    def _unplace_helpers(self):
        threads = self.data["threads"]
        if not threads:
            return
        if self._setaff and self._getaff:
            for pid, rec in list(threads.items()):
                if _read(os.path.join(self.proc, pid, "comm")) != rec.get("comm") or not self._owned(pid):
                    continue   # gone (or the pid now belongs to something else): nothing to undo
                for tid in self._tids(pid):
                    try:
                        if set(self._getaff(tid)) == set(rec["set"]):
                            self._setaff(tid, set(rec["orig"]))
                    except OSError:
                        continue
        self.data["threads"] = {}
        self._save()

    # -- the panel

    def status(self, appid):
        a = self.data["applied"]
        cap = self.cap()
        t = self.temp_c()
        return {
            "mode": self.mode(appid),
            "cur_mhz": _int(_read(os.path.join(self.policy, "scaling_cur_freq"))) // 1000 or None,
            "cap_mhz": cap // 1000 or None,
            "temp_c": round(t) if t is not None else None,
            "profile": self.profile() or None,
            "applied": bool(a) and a.get("appid") == str(appid) and a.get("hz") != a.get("orig") and cap == a.get("hz"),
            "battery_low": self.battery_low(),
            "writable": self.writable(),
        }
