"""0.8.15: presets.py (plan, live_wanted, hint), the new-game check
(gamecheck.py) and, on a fake Steam library in a temp folder (fake decky module), Plugin._apply_model / _game_state: FSR 4 always on
the NPU exact, no "window pick" (Steam's window back to the full screen), no render ratio override, the silent migration of old keys,
the kit repair (every file, atomically, OptiScaler.ini at LogLevel 2), the -dx12 rule for games with their own Steam launch
entries (Bright Memory: Infinite), "Off" while the game runs, and the per-run game logs. Files are moved, never deleted.
Run: python -I tests/test_presets.py"""
import json
import os
import re
import shutil
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import gamecheck  # noqa: E402
import presets  # noqa: E402
import upscale  # noqa: E402

# ---- presets
assert presets.MODELS == ("fsr4", "fsr4fast", "fsr3", "sgsr2")  # 0.8.16: + FSR 4 Fast
assert presets.legacy_model(True) == "fsr4" and presets.legacy_model(False) == "sgsr2"
assert presets.models_for("fsr4") == [("fsr4", True), ("fsr4fast", True), ("fsr3", True), ("sgsr2", True)]
assert presets.models_for("static", fsr4=True, fsr3=False)[2] == ("fsr3", False)
assert presets.models_for("sgsr2") == [("sgsr2", True)]  # DX11: OptiScaler's Dx11Upscaler is SGSR 2
assert presets.models_for("none") == [] and presets.models_for("any") == []
NPU_ON = {"layer_found": True, "service": {"installed": True, "k1h_running": True, "k1h360_running": True}}
NPU_NONE = {"layer_found": True, "service": {"installed": False}}
assert presets.npu_ready(NPU_ON) and not presets.npu_ready(NPU_NONE) and not presets.npu_ready(None)
assert presets.live_wanted("fsr4", NPU_ON) == {"model": "fsr4", "npu": 2, "npuexact": 1}  # always on, always exact
assert presets.live_wanted("fsr4", NPU_NONE) == {"model": "fsr4", "npu": None, "npuexact": None}
assert presets.live_wanted("sgsr2", NPU_ON) == {"model": "sgsr2", "npu": None, "npuexact": None}
assert presets.plan("fsr4", {"model": "fsr4", "npu": 2}) == {"kind": "fsr4", "model": "fsr4", "npu": True}
assert presets.plan("static", {"model": "fsr4lite"})["model"] == "fsr4"
assert presets.plan("static", {"model": "ai"})["model"] == "fsr4" and presets.plan("fsr4", {"model": "sr"})["model"] == "sgsr2"
assert presets.plan("sgsr2", {"model": "fsr4"})["model"] == "sgsr2"
assert presets.is_ultra("640x360", "1920x1080") and not presets.is_ultra("1280x720", "1920x1080") and not presets.is_ultra(None, "x")
f = presets.plan("fsr4", {"model": "fsr4", "npu": 2})
assert presets.hint(f, {"npu": NPU_NONE}) == {"id": "npu_install"}
assert presets.hint(f, {"npu": {"layer_found": False}}) == {"id": "npu_missing_layer"}
# FSR 4 picked, the game runs, no frame through its upscaler yet (its menu): nothing to do, the card says "waiting for the first frame"
for log in (None, {}, {"sgsr3": False}, {"sgsr3": True, "mode": "fsr4", "output": "1920x1080", "npu": None}):
    assert presets.hint(f, {"running": True, "npu": NPU_ON, "log": log}) is None, log
assert presets.hint(f, {"running": True, "npu": NPU_ON, "log": {"sgsr3": True, "output": "1280x720"}})["id"] == "game_res"
assert presets.hint(f, {"running": True, "npu": NPU_ON, "window_restart": True,
                        "log": {"sgsr3": True, "output": "1280x720"}}) == {"id": "restart_window"}
assert presets.hint(presets.plan("static", {"model": "fsr4"}), {"dx12_choice": True, "npu": NPU_ON}) is None  # RE4: no entries

