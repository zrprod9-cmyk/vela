"""Vela NPU (0.8.0): the FSR 4 network on the Hexagon NPU .

Three parts, two of them outside the plugin:
- the system component: fsr4npud, a host service that holds the HTP and listens on one abstract AF_UNIX socket per
  graph (@fsr4npud_nf4_k1h ...). Installed by the user (sysext + udev + units); a console may not have it. 0.8.15: FSR 4
  Lite is out of Vela, so nothing here looks at the Lite daemon (socket @fsr4npud, unit vela-npu.service): "installed" and
  "running" mean the daemons of the full FSR 4 graphs, and a console without the Lite daemon is complete.
- our native Vulkan layer VK_LAYER_VELA_npu: assets/npu/libVkLayer_VELA_npu.so. Its
  manifest goes to the user's implicit layer dir (~/.local/share/vulkan/implicit_layer.d, which pressure-vessel imports
  into the game's container) with the plugin's .so as an absolute path; it loads only with VELA_NPU=1 in the game's
  launch options. The manifest is there only while a game has the layer's tokens (sync_manifest). 0.8.15: a game Vela
  is set up in keeps the tokens whatever the model (the layer is idle when no model uses the NPU); the live key npu alone
  switches the NPU, so FSR 3 / SGSR 2 -> FSR 4 needs no restart.
- the fork (SGSR 3 / FSR 4 inside the game) reads the live key npu=0|1|2 (off / exact / async, live.py) and
  hands its FSR inputs to the layer; with no layer or no service it stays on its GPU path and says why in
  OptiScaler.log. The log lines both sides print are parsed by parse_fork / parse_layer below.
"""
import json
import os
import re
import subprocess
import time

LAYER_NAME = "VK_LAYER_VELA_npu"
LIB = "libVkLayer_VELA_npu.so"
MANIFEST = "VkLayer_VELA_npu.json"
ENABLE_ENV = ("VELA_NPU", "1")
DISABLE_ENV = ("DISABLE_VELA_NPU", "1")
# 0.8.3: the full FSR 4.1.1 network (graph nf4_exact) has its own daemon: package v3 unit vela-npu@nf4_exact.service,
# socket @fsr4npud_nf4_exact (the layer picks it from the provider's PROBE, VELA_NPU_F_GRAPH)
FULL_SOCKET = "fsr4npud_nf4_exact"
FULL_UNIT = "vela-npu@nf4_exact.service"
# 0.8.5: the 1080p performance preset runs the network + postpass K1 graph nf4_k1h (package v5 unit vela-npu@nf4_k1h.service,
# socket @fsr4npud_nf4_k1h); the other presets keep nf4_exact. Either daemon listening = the full model can use the NPU.
K1H_SOCKET = "fsr4npud_nf4_k1h"
K1H_UNIT = "vela-npu@nf4_k1h.service"
# 0.8.7 / 0.8.9: 1280x720 output runs graph nf4_k1h_360 (sysext v7 unit vela-npu@nf4_k1h_360.service); the panel's
# 720p mode asks for this daemon by name
K1H360_SOCKET = "fsr4npud_nf4_k1h_360"
K1H360_UNIT = "vela-npu@nf4_k1h_360.service"
# 0.8.9: 1600x900 output runs graph nf4_k1h_450 (sysext v8 unit vela-npu@nf4_k1h_450.service). The graph file
# tells v8 from v7 (an instance of the template unit has no file of its own in the unit dirs)
K1H450_SOCKET = "fsr4npud_nf4_k1h_450"
K1H450_UNIT = "vela-npu@nf4_k1h_450.service"
GRAPH_DIR = "/usr/lib64/vela-npu"
K1H450_GRAPH = "nf4_k1h_450.bin"
# 0.8.14: FSR 4 Ultra Performance (render 1/3 of the window) runs its own graphs nf4up_k1h / _450 / _360 (sysext v9 units
# vela-npu@nf4up_k1h*.service, sockets fsr4npud_nf4up_k1h*). The graph file tells v9 from v8.
UP_SOCKETS = {"1920x1080": "fsr4npud_nf4up_k1h", "1600x900": "fsr4npud_nf4up_k1h_450", "1280x720": "fsr4npud_nf4up_k1h_360"}
UP_GRAPH = "nf4up_k1h.bin"
# 0.8.15: the full FSR 4 daemons, by their sockets (in /proc/net/unix as "@<name>"): the graph of each preset / output / Ultra
# Performance. The list is what "the NPU is there" means; the Lite daemon (@fsr4npud) is not in it.
FULL_SOCKETS = (FULL_SOCKET, K1H_SOCKET, K1H360_SOCKET, K1H450_SOCKET) + tuple(UP_SOCKETS.values())
# the units that hold them: sysext v9 and v10 one instance per graph (template vela-npu@.service; v10 = v9 minus
# the Lite daemon vela-npu.service + vela-npu@nf4up_k1h_360 enabled, 7 daemons); the multi-graph daemons (vela-npu.service,
# vela-npu-up.service) are a v11 candidate, not shipped. A unit file of the template or the UP daemon says the component is
# installed (vela-npu.service alone is not a sign: in v9 it is the Lite daemon)
TEMPLATE_UNIT = "vela-npu@.service"
UP_UNIT = "vela-npu-up.service"
# the graph files of the full network (sysext: GRAPH_DIR); any of them = the component is installed
# the Qualcomm QNN runtime; missing = an incomplete install (the public image carries it)
QNN_LIB = "libQnnHtp.so"
FULL_GRAPHS = ("nf4_exact.bin", "nf4_k1h.bin", "nf4_k1h_360.bin", K1H450_GRAPH, UP_GRAPH)
# systemctl is asked for these, in this order, when no full daemon listens (the reason for the panel)
STATE_UNITS = (K1H_UNIT, "vela-npu.service", UP_UNIT)
UNIT_DIRS = ("/etc/systemd/system", "/run/systemd/system", "/usr/local/lib/systemd/system", "/usr/lib/systemd/system",
             "/lib/systemd/system")
