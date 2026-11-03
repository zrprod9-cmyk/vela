"""0.8.16: a Godot game is "Not supported" with its own reason instead of a false FSR 4 offer, and the
version Decky shows is 0.8.16. The Godot fixture is the console's "Sir, We Have an Orc Problem" (4594150) folder: swhaop.exe with
its pack embedded (the file ends with the pack size + "GDPC"), libgodotsteam.windows.template_release.x86_64.dll, steam_api64.dll,
the Linux build next to it; the exe has d3d12 + FSR 2 symbols), which made the old verdict "fsr4".
Run: python -I tests/test_zt.py"""
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import gamecheck  # noqa: E402
import sgsr3  # noqa: E402

tmp = tempfile.mkdtemp(prefix="vela_zt_")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


EXE = b"MZ" + b"\0" * (2 * 1024 * 1024) + b"d3d12.dll ffxFsr2ContextCreate"
PACK = b"GDPC" + b"\0" * 64
# 1) the console's layout: the pack embedded in the exe + GodotSteam
orc = os.path.join(tmp, "Sir, We Have an Orc Problem")
write(os.path.join(orc, "swhaop.exe"), EXE + PACK + struct.pack("<Q", len(PACK)) + b"GDPC")
for n in ("libgodotsteam.windows.template_release.x86_64.dll", "steam_api64.dll", "swhaop.x86_64",
          "libgodotsteam.linux.template_release.x86_64.so", "libsteam_api.so"):
    write(os.path.join(orc, n), b"x")
sc = sgsr3.scan_dir(orc, "4594150")
assert sc["exe"] == "swhaop.exe" and sc["engine"] == "Godot", sc
assert sgsr3.godot_signs(orc, "swhaop.exe") == ["godot_lib", "embedded_pck"], sgsr3.godot_signs(orc, "swhaop.exe")
v = gamecheck.verdict({**sc, "install_dir": orc}, {"dx12", "FSR 2"}, ag_assets=False)
assert (v["kind"], v["reason"]) == ("none", "godot"), v
# 2) a plain export: game.exe + game.pck, nothing else
plain = os.path.join(tmp, "Plain")
write(os.path.join(plain, "game.exe"), EXE)
write(os.path.join(plain, "game.pck"), PACK)
assert sgsr3.godot_signs(plain, "game.exe") == ["pck"]
assert sgsr3.scan_dir(plain)["engine"] == "Godot"
# 3) only the embedded pack (no library, no .pck)
emb = os.path.join(tmp, "Embedded")
write(os.path.join(emb, "game.exe"), EXE + PACK + struct.pack("<Q", len(PACK)) + b"GDPC")
assert sgsr3.godot_signs(emb, "game.exe") == ["embedded_pck"] and sgsr3.scan_dir(emb)["engine"] == "Godot"
# 4) not Godot: a big exe with FSR 2 in it stays fsr4 (Unreal / Unity keep their engine, a .pck is never looked at there)
other = os.path.join(tmp, "Other")
write(os.path.join(other, "game.exe"), EXE)
sc = sgsr3.scan_dir(other)
assert sc["engine"] == "" and sgsr3.godot_signs(other, "game.exe") == []
v = gamecheck.verdict({**sc, "install_dir": other}, {"dx12", "FSR 2"}, ag_assets=False)
assert (v["kind"], v["reason"]) == ("fsr4", None), v
ue = os.path.join(tmp, "UE", "Game", "Binaries", "Win64")
write(os.path.join(ue, "Game-Win64-Shipping.exe"), EXE + b"GDPC")
write(os.path.join(ue, "x.pck"), b"x")
assert sgsr3.scan_dir(os.path.join(tmp, "UE"))["engine"] == "Unreal"
# anti-cheat still wins over Godot (the reason the panel shows)
v = gamecheck.verdict({**sgsr3.scan_dir(orc), "install_dir": orc, "anticheat": ["EasyAntiCheat"]}, {"dx12"}, ag_assets=False)
assert v["reason"] == "anticheat", v