tmp = tempfile.mkdtemp(prefix="vela_pl3_")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


# ---- gamecheck: markers in the binary, ASCII and UTF-16, across a chunk border
exe = os.path.join(tmp, "m", "Game.exe")
gamecheck.CHUNK, old_chunk = 4096, gamecheck.CHUNK
write(exe, b"\0" * 4090 + b"D3D12.dll" + b"\0" * 5000 + "VULKAN-1.DLL".encode("utf-16-le") + b"ffxFsr2ContextCreate")
assert gamecheck.read_markers(exe) == {"dx12", "vulkan", "FSR 2"}, gamecheck.read_markers(exe)
assert gamecheck.read_markers(exe, budget=4096) == set()  # bounded
gamecheck.CHUNK = old_chunk

base = {"install_dir": "/g", "exe_dir": "/g/bin", "exe": "g.exe", "inputs": ["DLSS"], "anticheat": []}
V = gamecheck.verdict
assert V(base, {"dx12", "dx11"})["kind"] == "fsr4"
assert V(base, {"dx11"})["kind"] == "sgsr2"
assert V(base, set())["kind"] == "fsr4"  # nothing read: DX12 assumed, the first run tells
assert V(base, {"opengl"}) == {**V(base, {"opengl"}), "kind": "none", "reason": "opengl"}
assert V({**base, "anticheat": ["EasyAntiCheat"]}, {"dx12"})["reason"] == "anticheat"
assert V({**base, "install_dir": None}, set())["reason"] == "not_installed"
assert V(base, {"vulkan"})["kind"] == "any" and V(base, {"vulkan"})["reason"] == "vulkan_native"
v = V({**base, "inputs": []}, {"dx12"})
assert (v["kind"], v["reason"]) == ("any", "no_inputs"), v
v = V({**base, "inputs": []}, {"dx12", "FSR 2"})  # FSR linked into the exe: OptiScaler's pattern search
assert (v["kind"], v["inputs"], v["inputs_from"]) == ("fsr4", ["FSR 2"], "exe"), v
assert V({**base, "exe_dir": None, "exe": None, "inputs": []}, set())["reason"] == "no_exe"  # native Linux -> layer
assert V({**base, "exe_dir": None, "exe": None, "inputs": []}, set(), ag_assets=False)["kind"] == "none"
assert V(base, {"vulkan"}, ag_assets=False)["reason"] == "vulkan_native"  # 0.8.13: the reason without the layer
assert V(base, {"dx11"}, static=True)["kind"] == "static"

calls = []
gc = gamecheck.GameCheck(os.path.join(tmp, "settings"), log=lambda *a: None,
                         reader=lambda p: calls.append(p) or {"dx12"})
scan = {"install_dir": os.path.dirname(exe), "exe_dir": os.path.dirname(exe), "exe": "Game.exe", "inputs": ["XeSS"],
        "anticheat": []}
write(os.path.join(tmp, "m", "UnityPlayer.dll"), b"x")
os.makedirs(os.path.join(tmp, "m", "D3D12"), exist_ok=True)
assert gc.check(scan)["kind"] == "fsr4" and len(calls) == 2  # exe + UnityPlayer.dll
gc2 = gamecheck.GameCheck(os.path.join(tmp, "settings"), log=lambda *a: None, reader=lambda p: calls.append(p) or set())
assert gc2.markers(scan) == {"dx12"} and len(calls) == 2  # from gamecheck.json + the D3D12 folder, no second read

# ---- the one click on a fake library: Plugin._apply_model with a fake decky module
decky = types.ModuleType("decky")
decky.logger = types.SimpleNamespace(info=lambda *a: None, error=lambda *a: None, exception=lambda *a: None)
decky.DECKY_USER_HOME = os.path.join(tmp, "home")
decky.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "psettings")
sys.modules["decky"] = decky
sys.path.insert(0, ROOT)
import main  # noqa: E402
import gameutil  # noqa: E402
import gamecfg  # noqa: E402
import live  # noqa: E402
import npu  # noqa: E402
import anygame  # noqa: E402
import latency  # noqa: E402
import sgsr3  # noqa: E402

