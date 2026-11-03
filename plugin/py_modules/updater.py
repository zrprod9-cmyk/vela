"""Update check and install for a Vela installed from a release zip.

Decky updates only plugins from its own store, so Vela looks for its own new versions.

The check is one request: GET RELEASES_LATEST with a plain "Vela/<version>" User-Agent and nothing else about the
player. It runs by itself at most once every CHECK_EVERY seconds (and never when the player turned it off); the
panel's "Check now" button runs it at once. The answer is kept in <settings>/update.json.

Install: the panel hands Decky's own install-from-URL route (utilities/install_plugin) the release zip URL and its
sha256; Decky asks the player to confirm, downloads the zip, refuses it when the sha256 differs and reloads the plugin.
Decky removes the installed plugin before it checks the hash, so a wrong hash would leave the player without Vela:
the backend downloads the zip first and checks it itself (sha256 from the release's vela-X.Y.Z.zip.sha256 asset; an
older release without it: the .md5 asset, then the sha256 is computed from the checked zip). Nothing is installed
without the player's click."""
import hashlib
import io
import json
import os
import re
import threading
import time
import urllib.error
import urllib.request
import zipfile

REPO = "zrprod9-cmyk/vela"
RELEASES_LATEST = f"https://api.github.com/repos/{REPO}/releases/latest"
DOWNLOAD_PREFIX = f"https://github.com/{REPO}/releases/download/"
CHECK_EVERY = 24 * 3600
CHECK_TIMEOUT = 10
DOWNLOAD_TIMEOUT = 120
MAX_ANSWER = 1 << 20      # the release JSON
MAX_SUM = 4096            # a .sha256 / .md5 asset
MAX_ZIP = 64 << 20
CACHE = "update.json"
ZIP_DIR = "upscale"       # the folder inside the release zip (= the plugin's folder on the device)

_VER = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")
_HEX = {"sha256": re.compile(r"\b([0-9a-fA-F]{64})\b"), "md5": re.compile(r"\b([0-9a-fA-F]{32})\b")}


def parse_version(s):
    """"v1.2.3" / "1.2.3" -> (1, 2, 3); anything else -> None."""
    m = _VER.match(str(s or "").strip())
    return tuple(int(x) for x in m.groups()) if m else None


def is_newer(latest, current):
    """True when version string latest is above current, compared number by number (0.8.10 > 0.8.9)."""
    a, b = parse_version(latest), parse_version(current)
    return bool(a and b and a > b)


def parse_sum(text, kind):
    """The first hex digest of the kind ("sha256" / "md5") in a checksum file ("<hex>  name" or just "<hex>")."""
    m = _HEX[kind].search(text or "")
    return m.group(1).lower() if m else None


def release_info(answer):
    """The parts of GitHub's release JSON the update needs, or None when it is not a usable release."""
    if not isinstance(answer, dict) or answer.get("draft") or answer.get("prerelease"):
        return None
    tag = str(answer.get("tag_name") or "")
    ver = parse_version(tag)
    if not ver:
        return None
    version = ".".join(map(str, ver))
    assets = {}
    for a in answer.get("assets") or []:
        if isinstance(a, dict) and isinstance(a.get("name"), str):
            assets[a["name"]] = a
    zname = f"vela-{version}.zip"

    def url(name):
        u = str((assets.get(name) or {}).get("browser_download_url") or "")
        return u if u.startswith(DOWNLOAD_PREFIX) else None

    digest = str((assets.get(zname) or {}).get("digest") or "")
    return {
        "version": version,
        "tag": tag,
        "zip_url": url(zname),
        "sha256_url": url(zname + ".sha256"),
        "md5_url": url(zname + ".md5"),
        "zip_size": (assets.get(zname) or {}).get("size"),
        "digest": digest[7:].lower() if digest.startswith("sha256:") and len(digest) == 71 else None,
    }


class UpdateError(Exception):
    """A short reason the panel turns into text: network, github:<code>, bad_answer, no_zip, no_checksum,
    hash_mismatch, bad_zip, not_newer."""


