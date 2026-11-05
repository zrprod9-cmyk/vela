"""Vela 0.8.9: can this game be upscaled, and how -- decided once per game build, before the first start.

What the plugin can put into a game depends on three facts it reads from the game folder (sgsr3.scan_dir already
gives the exe, the engine, the input libraries and anti-cheat):
  api     which graphics APIs the game binary can use: the names of their libraries in the exe (and Unity's player
          DLL), ASCII or UTF-16 ("d3d12.dll", "d3d11.dll", "vulkan-1.dll", "opengl32.dll"). An engine that
          supports several (UE, Unity) lists several; the plugin asks for DX12 where it can (-dx12 for UE)
  inputs  the game's own temporal upscaler: an input DLL (sgsr3.INPUT_LIBS) or, with none, FSR / DLSS / XeSS
          symbols inside the binary (OptiScaler's pattern search then finds a static FSR)
  block   anti-cheat, not installed, no Windows exe (native Linux game), OpenGL only, a Godot game (0.8.16: its FSR 2
          is the engine's own Vulkan code, sgsr3.godot_signs)

kind (what the panel's modes run, presets.py):
  fsr4    DX12 + an input: our OptiScaler fork, FSR 4 / SGSR 2 / Vela AI live (the fsr4vk provider needs vkd3d)
  sgsr2   DX11 only + an input: OptiScaler runs SGSR 2 through 11on12 (sgsr3.ini_values Dx11Upscaler); no FSR 4
  static  FSR 2 built into the exe, our interceptor (profiles.static_fsr2, RE4)
  any     no input (or native Vulkan / Linux): the Vulkan layer Vela Any (anygame.py)
  none    nothing works: reason says why

Pure standard library; the reads are bounded (READ_BUDGET) and cached by (path, size, mtime) in memory and in
<settings>/gamecheck.json, so a game is read once per build.
"""
import json
import os

READ_BUDGET = 512 * 1024 * 1024   # bytes read per game at most (a UE shipping exe is 60-200 MB)
CHUNK = 8 * 1024 * 1024
OVERLAP = 64

# lower-case marker -> what it says; UTF-16LE variants are added below (LoadLibraryW names)
API_MARKERS = {
    b"d3d12.dll": "dx12", b"d3d12createdevice": "dx12",
    b"d3d11.dll": "dx11", b"d3d11createdevice": "dx11",
    b"vulkan-1.dll": "vulkan", b"vkcreateinstance": "vulkan",
    b"opengl32.dll": "opengl", b"wglcreatecontext": "opengl",
}
# symbols of temporal upscalers linked into the binary (no DLL of their own in the folder)
INPUT_MARKERS = {
    b"ffxfsr2contextcreate": "FSR 2", b"ffx_fsr2_": "FSR 2",
    b"ffxfsr3upscalercontextcreate": "FSR 3", b"ffx_fsr3upscaler": "FSR 3",
    b"nvsdk_ngx_d3d12_init": "DLSS", b"nvsdk_ngx_d3d11_init": "DLSS",
    b"xessd3d12createcontext": "XeSS",
}
# files next to the exe that name an API by themselves (Agility SDK, DXVK / vkd3d shipped by the game)
API_FILES = {"d3d12core.dll": "dx12", "d3d12sdklayers.dll": "dx12"}
API_DIRS = {"d3d12": "dx12"}
ENGINE_DLLS = ("unityplayer.dll",)


def _utf16(m):
    return b"".join(bytes([c, 0]) for c in m)


def _markers():
    out = {}
    for table in (API_MARKERS, INPUT_MARKERS):
        for m, v in table.items():
            out[m] = v
            if m.endswith(b".dll"):
                out[_utf16(m)] = v
    return out


MARKERS = _markers()
LONGEST = max(len(m) for m in MARKERS)


def read_markers(path, budget=READ_BUDGET):
    """Which markers the file has: a set of values ("dx12", "FSR 2", ...). Bounded: budget bytes at most."""
    found = set()
    tail = b""
    try:
        with open(path, "rb") as f:
            while budget > 0:
                block = f.read(min(CHUNK, budget))
                if not block:
                    break
                budget -= len(block)
                low = (tail + block).lower()
                for m, v in MARKERS.items():
                    if v not in found and m in low:
                        found.add(v)
                tail = low[-max(OVERLAP, LONGEST):]
    except OSError:
        pass
    return found