live.TMP_DIR = os.path.join(tmp, "vela-live")
gameutil.running_appids = lambda proc="/proc": set()
home = decky.DECKY_USER_HOME
steam = os.path.join(home, ".local/share/Steam")
write(os.path.join(steam, "steamapps/libraryfolders.vdf"), '"libraryfolders"\n{\n "0"\n {\n  "path"  "%s"\n }\n}\n' % steam)
write(os.path.join(steam, "steamapps/appmanifest_2239710.acf"),
      '"AppState"\n{\n "appid" "2239710"\n "name" "ITD"\n "installdir" "ITD"\n}\n')
groot = os.path.join(steam, "steamapps/common/ITD")
write(os.path.join(groot, "ITD/Binaries/Win64/ITD-Win64-Shipping.exe"), b"x" * 2_000_000 + b"d3d12.dll d3d11.dll")
write(os.path.join(groot, "Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll"), b"d")
assets = os.path.join(tmp, "assets")
shutil.copytree(os.path.join(ROOT, "assets", "sgsr3"), os.path.join(assets, "sgsr3"),
                ignore=shutil.ignore_patterns("*.dll", "*.asi"))
for f in ("OptiScaler.dll", "OptiPatcher.asi", "amd_fidelityfx_upscaler_vk.dll", "amd_fidelityfx_dx12.dll"):
    write(os.path.join(assets, "sgsr3", f), b"fake " + f.encode())
write(os.path.join(assets, "npu", "libVkLayer_VELA_npu.so"), b"so")
shutil.copy(os.path.join(ROOT, "assets", "npu", "VkLayer_VELA_npu.json"), os.path.join(assets, "npu"))
main.ASSETS = assets
settings = decky.DECKY_PLUGIN_SETTINGS_DIR


class Nop:
    def __getattr__(self, name):
        return lambda *a, **k: {"on": False}


P = main.Plugin()
P.up = upscale.Upscaler(settings, home, log=lambda *a: None)
P.s3 = sgsr3.Sgsr3(os.path.join(assets, "sgsr3"), home, settings, log=lambda *a: None)
P.ag = anygame.AnyGame(os.path.join(assets, "layer"), home, settings, log=lambda *a: None)
P.lat = latency.Latency(home, settings, log=lambda *a: None)
P.cfg = gamecfg.GameCfg(settings, log=lambda *a: None)
P.live = live.Live(settings, log=lambda *a: None)
unix = os.path.join(tmp, "unix")
write(unix, "Num RefCount Protocol Flags Type St Inode Path\n" +
      "".join(f"0: 00000002 0 10000 0001 01 1{i} @{n}\n" for i, n in enumerate(
          ["fsr4npud", "fsr4npud_nf4_k1h", "fsr4npud_nf4_k1h_360"])))
P.npu = npu.Npu(os.path.join(assets, "npu"), home, settings, log=lambda *a: None, proc_net_unix=unix, unit_dirs=(),
                systemctl=lambda: None)
P.floor = Nop()
P.cpu = Nop()
P.check = gamecheck.GameCheck(settings, log=lambda *a: None)
P._win_restart = set()
A = "2239710"
exe_dir = os.path.join(groot, "ITD/Binaries/Win64")
write(os.path.join(exe_dir, "OptiScaler.log"), "a log of the game's own OptiScaler from before Vela\n")
gs = P._game_state(A)
assert gs["check"]["kind"] == "fsr4" and gs["model"] == "off" and gs["hint"] is None and not gs["window_fix"], gs
assert [m["id"] for m in gs["models"]] == ["fsr4", "fsr4fast", "fsr3", "sgsr2"] and all(m["ok"] for m in gs["models"])
for gone in ("steps", "post", "modes", "mode", "captures"):
    assert gone not in gs, gone

