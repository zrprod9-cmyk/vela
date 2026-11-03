"""0.8.16: "Report a problem" in the panel.

The player picks a reason (+ a short text), Vela packs one zip (<= 5 MB) of what the run left -- the kept OptiScaler.log of
the run, the NPU layer log, the game state the panel shows, the live settings, Steam's launch options, versions + md5 of
the plugin / OptiScaler / provider / NPU layer, the OS build, Mesa and Proton, the kernel's GPU lines and gamescope's
journal lines of the run window -- anonymised (home path, user name, Steam ids and account names, host name, IPs, MACs,
token-like values), shows the file list, and only on "Send" posts it over HTTPS to our relay (a
Cloudflare Worker that forwards it to the developers' Telegram chat). The zip stays in <settings>/reports so it can be sent
again. Nothing is ever sent by itself: after a run that looks bad (no clean exit in a short run, a GPU hang / fault in the
run window, FSR 4 fell back, SGSR 2 instead, NPU fallbacks) the panel asks once per run, opt-in.

The relay URL: report.json "relay_url" in the settings folder, else DEFAULT_RELAY (empty in a build that has no relay yet:
the panel then says reports are not set up and keeps the zip). Client limit: RATE_MAX uploads per RATE_WINDOW."""
import hashlib
import io
import json
import os
import re
import socket
import ssl
import subprocess
import time
import urllib.error
import urllib.request
import uuid
import zipfile

MAX_ZIP = 5 * 1024 * 1024
RATE_MAX, RATE_WINDOW = 3, 3600
REASONS = ("crash", "fsr", "image", "slow", "other")
TEXT_MAX = 500
# release builds put the deployed Worker URL here; report.json "relay_url" overrides it
DEFAULT_RELAY = "https://vela-report.vela-upscale.workers.dev"
SCHEMA = 1
# a run with no clean-exit line asks only when it was short: Steam's "Exit game" kills the game too (no line either:
# 27 of 44 console runs had none), a crash at the start is what the report is for
SHORT_RUN_S = 180
CLEAN_EXIT = ("Unloading OptiScaler", "DLL_PROCESS_DETACH")
KEEP_PENDING = 50
# raw caps before compression (an OptiScaler Debug log reaches 9 MB); the head keeps the Init lines, the tail the end
RUN_HEAD, RUN_TAIL = 1 << 20, 5 << 20
NPU_TAIL = 1 << 20
KERNEL_LINES, JOURNAL_LINES, PLUGIN_LINES = 3000, 2000, 400
# the run window around the kept log: from its first line to its mtime (+ the kill / hangcheck right after)
WINDOW_BEFORE, WINDOW_AFTER = 60, 45
WINDOW_DEFAULT = 30 * 60

GPU_RX = re.compile(r"drm|msm|adreno|kgsl|gmu|\bgpu|hangcheck|gamescope|smmu|iommu|devcoredump|a7xx|freedreno|"
                    r"out of memory|oom-kill|segfault|systemd-coredump", re.I)
HANG_RX = re.compile(r"hangcheck|gpu lockup|gpu hang|offending task", re.I)
FAULT_RX = re.compile(r"\*\*\* gpu fault|adreno_fault_handler|arm-smmu.*fault", re.I)
GAMESCOPE_RX = re.compile(r"gamescope", re.I)
# a file name the bundle never carries, whatever asks for it (the bundle only takes the files below anyway)
DENY_RX = re.compile(r"(^|[/\\])\.env|token|secret|password|credential|cookie|\.(png|jpe?g|bmp|webp|dds|sav)$|"
                     r"(^|[/\\])saves?([/\\]|$)|savegame", re.I)
_RX_TS = re.compile(r"^\[(\d\d):(\d\d):(\d\d)(?:\.\d+)?\]", re.M)

# ---- anonymising ----------------------------------------------------------------------------------------------------

_SECRET = re.compile(r"\b([A-Za-z0-9_]*(?:TOKEN|SECRET|PASSWORD|PASSWD|API_?KEY|AUTH|COOKIE)[A-Za-z0-9_]*)"
                     r"(\s*[=:]\s*)(\"[^\"]*\"|'[^']*'|[^\s\"',;]+)", re.I)
