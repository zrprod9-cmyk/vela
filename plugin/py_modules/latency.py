"""Low latency, the part that is not OptiScaler's: Unreal Engine 4's r.OneFrameThreadLag=0 in the game's Engine.ini.

Measured in Deep Rock Galactic: "input -> photon" 156 -> 119 ms at the same FPS; with the
game's Reflex -> fakenvapi -> LatencyFleX (ini keys of our OptiScaler, sgsr3.py) 109 ms, with the Reflex frame-rate
limit just under the game's FPS 97 ms.

Where: <prefix>/drive_c/users/steamuser/AppData/Local/<Project>/Saved/Config/<WindowsNoEditor|Windows|WinGDK>/Engine.ini
(or <game>/<Project>/Saved/Config/… for games that keep Saved next to the exe), <Project> from <Project>-Win64-
Shipping.exe. The folder is made by the game's first run; until then the switch waits ("run the game once").

Undo: the file is copied to the plugin's settings folder first. Off -> the exact old bytes if the file is still what
we wrote; if the game changed it since, only our line (and the [SystemSettings] header if we added it) is removed.
UE4 rewrites its ini files when it exits, so while the game runs every change is done again after it has quit
(reconcile(), called on the panel's polls).
"""
import hashlib
import json
import os
import re
import shutil
import time

import gameutil
import sgsr3

SECTION = "SystemSettings"
KEY = "r.OneFrameThreadLag"
VALUE = "0"
CONFIG_DIRS = ("WindowsNoEditor", "Windows", "WinGDK")


def _md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def project_of(exe):
    m = re.match(r"(.+?)-(Win64|WinGDK)-Shipping\.exe$", exe or "", re.I)
    return m.group(1) if m else None


def find_engine_ini(home, appid, scan):
    """(path, exists) of the Engine.ini UE4 reads for this game, or (None, False) when the game never ran."""
    if scan.get("engine") != "Unreal":
        return None, False
    project = project_of(scan.get("exe"))
    if not project:
        return None, False
    roots = []
    pfx = gameutil.prefix_dir(home, appid)
    if pfx:
        roots.append(os.path.join(pfx, "drive_c", "users", "steamuser", "AppData", "Local", project, "Saved", "Config"))
    if scan.get("install_dir"):
        roots.append(os.path.join(scan["install_dir"], project, "Saved", "Config"))
    dirs = [os.path.join(r, d) for r in roots for d in CONFIG_DIRS if os.path.isdir(os.path.join(r, d))]
    for d in dirs:  # an existing Engine.ini first
        if os.path.isfile(os.path.join(d, "Engine.ini")):
            return os.path.join(d, "Engine.ini"), True
    for d in dirs:  # else the folder the game writes its other ini files into
        if any(f.lower().endswith(".ini") for f in os.listdir(d)):
            return os.path.join(d, "Engine.ini"), False
    return None, False


def has_line(text):
    return (sgsr3.ini_get(text, SECTION, KEY) or "").strip() == VALUE


def add_line(text):
    """(new text, whether the [SystemSettings] section was added)."""
    added = not re.search(rf"^\s*\[{SECTION}\]\s*$", text, re.M | re.I)
    if not text.strip():
        nl = "\r\n"
        return f"[{SECTION}]{nl}{KEY}={VALUE}{nl}", True
    return sgsr3.ini_set(text, SECTION, KEY, VALUE), added


def remove_line(text, drop_section):
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    out, cur = [], None
    rx = re.compile(rf"^\s*{re.escape(KEY)}\s*=", re.I)
    for line in lines:
        s = line.strip()
        if s.startswith("["):
            cur = s.strip("[]").lower()
        if cur == SECTION.lower() and rx.match(line):
            continue
        out.append(line)
    if drop_section:
        # our header with nothing left under it (blank lines only) goes too, with the blank line we put before it
        for i, line in enumerate(out):
            if line.strip().lower() == f"[{SECTION.lower()}]":
                j = i + 1
                while j < len(out) and not out[j].strip():
                    j += 1
                if j == len(out) or out[j].lstrip().startswith("["):
                    k = i
                    if k > 0 and not out[k - 1].strip():
                        k -= 1
                    del out[k:i + 1]
                break
    return nl.join(out)