# the first pick: files, launch options with the NPU layer, FSR 4 on the NPU exact, the window back to the full screen
r = P._apply_model(A, "fsr4", "", "1280x720")
assert r["model"] == "fsr4" and r["resolution"] == "Default", r  # an old "window pick" 720p goes
assert "VELA_NPU=1" in r["launch_options"] and r["launch_options"].endswith("%command% -dx12"), r  # UE, no own entries
assert P.live.get(A) == {"model": "fsr4", "npu": 2, "npuexact": 1}, P.live.get(A)
ini = open(os.path.join(exe_dir, "OptiScaler.ini"), encoding="utf-8").read()
assert sgsr3.ini_get(ini, "Log", "LogLevel") == "2" and sgsr3.ini_get(ini, "Log", "LogToFile") == "true"
assert sgsr3.ini_get(ini, "UpscaleRatio", "UpscaleRatioOverrideEnabled") == "false", "the game's preset sets the render"
gs = P._game_state(A, "Default")
assert gs["model"] == "fsr4" and gs["plan"] == {"kind": "fsr4", "model": "fsr4", "npu": True} and gs["hint"] is None
assert P._game_state(A, "1600x900")["window_fix"] and P._window_fix(A) == {"resolution": "Default"}

# FSR 3 / SGSR 2 live: the NPU key off, the layer tokens stay (no restart for FSR 4 again)
r3 = P._apply_model(A, "sgsr2", r["launch_options"], None)
assert P.live.get(A) == {"model": "sgsr2"} and r3["launch_options"] == r["launch_options"] and r3["resolution"] is None
r = P._apply_model(A, "fsr4", r3["launch_options"], None)
assert P.live.get(A)["npuexact"] == 1 and r["launch_options"] == r3["launch_options"]

# silent migration: a game saved by 0.8.14 with NPU off / fast, a ratio, the dev knob, Lite; the ini's ratio override
with open(os.path.join(P.live.dir, A + ".json"), "w") as fh:
    json.dump({"model": "fsr4lite", "lossy": 5, "ratio": "2.0", "npu": 0, "npuexact": 0, "npukick": "early"}, fh)
st = P.s3.read_state(exe_dir)
st["opts"]["ratio"] = "2.0"
P.s3._write_state(exe_dir, st)
gs = P._game_state(A, "Default")
assert gs["sgsr3"]["live"] == {"model": "fsr4", "npu": 2, "npuexact": 1}, gs["sgsr3"]["live"]
tmpini = open(os.path.join(live.TMP_DIR, A + ".ini")).read()
for k in ("lossy", "ratio", "npukick", "fsr4lite"):
    assert k not in tmpini, (k, tmpini)
assert P.s3.read_state(exe_dir)["opts"]["ratio"] == "game"
for bad in ("ai", "sr", "p2b", "quality", "manual"):
    try:
        P._apply_model(A, bad, r["launch_options"], None)
        raise AssertionError(bad + " accepted")
    except ValueError:
        pass
for gone in ("set_output", "npu_set", "live_set", "set_post", "set_profile", "gpu_floor_set", "cpu_boost_set"):
    assert not hasattr(P, gone), gone

# kit repair (Bright Memory): another plugin's uninstall took dxgi.dll and OptiScaler.ini, its ini (Debug) came back
gone = os.path.join(tmp, "_gone")
os.makedirs(gone, exist_ok=True)
os.replace(os.path.join(exe_dir, "dxgi.dll"), os.path.join(gone, "dxgi.dll"))
os.replace(os.path.join(exe_dir, "OptiScaler/plugins/OptiPatcher.asi"), os.path.join(gone, "OptiPatcher.asi"))
os.replace(os.path.join(exe_dir, "OptiScaler.ini"), os.path.join(gone, "OptiScaler.ini"))
assert P.s3.status(A)["intact"] is False
assert P.s3.refresh_dll(A)
for rel, data in (("dxgi.dll", b"fake OptiScaler.dll"), ("OptiScaler/plugins/OptiPatcher.asi", b"fake OptiPatcher.asi"),
                  ("amd_fidelityfx_upscaler_vk.dll", b"fake amd_fidelityfx_upscaler_vk.dll")):
    assert open(os.path.join(exe_dir, *rel.split("/")), "rb").read() == data, rel