# ---- the panel's text for the reason (strings.ts REASON) and the version Decky shows
en_json = open(os.path.join(ROOT, "src", "i18n", "en.json"), encoding="utf-8").read()  # EN: English texts only
assert '"reason.godot": "a Godot game: its FSR 2 is built into the engine' in en_json
strings = open(os.path.join(ROOT, "src", "strings.ts"), encoding="utf-8").read()
pkg = json.load(open(os.path.join(ROOT, "package.json"), encoding="utf-8"))
assert pkg["version"] == "0.8.16", pkg["version"]
assert 'export const VERSION = "0.8.16";' in strings
dist = os.path.join(ROOT, "dist", "index.js")
if os.path.isfile(dist):  # built: the bundle carries the Godot text
    assert "a Godot game: its FSR 2" in open(dist, encoding="utf-8").read()
# ---- gamecfg.ini_set on Ghostrunner's real mixed line endings (console file, 
import gamecfg  # noqa: E402
S = "/Script/Ghostrunner.GhostrunnerGameUserSettings"
gus = ("[ScalabilityGroups]\r\nsg.ShadingQuality=2\r\n\r\n[" + S + "]\r\nFidelityFSREnabled=False\r\nFidelityFSRMode=3\r\n"
       "DLSSEnabled=False\nCurrentUpscalingQuality=2\nDLSSMode=0\r\n\r\n[/Script/Engine.GameUserSettings]\r\nbUseVSync=False\r\n")
assert gamecfg.ini_get(gus, S, "DLSSEnabled") == "False"
out, ch = gamecfg.ini_set(gus, S, "DLSSEnabled", "True")
assert ch and gamecfg.ini_get(out, S, "DLSSEnabled") == "True", repr(out)
assert out == gus.replace("DLSSEnabled=False\n", "DLSSEnabled=True\n"), repr(out)  # every other byte and ending kept
out, ch = gamecfg.ini_set(out, S, "DLSSMode", 5)
assert ch and out.count("DLSSMode=5\r\n") == 1 and out.count("\r\n") == gus.count("\r\n")
assert gamecfg.ini_set(gus, S, "Missing", 1) == (gus, False) and gamecfg.ini_set(gus, "Nope", "DLSSMode", 1) == (gus, False)
assert gamecfg.ini_set("[A]\r\nk=1\r\n", "A", "k", 2) == ("[A]\r\nk=2\r\n", True)
# the console's own file, as the game left it (tests/fixtures, kept byte for byte: mixed line endings)
with open(os.path.join(HERE, "fixtures", "ghostrunner_user_settings_mixed_eol.ini"), encoding="utf-8", newline="") as fh:
    t = fh.read()
assert "\r\n" in t and t.count("\n") > t.count("\r\n")  # really mixed
out, ch = gamecfg.ini_set(t, S, "DLSSEnabled", "True")
assert ch and gamecfg.ini_get(out, S, "DLSSEnabled") == "True" and len(out) == len(t) - 1  # "False" -> "True"
# ---- Deep Rock Galactic: UpscalingType 0 -> 3 (FSR 2, console run drg3), only when off
import profiles  # noqa: E402
c = profiles.game_cfg("548430")
assert c["keys"] == {"UpscalingType": 3} and c["when"] == {"UpscalingType": [0]} and c["format"] == "ini", c
NL = chr(13) + chr(10)
drg = NL.join(["[/Script/FSD.FSDGameUserSettings]", "AntiAliasingType=1", "UpscalingType=0", "AmdFsrMode=1", ""])
out, ch = gamecfg.ini_set(drg, c["section"], "UpscalingType", 3)
assert ch and gamecfg.ini_get(out, c["section"], "UpscalingType") == "3" and out.count(NL) == 4
print("test_zt: ok")
