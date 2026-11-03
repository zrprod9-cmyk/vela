"""Checks for latency.py (UE4 Engine.ini), anygame.py (the layer for any game), gameutil.py (running games, Unity's
user.reg values) and main.suggest on a fake Steam library in a temp folder. Run: python -I tests/test_modes.py"""
import hashlib
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import anygame  # noqa: E402
import gameutil  # noqa: E402
import latency  # noqa: E402
import sgsr3  # noqa: E402


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def md5(path):
    return hashlib.md5(open(path, "rb").read()).hexdigest()


tmp = tempfile.mkdtemp()
home = os.path.join(tmp, "home")
steam = os.path.join(home, ".local/share/Steam")
write(os.path.join(steam, "steamapps/libraryfolders.vdf"), '"libraryfolders"\n{\n "0"\n {\n  "path"  "%s"\n }\n}\n' % steam)


def game(appid, installdir, files):
    write(os.path.join(steam, f"steamapps/appmanifest_{appid}.acf"),
          '"AppState"\n{\n "appid" "%s"\n "name" "%s"\n "installdir" "%s"\n}\n' % (appid, installdir, installdir))
    root = os.path.join(steam, "steamapps/common", installdir)
    for rel, data in files.items():
        write(os.path.join(root, rel), data)
    return root


settings = os.path.join(tmp, "settings")
s3 = sgsr3.Sgsr3(os.path.join(tmp, "noassets"), home, settings, log=lambda *a: None)

# ---- running games: Steam's reaper "SteamLaunch AppId=N"
proc = os.path.join(tmp, "proc")
write(os.path.join(proc, "100/cmdline"), b"/home/u/.steam/reaper\0SteamLaunch\0AppId=548430\0--\0/bin/game")
write(os.path.join(proc, "101/cmdline"), b"/usr/bin/python3\0x.py")
write(os.path.join(proc, "102/environ"), b"A=1\0WINEPREFIX=/lib/steamapps/compatdata/548430/pfx/\0B=2")
write(os.path.join(proc, "self/cmdline"), b"SteamLaunch AppId=1")
assert gameutil.running_appids(proc) == {"548430"}, gameutil.running_appids(proc)
assert gameutil.prefix_busy("/lib/steamapps/compatdata/548430/pfx", proc)
assert not gameutil.prefix_busy("/lib/steamapps/compatdata/1/pfx", proc)

# ---- UE4: Engine.ini in the prefix
drg = game("548430", "Deep Rock Galactic", {"FSD/Binaries/Win64/FSD-Win64-Shipping.exe": b"x" * 2000,
                                            "FSD.exe": b"stub"})
pfx = os.path.join(steam, "steamapps/compatdata/548430/pfx")
cfg = os.path.join(pfx, "drive_c/users/steamuser/AppData/Local/FSD/Saved/Config/WindowsNoEditor")
sc = s3.scan("548430")
assert sc["engine"] == "Unreal" and latency.project_of(sc["exe"]) == "FSD"
lat = latency.Latency(home, settings, log=lambda *a: None)
st = lat.status("548430", sc)
assert st["unreal"] and st["needs_run"] and not st["applied"], st
# the game never ran: the wish is kept, nothing is written
st = lat.set("548430", True, "off", sc)
assert st["on"] and st["needs_run"] and not os.path.exists(cfg)
lat.set("548430", False, "off", sc)
assert not os.listdir(os.path.join(settings, "latency")), "nothing kept when off and never applied"

# it ran once: GameUserSettings.ini exists, Engine.ini not yet -> created, removed again by "off"
write(os.path.join(cfg, "GameUserSettings.ini"), "[/Script/Engine.GameUserSettings]\r\nbUseVSync=False\r\n")
st = lat.set("548430", True, "30", sc)
eng = os.path.join(cfg, "Engine.ini")
assert st["applied"] and st["fps"] == "30" and os.path.isfile(eng), st
assert latency.has_line(open(eng).read())
lat.set("548430", False, "off", sc)
assert not os.path.exists(eng) and not os.listdir(os.path.join(settings, "latency"))

# an existing Engine.ini (CRLF, other sections): back byte for byte
orig = "[Core.System]\r\nPaths=../../../Engine/Content\r\n\r\n[/Script/Engine.Engine]\r\nbSmoothFrameRate=False\r\n"
write(eng, orig)
m0 = md5(eng)
lat.set("548430", True, "off", sc)
text = open(eng, newline="").read()
assert "[SystemSettings]\r\nr.OneFrameThreadLag=0" in text and text.startswith(orig.rstrip("\r\n")), repr(text)
lat.set("548430", False, "off", sc)
assert md5(eng) == m0

