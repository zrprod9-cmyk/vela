"""Game settings a profile changes while Vela is on, with exact undo (0.4.1: Control's HDR; 0.8.16: the game's upscaler).

A profile names one settings file or a list of them (profiles.py "game_cfg"), each:
  file     the path inside root
  root     "install" (the game folder, default) or "prefix" (the Proton prefix's user folder:
           <library>/steamapps/compatdata/<appid>/pfx/drive_c/users/steamuser -- UE's AppData/Local/<Game>/Saved/Config)
  format   "json" (JSON-style "key": value, as Control's renderer.ini has them; default) or "ini" (key=value in [section])
  section  the ini section (format "ini")
  keys     {key: value} Vela sets while it is on
  when     {key: [values]}: that key only when its current value is one of these (case-insensitive), else the player's value
           stays and is never touched (e.g. Ghostrunner's DLSSEnabled only when it is False)

On: the old values go into the plugin's settings folder (gamecfg/<appid>.json), then the new ones are written.
Off: the old values are written back, only those keys -- whatever else the player changed in the file stays.
Games write their settings when they quit, so with the game running the write waits until it has exited
(reconcile(), from the panel's polls and the plugin's 10 s loop); the setting is read at the next start anyway.
0.8.16: a file the game has not written yet (it never ran) waits too -- the keys go in after the first run
(before: nothing was recorded and the keys never went in). ensure() puts a profile's newer keys into a game that is on.
"""
import json
import os
import re


def key_rx(key):
    return re.compile(r'("%s"\s*:\s*)(-?[0-9.]+|true|false)' % re.escape(key))


def get_key(text, key):
    m = key_rx(key).search(text)
    if not m:
        return None
    v = m.group(2)
    return v == "true" if v in ("true", "false") else (float(v) if "." in v else int(v))


def set_key(text, key, value):
    v = "true" if value is True else "false" if value is False else str(value)
    rx = key_rx(key)
    if not rx.search(text):
        return text, False
    return rx.sub(lambda m: m.group(1) + v, text, count=1), True


def _ini_bounds(lines, section):
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == f"[{section.lower()}]"), None)
    if start is None:
        return None, None
    end = next((i for i in range(start + 1, len(lines)) if lines[i].lstrip().startswith("[")), len(lines))
    return start, end


def ini_get(text, section, key):
    """The value of key in [section] (a string), or None."""
    lines = text.splitlines()
    start, end = _ini_bounds(lines, section)
    if start is None:
        return None
    rx = re.compile(rf"^\s*{re.escape(key)}\s*=(.*)$", re.I)
    for i in range(start + 1, end):
        m = rx.match(lines[i])
        if m:
            return m.group(1).strip()
    return None


def ini_set(text, section, key, value):
    """key=value in [section]; only a key that is there (a game's settings file: a missing key is the game's own
    default, a new line could be read wrongly). Keeps every line's own ending. Returns (text, changed).
    0.8.16: line by line -- Ghostrunner's GameUserSettings.ini on the console mixes CRLF and LF (64 CRLF, 2 LF:
    "FidelityFSRMode=3\r\nDLSSEnabled=False\nCurrentUpscalingQuality=2"); a split on "\r\n" glued DLSSEnabled to the next
    line, the key was never found and DLSS stayed off (the plugin logged "set", the file was unchanged)."""
    lines = text.splitlines(True)
    bodies = [l.rstrip("\r\n") for l in lines]
    start, end = _ini_bounds(bodies, section)
    if start is None:
        return text, False
    rx = re.compile(rf"^(\s*{re.escape(key)}\s*=).*$", re.I)
    for i in range(start + 1, end):
        m = rx.match(bodies[i])
        if m:
            lines[i] = m.group(1) + str(value) + lines[i][len(bodies[i]):]
            return "".join(lines), True
    return text, False


def _str(v):
    return ("true" if v else "false") if isinstance(v, bool) else str(v)


def prefix_user(install_dir, appid):
    """The Proton prefix's user folder of the game: <library>/steamapps/compatdata/<appid>/pfx/drive_c/users/steamuser."""
    steamapps = os.path.dirname(os.path.dirname(os.path.normpath(install_dir)))  # <library>/steamapps/common/<dir>
    return os.path.join(steamapps, "compatdata", str(appid), "pfx", "drive_c", "users", "steamuser")


def entries(cfg):
    """A profile's game_cfg as a list of entries (a dict is one)."""
    if not cfg:
        return []
    return [cfg] if isinstance(cfg, dict) else list(cfg)


