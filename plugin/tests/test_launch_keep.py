"""0.8.15: a Resident Evil 4 session on Vela 0.8.14.
1. A model without the NPU (FSR 3, SGSR 2) took VELA_NPU=1 VELA_NPU_LOG_FILE=... out of the Steam launch options; a game
   started then had no NPU layer, and FSR 4 + NPU in the running game said "fallback nolayer" until a restart. Now a game Vela
   is set up in keeps the tokens whatever the model (the live key alone switches the NPU); games set up before are migrated
   once when the panel shows them (launch_fix).
2. The launch options carried VELA_FSR2_ALGO=ai (Vela AI left in 0.8.13): gone from new options and from old ones; a stale
   model=ai in /tmp/vela-live does not reach the interceptor ("model 'ai' is not offered").
Fake decky module, fake Steam library with Resident Evil 4 in a temp folder. Run: python -I tests/test_launch_keep.py"""
import json
import os
import shutil
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import anygame  # noqa: E402
import gamecfg  # noqa: E402
import gamecheck  # noqa: E402
import gameutil  # noqa: E402
import latency  # noqa: E402
import live  # noqa: E402
import npu  # noqa: E402
import presets  # noqa: E402
import sgsr3  # noqa: E402
import upscale  # noqa: E402

tmp = tempfile.mkdtemp(prefix="vela_lk_")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


decky = types.ModuleType("decky")
decky.logger = types.SimpleNamespace(info=lambda *a: None, error=lambda *a: None, exception=lambda *a: None)
decky.DECKY_USER_HOME = os.path.join(tmp, "home")
decky.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "psettings")
sys.modules["decky"] = decky
sys.path.insert(0, ROOT)
import main  # noqa: E402

live.TMP_DIR = os.path.join(tmp, "vela-live")
gameutil.running_appids = lambda proc="/proc": set()
home = decky.DECKY_USER_HOME
settings = decky.DECKY_PLUGIN_SETTINGS_DIR
steam = os.path.join(home, ".local/share/Steam")
write(os.path.join(steam, "steamapps/libraryfolders.vdf"), '"libraryfolders"\n{\n "0"\n {\n  "path"  "%s"\n }\n}\n' % steam)
A = "2050650"
write(os.path.join(steam, f"steamapps/appmanifest_{A}.acf"),
      '"AppState"\n{\n "appid" "%s"\n "name" "RE4"\n "installdir" "RE4"\n}\n' % A)
groot = os.path.join(steam, "steamapps/common/RE4")
write(os.path.join(groot, "re4.exe"), b"r" * (3 << 20))
assets = os.path.join(tmp, "assets")
shutil.copytree(os.path.join(ROOT, "assets", "sgsr3"), os.path.join(assets, "sgsr3"),
                ignore=shutil.ignore_patterns("*.dll", "*.asi"))
for f in ("OptiScaler.dll", "OptiPatcher.asi", "amd_fidelityfx_upscaler_vk.dll", "amd_fidelityfx_dx12.dll"):
    write(os.path.join(assets, "sgsr3", f), b"fake " + f.encode())
write(os.path.join(assets, "staticfsr", "dxgi.dll"), b"vela interceptor")
write(os.path.join(assets, "npu", "libVkLayer_VELA_npu.so"), b"so")
shutil.copy(os.path.join(ROOT, "assets", "npu", "VkLayer_VELA_npu.json"), os.path.join(assets, "npu"))
main.ASSETS = assets
unix = os.path.join(tmp, "unix")
write(unix, "Num RefCount Protocol Flags Type St Inode Path\n" +
      "".join(f"0: 00000002 0 10000 0001 01 1{i} @{n}\n" for i, n in enumerate(["fsr4npud", "fsr4npud_nf4_k1h"])))


class Nop:
    def __getattr__(self, name):
        return lambda *a, **k: {"on": False}


