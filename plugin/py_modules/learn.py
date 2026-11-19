"""Where the game of a non-Steam shortcut is, learned from a launch that is running.

A shortcut made by a launcher (Heroic, Xodus Nexus, Ubisoft Connect in a Proton prefix, Lutris ...) names the launcher,
not the game: shortcuts.vdf has no game folder for it, and every launcher keeps that in a private format of its own.
What they all share is how the game runs: a Wine process whose working folder is the game's install folder. This module
reads that from /proc while the game runs, and keeps it per shortcut in <settings>/learned.json.

Whose game it is (seen on the Armada console, 2026-11-16):
  certain    the game's own environment names the shortcut (SteamAppId=<appid>): Xodus Nexus, Steam's own Proton. This
             holds also when a launcher that was already open started the game (the game then sits under the LAUNCHER's
             Steam launch, with its own shortcut's id).
  candidate  the id is missing or 0 (Heroic's runtime sets SteamAppId=0) and the game sits under this shortcut's Steam
             launch. It may be the shortcut's game, or a game started from inside a launcher's window: the panel shows
             the folder and the player's choice of a model confirms it (Store.confirm).
  other      the game under this shortcut's launch carries ANOTHER shortcut's id: this shortcut is a launcher.
A shortcut that starts a second, different game folder while the first still exists is a launcher (Store.record).

Nothing but folders, the exe's name and the Wine prefix is kept: command lines and environments of launcher games hold
sign-in tokens and are never stored, logged or reported.

Pure standard library; every read takes the /proc root as an argument (tests build one in a temp folder)."""
import json
import os
import re
import time

RX_WIN_EXE = re.compile(r"^(?:\\\\\?\\)?[A-Za-z]:[\\/].*\.exe$", re.I)
RX_WINDOWS_DIR = re.compile(r"[\\/]windows[\\/]", re.I)
# programs that come with a game or a launcher and are never the game
RX_HELPER = re.compile(r"(crash|handler|report|webcore|webhelper|helper|launcher|^upc|xalia|^umu|setup|redist|unins|overlay|"
                       r"service|updater|bootstrap|easyanticheat|battleye|cefprocess|subprocess)[^\\/]*\.exe$", re.I)
ENV_KEYS = (b"SteamAppId", b"SteamGameId", b"STEAM_COMPAT_APP_ID", b"WINEPREFIX", b"STEAM_COMPAT_DATA_PATH")
MAX_DEPTH = 6
MAX_ENTRIES = 40000
PE_MACHINES = {0x8664: "x64", 0x014C: "x86", 0xAA64: "arm64"}


def base_name(win_path):
    return re.split(r"[\\/]", win_path)[-1]


def snapshot(proc="/proc", uid=None):
    """{pid: {"argv0", "ppid", "start", "rss"}} of this user's processes. Only the first word of each command line is
    kept (a launcher game's arguments hold sign-in codes)."""
    uid = os.getuid() if uid is None else uid
    out = {}
    try:
        pids = [p for p in os.listdir(proc) if p.isdigit()]
    except OSError:
        return out
    for pid in pids:
        d = os.path.join(proc, pid)
        try:
            if os.stat(d).st_uid != uid:
                continue
            with open(os.path.join(d, "cmdline"), "rb") as f:
                argv = f.read(8192).split(b"\0")
            with open(os.path.join(d, "stat"), "rb") as f:
                st = f.read()
            fields = st[st.rindex(b")") + 2:].split()
            out[int(pid)] = {"argv0": argv[0].decode("utf-8", "replace") if argv else "",
                             "launch": _launch_appid(argv), "ppid": int(fields[1]), "start": int(fields[19]),
                             "rss": int(fields[21])}
        except (OSError, ValueError, IndexError):
            continue
    return out


def _launch_appid(argv):
    """The appid of Steam's reaper ("reaper SteamLaunch AppId=<id> -- ..."), else None."""
    if len(argv) > 2 and os.path.basename(argv[0]) == b"reaper" and argv[1] == b"SteamLaunch":
        m = re.match(rb"AppId=(\d+)$", argv[2])
        return m.group(1).decode() if m else None
    return None


def launch_of(procs, pid):
    """(reaper pid, appid) of the Steam launch this process sits under, or (None, None)."""
    seen = set()
    while pid in procs and pid not in seen:
        seen.add(pid)
        if procs[pid]["launch"]:
            return pid, procs[pid]["launch"]
        pid = procs[pid]["ppid"]
    return None, None


def read_env(pid, proc="/proc"):
    """The few variables this module needs (ENV_KEYS), nothing else of the environment leaves this function."""
    out = {}
    try:
        with open(os.path.join(proc, str(pid), "environ"), "rb") as f:
            data = f.read()
    except OSError:
        return out
    for item in data.split(b"\0"):
        name, sep, value = item.partition(b"=")
        if sep and name in ENV_KEYS:
            out[name.decode()] = value.decode("utf-8", "replace")
    return out