class GameCfg:
    def __init__(self, settings_dir, log=print):
        self.dir = os.path.join(settings_dir, "gamecfg")
        self.log = log

    def _path(self, appid):
        return os.path.join(self.dir, f"{appid}.json")

    def read(self, appid):
        try:
            with open(self._path(appid), encoding="utf-8") as f:
                st = json.load(f)
        except (OSError, ValueError):
            return None
        if isinstance(st, dict) and "path" in st and "files" not in st:  # 0.4.1-0.8.16 state: one JSON-style file
            st = {"files": [{"path": st["path"], "format": "json", "section": None, "orig": st.get("orig") or {},
                             "want": st.get("want") or {}, "when": {}}],
                  "pending": bool(st.get("pending")), "off": bool(st.get("off"))}
        return st

    def _write(self, appid, st):
        os.makedirs(self.dir, exist_ok=True)
        tmp = self._path(appid) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(st, f, indent=2)
        os.replace(tmp, self._path(appid))

    def _drop(self, appid):
        try:
            os.remove(self._path(appid))
        except OSError:
            pass

    @staticmethod
    def _resolve(appid, install_dir, e):
        root = prefix_user(install_dir, appid) if e.get("root") == "prefix" else install_dir
        return os.path.join(root, e["file"])

    @staticmethod
    def _apply(f, restore=False):
        """Writes the entry's keys into its file. Returns None when the file is missing (the game has not written it yet),
        else the entry with orig filled for keys it now holds (keys failing their "when" are left out)."""
        path = f["path"]
        if not os.path.isfile(path):
            return None
        try:
            with open(path, encoding="utf-8", newline="") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError):
            return {**f, "skipped": True}
        ini = f.get("format") == "ini"
        orig = dict(f.get("orig") or {})
        new = text
        for k, v in (f.get("want") or {}).items():
            cur = ini_get(new, f["section"], k) if ini else get_key(new, k)
            if cur is None:
                continue  # the key is not in the file: the game's own default, untouched
            if not restore:
                when = (f.get("when") or {}).get(k)
                if k not in orig and when is not None and _str(cur).lower() not in [_str(w).lower() for w in when]:
                    continue  # the player's own value: kept, never restored
                if k not in orig:
                    orig[k] = cur
            new, _ = ini_set(new, f["section"], k, v) if ini else set_key(new, k, v)
        if new != text:
            tmp = path + ".vela-tmp"
            with open(tmp, "w", encoding="utf-8", newline="") as fh:
                fh.write(new)
            os.replace(tmp, path)
        return {**f, "orig": orig}

    def _files(self, appid, install_dir, cfg):
        return [{"path": self._resolve(appid, install_dir, e), "format": e.get("format", "json"),
                 "section": e.get("section"), "want": dict(e["keys"]), "when": dict(e.get("when") or {}), "orig": {}}
                for e in entries(cfg)]

    def enable(self, appid, install_dir, cfg, running):
        """cfg: the profile's game_cfg (one entry or a list). The original values a file had before Vela stay from the first
        enable on (a re-enable never takes Vela's own values as the player's). Returns the status."""
        if not cfg or not install_dir:
            return self.status(appid)
        st = self.read(appid)
        old = {f["path"]: f for f in (st or {}).get("files", [])}
        files = []
        for f in self._files(appid, install_dir, cfg):
            prev = old.get(f["path"])
            if prev:
                f["orig"] = {k: v for k, v in (prev.get("orig") or {}).items() if k in f["want"]}
            files.append(f)
        self._write(appid, {"files": files, "pending": True, "off": False})
        self.reconcile(set() if not running else {appid}, only=appid)
        return self.status(appid)

    def ensure(self, appid, install_dir, cfg, running):
        """0.8.16: a game Vela is on in gets the profile's current keys (a profile that gained keys after the game was
        set up, or a game set up before its profile had any). Nothing when the state already matches."""
        if not cfg or not install_dir:
            return self.status(appid)
        st = self.read(appid)
        want = [(f["path"], f["section"], f["want"], f["when"]) for f in self._files(appid, install_dir, cfg)]
        have = [(f["path"], f.get("section"), f.get("want"), f.get("when") or {}) for f in (st or {}).get("files", [])]
        if st and not st.get("off") and want == have:
            return self.status(appid)
        return self.enable(appid, install_dir, cfg, running)

    def disable(self, appid, running):
        st = self.read(appid)
        if st is None:
            return None
        files = [{**f, "want": {k: v for k, v in (f.get("orig") or {}).items() if v is not None}} for f in st["files"]
                 if f.get("orig")]
        self._write(appid, {"files": files, "pending": True, "off": True})
        self.reconcile(set() if not running else {appid}, only=appid)
        return self.status(appid)

    def has_pending(self):
        try:
            return any(n.endswith(".json") for n in os.listdir(self.dir))
        except OSError:
            return False

    def reconcile(self, running, only=None):
        """Writes what waits for games that are not running (and for files the game has written since)."""
        try:
            names = [f"{only}.json"] if only else [n for n in os.listdir(self.dir) if n.endswith(".json")]
        except OSError:
            return
        for n in names:
            appid = n[:-5]
            st = self.read(appid)
            if not st or not st.get("pending") or appid in running:
                continue
            off = bool(st.get("off"))
            files, waiting = [], False
            for f in st["files"]:
                if f.get("done") and not off:
                    files.append(f)
                    continue
                res = self._apply(f, restore=off)
                if res is None:
                    if not off:
                        waiting = True  # the game has not written this file yet: after its first run
                        files.append(f)
                    continue  # off and the file is gone: nothing to put back
                res["done"] = True
                self.log(f"gamecfg: {appid} {f['path']}: {'restored' if off else 'set'} {res['want']} (orig {res.get('orig')})")
                files.append(res)
            if off:
                self._drop(appid)
            else:
                self._write(appid, {"files": files, "pending": waiting, "off": False})

    def status(self, appid):
        st = self.read(appid)
        if not st:
            return {"on": False, "pending": False}
        keys = {}
        for f in st["files"]:
            keys.update(f.get("want") or {})
        return {"on": not st.get("off"), "pending": bool(st.get("pending")),
                "waiting": any(not f.get("done") and not os.path.isfile(f["path"]) for f in st["files"]),
                "keys": keys, "file": os.path.basename(st["files"][0]["path"]) if st["files"] else ""}