assert P.s3.status(A)["intact"] is True and not P.s3.refresh_dll(A), "whole: nothing to do"
write(os.path.join(exe_dir, "OptiScaler.ini"), ini.replace("LogLevel=2", "LogLevel=1"))
assert P.s3.refresh_dll(A)
assert sgsr3.ini_get(open(os.path.join(exe_dir, "OptiScaler.ini"), encoding="utf-8").read(), "Log", "LogLevel") == "2"
assert not [n for n in os.listdir(exe_dir) if n.endswith(".upscale-tmp")], "atomic: no temp file left"

# -dx12 (Bright Memory: Infinite 1178830): the game's own Steam launch entries pass -dx11 / -dx12 (appinfo.vdf, v29)
def appinfo(entries):
    data = b"\x00appinfo\x00\x00config\x00\x00launch\x00" + b"".join(
        b"\x00" + str(i).encode() + b"\x00\x01executable\x00BMI.exe\x00\x01arguments\x00" + a.encode() + b"\x00\x08"
        for i, a in enumerate(entries)) + b"\x08\x08\x08"
    other = b"\x01arguments\x00-dx11\x00"  # another game's entry: not ours
    body = (99).to_bytes(4, "little") + len(other).to_bytes(4, "little") + other + \
        int(A).to_bytes(4, "little") + len(data).to_bytes(4, "little") + data + (0).to_bytes(4, "little")
    write(os.path.join(steam, "appcache", "appinfo.vdf"), (0x07564429).to_bytes(4, "little") + b"\x01\x00\x00\x00" +
          (0).to_bytes(8, "little") + body)


assert P.s3.api_args(A) == set() and not P.s3.dx12_fix_pending(A)
appinfo(["-dx11", "-dx12"])
os.utime(os.path.join(steam, "appcache", "appinfo.vdf"), (1, 2))
assert sgsr3.appinfo_api_args(home, A) == {"-dx11", "-dx12"} and sgsr3.appinfo_api_args(home, "99") == {"-dx11"}
gs = P._game_state(A, "Default")
assert gs["launch_fix"] and gs["hint"] == {"id": "dx12_choice"}, gs["hint"]  # not run yet: pick the DX12 entry
BMI = "WINEDLLOVERRIDES=dxgi=n,b OPTI_SGSR3_STUDENT=1 OPTI_SGSR3_LINES=2 /usr/libexec/armada/armada-game-launch %command% -dx12"
fixed = P._launch_fix(A, r["launch_options"])["launch_options"]
assert not sgsr3.has_api_flag(fixed) and "VELA_NPU=1" in fixed and fixed.endswith("%command%"), fixed
assert not P.s3.dx12_fix_pending(A) and not P._game_state(A, "Default")["launch_fix"]
# 0.8.17: the -dx12 goes only as Vela's own (dx12_ours); the player's own flag stays (tests/test_launch_user.py)
new, added = sgsr3.launch_on(BMI, "student", True, True, own_api=True)
assert not added and not sgsr3.has_api_flag(new), new
assert sgsr3.launch_on(BMI, "student", True, False, own_api=True)[0].endswith("%command% -dx12")
for entry in ("BrightMemoryInfinite.exe -dx11", "BrightMemoryInfinite.exe -dx12"):  # what Steam runs for each entry
    line = new.replace("%command%", entry)
    assert len(re.findall(r"(?<!\S)-(?:dx1[12]|d3d1[12])(?!\S)", line)) == 1, line  # one API flag: no UE fatal error