def mapped_exe(pid, name, proc="/proc"):
    """The host path of the process's own exe from its memory map (Wine maps the PE file by its real path), or None."""
    want = name.lower()
    try:
        with open(os.path.join(proc, str(pid), "maps"), encoding="utf-8", errors="replace") as f:
            for line in f:
                i = line.find("/")
                if i < 0:
                    continue
                path = line[i:].rstrip("\n")
                if path.endswith(" (deleted)"):
                    path = path[:-10]
                if os.path.basename(path).lower() == want:
                    return path
    except OSError:
        pass
    return None


def candidates(procs, proc="/proc"):
    """The Windows programs that run now, outside Wine's own windows folder."""
    out = []
    for pid, p in procs.items():
        a0 = p["argv0"]
        if not RX_WIN_EXE.match(a0) or RX_WINDOWS_DIR.search(a0):
            continue
        try:
            cwd = os.readlink(os.path.join(proc, str(pid), "cwd"))
        except OSError:
            continue
        name = base_name(a0)
        env = read_env(pid, proc)
        rp, launch = launch_of(procs, pid)
        out.append({"pid": pid, "exe": name, "cwd": cwd, "rss": p["rss"], "start": p["start"],
                    "helper": bool(RX_HELPER.search(name)), "launch": launch, "launch_pid": rp,
                    "id": env.get("SteamAppId") or None, "env": env})
    return out


def _own_id(c):
    return c["id"] if c["id"] and c["id"] != "0" else None


def find_exe(root, name, hint=None):
    """The exe's path relative to the install folder (bounded walk, case-insensitive). With several of that name, the
    one whose path ends like hint (the running exe's host path) wins."""
    want, found = name.lower(), []
    budget = MAX_ENTRIES
    for cur, dirs, files in os.walk(root):
        rel = os.path.relpath(cur, root)
        if (0 if rel == "." else rel.count(os.sep) + 1) >= MAX_DEPTH:
            dirs[:] = []
        budget -= len(dirs) + len(files)
        if budget < 0:
            break
        for f in files:
            if f.lower() == want:
                found.append(os.path.normpath(os.path.join(rel, f)))
    if not found:
        return None
    if hint and len(found) > 1:
        tail = hint.replace("\\", "/").lower()
        found.sort(key=lambda r: (not tail.endswith("/" + r.replace(os.sep, "/").lower()), len(r)))
    else:
        found.sort(key=len)
    return found[0]


def pe_machine(path):
    """"x64", "x86" or "arm64" for a Windows program, None for anything else (an encrypted or missing file)."""
    try:
        with open(path, "rb") as f:
            head = f.read(64)
            if head[:2] != b"MZ" or len(head) < 64:
                return None
            f.seek(int.from_bytes(head[60:64], "little"))
            sig = f.read(6)
    except OSError:
        return None
    if sig[:4] != b"PE\0\0":
        return None
    return PE_MACHINES.get(int.from_bytes(sig[4:6], "little"))


def prefix_of(env):
    """The Wine prefix (the folder with drive_c) from the game's environment, or None."""
    for path in (env.get("WINEPREFIX"), os.path.join(env.get("STEAM_COMPAT_DATA_PATH") or "", "pfx")):
        if path and os.path.isabs(path) and os.path.isdir(os.path.join(path, "drive_c")):
            return os.path.realpath(path)
    return None


def run_id(env):
    """The id Vela's library inside the game reads its live settings by (fork patch 0010, VelaLive.h Path): the first of
    these variables that starts with a digit. A launcher's runtime may set it to something else than the shortcut's appid
    (Heroic: SteamAppId=0)."""
    for key in ("SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID"):
        value = env.get(key) or ""
        if value[:1].isdigit():
            return value
    return None


def observe(appid, proc="/proc", own_dirs=(), uid=None):
    """What runs for this shortcut now.

    {"status": "found", "certain": bool, "handoff": bool, "install_dir", "exe" (relative), "exe_name", "running_exe"
     (host path of the exe that runs; a launcher's temporary copy may differ from install_dir/exe), "machine", "prefix"}
    {"status": "other", "appid": <whose game runs under this shortcut>}   this shortcut is a launcher
    {"status": "none"}                                                       nothing to learn from now
    own_dirs: folders of the shortcut's own program (a launcher's helpers run from there and are never the game)."""
    appid = str(appid)
    procs = snapshot(proc, uid)
    cands = candidates(procs, proc)
    own = {os.path.realpath(d) for d in own_dirs if d}
    games = [c for c in cands if not c["helper"] and os.path.realpath(c["cwd"]) not in own]
    mine = [c for c in games if _own_id(c) == appid]
    certain = bool(mine)
    if not mine:
        under = [c for c in games if c["launch"] == appid]
        foreign = sorted({_own_id(c) for c in under if _own_id(c)})
        if foreign:
            return {"status": "other", "appid": foreign[0]}
        mine = under
    if not mine:
        return {"status": "none"}
    g = max(mine, key=lambda c: (c["rss"], -c["start"]))
    running = mapped_exe(g["pid"], g["exe"], proc)
    install = os.path.realpath(g["cwd"])
    rel = find_exe(install, g["exe"], running) if os.path.isdir(install) else None
    if rel is None and running and os.path.isfile(running):
        # the game does not run from inside its working folder: the exe's own folder, when that is a lasting one
        install, rel = os.path.dirname(os.path.realpath(running)), os.path.basename(running)
    if rel is None:
        return {"status": "none"}
    return {"status": "found", "certain": certain, "handoff": g["launch"] != appid, "install_dir": install, "exe": rel,
            "exe_name": g["exe"], "running_exe": running, "run_id": run_id(g["env"]),
            "machine": pe_machine(running) if running else pe_machine(os.path.join(install, rel)),
            "prefix": prefix_of(g["env"])}