# the game changed the file while on (UE4 rewrites it on exit): only our line and our header go
lat.set("548430", True, "off", sc)
write(eng, open(eng, newline="").read().replace("bSmoothFrameRate=False", "bSmoothFrameRate=True"))
lat.set("548430", False, "off", sc)
text = open(eng, newline="").read()
assert "OneFrameThreadLag" not in text and "SystemSettings" not in text and "bSmoothFrameRate=True" in text, repr(text)

# a [SystemSettings] the game already had stays, only the line goes
write(eng, "[SystemSettings]\r\nr.Foo=1\r\n")
lat.set("548430", True, "off", sc)
write(eng, open(eng, newline="").read() + "[X]\r\ny=1\r\n")
lat.set("548430", False, "off", sc)
assert open(eng, newline="").read() == "[SystemSettings]\r\nr.Foo=1\r\n[X]\r\ny=1\r\n", repr(open(eng, newline="").read())

# the line was there already (user's own tweak): not ours to remove
write(eng, "[SystemSettings]\r\nr.OneFrameThreadLag=0\r\n")
lat.set("548430", True, "off", sc)
lat.set("548430", False, "off", sc)
assert latency.has_line(open(eng).read())

# while the game runs: off now, the game writes our line back when it quits, reconcile removes it again
write(eng, orig)
lat.set("548430", True, "off", sc)
st = lat.set("548430", False, "off", sc, running=True)
assert st["pending"] and md5(eng) == m0
assert lat.has_pending()
write(eng, orig + "[SystemSettings]\r\nr.OneFrameThreadLag=0\r\n")   # UE4 on exit
lat.reconcile({"548430"})
assert latency.has_line(open(eng).read()), "not while it runs"
lat.reconcile(set())
assert not latency.has_line(open(eng).read()) and not os.listdir(os.path.join(settings, "latency"))
assert not lat.has_pending()
# on while it runs, UE4 drops the line on exit: written again
lat.set("548430", True, "off", sc, running=True)
write(eng, orig)
lat.reconcile(set())
assert latency.has_line(open(eng).read())
lat.set("548430", False, "off", sc)
assert md5(eng) == m0, "back to the original after all that"
try:
    lat.set("548430", True, "25", sc)
    raise AssertionError("unknown fps accepted")
except ValueError:
    pass

# not Unreal: the wish is kept for OptiScaler's part, no file
uni = game("553420", "TUNIC", {"Tunic.exe": b"t" * (2 << 20), "UnityPlayer.dll": b"u"})
scu = s3.scan("553420")
st = lat.set("553420", True, "40", scu)
assert not st["unreal"] and not st["needs_run"] and st["on"] and lat.wanted("553420") == {"on": True, "fps": "40"}
lat.set("553420", False, "off", scu)

# ---- Unity's screen values in user.reg
reg = ('WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n'
       '[Software\\\\Andrew Shouldice\\\\Secret Legend] 1729700000\n#time=1db2\n'
       '"Screenmanager Fullscreen mode_h3630240806"=dword:00000001\n'
       '"Screenmanager Resolution Height_h2627697771"=dword:00000438\n'
       '"Screenmanager Resolution Width_h182942802"=dword:00000780\n"unity.cloud_userid_h2665564582"=hex:61,62\n\n'
       '[Software\\\\Wine] 1729700001\n"Version"="win10"\n')
saved = gameutil.unity_screen_values(reg)
assert len(saved) == 3, saved
played = reg.replace("00000438", "000002d0").replace("00000780", "00000500").replace(
    '"Screenmanager Fullscreen mode_h3630240806"=dword:00000001\n', "") \
    .replace('"unity.cloud_userid', '"Screenmanager Resolution Use Native_h1405027254"=dword:00000000\n"unity.cloud_userid')
assert gameutil.unity_restore(played, saved) == reg, gameutil.unity_restore(played, saved)
assert gameutil.unity_restore(reg, saved) == reg
assert gameutil.unity_restore(played.replace("\n", "\r\n"), saved) == reg.replace("\n", "\r\n")

# ---- the layer for any game
lay = os.path.join(tmp, "layer")
ag = anygame.AnyGame(lay, home, settings, log=lambda *a: None)
assert not ag.status("553420", scu)["available"], "no layer files"
for f in ("libVkLayer_UPSCALE_any.so", "VkLayer_UPSCALE_any.json", "sany/x1.5/lut_layer.spv", "sany/x1.5/lut_f16.bin",
          "sany/x2/lut_layer.spv", "sany/x2/lut_f16.bin"):
    write(os.path.join(lay, f), b"x")