class GameCheck:
    def __init__(self, settings_dir, log=print, reader=read_markers):
        self.path = os.path.join(settings_dir, "gamecheck.json")
        self.log = log
        self.reader = reader
        self.cache = None

    def _load(self):
        if self.cache is None:
            try:
                with open(self.path, encoding="utf-8") as f:
                    self.cache = json.load(f)
            except (OSError, ValueError):
                self.cache = {}
        return self.cache

    def _save(self):
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.cache, f, indent=1)
            os.replace(tmp, self.path)
        except OSError as e:
            self.log(f"gamecheck: {e}")

    def binaries(self, scan):
        """The files whose markers count: the game's exe and an engine player DLL next to it."""
        d, exe = scan.get("exe_dir"), scan.get("exe")
        if not d or not exe:
            return []
        out = [os.path.join(d, exe)]
        try:
            names = os.listdir(d)
        except OSError:
            names = []
        out += [os.path.join(d, n) for n in names if n.lower() in ENGINE_DLLS]
        return out

    def markers(self, scan):
        """Union of the binaries' markers plus API files / folders next to the exe; cached per (path, size, mtime)."""
        cache = self._load()
        found, dirty = set(), False
        for path in self.binaries(scan):
            try:
                st = os.stat(path)
            except OSError:
                continue
            key = f"{st.st_size}:{int(st.st_mtime)}"
            hit = cache.get(path)
            if not hit or hit.get("key") != key:
                hit = {"key": key, "found": sorted(self.reader(path))}
                cache[path] = hit
                dirty = True
            found |= set(hit["found"])
        d = scan.get("exe_dir")
        if d:
            try:
                for n in os.listdir(d):
                    low = n.lower()
                    if low in API_FILES:
                        found.add(API_FILES[low])
                    elif low in API_DIRS and os.path.isdir(os.path.join(d, n)):
                        found.add(API_DIRS[low])
            except OSError:
                pass
        if dirty:
            self._save()
        return found

    def check(self, scan, static=False, s3_assets=True, ag_assets=True, appid=None):
        return verdict(scan, self.markers(scan) if scan.get("exe_dir") else set(), static, s3_assets, ag_assets, appid)


API_ORDER = ("dx12", "dx11", "vulkan", "opengl")
API_LABEL = {"dx12": "DirectX 12", "dx11": "DirectX 11", "vulkan": "Vulkan", "opengl": "OpenGL"}


def is_shortcut(appid):
    """A non-Steam shortcut in Steam's library (Heroic, Lutris ...): its appid has the top bit set, so it is at least 2^31
    (negative when read as a signed number)."""
    try:
        n = int(appid)
    except (TypeError, ValueError):
        return False
    return n >= 2 ** 31 or n < 0


def verdict(scan, markers, static=False, s3_assets=True, ag_assets=True, appid=None):
    """{"kind", "reason", "api": [...], "inputs": [...], "inputs_from": "files" | "exe" | None}; pure (tested)."""
    api = [a for a in API_ORDER if a in markers]
    inputs = list(scan.get("inputs") or [])
    inputs_from = "files" if inputs else None
    if not inputs:
        inputs = sorted({m for m in markers if m not in API_ORDER})
        inputs_from = "exe" if inputs else None
    out = {"api": api, "inputs": inputs, "inputs_from": inputs_from, "reason": None}
    if scan.get("anticheat"):
        return {**out, "kind": "none", "reason": "anticheat"}
    if not scan.get("install_dir"):
        return {**out, "kind": "none", "reason": "non_steam" if is_shortcut(appid) else "not_installed"}
    if static and s3_assets:
        return {**out, "kind": "static"}
    if api == ["opengl"]:
        return {**out, "kind": "none", "reason": "opengl"}
    if scan.get("engine") == "Godot":
        # 0.8.16: Godot's FSR 2.2 is engine code on Vulkan (no DLL, no OptiScaler pattern): "Not supported", not FSR 4
        return {**out, "kind": "none", "reason": "godot"}
    windows = bool(scan.get("exe_dir"))
    d3d = "dx12" in api or "dx11" in api
    if windows and s3_assets and inputs and (d3d or not api):
        # no API marker read (a packed exe): DX12 is assumed, the first run's log tells the truth
        return {**out, "kind": "fsr4" if ("dx12" in api or not api) else "sgsr2"}
    reason = "no_exe" if not windows else "vulkan_native" if api and not d3d else "no_inputs"
    if ag_assets:
        return {**out, "kind": "any", "reason": reason}
    return {**out, "kind": "none", "reason": reason}  # 0.8.13: no any-game layer (main._check passes ag_assets=False)