class Latency:
    def __init__(self, home, settings_dir, log=print):
        self.home = home
        self.dir = os.path.join(settings_dir, "latency")
        self.log = log

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

    def _drop(self, appid):
        for p in (self._state_path(appid), os.path.join(self.dir, f"{appid}.Engine.ini")):
            try:
                os.remove(p)
            except OSError:
                pass

    def wanted(self, appid):
        st = self.read(appid)
        return {"on": bool(st and st.get("want")), "fps": (st or {}).get("fps", "off")}

    def status(self, appid, scan, running=False):
        st = self.read(appid) or {}
        path = st.get("path")
        if not path:
            path, _ = find_engine_ini(self.home, appid, scan)
        applied = False
        if path and os.path.isfile(path):
            try:
                applied = has_line(open(path, encoding="utf-8", errors="replace").read())
            except OSError:
                pass
        unreal = scan.get("engine") == "Unreal"
        return {"on": bool(st.get("want")), "fps": st.get("fps", "off"), "unreal": unreal,
                "engine_ini": path, "applied": applied, "needs_run": unreal and not path,
                "pending": bool(st.get("pending")) and running}

    # -- switch

    def set(self, appid, on, fps, scan, running=False):
        """Remember the wish and apply the Engine.ini part now (and again after the game quits, if it runs)."""
        if fps not in sgsr3.FPS_LIMITS:
            raise ValueError(f"unknown frame-rate limit {fps}")
        st = self.read(appid) or {}
        st.update({"want": bool(on), "fps": fps, "updated_at": time.time()})
        self._write(appid, st)
        recheck = None
        if on:
            self._apply(appid, scan, st)
        else:
            if st.get("path") and not st.get("had_line"):
                recheck = {"path": st["path"], "added_section": st.get("added_section", False)}
            self._restore(appid, st)
        st = self.read(appid)
        if st is not None:
            st["pending"] = bool(running)
            if running and recheck:
                st["recheck"] = recheck  # UE4 may write our line back when it quits: remove it again then
            if not st.get("want") and not st.get("path") and not st.get("recheck"):
                self._drop(appid)
            else:
                self._write(appid, st)
        return self.status(appid, scan, running)

    def _apply(self, appid, scan, st):
        path = st.get("path")
        if not path:
            path, _exists = find_engine_ini(self.home, appid, scan)
            if not path:
                return False  # not UE4, or the game never ran: nothing to write yet
        try:
            data = open(path, "rb").read()
            existed = True
        except OSError:
            data, existed = b"", False
        text = data.decode("utf-8", errors="surrogateescape")
        if has_line(text):
            if not st.get("path"):
                st.update({"path": path, "existed": existed, "written_md5": _md5_bytes(data), "added_section": False,
                           "had_line": True})
                self._write(appid, st)
            return True
        if not st.get("path"):
            # first time: keep the original next to the plugin's settings
            os.makedirs(self.dir, exist_ok=True)
            if existed:
                with open(os.path.join(self.dir, f"{appid}.Engine.ini"), "wb") as f:
                    f.write(data)
            st.update({"path": path, "existed": existed, "orig_md5": _md5_bytes(data) if existed else None})
        new, added = add_line(text)
        out = new.encode("utf-8", errors="surrogateescape")
        tmp = path + ".upscale-tmp"
        with open(tmp, "wb") as f:
            f.write(out)
        os.replace(tmp, path)
        st.update({"written_md5": _md5_bytes(out), "added_section": bool(st.get("added_section") or added)})
        self._write(appid, st)
        self.log(f"latency: {appid}: {KEY}={VALUE} in {path}")
        return True

    def _restore(self, appid, st):
        path = st.get("path")
        if not path:
            return True
        if st.get("had_line"):
            self._forget(appid, st)  # the line was the game's (or the user's) own: not ours to remove
            return True
        try:
            data = open(path, "rb").read()
        except OSError:
            data = None
        orig = os.path.join(self.dir, f"{appid}.Engine.ini")
        if data is not None and _md5_bytes(data) == st.get("written_md5"):
            if st.get("existed"):
                shutil.copyfile(orig, path + ".upscale-tmp")
                os.replace(path + ".upscale-tmp", path)
            else:
                os.remove(path)
        elif data is not None:
            text = data.decode("utf-8", errors="surrogateescape")
            if has_line(text):
                new = remove_line(text, st.get("added_section", False)).encode("utf-8", errors="surrogateescape")
                with open(path + ".upscale-tmp", "wb") as f:
                    f.write(new)
                os.replace(path + ".upscale-tmp", path)
        self.log(f"latency: {appid}: {KEY} removed from {path}")
        self._forget(appid, st)
        return True

    def _forget(self, appid, st):
        for k in ("path", "existed", "orig_md5", "written_md5", "added_section", "had_line"):
            st.pop(k, None)
        try:
            os.remove(os.path.join(self.dir, f"{appid}.Engine.ini"))
        except OSError:
            pass
        self._write(appid, st)

    def has_pending(self):
        try:
            names = [f[:-5] for f in os.listdir(self.dir) if f.endswith(".json")]
        except OSError:
            return False
        for appid in names:
            st = self.read(appid)
            if st and st.get("pending"):
                return True
        return False

    def reconcile(self, running):
        """After a game quits: UE4 may have rewritten Engine.ini on exit. Put our wish back in place."""
        try:
            names = [f[:-5] for f in os.listdir(self.dir) if f.endswith(".json")]
        except OSError:
            return
        for appid in names:
            st = self.read(appid)
            if not st or not st.get("pending") or appid in running:
                continue
            st["pending"] = False
            self._write(appid, st)
            path = st.get("path")
            try:
                if st.get("want") and path:
                    text = open(path, encoding="utf-8", errors="surrogateescape").read() if os.path.isfile(path) else ""
                    if not has_line(text):
                        st2 = dict(st)
                        st2["written_md5"] = None
                        self._apply_again(appid, st2)
                elif not st.get("want") and path:
                    self._restore(appid, st)
                elif not st.get("want") and st.get("recheck"):
                    rc = st.pop("recheck")
                    p = rc["path"]
                    if os.path.isfile(p):
                        text = open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
                        if has_line(text):
                            new = remove_line(text, rc.get("added_section", False))
                            with open(p + ".upscale-tmp", "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                                f.write(new)
                            os.replace(p + ".upscale-tmp", p)
                            self.log(f"latency: {appid}: {KEY} the game wrote back on exit removed again")
                    self._write(appid, st)
                if not st.get("want"):
                    st = self.read(appid)
                    if st and not st.get("path") and not st.get("recheck"):
                        self._drop(appid)
            except OSError as e:
                self.log(f"latency: {appid}: reconcile failed: {e}")

    def _apply_again(self, appid, st):
        path = st["path"]
        data = open(path, "rb").read() if os.path.isfile(path) else b""
        new, added = add_line(data.decode("utf-8", errors="surrogateescape"))
        out = new.encode("utf-8", errors="surrogateescape")
        with open(path + ".upscale-tmp", "wb") as f:
            f.write(out)
        os.replace(path + ".upscale-tmp", path)
        st.update({"written_md5": _md5_bytes(out), "added_section": bool(st.get("added_section") or added)})
        self._write(appid, st)
        self.log(f"latency: {appid}: {KEY}={VALUE} written again after the game quit")