assert ag.status("553420", scu)["available"]
write(os.path.join(steam, "steamapps/compatdata/553420/pfx/user.reg"), reg)
regp = os.path.join(steam, "steamapps/compatdata/553420/pfx/user.reg")
launch0 = "/usr/libexec/armada/armada-game-launch %command%"
r = ag.enable("553420", {}, launch0, "Default", scu)
assert r["resolution"] == "960x540", "\u0411\u0430\u043b\u0430\u043d\u0441 by default: 540p"
ag.disable("553420", r["launch_options"], scu)
r = ag.enable("553420", {"render": "1280x720", "mode": "quality"}, launch0, "Default", scu)
lo = r["launch_options"]
assert lo.endswith(" " + launch0) and r["resolution"] == "1280x720", r
assert f"VK_ADD_IMPLICIT_LAYER_PATH={lay}" in lo and "UPSCALE_ANY=1" in lo and "TUP_OUTPUT=1920x1080" in lo
assert os.path.join(lay, "sany", "x1.5", "lut_f16.bin") in lo
assert r["status"]["installed"] and r["status"]["unity"]
assert open(os.path.join(settings, "anygame", "filter-553420")).read().strip() == "shader"
# change the render size: tokens replaced, not added
r2 = ag.enable("553420", {"render": "960x540"}, lo, None, scu)
assert r2["launch_options"].count("UPSCALE_ANY=1") == 1 and "x2" + os.sep + "lut_f16.bin" in r2["launch_options"]
assert r2["resolution"] == "960x540"
assert ag.read("553420")["resolution_prev"] == "Default" and ag.read("553420")["launch_prev"] == launch0
r3 = ag.enable("553420", {"filter": "linear"}, r2["launch_options"], None, scu)
assert "TUP_UPSCALE_LUT" not in r3["launch_options"]
try:
    ag.enable("553420", {"render": "1600x900"}, r3["launch_options"], None, scu)
    raise AssertionError("unknown render accepted")
except ValueError:
    pass
# the game ran with the layer: log, Unity remembered 720p
write(os.path.join(settings, "anygame", "layer-553420.log"),
      "tup: instance created (app Tunic)\ntup: upscale: S-any pass writes the swapchain image directly\n"
      "tup: upscale: game swapchain 1280x720 -> real 1920x1080\n"
      "tup: upscale: 300 presents, S-any LUT direct 1280x720 -> 1920x1080, avg GPU 0.842 ms (10 samples)\n")
lg = ag.status("553420", scu)["log"]
assert lg["render"] == "1280x720" and lg["path"] == "S-any LUT direct" and abs(lg["gpu_ms"] - 0.842) < 1e-9, lg
write(regp, played)
# off while it runs: launch options and override back at once, user.reg after it quits
off = ag.disable("553420", r3["launch_options"], scu, running=True)
assert off["launch_options"] == launch0 and off["resolution"] == "Default", off
assert open(regp).read() == played and off["status"]["pending_restore"]
assert ag.has_pending()
ag.reconcile({"553420"})
assert open(regp).read() == played
ag.reconcile(set())
assert open(regp).read() == reg and not os.path.exists(os.path.join(settings, "anygame", "553420.json"))
# user edited the launch options meanwhile: only our tokens go
r = ag.enable("553420", {"render": "game"}, "", "1280x720", scu)
assert r["resolution"] is None and r["launch_options"].endswith("%command%")
off = ag.disable("553420", r["launch_options"] + " -nosplash", scu)
# 0.8.17: the options had no %command% before (""): the one the layer put in goes with its tokens (same command line)
assert off["launch_options"] == "-nosplash" and off["resolution"] == "1280x720", off
assert open(regp).read() == reg
# compare: linear for a moment
r = ag.enable("553420", {}, launch0, "Default", scu)
assert ag.compare("553420", 0.05)
assert open(os.path.join(settings, "anygame", "filter-553420")).read().strip() == "linear"
import time
time.sleep(0.3)
assert open(os.path.join(settings, "anygame", "filter-553420")).read().strip() == "shader"
ag.disable("553420", r["launch_options"], scu)
assert not ag.status("553420", scu)["installed"]

# anti-cheat: no layer either
game("222", "Online", {"Online.exe": b"o" * (2 << 20), "EasyAntiCheat/EasyAntiCheat_EOS_Setup.exe": b"e"})
try:
    ag.enable("222", {}, "", None, s3.scan("222"))
    raise AssertionError("anti-cheat game accepted")
except RuntimeError:
    pass

# ---- which branch the plugin offers
sys.path.insert(0, os.path.join(HERE, ".."))
import types  # noqa: E402
sys.modules["decky"] = types.ModuleType("decky")
import main  # noqa: E402
s3st = {"available": True}
assert main.suggest({"install_dir": "x", "inputs": ["DLSS"], "anticheat": []}, s3st, {"available": True}) == "ingame"
assert main.suggest({"install_dir": "x", "inputs": [], "anticheat": []}, s3st, {"available": True}) == "anygame"
assert main.suggest({"install_dir": "x", "inputs": [], "anticheat": ["BattlEye"]}, s3st, {"available": True}) is None
print("ok")