class Updater:
    def __init__(self, settings_dir, plugin_dir, log=print, urlopen=urllib.request.urlopen, now=time.time,
                 ssl_context=None):
        self.path = os.path.join(settings_dir, CACHE)
        self.plugin_dir = plugin_dir
        self.log = log
        self._urlopen = urlopen
        self.now = now
        self._ssl_factory = ssl_context
        self._ssl = None
        self._lock = threading.Lock()

    # ---- what is installed ----

    def _plugin_json(self, name):
        try:
            with open(os.path.join(self.plugin_dir, name), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def current(self):
        return str(self._plugin_json("package.json").get("version") or "")

    def plugin_name(self):
        return str(self._plugin_json("plugin.json").get("name") or "Vela")

    # ---- the cache ----

    def _load(self):
        try:
            with open(self.path, encoding="utf-8") as f:
                d = json.load(f)
            return d if isinstance(d, dict) else {}
        except (OSError, ValueError):
            return {}

    def _save(self, d):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(d, f, indent=1)
        os.replace(tmp, self.path)

    def enabled(self):
        return self._load().get("enabled", True) is not False

    def set_enabled(self, on):
        d = self._load()
        d["enabled"] = bool(on)
        self._save(d)
        return self.state()

    def due(self):
        """The automatic check may run: it is on and the last one is CHECK_EVERY old (or the clock went back)."""
        d = self._load()
        if d.get("enabled", True) is False:
            return False
        last = d.get("checked_at")
        if not isinstance(last, (int, float)):
            return True
        age = self.now() - last
        return age >= CHECK_EVERY or age < 0

    def state(self):
        d = self._load()
        cur = self.current()
        rel = d.get("latest") if isinstance(d.get("latest"), dict) else None
        latest = rel.get("version") if rel else None
        return {
            "enabled": d.get("enabled", True) is not False,
            "current": cur,
            "latest": latest,
            "available": bool(rel and rel.get("zip_url") and is_newer(latest, cur)),
            "checked_at": d.get("checked_at"),
            "error": d.get("error"),
        }

    # ---- network ----

    def _context(self, url):
        if not url.startswith("https://") or self._ssl_factory is None:
            return {}
        if self._ssl is None:
            self._ssl = self._ssl_factory()
        return {"context": self._ssl}

    def _get(self, url, limit, timeout, accept=None):
        headers = {"User-Agent": f"Vela/{self.current() or '0'}"}
        if accept:
            headers["Accept"] = accept
        req = urllib.request.Request(url, headers=headers, method="GET")
        try:
            with self._urlopen(req, timeout=timeout, **self._context(url)) as r:
                data = r.read(limit + 1)
        except urllib.error.HTTPError as e:
            raise UpdateError(f"github:{e.code}")
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise UpdateError(f"network:{getattr(e, 'reason', e)}")
        if len(data) > limit:
            raise UpdateError("bad_answer")
        return data

    def check(self, force=False):
        """Ask GitHub for the latest release when due (force: now, also with the automatic check off).
        Returns the panel state; a failed check is in state["error"] and never raises."""
        with self._lock:
            if not force and not self.due():
                return self.state()
            d = self._load()
            d["checked_at"] = self.now()
            try:
                raw = self._get(RELEASES_LATEST, MAX_ANSWER, CHECK_TIMEOUT, "application/vnd.github+json")
                try:
                    rel = release_info(json.loads(raw.decode("utf-8")))
                except ValueError:
                    rel = None
                if not rel:
                    raise UpdateError("bad_answer")
                d["latest"] = rel
                d["error"] = None
                self.log(f"update check: latest {rel['version']}, installed {self.current()}")
            except UpdateError as e:
                d["error"] = str(e)
                self.log(f"update check failed: {e}")
            self._save(d)
            return self.state()

    # ---- install ----

    def _sum(self, url, kind):
        if not url:
            return None
        return parse_sum(self._get(url, MAX_SUM, CHECK_TIMEOUT).decode("utf-8", "replace"), kind)

    def verify_zip(self, data, version):
        """The zip is a Vela plugin of that version in the folder Decky keeps it in; raises UpdateError("bad_zip")."""
        try:
            z = zipfile.ZipFile(io.BytesIO(data))
            plugin = json.loads(z.read(f"{ZIP_DIR}/plugin.json").decode("utf-8"))
            package = json.loads(z.read(f"{ZIP_DIR}/package.json").decode("utf-8"))
            names = z.namelist()
        except (zipfile.BadZipFile, KeyError, ValueError, UnicodeDecodeError):
            raise UpdateError("bad_zip")
        if plugin.get("name") != self.plugin_name() or package.get("version") != version or \
                any(not n.startswith(ZIP_DIR + "/") for n in names) or f"{ZIP_DIR}/main.py" not in names:
            raise UpdateError("bad_zip")

    def prepare(self):
        """What the panel passes to Decky's install-from-URL: {url, name, version, hash, install_type}.
        The zip is downloaded and checked here first; any doubt raises UpdateError and nothing is installed."""
        rel = self._load().get("latest")
        if not isinstance(rel, dict) or not is_newer(rel.get("version"), self.current()):
            raise UpdateError("not_newer")
        if not rel.get("zip_url"):
            raise UpdateError("no_zip")
        sha = self._sum(rel.get("sha256_url"), "sha256")
        md5 = None if sha else self._sum(rel.get("md5_url"), "md5")
        if not sha and not md5:
            raise UpdateError("no_checksum")
        data = self._get(rel["zip_url"], MAX_ZIP, DOWNLOAD_TIMEOUT)
        got = hashlib.sha256(data).hexdigest()
        if sha and got != sha:
            raise UpdateError("hash_mismatch")
        if md5 and hashlib.md5(data).hexdigest() != md5:
            raise UpdateError("hash_mismatch")
        if rel.get("digest") and rel["digest"] != got:
            raise UpdateError("hash_mismatch")
        self.verify_zip(data, rel["version"])
        self.log(f"update {rel['version']}: zip checked ({'sha256' if sha else 'md5'}), sha256 {got}")
        # Decky's PluginInstallType: 2 = update
        return {"url": rel["zip_url"], "name": self.plugin_name(), "version": rel["version"], "hash": got,
                "install_type": 2}