MODES = (0, 1, 2)              # live key npu: off, exact (same output, no extra latency), async (+1 frame)
CACHE_S = 30.0
LOG_TAIL = 256 * 1024

# ---- log lines (parse_fork / parse_layer) ---------------------------------------------------------------------
# fork, in OptiScaler.log; the last of on / fallback / off in the run segment is the state
_RX_STATE = re.compile(r"Vela NPU: (?:(on), (exact|async)|(fallback) (\w+)(?: (\d+x\d+)->(\d+x\d+))?, GPU path|(off))\b")
# 0.8.3: "fallback size 640x360->1920x1080" names the render and output size (older forks: "fallback size")
_RX_STATS = re.compile(r"Vela NPU: (\d+) frames, HTP ([\d.]+) ms, fallbacks (\d+)")
# layer, in its own file VELA_NPU_LOG_FILE
_RX_LOADED = re.compile(r"Vela NPU layer: loaded v(\S+)")
_RX_DAEMON = re.compile(r"Vela NPU layer: daemon (connected|not reachable)(?: \(([^)]*)\))?")
_RX_JOBS = re.compile(r"Vela NPU layer: (\d+) jobs, HTP ([\d.]+) ms, timeouts (\d+), errors (\d+)")
REASONS = ("nolayer", "nodaemon", "badargs", "busy", "timeout", "size", "error")


def parse_fork(text):
    """What the fork's NPU path did in this run segment; None when it printed no NPU line (npu=0, an older fork)."""
    last = None
    for m in _RX_STATE.finditer(text or ""):
        last = m
    stats = _RX_STATS.findall(text or "")
    if not last and not stats:
        return None
    out = {"state": None, "mode": None, "reason": None, "render": None, "output": None, "frames": None, "htp_ms": None,
           "fallbacks": None}
    if last:
        if last.group(1):
            out.update(state="on", mode=last.group(2))
        elif last.group(3):
            r = last.group(4)
            out.update(state="fallback", reason=r if r in REASONS else "error", render=last.group(5), output=last.group(6))
        else:
            out["state"] = "off"
    if stats:
        out.update(frames=int(stats[-1][0]), htp_ms=float(stats[-1][1]), fallbacks=int(stats[-1][2]))
    return out


def parse_layer(text):
    """The layer's own log: loaded, the daemon reached or not (with the error), job statistics."""
    loaded = _RX_LOADED.findall(text or "")
    if not loaded:
        return None
    d = _RX_DAEMON.findall(text)
    jobs = _RX_JOBS.findall(text)
    return {"version": loaded[-1], "daemon": (d[-1][0] == "connected") if d else None,
            "daemon_error": (d[-1][1] or None) if d and d[-1][0] != "connected" else None,
            "jobs": int(jobs[-1][0]) if jobs else None, "htp_ms": float(jobs[-1][1]) if jobs else None,
            "timeouts": int(jobs[-1][2]) if jobs else None, "errors": int(jobs[-1][3]) if jobs else None}


def _tail(path, since=0.0):
    try:
        st = os.stat(path)
    except OSError:
        return None
    if st.st_mtime < since:
        return None
    with open(path, "rb") as f:
        if st.st_size > LOG_TAIL:
            f.seek(st.st_size - LOG_TAIL)
        return f.read().decode("utf-8", "replace")