def plugin(service=True):
    P = main.Plugin()
    P.up = upscale.Upscaler(settings, home, log=lambda *a: None)
    P.up.refresh = lambda: None
    P.s3 = sgsr3.Sgsr3(os.path.join(assets, "sgsr3"), home, settings, log=lambda *a: None)
    P.ag = anygame.AnyGame(os.path.join(assets, "layer"), home, settings, log=lambda *a: None)
    P.lat = latency.Latency(home, settings, log=lambda *a: None)
    P.cfg = gamecfg.GameCfg(settings, log=lambda *a: None)
    P.live = live.Live(settings, log=lambda *a: None)
    P.npu = npu.Npu(os.path.join(assets, "npu"), home, settings, log=lambda *a: None,
                    proc_net_unix=unix if service else os.path.join(tmp, "nounix"), systemctl=lambda: None)
    P.floor = Nop()
    P.cpu = Nop()
    P.check = gamecheck.GameCheck(settings, log=lambda *a: None)
    P._win_restart = set()
    return P


P = plugin()
LO0 = "/usr/libexec/armada/armada-game-launch /var/home/armada/.local/share/optiscaler/optiscaler-launch.sh %command%"
TOK = "VELA_NPU=1 VELA_NPU_LOG_FILE=" + P.npu.layer_log(A)

# ---- 1. a fresh setup: FSR 3 first (no NPU model) -> the tokens are in all the same, and no VELA_FSR2_ALGO
r = P._apply_model(A, "fsr3", LO0, "Default")
lo = r["launch_options"]
assert lo == TOK + ' WINEDLLOVERRIDES="dxgi=n,b" VELA_FSR2=replace RECON_LEVEL=1 VELA_RCAS=0 ' + LO0, lo
assert "ALGO" not in lo
assert P.live.get(A)["model"] == "fsr3" and "npu" not in P.live.get(A)
assert "npu=" not in open(os.path.join(live.TMP_DIR, A + ".ini")).read()
assert P._game_state(A)["launch_fix"] is False  # nothing left to migrate

# ... then FSR 4 + NPU in the running game: the options do not change (no restart needed for the layer), the live key goes on
for model in ("fsr4", "sgsr2", "fsr3", "fsr4"):
    r = P._apply_model(A, model, lo, None)
    assert r["launch_options"] == lo, (model, r["launch_options"])
    lv = P.live.get(A)
    assert lv["model"] == model and (lv.get("npu") == 2) == (model in presets.FSR4_MODELS), (model, lv)
# 0.8.15: no NPU switch in the panel (FSR 4 always on the NPU when it is ready)
assert not hasattr(P, "_npu_set")
# off: Steam's options exactly as before Vela
off = P._apply_model(A, "off", lo, None)
assert off["launch_options"] == LO0, off
assert not P.npu.games_on() and not os.path.exists(P.npu._installed_path())  # the layer manifest goes with the last game

# ---- 2. a game set up by 0.8.14: FSR 3 picked (tokens out), VELA_FSR2_ALGO=ai in Steam's options and in both states
LEGACY = 'WINEDLLOVERRIDES="dxgi=n,b" VELA_FSR2=replace VELA_FSR2_ALGO=ai RECON_LEVEL=1 VELA_RCAS=0 ' + LO0
P = plugin()
r = P._apply_model(A, "fsr3", LO0, "Default")
P.s3.rewrite_launch_set(A, lambda t: t.replace("VELA_FSR2=replace ", "VELA_FSR2=replace VELA_FSR2_ALGO=ai "))
P.npu.disable(A, r["launch_options"])  # 0.8.14 took the tokens out for FSR 3 / SGSR 2
assert not (P.npu.read(A) or {}).get("on") and "VELA_FSR2_ALGO=ai" in P.s3.launch_set(A)
gs = P._game_state(A)
assert gs["launch_fix"] is True and gs["model"] == "fsr3", gs["launch_fix"]
f = P._launch_fix(A, LEGACY)["launch_options"]
assert f == TOK + ' WINEDLLOVERRIDES="dxgi=n,b" VELA_FSR2=replace RECON_LEVEL=1 VELA_RCAS=0 ' + LO0, f
assert P._game_state(A)["launch_fix"] is False
assert P._launch_fix(A, f)["launch_options"] == f  # once is enough
assert P._launch_fix(A, None)["launch_options"] is None  # Steam's options not read: nothing is rewritten
assert "ALGO" not in P.s3.launch_set(A) and "ALGO" not in json.dumps(P.npu.read(A))
assert P._apply_model(A, "off", f, None)["launch_options"] == LO0  # the undo is still exact after the migration
assert P._game_state(A)["launch_fix"] is False