assert sgsr3.launch_on(BMI, "student", True, False)[0].endswith("-dx12"), "a game without entries keeps the user's flag"
# the hint goes once the game ran through its upscaler on the DX12 entry; SGSR 2 instead of FSR 4 = the DX11 entry
p = presets.plan("fsr4", {"model": "fsr4", "npu": 2})
ok_npu = {"layer_found": True, "service": {"installed": True, "k1h_running": True}}
assert presets.hint(p, {"dx12_choice": True, "running": True, "npu": ok_npu,
                        "log": {"sgsr3": True, "mode": "fsr4", "output": "1920x1080"}}) is None
assert presets.hint(p, {"dx12_choice": True, "running": True, "npu": ok_npu,
                        "log": {"sgsr3": True, "mode": "sgsr2", "output": "1920x1080"}}) == {"id": "dx12_choice"}

# "Off" while the game runs and writes its log: everything back, no half-undone state (restore check skips the runtime log)
gameutil.running_appids = lambda proc="/proc": {A}
write(os.path.join(exe_dir, "OptiScaler.log"), "the running game's log\n")
off = P._apply_model(A, "off", fixed, None)
assert off["model"] == "off" and P.s3.read_state(exe_dir) is None and "VELA_NPU" not in off["launch_options"], off
gameutil.running_appids = lambda proc="/proc": set()
assert P._game_state(A)["model"] == "off"
r = P._apply_model(A, "fsr4", off["launch_options"], None)
assert not sgsr3.has_api_flag(r["launch_options"]), "own entries: Vela adds no -dx12"
assert len(re.findall(r"(?<!\S)-dx1[12](?!\S)", BMI.replace("%command%", "BMI.exe -dx11"))) == 2  # the bug: both flags

# ---- 0.8.13: every run's log kept (the next launch overwrites the game folder's one), the last 5, older to _old
logs = P.s3.logs_dir
glog = os.path.join(exe_dir, "OptiScaler.log")
import time  # noqa: E402
base = time.time() + 100
for i in range(7):
    write(glog, f"run {i}\n")
    os.utime(glog, (base + i * 60, base + i * 60))
    assert P.s3.keep_runs(A, True) is None  # the game runs: its log is still being written
    k = P.s3.keep_runs(A, False)
    assert k and os.path.basename(k).startswith(A + "-") and k.endswith(".log") and open(k).read() == f"run {i}\n", k
    assert P.s3.keep_runs(A, False) is None  # the same run once
runs = sorted(n for n in os.listdir(logs) if re.match(A + r"-\d{8}-\d{6}\.log$", n))
assert len(runs) == 5 and open(os.path.join(logs, runs[-1])).read() == "run 6\n", runs
assert len(os.listdir(os.path.join(logs, "_old"))) == 2  # moved, not deleted
assert P.s3.keep_runs(A, False) is None  # the run already in the list is not copied again

# off: the folder, the launch options, the pick
off = P._apply_model(A, "off", r["launch_options"], None)

# 0.8.16: "auto" = the device's best model (FSR 4 on the NPU here), the same as a pick by hand; game_state names it
assert P._game_state(A)["auto_model"] == presets.default_model("fsr4", True, P.npu.status(A)) == "fsr4"
ra = P._apply_model(A, "auto", off["launch_options"], None)
assert ra["model"] == "fsr4" and P.live.get(A)["model"] == "fsr4" and "VELA_NPU=1" in ra["launch_options"], ra
assert P._game_state(A)["model"] == "fsr4"
off = P._apply_model(A, "off", ra["launch_options"], None)

# a game with anti-cheat: no models, the reason
write(os.path.join(groot, "EasyAntiCheat/EasyAntiCheat_x64.dll"), b"e")
P.s3.cache.clear()
gs = P._game_state(A)
assert gs["check"]["kind"] == "none" and gs["check"]["reason"] == "anticheat" and gs["models"] == []
print("test_presets: ok")
