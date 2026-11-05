"""0.8.19: anti-cheat games Vela stays out of. Steam's appinfo.vdf (a real v29 slice and a v28 file built here), the game
folder, the panel's verdict and "Off" for a game set up before its anti-cheat was known.
Run: python -I tests/test_blockers.py"""
import os
import shutil
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import gamecheck  # noqa: E402
import sgsr3  # noqa: E402

tmp = tempfile.mkdtemp(prefix="vela_dnv_")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


# ---- a real appinfo.vdf (v29, armadaOS console, 2026-11): seven apps byte for byte with the whole key string table
real = os.path.join(tmp, "real")
write(os.path.join(real, "appcache", "appinfo.vdf"), open(os.path.join(HERE, "fixtures", "appinfo_v29_slice.vdf"), "rb").read())
AC = lambda a: sgsr3.appinfo_anticheat("/nowhere", a, roots=[real])  # noqa: E731
assert AC("671860") == {"EasyAntiCheat"}  # BattleBit Remastered: easy.ac support URL, EasyAntiCheat_EOS_Setup.exe entry
assert AC("1172470") == {"EA AntiCheat", "EasyAntiCheat", "anti-cheat"}, AC("1172470")  # Apex: EAAntiCheat launcher, Deck review
assert AC("1938090") == {"anti-cheat"}  # Call of Duty: Valve's review "UnsupportedAntiCheat_Other"
assert AC("1874880") == set()  # an empty anti_cheat_support_url says nothing
for clean in ("2050650", "1178830", "1941540", "99"):  # RE4, Bright Memory: Infinite, Mafia: The Old Country, absent
    assert AC(clean) == set(), (clean, AC(clean))
# the launch-entry reader on the same real file: Bright Memory: Infinite's DX11 / DX12 entries
assert sgsr3.appinfo_api_args("/nowhere", "1178830", roots=[real]) == {"-dx11", "-dx12"}
assert sgsr3.appinfo_api_args("/nowhere", "2050650", roots=[real]) == set()

# ---- v28 (keys inline), built here: each signal alone, and nothing from an unrelated value
def kv(node):
    out = b""
    for k, v in node.items():
        if isinstance(v, dict):
            out += b"\x00" + k.encode() + b"\x00" + kv(v) + b"\x08"
        elif isinstance(v, int):
            out += b"\x02" + k.encode() + b"\x00" + v.to_bytes(4, "little")
        else:
            out += b"\x01" + k.encode() + b"\x00" + v.encode() + b"\x00"
    return out


def v28(apps):
    body = b""
    for appid, node in apps.items():
        data = b"\x00" * sgsr3.APPINFO_HEADER + kv({"appinfo": node}) + b"\x08"
        body += int(appid).to_bytes(4, "little") + len(data).to_bytes(4, "little") + data
    return (0x07564428).to_bytes(4, "little") + b"\x01\x00\x00\x00" + body + b"\x00\x00\x00\x00"


syn = os.path.join(tmp, "syn")
write(os.path.join(syn, "appcache", "appinfo.vdf"), v28({
    "11": {"extended": {"anti_cheat_support_url": "https://support.example.com/anticheat"}},
    "12": {"common": {"steam_deck_compatibility": {"category": 1, "tests": {"0": {
        "display": 1, "token": "#SteamDeckVerified_TestResult_UnsupportedAntiCheatConfiguration"}}}}},
    "13": {"config": {"launch": {"0": {"executable": "Game.exe"}, "1": {"executable": "BattlEye\\BEService_x64.exe"}}}},
    "14": {"common": {"name": "Anti-Cheat Simulator", "steam_deck_compatibility": {"tests": {"0": {
        "token": "#SteamDeckVerified_TestResult_DefaultConfigurationIsPerformant"}}}},
           "extended": {"anti_cheat_support_url": "", "description": "no BattlEye here"},
           "config": {"launch": {"0": {"executable": "Game.exe", "arguments": "-dx12"}}}},
}))
S = lambda a: sgsr3.appinfo_anticheat("/nowhere", a, roots=[syn])  # noqa: E731
assert S("11") == {"anti-cheat"} and S("12") == {"anti-cheat"} and S("13") == {"BattlEye"}, (S("11"), S("12"), S("13"))
assert S("14") == set(), S("14")
assert sgsr3.appinfo_api_args("/nowhere", "14", roots=[syn]) == {"-dx12"}
# not readable / broken: nothing, no error
broken = os.path.join(tmp, "broken")
write(os.path.join(broken, "appcache", "appinfo.vdf"), open(os.path.join(syn, "appcache", "appinfo.vdf"), "rb").read()[:120])
assert sgsr3.appinfo_anticheat("/nowhere", "13", roots=[broken]) == set()
assert sgsr3.appinfo_anticheat("/nowhere", "13", roots=[os.path.join(tmp, "none")]) == set()

# ---- anticheat_labels(): folder + Steam
assert sgsr3.anticheat_labels(["EasyAntiCheat"], {"anti-cheat"}) == ["EasyAntiCheat"]  # a named one replaces the generic
assert sgsr3.anticheat_labels([], {"anti-cheat"}) == ["anti-cheat"]
assert sgsr3.anticheat_labels(["BattlEye"], {"EA AntiCheat"}) == ["BattlEye", "EA AntiCheat"]
assert sgsr3.anticheat_labels([], set()) == []