# ---- the system component ---------------------------------------------------------------------------------------

def socket_listening(proc_net_unix="/proc/net/unix", name=K1H_SOCKET):
    """The daemon's abstract socket is bound: read from /proc, nothing connects to the daemon."""
    try:
        with open(proc_net_unix, encoding="utf-8", errors="replace") as f:
            return any(line.split()[-1] == "@" + name for line in f if line.strip() and len(line.split()) >= 8)
    except OSError:
        return False


def unit_installed(dirs=UNIT_DIRS, unit=TEMPLATE_UNIT):
    return any(os.path.exists(os.path.join(d, unit)) for d in dirs)


def unit_active(unit=K1H_UNIT):
    try:
        r = subprocess.run(["systemctl", "is-active", unit], capture_output=True, text=True, timeout=1.0)
        return r.stdout.strip() or None  # "active", "inactive", "failed", "activating"
    except (OSError, subprocess.SubprocessError):
        return None


class Npu:
    def __init__(self, assets_dir, home, settings_dir, log=print, proc_net_unix="/proc/net/unix", unit_dirs=UNIT_DIRS,
                 systemctl=unit_active, graph_dir=GRAPH_DIR, chip=None):
        self.assets = assets_dir                  # <plugin>/assets/npu
        # 0.8.16: device.policy() of this console's chip; None = no chip gate (behaves as on SM8550)
        self.chip = chip
        self.home = home
        self.dir = os.path.join(settings_dir, "npu")
        self.layer_dir = os.path.join(home, ".local", "share", "vulkan", "implicit_layer.d")
        self.log = log
        self._proc = proc_net_unix
        self._unit_dirs = unit_dirs
        self._systemctl = systemctl
        self._graph_dir = graph_dir
        self._svc = None
        self._svc_at = 0.0

    # -- the service: installed / running, cached (the panel asks on every refresh)

    def service(self, fresh=False):
        now = time.monotonic()
        if self._svc is None or fresh or now - self._svc_at > CACHE_S:
            # 0.8.15: the full FSR 4 daemons only. running = one of them listens; installed = one does, or the
            # template / UP unit or a graph file of the full network is on the system (the Lite daemon counts for nothing)
            running = any(socket_listening(self._proc, s) for s in FULL_SOCKETS)
            units = any(unit_installed(self._unit_dirs, u) for u in (TEMPLATE_UNIT, UP_UNIT))
            graphs = any(os.path.isfile(os.path.join(self._graph_dir, g)) for g in FULL_GRAPHS)
            installed = running or units or graphs
            # systemctl only when the component is there and no daemon listens: the reason for the panel
            state = "active" if running else self._unit_state() if installed else None
            self._svc = {"installed": installed, "running": running, "unit_state": state,
                         "qnn": running or os.path.isfile(os.path.join(self._graph_dir, QNN_LIB)),
                         "full_running": socket_listening(self._proc, FULL_SOCKET) or socket_listening(self._proc, K1H_SOCKET), "full_unit": FULL_UNIT,
                         "k1h_running": socket_listening(self._proc, K1H_SOCKET), "k1h_unit": K1H_UNIT,
                         "k1h360_running": socket_listening(self._proc, K1H360_SOCKET), "k1h360_unit": K1H360_UNIT,
                         "k1h450_running": socket_listening(self._proc, K1H450_SOCKET), "k1h450_unit": K1H450_UNIT,
                         "k1h450_graph": os.path.isfile(os.path.join(self._graph_dir, K1H450_GRAPH)),
                         "up_graph": os.path.isfile(os.path.join(self._graph_dir, UP_GRAPH)),
                         "up_running": {o: socket_listening(self._proc, s) for o, s in UP_SOCKETS.items()}}
            self._svc_at = now
        return dict(self._svc)

    def _unit_state(self):
        """What systemctl says about the first of STATE_UNITS it knows (None when it knows none)."""
        for u in STATE_UNITS:
            state = self._systemctl(u)
            if state:
                return state
        return None

    def layer_found(self):
        return os.path.isfile(os.path.join(self.assets, LIB)) and os.path.isfile(os.path.join(self.assets, MANIFEST))

    def ready(self):
        """Whether the panel may offer the NPU toggle: the layer is in the plugin and the full FSR 4 component is installed."""
        if self.chip is not None and not self.chip.get("npu"):
            return False  # 0.8.16: no NPU graphs for this chip (device.policy)
        return self.layer_found() and self.service()["installed"]

    # -- the layer's manifest in the user's layer dir

    def manifest_text(self):
        with open(os.path.join(self.assets, MANIFEST), encoding="utf-8") as f:
            m = json.load(f)
        layer = m.setdefault("layer", {})
        layer["name"] = LAYER_NAME
        layer["library_path"] = os.path.join(os.path.abspath(self.assets), LIB)
        layer["enable_environment"] = {ENABLE_ENV[0]: ENABLE_ENV[1]}
        layer["disable_environment"] = {DISABLE_ENV[0]: DISABLE_ENV[1]}
        return json.dumps(m, indent=2) + "\n"

    def _installed_path(self):
        return os.path.join(self.layer_dir, MANIFEST)

    def sync_manifest(self):
        """The manifest is in the user's layer dir while some game has the NPU on, and only then."""
        path = self._installed_path()
        want = self.layer_found() and bool(self.games_on())
        if want:
            text = self.manifest_text()
            try:
                with open(path, encoding="utf-8") as f:
                    if f.read() == text:
                        return True
            except OSError:
                pass
            os.makedirs(self.layer_dir, exist_ok=True)
            tmp = path + ".upscale-tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(text)
            os.replace(tmp, path)
            self.log(f"npu: layer manifest -> {path}")
            return True
        if os.path.exists(path):
            os.remove(path)
            self.log(f"npu: layer manifest removed ({path})")
        return False

    # -- per game state and launch options (exact restore like anygame)

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

    def games_on(self):
        try:
            names = [f[:-5] for f in os.listdir(self.dir) if f.endswith(".json")]
        except OSError:
            return []
        return [a for a in names if (self.read(a) or {}).get("on")]

    def layer_log(self, appid):
        return os.path.join(self.dir, f"layer-{appid}.log")

    def tokens(self, appid):
        return [f"{ENABLE_ENV[0]}={ENABLE_ENV[1]}", f"VELA_NPU_LOG_FILE={self.layer_log(appid)}"]

    @staticmethod
    def strip(options, tokens):
        out = options or ""
        for tok in tokens or []:
            out = re.sub(rf"(?<!\S){re.escape(tok)}(?!\S)", " ", out)
        return re.sub(r"\s+", " ", out).strip()

    def enable(self, appid, launch_options):
        """Launch options with the layer on (it loads at the next start of the game)."""
        if not self.layer_found():
            raise RuntimeError(f"plugin files assets/npu ({LIB}, {MANIFEST}) are missing")
        st = self.read(appid) or {}
        tokens = self.tokens(appid)
        if st.get("on") and launch_options is not None and launch_options == st.get("launch_set"):
            return launch_options
        if st.get("on"):
            base = self.strip(launch_options, st.get("tokens"))  # options changed since: ours again, theirs kept
        else:
            st = {"on": True, "launch_prev": launch_options, "installed_at": time.time()}
            base = self.strip(launch_options, tokens)
        if "%command%" not in base:
            base = f"%command% {base}".strip()
        new = " ".join(tokens + [base])
        st.update({"tokens": tokens, "launch_set": new, "updated_at": time.time()})
        self._write(appid, st)
        self.sync_manifest()
        self.log(f"npu: {appid} on, launch: {new}")
        return new

    def rewrite_state(self, appid, fn):
        """0.8.15: the options the state remembers (launch_prev, launch_set) through fn, so the exact restore and the
        "unchanged since we set them" check still match after a token left the options. True: changed."""
        st = self.read(appid)
        if not st or not st.get("on"):
            return False
        changed = False
        for key in ("launch_prev", "launch_set"):
            if isinstance(st.get(key), str) and fn(st[key]) != st[key]:
                st[key] = fn(st[key])
                changed = True
        if changed:
            self._write(appid, st)
        return changed

    def disable(self, appid, launch_options):
        """Launch options without the layer: exactly the previous ones if nobody touched them since."""
        st = self.read(appid)
        if not st or not st.get("on"):
            return launch_options
        if launch_options is None or launch_options == st.get("launch_set"):
            restored = st.get("launch_prev") or ""
        else:
            restored = self.strip(launch_options, st.get("tokens"))
            if restored == "%command%" and not (st.get("launch_prev") or "").strip():
                restored = ""
        try:
            os.remove(self._state_path(appid))
        except OSError:
            pass
        self.sync_manifest()
        self.log(f"npu: {appid} off, launch: {restored}")
        return restored

    # -- status for the panel

    def status(self, appid):
        st = self.read(appid) or {}
        on = bool(st.get("on"))
        text = _tail(self.layer_log(appid), st.get("installed_at", 0)) if on else None
        return {"service": self.service(), "layer_found": self.layer_found(), "ready": self.ready(), "on": on,
                "layer": parse_layer(text) if text else None}