# a game set up by 0.8.14 with FSR 4 (tokens in, VELA_FSR2_ALGO=ai in them): the algo token goes, the rest stays
P = plugin()
r = P._apply_model(A, "fsr4", LO0, "Default")
P.s3.rewrite_launch_set(A, lambda t: t.replace("VELA_FSR2=replace ", "VELA_FSR2=replace VELA_FSR2_ALGO=ai "))
P.npu.rewrite_state(A, lambda t: t.replace("VELA_FSR2=replace ", "VELA_FSR2=replace VELA_FSR2_ALGO=ai "))
old = r["launch_options"].replace("VELA_FSR2=replace ", "VELA_FSR2=replace VELA_FSR2_ALGO=ai ")
assert P._game_state(A)["launch_fix"] is True
f = P._launch_fix(A, old)["launch_options"]
assert f == r["launch_options"] and "ALGO" not in f, f
assert P._game_state(A)["launch_fix"] is False
assert P._apply_model(A, "off", f, None)["launch_options"] == LO0

# the user's own edit after Vela stays: foreign tokens are kept, ours are not doubled
P = plugin()
r = P._apply_model(A, "fsr3", LO0, "Default")
f = P._launch_fix(A, r["launch_options"] + " -windowed")["launch_options"]
assert f.count("VELA_NPU=1") == 1 and f.endswith("-windowed") and "ALGO" not in f, f

# a console without the NPU system component: no tokens (the layer would load for nothing), nothing to migrate
P = plugin(service=False)
r = P._apply_model(A, "fsr3", LO0, "Default")
assert "VELA_NPU" not in r["launch_options"] and not P._game_state(A)["launch_fix"], r
r = P._apply_model(A, "off", r["launch_options"], None)
assert r["launch_options"] == LO0

# a game that is not set up is never touched
P = plugin()
assert P._launch_fix(A, LO0)["launch_options"] == LO0 and P._game_state(A)["launch_fix"] is False

# ---- 3. the interceptor's live file: no model it does not offer after a plugin start
d = live.TMP_DIR
write(os.path.join(d, "111.ini"), "model=ai\nnpu=2\n")        # 0.8.12, no record of ours
write(os.path.join(d, "222.ini"), "model=sr\n")                # only the old model: the file goes
write(os.path.join(d, "333.ini"), "model=fsr4\nnpu=2\n")  # a good one stays as it is
write(os.path.join(d, "444.ini"), "model=fsr4lite\nlossy=4\nnpu=2\n")  # 0.8.15: Lite is gone: fsr4, no lossy, the rest stays
lv = live.Live(settings, log=lambda *a: None)
lv.restore_all()
assert open(os.path.join(d, "111.ini")).read() == "npu=2\n"
assert not os.path.exists(os.path.join(d, "222.ini"))
assert open(os.path.join(d, "333.ini")).read() == "model=fsr4\nnpu=2\n"
assert open(os.path.join(d, "444.ini")).read() == "model=fsr4\nnpu=2\n"
# a recorded game with an old model is migrated by Live.get, as before
os.makedirs(lv.dir, exist_ok=True)
write(os.path.join(lv.dir, "444.json"), json.dumps({"model": "ai", "npu": 2}))
lv.restore_all()
assert "model=ai" not in open(os.path.join(d, "444.ini")).read()
assert "model=sgsr2" in open(os.path.join(d, "444.ini")).read()

print("test_launch_keep: ok")