class Store:
    """<settings>/learned.json: {appid: {"install_dir", "exe", "prefix", "machine", "confirmed", "launcher", "sig",
    "markers", "at"}}. sig: what the shortcut pointed at when its game was learned (a changed shortcut learns again)."""

    def __init__(self, settings_dir, log=print):
        self.path = os.path.join(settings_dir, "learned.json")
        self.log = log
        self.data = None

    def _load(self):
        if self.data is None:
            try:
                with open(self.path, encoding="utf-8") as f:
                    self.data = json.load(f)
                if not isinstance(self.data, dict):
                    self.data = {}
            except (OSError, ValueError):
                self.data = {}
        return self.data

    def _save(self):
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.data, f, indent=1)
            os.replace(tmp, self.path)
        except OSError as e:
            self.log(f"learn: {e}")

    def entry(self, appid):
        e = self._load().get(str(appid))
        return e if isinstance(e, dict) else None

    def get(self, appid, sig=None):
        """The learned game of a shortcut that can be used now: its folder and exe are there, the shortcut still points
        at what it did, it is not a launcher. None otherwise."""
        e = self.entry(appid)
        if not e or e.get("launcher") or not e.get("install_dir"):
            return None
        if sig is not None and e.get("sig") not in (None, sig):
            return None
        if not os.path.isfile(os.path.join(e["install_dir"], e.get("exe") or "")):
            return None
        return e

    def is_launcher(self, appid):
        return bool((self.entry(appid) or {}).get("launcher"))

    def record(self, appid, obs, sig=None, markers=None):
        """Keeps what observe() found. Returns the entry, or None when nothing is kept."""
        appid = str(appid)
        data = self._load()
        old = data.get(appid) if isinstance(data.get(appid), dict) else None
        if obs.get("status") == "other":
            if not old or not old.get("launcher"):
                data[appid] = {"launcher": True, "sig": sig, "at": int(time.time())}
                self._save()
                self.log(f"learn: {appid} starts another shortcut's game: a launcher")
            return None
        if obs.get("status") != "found":
            return None
        if old and old.get("launcher") and old.get("sig") in (None, sig):
            return None
        new = {"install_dir": obs["install_dir"], "exe": obs["exe"], "prefix": obs.get("prefix"),
               "machine": obs.get("machine"), "run_id": obs.get("run_id"), "confirmed": bool(obs.get("certain")), "launcher": False, "sig": sig,
               "markers": sorted(markers) if markers is not None else None, "at": int(time.time())}
        if old and old.get("install_dir") and old.get("sig") in (None, sig):
            same = os.path.realpath(old["install_dir"]) == os.path.realpath(new["install_dir"])
            if same:
                new["confirmed"] = new["confirmed"] or bool(old.get("confirmed"))
                if new["markers"] is None:
                    new["markers"] = old.get("markers")
                if all(old.get(k) == new[k] for k in ("exe", "prefix", "machine", "confirmed", "markers", "run_id")):
                    return old
            elif not obs.get("certain") and os.path.isdir(old["install_dir"]):
                # no id of its own, and now a second game folder while the first is still there: a launcher's window
                data[appid] = {"launcher": True, "sig": sig, "at": int(time.time())}
                self._save()
                self.log(f"learn: {appid} started two different games: a launcher")
                return None
        data[appid] = new
        self._save()
        self.log(f"learn: {appid}: {new['install_dir']} ({new['exe']}, "
                 f"{'certain' if obs.get('certain') else 'candidate'})")
        return new

    def note_native(self, appid, sig=None):
        """The shortcut ran for a while and no Windows program came up under it: a Linux program (an emulator)."""
        appid = str(appid)
        old = self.entry(appid)
        if old and old.get("sig") in (None, sig) and (old.get("native") or old.get("launcher") or old.get("install_dir")):
            return
        self._load()[appid] = {"native": True, "sig": sig, "at": int(time.time())}
        self._save()

    def confirm(self, appid):
        """The player set Vela up for the game the panel showed: a candidate is the shortcut's game from now on."""
        e = self.entry(appid)
        if e and not e.get("launcher") and not e.get("confirmed"):
            e["confirmed"] = True
            self._save()

    def forget(self, appid):
        if self._load().pop(str(appid), None) is not None:
            self._save()