_BEARER = re.compile(r"\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]{8,}")
_TGTOKEN = re.compile(r"\b\d{6,12}:[A-Za-z0-9_-]{30,}\b")  # a Telegram bot token pasted anywhere
_STEAM64 = re.compile(r"\b7656119\d{10}\b")
_STEAM3 = re.compile(r"\[U:1:\d+\]")
_USERDATA = re.compile(r"(userdata[/\\])\d+")
_EMAIL = re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b")
_MAC = re.compile(r"(?<![0-9A-Fa-f:-])(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:-])")
_IP4 = re.compile(r"(?<![\w.])(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(?![\w.])")
_IP6_FULL = re.compile(r"(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}(?![0-9A-Fa-f:])")
_IP6_ZIP = re.compile(r"(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){1,6}:(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4}){0,5})?"
                      r"(?![0-9A-Za-z:])")
KEEP_IP4 = ("127.0.0.1", "0.0.0.0")
STEAM64_BASE = 76561197960265728


def _word(s, flags=0):
    return re.compile(r"(?<![A-Za-z0-9_])" + re.escape(s) + r"(?![A-Za-z0-9_])", flags)


def steam_identities(home):
    """Account names, persona names and Steam ids of the users this Steam remembers (config/loginusers.vdf): the
    anonymiser replaces each of them wherever it appears."""
    names, ids = set(), set()
    for root in (".local/share/Steam", ".steam/steam"):
        try:
            with open(os.path.join(home, root, "config", "loginusers.vdf"), encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for sid in re.findall(r"\"(7656119\d{10})\"", text):
            ids.add(sid)
            ids.add(str(int(sid) - STEAM64_BASE))
        names.update(re.findall(r"\"(?:AccountName|PersonaName)\"\s+\"([^\"]+)\"", text))
        break
    return names, ids


class Anon:
    """Replaces what names the player or the device: the home path (also in Wine / kernel escaped forms), the user and host
    names, Steam ids / account / persona names, e-mails, IPs and MACs, and the value of every token-like key."""

    def __init__(self, home=None, user=None, hosts=(), names=(), ids=()):
        self.rules = [(_TGTOKEN, "<token>"), (_BEARER, r"\1 <redacted>"), (_SECRET, r"\1\2<redacted>")]
        paths = set()
        if home:
            paths.add(home.rstrip("/"))
        if user and len(user) >= 2:
            paths.update({f"/home/{user}", f"/var/home/{user}", f"/run/media/{user}", f"/media/{user}"})
        for p in sorted(paths, key=len, reverse=True):
            for form in (p, p.replace("/", "\\"), p.replace("/", "\\\\"), p.replace("/", "\\x5c")):
                self.rules.append((re.compile(re.escape(form) + r"(?![A-Za-z0-9_])"), "~"))
        self.rules += [(_STEAM64, "<steamid>"), (_STEAM3, "[U:1:<id>]"), (_USERDATA, r"\1<id>")]
        for i in sorted({str(x) for x in ids if x and len(str(x)) >= 5}, key=len, reverse=True):
            self.rules.append((_word(i), "<steamid>"))
        for n in sorted({x for x in names if x and len(x) >= 3}, key=len, reverse=True):
            self.rules.append((_word(n, re.I), "<steam-name>"))
        if user and len(user) >= 3:
            self.rules.append((_word(user), "<user>"))
        for h in sorted({x for x in hosts if x and len(x) >= 3 and x not in ("localhost",)}, key=len, reverse=True):
            self.rules.append((_word(h), "<host>"))
        self.rules += [(_EMAIL, "<email>"), (_MAC, "<mac>"), (_IP6_FULL, "<ip6>"), (_IP6_ZIP, self._ip6),
                       (_IP4, self._ip4)]

    @staticmethod
    def _ip4(m):
        return m.group(0) if m.group(0) in KEEP_IP4 else "<ip>"

    @staticmethod
    def _ip6(m):
        s = m.group(0)
        # "::" alone or a C++ scope ("ffx::") is not an address: an address has a digit and two groups
        return "<ip6>" if re.search(r"\d", s) and len([g for g in s.split(":") if g]) >= 2 else s

    def __call__(self, text):
        if not text:
            return text
        for rx, rep in self.rules:
            text = rx.sub(rep, text)
        return text

    def obj(self, value):
        """The same over a JSON-like value (keys too)."""
        if isinstance(value, str):
            return self(value)
        if isinstance(value, dict):
            return {self(str(k)): self.obj(v) for k, v in value.items()}
        if isinstance(value, (list, tuple)):
            return [self.obj(v) for v in value]
        return value


def anon_for(home, extra_hosts=()):
    user = os.path.basename((home or "").rstrip("/")) or None
    for k in ("USER", "LOGNAME"):
        if os.environ.get(k) and os.environ[k] != "root":
            user = user or os.environ[k]
    hosts = set(extra_hosts)
    try:
        hosts.add(socket.gethostname())
    except OSError:
        pass
    for p in ("/etc/hostname",):
        try:
            with open(p) as f:
                hosts.add(f.read().strip())
        except OSError:
            pass
    names, ids = steam_identities(home) if home else (set(), set())
    return Anon(home, user, hosts, names, ids)


# ---- what went wrong in a run --------------------------------------------------------------------------------------

def log_duration(text):
    """Seconds from the first to the last [HH:MM:SS] line (OptiScaler.log and the interceptor's log), None without two."""
    ts = _RX_TS.findall(text or "")
    if len(ts) < 2:
        return None
    a = int(ts[0][0]) * 3600 + int(ts[0][1]) * 60 + int(ts[0][2])
    b = int(ts[-1][0]) * 3600 + int(ts[-1][1]) * 60 + int(ts[-1][2])
    return (b - a) % 86400


def assess(log_text, summary=None, kernel=None, npu_layer=None, intercept=False, run_s=None):
    """The flags of one run. bad = the panel asks "Send a report to the developers?" (once, opt-in). Strong signs ask always;
    no clean-exit line asks only in a short run (Steam's exit kills the game too: 27/44 runs had no line). run_s = how
    long the game ran as the plugin saw it (Reporter.observe); the log's own span is no run length (OptiScaler stops writing
    after the start at Info level: a dry run on the console read 17/29 runs as "short" from it)."""
    s = summary or {}
    n = s.get("npu") or {}
    kernel = kernel or []
    dur = log_duration(log_text)
    clean = None if intercept or not log_text else any(k in log_text for k in CLEAN_EXIT)
    layer = npu_layer or {}
    flags = {
        "no_clean_exit": clean is False,
        "duration_s": None if run_s is None else int(run_s),
        "log_span_s": dur,
        "short_run": clean is False and run_s is not None and run_s < SHORT_RUN_S,
        "gpu_hang": sum(1 for line in kernel if HANG_RX.search(line)) or 0,
        "gpu_faults": sum(1 for line in kernel if FAULT_RX.search(line)) or 0,
        "fsr4_fallback": bool(s.get("fsr4_fallback")),
        "sgsr2_instead": s.get("sgsr2_instead") or None,
        "fsr3_fallback": bool(s.get("fsr3_fallback")),
        "npu_fallback": (n.get("reason") or "fallback") if n.get("state") == "fallback" else None,
        "npu_fallbacks": n.get("fallbacks") or 0,
        "npu_errors": (layer.get("errors") or 0) + (layer.get("timeouts") or 0),
        "mode": s.get("mode"),
    }
    why = [k for k in ("gpu_hang", "gpu_faults", "fsr4_fallback", "sgsr2_instead", "fsr3_fallback", "npu_fallback",
                       "npu_fallbacks", "npu_errors", "short_run") if flags[k]]
    flags["bad"] = bool(why)
    flags["why"] = why
    return flags


# ---- small file helpers ----------------------------------------------------------------------------------------------

def read_text(path, head=0, tail=None):
    """The file as text; with tail: the first `head` bytes + a cut line + the last `tail` bytes. None when it cannot be read."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            if tail is None or size <= head + tail:
                data = f.read()
            else:
                a = f.read(head) if head else b""
                f.seek(size - tail)
                data = a + f"\n... [{size - head - tail} bytes cut by Vela report] ...\n".encode() + f.read()
    except OSError:
        return None
    return data.decode("utf-8", "replace")


def file_md5(path):
    try:
        h = hashlib.md5()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def os_release(path="/etc/os-release"):
    out = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                if k in ("PRETTY_NAME", "VERSION_ID", "BUILD_ID", "IMAGE_VERSION", "ID", "ID_LIKE"):
                    out[k] = v.strip().strip('"')
    except OSError:
        pass
    return out


CA_BUNDLES = ["/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/certs/ca-bundle.crt",
              "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/cert.pem", "/etc/ssl/ca-bundle.pem"]


def ssl_context():
    """Decky's frozen Python has no CA paths of Fedora-based Armada OS: the system bundle that exists."""
    for path in [os.environ.get("SSL_CERT_FILE")] + CA_BUNDLES:
        if path and os.path.isfile(path):
            return ssl.create_default_context(cafile=path)
    return ssl.create_default_context()


def _clean_env():
    env = {k: v for k, v in os.environ.items() if k not in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONHOME", "PYTHONPATH")}
    env.setdefault("LANG", "C")
    return env


def multipart(fields, files):
    """multipart/form-data body: fields {name: str}, files {name: (filename, bytes, content type)}."""
    boundary = "vela" + uuid.uuid4().hex
    b = io.BytesIO()
    for name, value in fields.items():
        b.write(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n"
                f"Content-Type: application/json; charset=utf-8\r\n\r\n".encode())
        b.write(value.encode("utf-8") + b"\r\n")
    for name, (fname, data, ctype) in files.items():
        b.write(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"; filename=\"{fname}\"\r\n"
                f"Content-Type: {ctype}\r\n\r\n".encode())
        b.write(data + b"\r\n")
    b.write(f"--{boundary}--\r\n".encode())
    return b.getvalue(), f"multipart/form-data; boundary={boundary}"


class Reporter:
    def __init__(self, settings_dir, home, plugin_dir, s3=None, npu=None, live=None, log=print, runner=subprocess.run,
                 now=time.time, urlopen=urllib.request.urlopen, plugin_log_dir=None, anon=None, live_tmp="/tmp/vela-live",
                 os_release_path="/etc/os-release"):
        self.dir = os.path.join(settings_dir, "reports")
        self.settings = settings_dir
        self.home = home
        self.plugin_dir = plugin_dir
        self.s3, self.npu, self.live = s3, npu, live
        self.log = log
        self._run = runner
        self.now = now
        self._urlopen = urlopen
        self.plugin_log_dir = plugin_log_dir
        self.live_tmp = live_tmp
        self.os_release_path = os_release_path
        self._anon = anon
        self._ssl = None
        self._runs = {}    # appid -> when the plugin first saw it running (None: it ran before the plugin started)
        self._ended = {}   # appid -> seconds the last run lasted (None: unknown)
        self._primed = False

    def observe(self, running):
        """Called with gameutil.running_appids() (reconcile loop, panel): the run length the bad-run check needs."""
        t = self.now()
        for a in running:
            if a not in self._runs:
                self._runs[a] = t if self._primed else None
        for a in [a for a in self._runs if a not in running]:
            start = self._runs.pop(a)
            self._ended[a] = None if start is None else t - start
        self._primed = True

    # -- settings / state files

    @property
    def anon(self):
        if self._anon is None:
            self._anon = anon_for(self.home)
        return self._anon

    def _path(self, name):
        return os.path.join(self.dir, name)

    def _read(self, name, default):
        try:
            with open(self._path(name), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return default

    def _write(self, name, data):
        os.makedirs(self.dir, exist_ok=True)
        tmp = self._path(name) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        os.replace(tmp, self._path(name))

    def cfg(self):
        try:
            with open(os.path.join(self.settings, "report.json"), encoding="utf-8") as f:
                c = json.load(f)
        except (OSError, ValueError):
            c = {}
        return c if isinstance(c, dict) else {}

    def set_cfg(self, **kw):
        c = {**self.cfg(), **kw}
        tmp = os.path.join(self.settings, "report.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(c, f, indent=1)
        os.replace(tmp, os.path.join(self.settings, "report.json"))
        return c

    def relay(self):
        url = str(self.cfg().get("relay_url") or DEFAULT_RELAY).strip()
        # HTTPS only; plain http only to this device (tests, a local relay)
        if url.startswith("https://") or re.match(r"^http://(127\.0\.0\.1|localhost)(:\d+)?/", url + "/"):
            return url
        return ""

    def sent_times(self):
        t = self.now()
        return [x for x in self._read("sent.json", []) if isinstance(x, (int, float)) and t - x < RATE_WINDOW]

    def rate_left(self):
        return max(0, RATE_MAX - len(self.sent_times()))

    def rate_wait_s(self):
        times = sorted(self.sent_times())
        if len(times) < RATE_MAX:
            return 0
        return int(times[-RATE_MAX] + RATE_WINDOW - self.now()) + 1

    def panel(self):
        """What the panel needs every refresh: set up or not, the uploads left this hour, the run waiting for an answer."""
        pend = [p for p in self._read("pending.json", []) if p.get("state") == "ask"]
        return {"configured": bool(self.relay()), "rate_left": self.rate_left(), "auto_prompt": self.auto_prompt(),
                "pending": pend[-1] if pend else None}

    def auto_prompt(self):
        return self.cfg().get("auto_prompt", True) is not False

    # -- the run's files

    def run_log(self, appid, running=False, name=None):
        """(path, label) of the log to send: a named kept run, else the live log of a running game, else the last kept
        run, else the copy <appid>-OptiScaler.log; (None, None) when Vela has no log of this game."""
        logs = getattr(self.s3, "logs_dir", None) if self.s3 else None
        if name:
            if not re.match(r"^\d+-[\w.-]+\.log$", name) or not logs:
                raise ValueError(f"bad log name {name}")
            for d in (logs, os.path.join(logs, "_old")):
                if os.path.isfile(os.path.join(d, name)):
                    return os.path.join(d, name), name
            raise FileNotFoundError(name)
        if running and self.s3:
            try:
                sc = self.s3.scan(appid)
                p = os.path.join(sc["exe_dir"], self.s3.kit(appid)["log"]) if sc.get("exe_dir") else None
                if p and os.path.isfile(p):
                    return p, "live " + os.path.basename(p)
            except Exception:
                pass
        if logs:
            rx = re.compile(re.escape(str(appid)) + r"-\d{8}-\d{6}\.log$")
            try:
                runs = sorted(n for n in os.listdir(logs) if rx.match(n))
            except OSError:
                runs = []
            if runs:
                return os.path.join(logs, runs[-1]), runs[-1]
            for n in (f"{appid}-OptiScaler.log", f"{appid}-sgsr3intercept.log"):
                if os.path.isfile(os.path.join(logs, n)):
                    return os.path.join(logs, n), n
        return None, None

    def window(self, path, text, running=False):
        """(start, end) epoch seconds of the run: from the log's first line to its last write (+ margins)."""
        end = self.now()
        if path and not running:
            try:
                end = os.path.getmtime(path)
            except OSError:
                pass
        dur = log_duration(text)
        start = end - (dur if dur is not None else WINDOW_DEFAULT)
        return int(start - WINDOW_BEFORE), int((self.now() if running else end) + WINDOW_AFTER)

    def _cmd(self, args, timeout=10):
        try:
            r = self._run(args, capture_output=True, text=True, timeout=timeout, env=_clean_env(),
                          stdin=subprocess.DEVNULL)
            return r.stdout if r.returncode == 0 else None
        except (OSError, subprocess.SubprocessError, ValueError):
            return None

    def kernel_lines(self, start, end):
        out = self._cmd(["journalctl", "_TRANSPORT=kernel", "-q", "--no-pager", "-o", "short-iso", f"--since=@{start}", f"--until=@{end}"])
        if out is None:
            return None
        return [line for line in out.splitlines() if GPU_RX.search(line)][-KERNEL_LINES:]

    def gamescope_lines(self, start, end):
        out = self._cmd(["journalctl", "-q", "--no-pager", "-o", "short-iso", f"--since=@{start}", f"--until=@{end}"])
        if out is None:
            return None
        return [line for line in out.splitlines() if GAMESCOPE_RX.search(line)][-JOURNAL_LINES:]

    def plugin_log_lines(self, appid):
        d = self.plugin_log_dir
        try:
            names = sorted(n for n in os.listdir(d) if n.endswith(".log")) if d else []
        except OSError:
            names = []
        lines = []
        for n in names[-2:]:
            t = read_text(os.path.join(d, n), 0, 4 << 20) or ""
            lines += [x for x in t.splitlines() if str(appid) in x or "ERROR" in x or "Traceback" in x]
        return lines[-PLUGIN_LINES:]

    def versions(self, appid):
        a = os.path.join(self.plugin_dir, "assets")
        v = {}
        try:
            with open(os.path.join(self.plugin_dir, "package.json"), encoding="utf-8") as f:
                v["plugin"] = json.load(f).get("version")
        except (OSError, ValueError):
            v["plugin"] = None
        try:
            with open(os.path.join(a, "sgsr3", "VERSION.json"), encoding="utf-8") as f:
                vj = json.load(f)
            v["optiscaler"] = vj.get("dll")
        except (OSError, ValueError):
            v["optiscaler"] = None
        v["md5"] = {rel: file_md5(os.path.join(a, rel)) for rel in (
            "sgsr3/OptiScaler.dll", "sgsr3/amd_fidelityfx_upscaler_vk.dll", "sgsr3/amd_fidelityfx_dx12.dll",
            "sgsr3/OptiPatcher.asi", "staticfsr/dxgi.dll", "npu/libVkLayer_VELA_npu.so")}
        # what is in the game folder now (ours, an older build of ours, or the game's own)
        if self.s3:
            try:
                sc = self.s3.scan(appid)
                if sc.get("exe_dir"):
                    v["game_md5"] = {n: file_md5(os.path.join(sc["exe_dir"], n)) for n in (
                        "dxgi.dll", "amd_fidelityfx_upscaler_vk.dll", "amd_fidelityfx_dx12.dll", "OptiScaler.ini")}
            except Exception as e:
                v["game_md5"] = {"error": str(e)}
        return v

    def system(self, appid):
        s = {"os": os_release(self.os_release_path)}
        try:
            u = os.uname()
            s["kernel"] = u.release
            s["arch"] = u.machine
        except (AttributeError, OSError):
            pass
        for p in ("/sys/firmware/devicetree/base/model", "/sys/class/dmi/id/product_name"):
            t = read_text(p)
            if t:
                s["device"] = t.strip("\x00\n ")
                break
        for q in (["rpm", "-q", "mesa-vulkan-drivers"], ["pacman", "-Q", "vulkan-freedreno", "mesa"]):
            out = self._cmd(q, timeout=5)
            if out:
                s["mesa"] = out.strip().splitlines()
                break
        s["proton"] = self.proton(appid)
        return s

    def proton(self, appid):
        """The game's Wine prefix: its Proton version file and the first line of config_info."""
        if not self.s3:
            return None
        try:
            inst = self.s3.scan(appid).get("install_dir") or ""
        except Exception:
            return None
        m = re.match(r"^(.*[/\\]steamapps)[/\\]common[/\\]", inst)
        if not m:
            return None
        cd = os.path.join(m.group(1), "compatdata", str(appid))
        out = {}
        for n in ("version", "config_info"):
            t = read_text(os.path.join(cd, n))
            if t:
                out[n] = t.strip().splitlines()[0][:200]
        return out or None

    def live_values(self, appid):
        out = {}
        try:
            out["settings"] = self.live.get(appid) if self.live else None
        except Exception as e:
            out["settings"] = {"error": str(e)}
        t = read_text(os.path.join(self.live_tmp, f"{appid}.ini"))
        out["tmp_ini"] = t
        return out

    # -- the bundle

    def build(self, appid, reason, text="", launch=None, game_state=None, game_name=None, log_name=None, running=False,
              trigger="button", flags=None):
        """Packs the zip into <settings>/reports/<id>.zip (+ <id>.json) and returns what the panel shows before sending."""
        appid = str(appid)
        if not re.match(r"^\d{1,10}$", appid):
            raise ValueError(f"bad appid {appid}")
        if reason not in REASONS:
            raise ValueError(f"unknown reason {reason}")
        A = self.anon
        files, errors = {}, []
        path, label = self.run_log(appid, running, log_name)
        run_text = read_text(path, RUN_HEAD, RUN_TAIL) if path else None
        start, end = self.window(path, run_text, running)
        kernel = self.kernel_lines(start, end)
        if kernel is None:
            errors.append("kernel journal not readable")
        gsc = self.gamescope_lines(start, end)
        if gsc is None:
            errors.append("journal not readable")
        npu_path = self.npu.layer_log(appid) if self.npu else None
        npu_text = read_text(npu_path, 0, NPU_TAIL) if npu_path else None
        intercept = bool(path and "intercept" in os.path.basename(path))
        summary = (game_state or {}).get("sgsr3", {}).get("log") if game_state else None
        if summary is None and path and self.s3 is not None:
            try:
                import sgsr3
                summary = sgsr3.static_log_summary(path) if intercept else sgsr3.log_summary(path)
            except Exception as e:
                errors.append(f"log summary: {e}")
        layer = None
        if npu_text:
            try:
                import npu as npu_mod
                layer = npu_mod.parse_layer(npu_text)
            except Exception:
                layer = None
        if flags is None and log_name:  # the prompt's run: its flags were taken when it ended (with the run length)
            flags = next((p.get("flags") for p in self._read("pending.json", []) if p.get("key") == log_name), None)
        fl = flags or assess(run_text, summary, kernel, layer, intercept)
        sysinfo = self.system(appid)
        vers = self.versions(appid)
        t = self.now()
        local_id = f"{appid}-{time.strftime('%Y%m%d-%H%M%S', time.localtime(t))}"
        meta = {
            "schema": SCHEMA, "local_id": local_id, "created": int(t), "trigger": trigger,
            "appid": appid, "game": A(str(game_name or ""))[:120], "reason": reason, "text": A(str(text or "")[:TEXT_MAX]),
            "plugin": vers.get("plugin"), "optiscaler": vers.get("optiscaler"),
            "os": (sysinfo.get("os") or {}).get("PRETTY_NAME"), "mesa": (sysinfo.get("mesa") or [None])[0],
            "proton": (sysinfo.get("proton") or {}).get("version"),
            "log": label, "window": [start, end],
            "flags": {k: v for k, v in fl.items() if k not in ("why",)}, "why": fl.get("why", []),
        }
        if path:
            files["optiscaler.log"] = A(run_text or "")
        if npu_text:
            files["npu_layer.log"] = A(npu_text)
        if game_state is not None:
            files["game_state.json"] = json.dumps(A.obj(game_state), ensure_ascii=False, indent=1)
        files["live.json"] = json.dumps(A.obj(self.live_values(appid)), ensure_ascii=False, indent=1)
        files["launch_options.txt"] = A(launch if isinstance(launch, str) else "(not read)") + "\n"
        files["versions.json"] = json.dumps({"versions": vers, "system": A.obj(sysinfo)}, ensure_ascii=False, indent=1)
        if kernel is not None:
            files["kernel_gpu.txt"] = A("\n".join(kernel)) + "\n"
        if gsc is not None:
            files["gamescope.txt"] = A("\n".join(gsc)) + "\n"
        pl = self.plugin_log_lines(appid)
        if pl:
            files["plugin.log"] = A("\n".join(pl)) + "\n"
        if self.s3:
            try:
                sc = self.s3.scan(appid)
                ini = read_text(os.path.join(sc["exe_dir"], "OptiScaler.ini")) if sc.get("exe_dir") else None
                if ini:
                    files["OptiScaler.ini"] = A(ini)
            except Exception:
                pass
        meta["errors"] = errors
        files["meta.json"] = json.dumps(meta, ensure_ascii=False, indent=1)
        files["README.txt"] = README
        for n in files:
            if DENY_RX.search(n):
                raise RuntimeError(f"refused file {n}")
        data = self._zip(files)
        if len(data) > MAX_ZIP and "optiscaler.log" in files:
            files["optiscaler.log"] = A(read_text(path, 256 << 10, 1 << 20) or "")
            data = self._zip(files)
        if len(data) > MAX_ZIP:
            raise RuntimeError(f"report is {len(data) // 1024} KB, more than {MAX_ZIP // 1024} KB")
        os.makedirs(self.dir, exist_ok=True)
        with open(self._path(local_id + ".zip.tmp"), "wb") as f:
            f.write(data)
        os.replace(self._path(local_id + ".zip.tmp"), self._path(local_id + ".zip"))
        rec = {"meta": meta, "files": [{"name": n, "bytes": len(c.encode("utf-8"))} for n, c in files.items()],
               "zip_bytes": len(data), "status": "saved", "report_id": None}
        self._write(local_id + ".json", rec)
        self.log(f"report: {local_id} saved ({len(data) // 1024} KB, {len(files)} files, {trigger}, why {fl.get('why')})")
        return self.preview(local_id)

    @staticmethod
    def _zip(files):
        b = io.BytesIO()
        with zipfile.ZipFile(b, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for n, c in files.items():
                z.writestr(n, c.encode("utf-8"))
        return b.getvalue()

    def preview(self, local_id):
        rec = self.record(local_id)
        return {"id": local_id, "files": rec["files"], "zip_bytes": rec["zip_bytes"], "status": rec["status"],
                "report_id": rec.get("report_id"), "configured": bool(self.relay()), "rate_left": self.rate_left(),
                "path": self._path(local_id + ".zip"), "why": rec["meta"].get("why", [])}

    def record(self, local_id):
        if not re.match(r"^\d{1,10}-\d{8}-\d{6}$", str(local_id)):
            raise ValueError(f"bad report id {local_id}")
        rec = self._read(local_id + ".json", None)
        if not rec or not os.path.isfile(self._path(local_id + ".zip")):
            raise FileNotFoundError(f"report {local_id} not found")
        return rec

    def saved(self, limit=10):
        try:
            names = sorted((n[:-5] for n in os.listdir(self.dir) if re.match(r"^\d+-\d{8}-\d{6}\.json$", n)), reverse=True)
        except OSError:
            names = []
        out = []
        for n in names[:limit]:
            try:
                out.append(self.preview(n))
            except (OSError, ValueError):
                pass
        return out

    # -- upload

    def send(self, local_id):
        """POST the saved zip + its meta to the relay. Returns {"report_id": "VELA-..."}; raises with a reason for the panel."""
        rec = self.record(local_id)
        if rec.get("status") == "sent" and rec.get("report_id"):
            return {"report_id": rec["report_id"], "already": True}
        url = self.relay()
        if not url:
            raise RuntimeError("not_configured")
        wait = self.rate_wait_s()
        if wait:
            raise RuntimeError(f"rate_limited:{wait}")
        with open(self._path(local_id + ".zip"), "rb") as f:
            data = f.read()
        if len(data) > MAX_ZIP:
            raise RuntimeError("too_big")
        body, ctype = multipart({"meta": json.dumps(rec["meta"], ensure_ascii=False)},
                                {"file": (f"vela-{local_id}.zip", data, "application/zip")})
        self._write("sent.json", self.sent_times() + [self.now()])
        req = urllib.request.Request(url, data=body, method="POST",
                                     headers={"Content-Type": ctype, "User-Agent": f"Vela/{rec['meta'].get('plugin')}"})
        kw = {"timeout": 60}
        if url.startswith("https://"):
            if self._ssl is None:
                self._ssl = ssl_context()
            kw["context"] = self._ssl
        try:
            with self._urlopen(req, **kw) as r:
                ans = json.loads(r.read(4096).decode("utf-8", "replace") or "{}")
        except urllib.error.HTTPError as e:
            try:
                ans = json.loads(e.read(4096).decode("utf-8", "replace") or "{}")
            except ValueError:
                ans = {}
            raise RuntimeError(f"relay:{e.code}:{str(ans.get('error') or '')[:80]}")
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise RuntimeError(f"network:{getattr(e, 'reason', e)}")
        rid = str(ans.get("id") or "")
        if not ans.get("ok") or not re.match(r"^VELA-[A-Z0-9]{4,12}$", rid):
            raise RuntimeError(f"relay:bad answer:{str(ans.get('error') or '')[:80]}")
        rec.update(status="sent", report_id=rid, sent_at=int(self.now()))
        self._write(local_id + ".json", rec)
        self._mark(rec["meta"].get("log"), "sent")
        self.log(f"report: {local_id} sent as {rid}")
        return {"report_id": rid}

    # -- the prompt after a bad run

    def on_run_end(self, appid, log_path, summary=None, run_s=None):
        """A run's log was just kept (sgsr3.keep_runs): when the run looks bad, one pending prompt for it (opt-in)."""
        if not log_path or not self.auto_prompt():
            return None
        key = os.path.basename(log_path)
        pend = self._read("pending.json", [])
        if any(p.get("key") == key for p in pend):
            return None
        text = read_text(log_path, RUN_HEAD, RUN_TAIL)
        intercept = "intercept" in key or (self.s3 is not None and bool(getattr(self.s3, "kit")(appid).get("static")))
        if summary is None:
            try:
                import sgsr3
                summary = sgsr3.static_log_summary(log_path) if intercept else sgsr3.log_summary(log_path)
            except Exception:
                summary = None
        start, end = self.window(log_path, text)
        kernel = self.kernel_lines(start, end) or []
        layer = None
        if self.npu:
            try:
                import npu as npu_mod
                lt = read_text(self.npu.layer_log(appid), 0, NPU_TAIL)
                layer = npu_mod.parse_layer(lt) if lt else None
            except Exception:
                layer = None
        if run_s is None:
            run_s = self._ended.pop(str(appid), None)
        fl = assess(text, summary, kernel, layer, intercept, run_s)
        if not fl["bad"]:
            return None
        entry = {"key": key, "appid": str(appid), "at": int(self.now()), "why": fl["why"], "flags": fl, "state": "ask"}
        self._write("pending.json", (pend + [entry])[-KEEP_PENDING:])
        self.log(f"report: {appid} run {key} looks bad ({', '.join(fl['why'])}): the panel asks")
        return entry

    def _mark(self, key, state):
        if not key:
            return
        pend = self._read("pending.json", [])
        hit = False
        for p in pend:
            if p.get("key") == key and p.get("state") == "ask":
                p["state"] = state
                hit = True
        if hit:
            self._write("pending.json", pend)

    def dismiss(self, key, never=False):
        self._mark(key, "dismissed")
        if never:
            self.set_cfg(auto_prompt=False)
        return self.panel()


README = """Vela problem report
===================
Made by the Vela Decky plugin when the player pressed «Send» (nothing is sent without that).
Anonymised: home path -> ~, user / host names, Steam ids and account names, e-mails, IPs, MACs and token-like values replaced.
Never included: saves, screenshots, .env-like files, tokens, Steam login data.

meta.json          reason, the player's text, flags of the run (no clean exit, GPU hang/faults, FSR 4 fallback, NPU fallback)
optiscaler.log     the run's OptiScaler.log (or the interceptor's log), head + tail when large
npu_layer.log      the Vela NPU layer's log of this game
game_state.json    what the panel showed (scan, model, log summary)
live.json          live settings (model, NPU)
launch_options.txt Steam launch options of the game
versions.json      plugin / OptiScaler / provider / NPU layer versions + md5, OS build, kernel, Mesa, Proton
kernel_gpu.txt     kernel lines about the GPU / display in the run window
gamescope.txt      gamescope journal lines in the run window
plugin.log         Vela's own log lines about this game
OptiScaler.ini     OptiScaler's settings in the game folder
"""