# ---- verdict: the panel's reason
base = {"install_dir": "/g", "exe_dir": "/g/bin", "exe": "g.exe", "inputs": ["DLSS"]}
assert gamecheck.verdict({**base, "anticheat": ["anti-cheat"]}, {"dx12"})["reason"] == "anticheat"
assert gamecheck.verdict({**base, "anticheat": ["EA AntiCheat"]}, {"dx12"})["kind"] == "none"
assert gamecheck.verdict({**base, "anticheat": []}, {"dx12"})["kind"] == "fsr4"

# ---- the game folder: EA AntiCheat's files
ea = os.path.join(tmp, "ea")
write(os.path.join(ea, "Game.exe"), b"g" * (2 << 20))
write(os.path.join(ea, "EAAntiCheat.Installer.exe"), b"e")
assert sgsr3.scan_dir(ea)["anticheat"] == ["EA AntiCheat"] and sgsr3.scan_dir(ea)["exe"] == "Game.exe"

# ---- the panel on a fake library (fake decky module): a game only Steam's appinfo flags
decky = types.ModuleType("decky")
decky.logger = types.SimpleNamespace(info=lambda *a: None, error=lambda *a: None, exception=lambda *a: None)
decky.DECKY_USER_HOME = os.path.join(tmp, "home")
decky.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "psettings")
sys.modules["decky"] = decky
sys.path.insert(0, ROOT)
import main  # noqa: E402
import anygame  # noqa: E402
import gamecfg  # noqa: E402
import gameutil  # noqa: E402
import latency  # noqa: E402
import live  # noqa: E402
import npu  # noqa: E402
import upscale  # noqa: E402

live.TMP_DIR = os.path.join(tmp, "vela-live")
gameutil.running_appids = lambda proc="/proc": set()
home = decky.DECKY_USER_HOME
steam = os.path.join(home, ".local/share/Steam")
write(os.path.join(steam, "steamapps/libraryfolders.vdf"), '"libraryfolders"\n{\n "0"\n {\n  "path"  "%s"\n }\n}\n' % steam)

BB = "671860"  # BattleBit Remastered
write(os.path.join(steam, f"steamapps/appmanifest_{BB}.acf"),
      '"AppState"\n{\n "appid" "%s"\n "name" "BattleBit Remastered"\n "installdir" "BattleBit"\n}\n' % BB)
# a DX12 game with a DLSS input and no anti-cheat files in its folder: only Steam's appinfo can say no
write(os.path.join(steam, "steamapps/common/BattleBit/Game.exe"), b"x" * 2_000_000 + b"d3d12.dll")
write(os.path.join(steam, "steamapps/common/BattleBit/nvngx_dlss.dll"), b"d")
assets = os.path.join(tmp, "assets")
shutil.copytree(os.path.join(ROOT, "assets", "sgsr3"), os.path.join(assets, "sgsr3"),
                ignore=shutil.ignore_patterns("*.dll", "*.asi"))
for f in ("OptiScaler.dll", "OptiPatcher.asi", "amd_fidelityfx_upscaler_vk.dll", "amd_fidelityfx_dx12.dll"):
    write(os.path.join(assets, "sgsr3", f), b"fake " + f.encode())
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
P.npu = npu.Npu(os.path.join(assets, "npu"), home, settings, log=lambda *a: None, proc_net_unix=os.path.join(tmp, "nounix"),
                systemctl=lambda: None)
P.floor = Nop()
P.cpu = Nop()
P.check = gamecheck.GameCheck(settings, log=lambda *a: None)
P._win_restart = set()


# before Steam's appinfo is there (an older Vela did not read it): the game is set up
assert P._game_state(BB)["check"]["kind"] == "fsr4"
exe_dir = P.s3.scan(BB, fresh=True)["exe_dir"]
before = sorted(os.listdir(exe_dir))
on = P._apply_model(BB, "fsr3", "MY_VAR=1 %command% -windowed", None)
assert on["model"] == "fsr3" and P.s3.read_state(exe_dir) is not None and "dxgi=n,b" in on["launch_options"], on

# the real appinfo.vdf slice (BattleBit's entry is in it): the panel says why, offers no model, Off removes everything
shutil.copytree(real, steam, dirs_exist_ok=True)
P.s3.cache.clear()
assert P.s3.scan(BB, fresh=True)["anticheat"] == ["EasyAntiCheat"], P.s3.scan(BB)["anticheat"]
gs = P._game_state(BB)
assert gs["check"]["kind"] == "none" and gs["check"]["reason"] == "anticheat" and gs["models"] == [], gs["check"]
assert P.s3.status(BB)["installed"] and not P.s3.status(BB)["available"]  # still in the folder until Off
assert main.suggest(P.s3.scan(BB), P.s3.status(BB), {"available": True}) is None
for m in ("fsr4", "auto"):
    try:
        P._apply_model(BB, m, on["launch_options"], None)
    except RuntimeError as e:
        assert "anticheat" in str(e), e
    else:
        raise AssertionError(f"{m} was set up")
off = P._apply_model(BB, "off", on["launch_options"], None)
assert off["model"] == "off" and off["launch_options"] == "MY_VAR=1 %command% -windowed", off
assert P.s3.read_state(exe_dir) is None and sorted(os.listdir(exe_dir)) == before, sorted(os.listdir(exe_dir))
try:
    P.s3.enable(BB, {}, "", None)
except RuntimeError as e:
    assert str(e).startswith("anti-cheat in the game (EasyAntiCheat)"), e
else:
    raise AssertionError("sgsr3.enable installed")
assert P.s3.read_state(exe_dir) is None and P._game_state(BB)["check"]["reason"] == "anticheat"
print("test_blockers: ok")
