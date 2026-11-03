"""Checks for py_modules/sgsr3.py on a fake Steam library in a temp folder. Run: python -I tests/test_sgsr3.py"""
import hashlib
import json
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import sgsr3
import profiles  # noqa: E402
import upscale  # noqa: E402


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def tree(root):
    """{relative path: md5} of every file under root."""
    out = {}
    for cur, _dirs, files in os.walk(root):
        for f in files:
            p = os.path.join(cur, f)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = hashlib.md5(open(p, "rb").read()).hexdigest()
    return out


tmp = tempfile.mkdtemp()
home = os.path.join(tmp, "home")
steam = os.path.join(home, ".local/share/Steam")
sd = os.path.join(tmp, "sdcard")
write(os.path.join(steam, "steamapps/libraryfolders.vdf"),
      '"libraryfolders"\n{\n "0"\n {\n  "path"  "%s"\n }\n "1"\n {\n  "path"  "%s"\n }\n}\n' % (steam, sd))


def game(lib, appid, installdir, files):
    write(os.path.join(lib, f"steamapps/appmanifest_{appid}.acf"),
          '"AppState"\n{\n "appid" "%s"\n "name" "%s"\n "installdir" "%s"\n}\n' % (appid, installdir, installdir))
    root = os.path.join(lib, "steamapps/common", installdir)
    for rel, data in files.items():
        write(os.path.join(root, rel), data)
    return root


# UE4 game on the SD card with a manual OptiScaler install already in place (like Bright Memory on the console)
bmi = game(sd, "1178830", "Bright Memory Infinite", {
    "BrightMemoryInfinite.exe": b"stub" * 100,
    "BrightMemoryInfinite/Binaries/Win64/BrightMemoryInfinite-Win64-Shipping.exe": b"x" * 2000,
    "BrightMemoryInfinite/Binaries/Win64/dxgi.dll": b"manual sgsr3 build",
    "BrightMemoryInfinite/Binaries/Win64/dxgi.dll.phase0": b"phase 0 build",
    "BrightMemoryInfinite/Binaries/Win64/OptiScaler.ini": "[Upscalers]\nDx12Upscaler=sgsr3\n",
    "BrightMemoryInfinite/Binaries/Win64/OptiScaler/plugins/OptiPatcher.asi": b"old patcher",
    "BrightMemoryInfinite/Binaries/Win64/OptiScaler.log": "old log\n",
    "Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll": b"dlss",
    "Engine/Plugins/Runtime/Intel/XeSS/Binaries/ThirdParty/Win64/libxess.dll": b"xess",
    "Engine/Binaries/Win64/CrashReportClient.exe": b"c" * 3000,
})
win64 = os.path.join(bmi, "BrightMemoryInfinite/Binaries/Win64")
# Unity game, clean folder
uni = game(steam, "111", "Unity Game", {"Game.exe": b"g" * (2 << 20), "UnityPlayer.dll": b"u", "Game_Data/x": b"",
                                        "Game_Data/Plugins/x86_64/libxess.dll": b"x"})
# online games: anti-cheat folder, and a known appid without it
game(steam, "222", "Online", {"Online.exe": b"o" * (2 << 20), "EasyAntiCheat/EasyAntiCheat_EOS_Setup.exe": b"e"})
game(steam, "393380", "Squad", {"Squad/Binaries/Win64/SquadGame-Win64-Shipping.exe": b"s"})

assets = os.path.join(tmp, "assets")
write(os.path.join(assets, "OptiScaler.dll"), b"our sgsr3 build")
write(os.path.join(assets, "OptiPatcher.asi"), b"new patcher")
write(os.path.join(assets, "amd_fidelityfx_dx12.dll"), b"ffx sdk 1.1.4")
template = open(os.path.join(HERE, "..", "assets", "sgsr3", "OptiScaler.ini"), encoding="utf-8").read()
write(os.path.join(assets, "OptiScaler.ini"), template)
write(os.path.join(assets, "VERSION.json"), json.dumps({"dll": "sgsr3-v12", "md5": hashlib.md5(b"our sgsr3 build").hexdigest()}))
settings = os.path.join(tmp, "settings")
s3 = sgsr3.Sgsr3(assets, home, settings, log=lambda *a: None)

# ---- scan
sc = s3.scan("1178830")
assert sc["engine"] == "Unreal" and os.path.samefile(sc["exe_dir"], win64), sc
assert sorted(sc["inputs"]) == ["DLSS", "XeSS"] and not sc["anticheat"], sc
sc = s3.scan("111")
assert sc["engine"] == "Unity" and os.path.samefile(sc["exe_dir"], uni) and sc["inputs"] == ["XeSS"], sc
assert s3.scan("222")["anticheat"] == ["EasyAntiCheat"], s3.scan("222")
assert s3.scan("393380")["anticheat"] == ["Squad"]
assert not s3.status("222")["available"] and not s3.status("393380")["available"]
assert s3.scan("999")["install_dir"] is None
for appid in ("222", "393380", "999"):
    try:
        s3.enable(appid, {}, "%command%")
        raise AssertionError(f"enabled in {appid}")
    except RuntimeError:
        pass

# ---- launch options
on, dx = sgsr3.launch_on("/usr/libexec/armada/armada-game-launch %command%", "v1f", True)
assert on == 'WINEDLLOVERRIDES="dxgi=n,b" /usr/libexec/armada/armada-game-launch %command% -dx12' and dx, on
on2, dx2 = sgsr3.launch_on(on, "student", True, dx)
assert on2 == 'WINEDLLOVERRIDES="dxgi=n,b" OPTI_SGSR3_STUDENT=1 /usr/libexec/armada/armada-game-launch %command% -dx12' and dx2, on2
assert sgsr3.launch_on("", "v1f", False) == ('WINEDLLOVERRIDES="dxgi=n,b" %command%', False)
assert sgsr3.launch_on("", "v1", False)[0] == 'WINEDLLOVERRIDES="dxgi=n,b" OPTI_SGSR3_V1CS=1 %command%'
on3, _ = sgsr3.launch_on(on2, "v1f", True, True, fg=True)
assert on3 == 'WINEDLLOVERRIDES="dxgi=n,b" OPTI_SGSRFG=1 /usr/libexec/armada/armada-game-launch %command% -dx12', on3
# the key of 0.4.11-0.4.15 (TU autotune, not supported on A740) goes away with ours
assert sgsr3.strip_ours("TU_AUTOTUNE_FLAGS=preempt_optimize x %command%") == "x %command%"
assert "TU_AUTOTUNE" not in sgsr3.strip_ours(on3, True)
assert sgsr3.strip_ours(on3, True) == "/usr/libexec/armada/armada-game-launch %command%"
assert sgsr3.strip_ours("XOPTI_SGSRFG=1 %command%") == "XOPTI_SGSRFG=1 %command%", "only whole tokens"
assert sgsr3.launch_on("-windowed", "v1f", False)[0] == 'WINEDLLOVERRIDES="dxgi=n,b" %command% -windowed'
assert sgsr3.launch_on("x %command% -dx12", "v1f", True) == ('WINEDLLOVERRIDES="dxgi=n,b" x %command% -dx12', False)
assert sgsr3.launch_on("WINEDLLOVERRIDES=dxgi=n,b x %command%", "v1f", False)[0] == "WINEDLLOVERRIDES=dxgi=n,b x %command%"
assert sgsr3.launch_off(on, "orig %command%", on, True) == "orig %command%"
assert sgsr3.launch_off(on + " -foo", "orig %command%", on, True) == "/usr/libexec/armada/armada-game-launch %command% -foo"
# 0.8.17: what Vela wrote decides what is Vela's (here its override, its -dx12 and the %command% "" had not)
assert sgsr3.launch_off('WINEDLLOVERRIDES="dxgi=n,b" %command%', "", 'WINEDLLOVERRIDES="dxgi=n,b" %command% -dx12', True) == ""
assert sgsr3.launch_off('WINEDLLOVERRIDES="dxgi=n,b" %command%', "", "x", False) == 'WINEDLLOVERRIDES="dxgi=n,b" %command%'
assert sgsr3.launch_off("x %command% -dx12", "x %command% -dx12", "y", False) == "x %command% -dx12"

# ---- ini
ini = sgsr3.make_ini(template, {"ratio": "2.25"})
assert sgsr3.ini_get(ini, "Upscalers", "Dx12Upscaler") == "sgsr3"
assert sgsr3.ini_get(ini, "Spoofing", "Vulkan") == "false" and sgsr3.ini_get(ini, "Spoofing", "Dxgi") == "true"
assert sgsr3.ini_get(ini, "Plugins", "LoadAsiPlugins") == "true"
# 0.8.15: never an UpscaleRatioOverride (the game's preset sets the render), always LogLevel 2 (Info)
assert sgsr3.ini_get(ini, "UpscaleRatio", "UpscaleRatioOverrideEnabled") == "false"
assert sgsr3.ini_get(ini, "UpscaleRatio", "UpscaleRatioOverrideValue") == "auto"
assert sgsr3.ini_get(ini, "Log", "LogLevel") == "2" and sgsr3.ini_get(ini, "Log", "LogToFile") == "true"
assert sgsr3.ini_get(sgsr3.make_ini(template, {"ratio": "game"}), "UpscaleRatio", "UpscaleRatioOverrideEnabled") == "false"
assert len(ini.splitlines()) == len(template.splitlines()), "keys replaced in place, nothing appended"
assert sgsr3.ini_set("[A]\r\nx=1\r\n[B]\r\n", "B", "y", "2") == "[A]\r\nx=1\r\n[B]\r\ny=2\r\n"
assert sgsr3.ini_set("[A]\nx=1\n", "C", "y", "2") == "[A]\nx=1\n\n[C]\ny=2\n"

# the menu is hidden; FG, low latency and the limit off by default
assert sgsr3.ini_get(ini, "Menu", "ShortcutKey") == "-1" and sgsr3.ini_get(ini, "Menu", "DisableSplash") == "true"
assert sgsr3.ini_get(ini, "FrameGen", "Enabled") == "false" and sgsr3.ini_get(ini, "fakenvapi", "ForceReflex") == "auto"
assert sgsr3.ini_get(ini, "Framerate", "FramerateLimit") == "auto"
ini2 = sgsr3.make_ini(template, {"fg": True, "latency": True, "fps": "30"})
assert sgsr3.ini_get(ini2, "FrameGen", "Enabled") == "true" and sgsr3.ini_get(ini2, "FrameGen", "FGInput") == "upscaler"
assert sgsr3.ini_get(ini2, "FrameGen", "FGOutput") == "fsrfg" and sgsr3.ini_get(ini2, "OptiFG", "HUDFix") == "true"
assert sgsr3.ini_get(ini2, "fakenvapi", "ForceReflex") == "2" and sgsr3.ini_get(ini2, "fakenvapi", "ForceLatencyFlex") == "true"
assert sgsr3.ini_get(ini2, "Framerate", "FramerateLimit") == "30"
assert len(ini2.splitlines()) == len(template.splitlines()), "every key exists in the template"
# every input of the game is replaced; FSR pattern search only without an FSR DLL in the folder
for k in ("EnableDlssInputs", "EnableXeSSInputs", "EnableFsr2Inputs", "UseFsr2Inputs", "EnableFsr3Inputs", "UseFsr3Inputs",
          "EnableFfxInputs", "UseFfxInputs"):
    assert sgsr3.ini_get(ini2, "Inputs", k) == "true", k
assert sgsr3.ini_get(sgsr3.make_ini(template, {"fsr_pattern": True}), "Inputs", "Fsr2Pattern") == "true"
assert sgsr3.ini_get(ini2, "Inputs", "Fsr3Pattern") == "auto"
assert sgsr3.fsr_pattern({"inputs": ["DLSS"]}) and not sgsr3.fsr_pattern({"inputs": ["FSR 2", "DLSS"]})
assert sgsr3.menu_hint({"inputs": ["FSR 2", "DLSS"]}) == "choose FSR 2 or DLSS"
assert sgsr3.menu_hint({"inputs": ["DLSS", "XeSS"]}) == "choose DLSS or XeSS"

# ---- enable over a manual install, reconfigure, disable: the folder is exactly as before
before = tree(bmi)
launch0 = "/usr/libexec/armada/armada-game-launch %command%"
r = s3.enable("1178830", {}, launch0, "1280x720")
# Bright Memory's profile: the line detector at threshold 2 (OPTI_SGSR3_LINES=2), Vela SR (v1f) in every mode
assert r["launch_options"] == 'WINEDLLOVERRIDES="dxgi=n,b" OPTI_SGSR3_LINES=2 ' + launch0 + " -dx12", r["launch_options"]
assert r["status"]["profile"]["lines"] == 2 and s3.status("111")["profile"] is None
assert sgsr3.strip_ours(r["launch_options"], True) == launch0
# smoothing: "like DLSS" by default (the BMI profile picks "sharp"), the value goes to OPTI_SGSR3_SOFT
assert sgsr3.smooth_env("dlss") == ["OPTI_SGSR3_FRESH=0.5"] and sgsr3.smooth_env("sharp") == []
assert sgsr3.strip_ours("OPTI_SGSR3_FRESH=0.5 x %command%") == "x %command%"
assert sgsr3.strip_ours("OPTI_SGSR3_SOFT=0.75 x %command%") == "x %command%"
# report 31 switches (profiles' "env") go away with ours; Control's profile: no HUD locks
assert sgsr3.strip_ours("OPTI_SGSR3_SQ=1 OPTI_SGSR3_FRESH=0.5 OPTI_SGSR3_DISOCC=1 x %command%") == "x %command%"
assert profiles.env_tokens("870780") == ["OPTI_SGSR3_NOLOCK=1", "OPTI_SGSR3_SQ=1"]
assert profiles.game_cfg("870780")["file"] == "renderer.ini" and profiles.game_cfg("870780")["keys"]["m_eAllowHDR"] == 1
assert profiles.game_cfg("1") is None  # 0.8.16: Control's output / render keys, tests/test_auto.py
# 0.4.1: every fork key goes with ours, also A/B keys set by hand
assert sgsr3.strip_ours('OPTI_SGSR3_NOHP=1 OPTI_SGSR3_V1CS=1 OPTI_SGSR_X=2 A=1 %command%') == "A=1 %command%"
# Manor Lords: its vkd3d keys come and go with ours
# 0.4.11: one Vela AI for all games -- no per-game model keys (alphaK / mk) in any profile
assert profiles.env_tokens("1363080") == ["VKD3D_SHADER_MODEL=6_6", "VKD3D_FEATURE_LEVEL=12_2"]
assert not any(k.startswith(("OPTI_SGSR3_ALPHAK", "OPTI_SGSR3_MK", "OPTI_SGSR3_AMAX", "VELA_AMAX"))
               for a in profiles.PROFILES for k in profiles.env_tokens(a) + (profiles.static_fsr2(a) or {}).get("env", []))
assert sgsr3.strip_ours("VKD3D_SHADER_MODEL=6_6 VKD3D_FEATURE_LEVEL=12_2 x %command%") == "x %command%"
profiles.PROFILES["998"] = {"name": "t", "env": ["OPTI_SGSR3_SQ=1"]}
assert profiles.env_tokens("998") == ["OPTI_SGSR3_SQ=1"]
del profiles.PROFILES["998"]
st = r["status"]
assert st["installed"] and st["intact"] and st["opts"] == {**sgsr3.DEFAULT_OPTS, "smooth": "sharp", "algo": "v1f"}, st["opts"]
assert st["opts"]["algo"] == "v1f" and st["opts"]["ratio"] == "2.0" and not st["opts"]["fg"]
assert st["modes"]["quality"] == {"algo": "v1f", "ratio": "1.5"}, st["modes"]
# modes elsewhere: Vela AI (the student) at 720p / 540p / 480p
assert s3.status("111")["opts"]["algo"] == "student" and s3.status("111")["modes"]["speed"] == {"algo": "student", "ratio": "2.25"}
assert sgsr3.resolve_mode({**sgsr3.DEFAULT_OPTS, "mode": "speed"})[("ratio")] == "2.25"
assert sgsr3.resolve_mode({**sgsr3.DEFAULT_OPTS, "mode": "quality", "model": "v1f", "res": "3.0"}) == \
    {**sgsr3.DEFAULT_OPTS, "mode": "quality", "model": "v1f", "res": "3.0", "algo": "v1f", "ratio": "3.0"}
assert sgsr3.resolve_mode({**sgsr3.DEFAULT_OPTS, "mode": "custom", "algo": "v1", "ratio": "game"})["algo"] == "v1"
# a heavy game's profile: "Speed" with Vela SR
profiles.PROFILES["999"] = {"name": "heavy", "speed": ("v1f", "3.0")}
assert sgsr3.resolve_mode({**sgsr3.DEFAULT_OPTS, "mode": "speed"}, "999")[("algo")] == "v1f"
assert sgsr3.resolve_mode({**sgsr3.DEFAULT_OPTS, "mode": "balance"}, "999")[("algo")] == "student"
del profiles.PROFILES["999"]
# 0.3.x state (no mode): custom, its algorithm stays
assert sgsr3.Sgsr3.migrate({"algo": "v1", "ratio": "game"})["mode"] == "custom" and sgsr3.Sgsr3.migrate(None) == {}
assert not os.path.exists(os.path.join(win64, "amd_fidelityfx_dx12.dll")), "no FG file without FG"
assert open(os.path.join(win64, "dxgi.dll"), "rb").read() == b"our sgsr3 build"
assert open(os.path.join(win64, "OptiScaler/plugins/OptiPatcher.asi"), "rb").read() == b"new patcher"
assert sgsr3.ini_get(open(os.path.join(win64, "OptiScaler.ini")).read(), "Upscalers", "Dx12Upscaler") == "sgsr3"
assert sgsr3.ini_get(open(os.path.join(win64, "OptiScaler.ini")).read(), "Inputs", "Fsr2Pattern") == "true", "BMI ships no FSR DLL"
assert st["menu_hint"] == "choose DLSS or XeSS", st["menu_hint"]
assert open(os.path.join(win64, ".upscale-sgsr3/backup/dxgi.dll"), "rb").read() == b"manual sgsr3 build"
assert open(os.path.join(win64, "dxgi.dll.phase0"), "rb").read() == b"phase 0 build", "other files untouched"
assert st["log"] is None, "the old log is from before the install"

r2 = s3.enable("1178830", {"model": "student", "mode": "quality"}, r["launch_options"], "Default")
assert "OPTI_SGSR3_STUDENT=1" in r2["launch_options"] and r2["launch_options"].count("-dx12") == 1, r2
assert open(os.path.join(win64, ".upscale-sgsr3/backup/dxgi.dll"), "rb").read() == b"manual sgsr3 build", "backup kept"
assert s3.read_state(win64)["resolution_prev"] == "1280x720", "first resolution is the one to restore"
assert sgsr3.ini_get(open(os.path.join(win64, "OptiScaler.ini")).read(), "UpscaleRatio", "UpscaleRatioOverrideValue") == "auto"  # 0.8.15: no override

# frame generation on: FidelityFX DLL in, SGSR-FG variable; off again: the file goes, the variable too
r3 = s3.enable("1178830", {"fg": True}, r2["launch_options"], None)
assert open(os.path.join(win64, "amd_fidelityfx_dx12.dll"), "rb").read() == b"ffx sdk 1.1.4"
assert "OPTI_SGSRFG=1" in r3["launch_options"] and "OPTI_SGSR3_STUDENT=1" in r3["launch_options"], r3
assert sgsr3.ini_get(open(os.path.join(win64, "OptiScaler.ini")).read(), "FrameGen", "FGOutput") == "fsrfg"
st3 = s3.set_ini("1178830", {"latency": True, "fps": "30"})
ini3 = open(os.path.join(win64, "OptiScaler.ini")).read()
assert sgsr3.ini_get(ini3, "fakenvapi", "ForceLatencyFlex") == "true" and sgsr3.ini_get(ini3, "Framerate", "FramerateLimit") == "30"
assert sgsr3.ini_get(ini3, "FrameGen", "Enabled") == "true", "set_ini keeps the other options"
assert st3["opts"]["fps"] == "30" and st3["opts"]["fg"]
r2 = s3.enable("1178830", {"fg": False}, r3["launch_options"], None)
assert not os.path.exists(os.path.join(win64, "amd_fidelityfx_dx12.dll"))
assert "OPTI_SGSRFG=1" not in r2["launch_options"] and "OPTI_SGSR3_STUDENT=1" in r2["launch_options"]
try:
    s3.clean_opts({"fps": "25"})
    raise AssertionError("unknown fps accepted")
except ValueError:
    pass

# the game ran: OptiScaler wrote its log
time.sleep(0.01)
write(os.path.join(win64, "OptiScaler.log"), "[info] Initializing SGSR3 1279x720 -> 1920x1080\n[info] SGSR3 student: 10 passes\n")
lg = s3.status("1178830")["log"]
assert lg and lg["sgsr3"] and lg["mode"] == "student" and lg["render"] == "1279x720" and lg["output"] == "1920x1080", lg
write(os.path.join(win64, "OptiScaler.log"), "Initializing SGSR3 960x540 -> 1920x1080\n"
      "SGSR3: the student (Vulkan interop) is not available; SGSR 3 v1 instead\nSGSR3 student: x\n")
lg = s3.status("1178830")["log"]
assert lg["mode"] == "v1" and lg["student_fallback"], lg
# 0.4.0: the student hands over to v1f (failed start, or failed in the game)
write(os.path.join(win64, "OptiScaler.log"), "Initializing SGSR3 960x540 -> 1920x1080\n"
      "SGSR3: the student (Vulkan interop) is not available; v1f instead\nSGSR3 v1f: v1f v1b fragment, temporal box hp\n")
lg = s3.status("1178830")["log"]
assert lg["mode"] == "v1f" and lg["student_fallback"], lg
write(os.path.join(win64, "OptiScaler.log"), "Initializing SGSR3 960x540 -> 1920x1080\nSGSR3 student: 10 passes\n"
      "SGSR3: the student failed; v1f from the next frame\nSGSR3 v1f: v1f v1b fragment\n"
      "SGSR3: v1f started as the student's fallback\n")
lg = s3.status("1178830")["log"]
assert lg["mode"] == "v1f" and lg["student_fallback"], lg
write(os.path.join(win64, "OptiScaler.log"), "SGSR3 v1f: v1f v1b fragment, compact (A2B10G10R10 + R16F), low-frequency "
      "lock, thin-feature locks, temporal box hp (RG8), shaderFloat16 on\nInitializing SGSR3 1280x720 -> 1920x1080\n"
      "LowLatency algo: LatencyFlex\nSGSR-FG: on\n")
lg = s3.status("1178830")["log"]
assert lg["mode"] == "v1f" and lg["hp"] and lg["fg"] == "on" and lg["low_latency"] == "LatencyFlex", lg
assert not lg["lines2"]
write(os.path.join(win64, "OptiScaler.log"), "SGSR3 v1f: v1f v1b fragment, shaderFloat16 OFF\n"
      "SGSR3: v1f (Vulkan interop) is not available; SGSR 3 v1 compute instead\nInitializing SGSR3 1280x720 -> 1920x1080\n")
lg = s3.status("1178830")["log"]
assert lg["mode"] == "v1" and lg["v1f_fallback"] and not lg["hp"], lg

# 0.7.1: live model changes re-create the upscaler in the same log; the status is the last upscaler's (Into the Dead,
# 30.10: an earlier Lite try at 720p fell back to Vela SR, then FSR 4.1 Lite ran at 540p, the panel said "Vela SR")
ITD = ("Initializing SGSR3 1344x756 -> 1920x1080\nSGSR3: Vela live model fsr4lite\n"
       "Init Vela FSR 4 VK: ffxCreateContext failed (3) for 1344x756 -> 1920x1080 (FSR 4.1.2 Lite: ...)\n"
       "SGSR3: FSR 4 VK is not available here; Vela AI instead\nInit SGSR3 student: model L2eh_ha2\n"
       "SGSR3: frame sizes differ from the student's (1344x756 -> 1920x1080); v1f instead\n"
       "Init SGSR3 v1f: v1f v1a fragment, temporal box hp (RG8)\nSGSR3: v1f started as the student's fallback\n"
       "Poll Vela live (Z:\\tmp\\vela-live\\2239710.ini): model fsr4lite ratio 2 lossy 5\n"
       "TryEvaluateOptiFeature Vela live: handle 1000003 model fsr4lite -> SGSR 3 (the game draws 960x540)\n"
       "SetInitParameters Render Resolution: 960x540, Display Resolution 1920x1080, Quality: 3\n"
       "InitInternal Initializing SGSR3 960x540 -> 1920x1080\nCreatePipelines SGSR3: Vela live model fsr4lite\n"
       "Init Vela FSR 4 VK: FSR 4.1.2 Lite VK INT8 context 960x540 -> 1920x1080 (hdr true, network every 5 frames)\n"
       "Init SGSR3 student: model L2eh_ha2\nEvaluate Vela FSR 4 VK: first frame dispatched\n")
write(os.path.join(win64, "OptiScaler.log"), ITD)
lg = s3.status("1178830")["log"]
# 0.8.15: an old Lite line is read as FSR 4 (tolerant, no Lite model, no lossy key)
assert lg["mode"] == "fsr4" and "lossy" not in lg and lg["fsr4_version"] == "4.1.2 Lite", lg
assert not lg["student_fallback"] and not lg["fsr4_fallback"] and lg["render"] == "960x540", lg
write(os.path.join(win64, "OptiScaler.log"), ITD.replace("4.1.2 Lite VK", "4.1.1 VK")
      .replace(", network every 5 frames", ", network every 1 frames").replace("model fsr4lite", "model fsr4"))
lg = s3.status("1178830")["log"]
assert lg["mode"] == "fsr4" and lg["fsr4_version"] == "4.1.1" and "lossy" not in lg, lg
# SGSR 2 at the size the game draws (patch 0012: the subrect, not the feature's creation size)
write(os.path.join(win64, "OptiScaler.log"), ITD + "Vela live: handle 1000003 model sgsr2 -> SGSR 2 (the game draws "
      "1344x756)\nSGSR2: render size 1344x756 from the subrect (feature made for 960x540)\n"
      "Initializing SGSR2 1344x756 -> 1920x1080\n")
lg = s3.status("1178830")["log"]
assert lg["mode"] == "sgsr2" and lg["render"] == "1344x756" and lg["sgsr3"] and not lg["student_fallback"], lg
# Lite picked at 720p: the fork refuses it (0.7.1 line) and Vela AI runs
write(os.path.join(win64, "OptiScaler.log"), ITD + "Vela live: handle 1000003 model fsr4lite -> SGSR 3 (the game draws "
      "1344x756)\nInitializing SGSR3 1344x756 -> 1920x1080\nSGSR3: Vela live model fsr4lite\nSGSR3: FSR 4.1 Lite runs only "
      "960x540 -> 1920x1080, not 1344x756 -> 1920x1080; Vela AI instead\nInit SGSR3 student: model L2eh_ha2\n")
lg = s3.status("1178830")["log"]
assert lg["mode"] == "student" and lg["fsr4_fallback"] == "failed" and "lossy" not in lg, lg
# 0.8.2-rs: Lite runs from 645x360 up; 640x360 is refused with the new line (another AMD model) and Vela AI runs
write(os.path.join(win64, "OptiScaler.log"), ITD + "Vela live: handle 1000003 model fsr4lite -> SGSR 3 (the game draws "
      "640x360)\nInitializing SGSR3 640x360 -> 1920x1080\nSGSR3: Vela live model fsr4lite\nSGSR3: FSR 4.1 Lite runs only from "
      "645x360 to 1920x1080 render -> 1920x1080, not 640x360 -> 1920x1080; Vela AI instead\nInit SGSR3 student: model L2eh_ha2\n")
lg = s3.status("1178830")["log"]
assert lg["mode"] == "student" and lg["fsr4_fallback"] == "failed" and lg["render"] == "640x360", lg

off = s3.disable("1178830", r2["launch_options"])
assert off["launch_options"] == launch0 and off["resolution"] == "1280x720", off
assert tree(bmi) == before, "game folder restored exactly"
assert not off["status"]["installed"]
assert "SGSR3" in open(os.path.join(settings, "sgsr3-logs/1178830-OptiScaler.log")).read(), "SGSR 3 log kept"
try:
    s3.disable("1178830", launch0)
    raise AssertionError("disabled twice")
except RuntimeError:
    pass

# clean folder: everything we add goes away again, including the OptiScaler/plugins folders
before = tree(uni)
r = s3.enable("111", {}, "")
# Vela AI by default; smoothing is Vela SR's (v1f) only
assert r["launch_options"] == 'WINEDLLOVERRIDES="dxgi=n,b" OPTI_SGSR3_STUDENT=1 %command%', r["launch_options"]
v = s3.enable("111", {"model": "v1f"}, r["launch_options"])["launch_options"]
assert "OPTI_SGSR3_FRESH=0.5" in v and "AMAX" not in v, v
# 0.4.13: amax by the ratio, min(C, 0.6 (2 / r)^2.2) with C <= 0.6 -- off by default; 0.4.14: the fork's fixed amax
# is 0 (P2b limits inside, no key), the interceptor's 0.6 (still L6acw)
assert [sgsr3.amax_str(x) for x in ("1.5", "2.0", "3.0")] == ["0", "0", "0"]
# 0.4.15: the interceptor carries P2b too: no VELA_AMAX either
assert [sgsr3.amax_str(x, sgsr3.STATIC_AMAX_FIXED) for x in ("1.5", "3.0")] == ["0", "0"]
assert "VELA_AMAX" not in " ".join(sgsr3.static_env({"algo": "ai"}, 3.0))
# 0.8.15: no VELA_FSR2_ALGO in the launch options (Vela AI is gone): the live file / the interceptor's default name the model
assert not any("ALGO" in t for t in sgsr3.static_env({"algo": "ai", "env": ["VELA_RCAS=0"]}, 2.0))
assert sgsr3.static_env({"algo": "ai", "env": ["VELA_RCAS=0"]}, 2.0) == ["VELA_FSR2=replace", "RECON_LEVEL=1", "VELA_RCAS=0"]
assert sgsr3.drop_legacy_env("VELA_NPU=1 VELA_FSR2=replace VELA_FSR2_ALGO=ai RECON_LEVEL=1 x %command%") ==     "VELA_NPU=1 VELA_FSR2=replace RECON_LEVEL=1 x %command%"
assert sgsr3.drop_legacy_env("VELA_FSR2_ALGO=fsr4lite %command% -foo") == "%command% -foo"
assert sgsr3.drop_legacy_env("MY_VELA_FSR2_ALGO=ai %command%") == "MY_VELA_FSR2_ALGO=ai %command%"  # another key, left alone
assert sgsr3.drop_legacy_env("a  %command%") == "a  %command%" and sgsr3.drop_legacy_env(None) is None and sgsr3.drop_legacy_env("") == ""
v = s3.enable("111", {"mode": "speed", "model": "auto"}, r["launch_options"])["launch_options"]
assert "AMAX" not in v, v
sgsr3.AMAX_BY_RATIO = True  # the one switch
assert [sgsr3.amax_str(x) for x in ("1.5", "2.0", "2.25", "3.0", 4)] == ["0.6", "0.6", "0.463", "0.246", "0.131"]
v = s3.enable("111", {"mode": "speed", "model": "auto"}, r["launch_options"])["launch_options"]
assert "OPTI_SGSR3_AMAX=0.463" in v and v.count("AMAX") == 1, v
v = s3.enable("111", {"mode": "speed", "res": "3.0"}, v)["launch_options"]
assert "OPTI_SGSR3_AMAX=0.246" in v and v.count("AMAX") == 1, v
sgsr3.AMAX_BY_RATIO = False
r = s3.enable("111", {"model": "auto"}, "")
write(os.path.join(uni, "OptiScaler.log"), "log")
off = s3.disable("111", "user edited " + r["launch_options"])
assert off["launch_options"] == "user edited %command%", off["launch_options"]
assert tree(uni) == before and not os.path.exists(os.path.join(uni, "OptiScaler")), tree(uni)

# upgrade from 0.2.x: its state.json lists 3 files; the game ships its own amd_fidelityfx_dx12.dll
write(os.path.join(uni, "amd_fidelityfx_dx12.dll"), b"the game's own ffx")
before = tree(uni)
r = s3.enable("111", {}, "")
stj = s3.read_state(uni)
del stj["files"]["amd_fidelityfx_dx12.dll"]
s3._write_state(uni, stj)
s3.disable("111", r["launch_options"])
assert tree(uni) == before, "0.2.x state: the unlisted file is left alone"
r = s3.enable("111", {}, "")
stj = s3.read_state(uni)
del stj["files"]["amd_fidelityfx_dx12.dll"]
s3._write_state(uni, stj)
r = s3.enable("111", {"fg": True}, r["launch_options"])  # backs the game's file up before replacing it
assert open(os.path.join(uni, "amd_fidelityfx_dx12.dll"), "rb").read() == b"ffx sdk 1.1.4"
s3.disable("111", r["launch_options"])
assert tree(uni) == before, tree(uni)
os.remove(os.path.join(uni, "amd_fidelityfx_dx12.dll"))

try:
    s3.enable("111", {"algo": "dlss"}, "")
    raise AssertionError("unknown algorithm accepted")
except ValueError:
    pass

# ---- Gamescope stays on Linear while SGSR 3 is on
calls = []


class Fake(upscale.Upscaler):
    def _ctl(self, convar, value):
        calls.append((convar, value))


up = Fake(tempfile.mkdtemp(), "/nonexistent", log=lambda *a: None)
up.atoms = {"GAMESCOPE_FOCUSED_APP": 1178830, "GAMESCOPE_NEW_SCALING_FILTER": 2, "GAMESCOPE_FSR_SHARPNESS": 2}
# 0.8.15: no Gamescope filter from Vela any more (no Linear for a Vela game, no profile): set_sgsr3 keeps the list only
n = len(calls)
up.set_sgsr3("1178830", True)
up.set_sgsr3("1178830", False)
assert len(calls) == n, calls
# ---- 0.4.7: Resident Evil 4, FSR 2 inside the exe -> our interceptor DLL alone (no OptiScaler, no ini)
re4 = game(steam, "2050650", "RESIDENT EVIL 4  BIOHAZARD RE4", {"re4.exe": b"r" * (3 << 20), "dxgi.dll.bak": b"x"})
write(os.path.join(tmp, "staticfsr", "dxgi.dll"), b"vela interceptor")
before = tree(re4)
lo0 = "/usr/libexec/armada/armada-game-launch %command%"
st = s3.status("2050650")
assert st["static_fsr2"]["algo"] == "ai" and st["static_fsr2"]["env"] == ["VELA_RCAS=0"] and "Ambient Occlusion" in st["profile"]["hint"] and "960×540" in st["profile"]["hint"] and st["opts"]["algo"] == "student" and not st["fg_available"]
r = s3.enable("2050650", {"mode": "balance"}, lo0, "Default")
assert r["launch_options"] == ('WINEDLLOVERRIDES="dxgi=n,b" VELA_FSR2=replace RECON_LEVEL=1 VELA_RCAS=0 '
                               + lo0), r["launch_options"]
assert open(os.path.join(re4, "dxgi.dll"), "rb").read() == b"vela interceptor"
assert not os.path.exists(os.path.join(re4, "OptiScaler.ini")) and not os.path.exists(os.path.join(re4, "OptiScaler"))
st = r["status"]
assert st["installed"] and st["intact"] and st["log"] is None and st["opts"]["algo"] == "student", st
assert s3.set_ini("2050650", {"latency": True}) is None, "no ini for the interceptor"
# ---- 0.8.11: after a plugin update the old interceptor of ours is refreshed (game closed), not "changed"
src_dxgi, game_dxgi = os.path.join(tmp, "staticfsr", "dxgi.dll"), os.path.join(re4, "dxgi.dll")
re4_state = os.path.join(re4, sgsr3.STATE_DIR, "state.json")
assert json.load(open(re4_state))["dxgi_md5"] == sgsr3.md5(src_dxgi), "the install records our hash"
write(src_dxgi, b"vela interceptor 0.8.11")  # the plugin was updated
st = s3.status("2050650")
assert st["intact"] and st["dxgi_old"], st  # an older build of ours: green, not "Game files changed"
assert s3.refresh_dll("2050650") and open(game_dxgi, "rb").read() == b"vela interceptor 0.8.11"
st = s3.status("2050650")
assert st["intact"] and not st["dxgi_old"] and json.load(open(re4_state))["dxgi_md5"] == sgsr3.md5(src_dxgi), st
assert not s3.refresh_dll("2050650"), "nothing to do"
# a foreign dxgi.dll (a game update overwrote ours): red, and never replaced
write(game_dxgi, b"some other dxgi of a game update")
st = s3.status("2050650")
assert st["intact"] is False and not st["dxgi_old"], st
assert not s3.refresh_dll("2050650") and open(game_dxgi, "rb").read() == b"some other dxgi of a game update"
# an install before 0.8.11 (no hash in the state): a build an earlier version shipped is ours
legacy = json.load(open(re4_state))
legacy.pop("dxgi_md5")
write(re4_state, json.dumps(legacy))
os.utime(game_dxgi, (legacy["updated_at"] + 60, legacy["updated_at"] + 60))
assert s3.static_dxgi(re4, legacy, s3.kit("2050650")) == "foreign"  # written after our last install
os.utime(game_dxgi, (legacy["updated_at"] - 60, legacy["updated_at"] - 60))
assert s3.static_dxgi(re4, legacy, s3.kit("2050650")) == "old"  # not touched since our last write: ours
os.utime(game_dxgi, None)
sgsr3.OLD_STATIC_DXGI = sgsr3.OLD_STATIC_DXGI | {sgsr3.md5(game_dxgi)}  # as 55096b0c (0.8.4..0.8.10) in a real install
assert s3.status("2050650")["intact"] and s3.refresh_dll("2050650")
assert open(game_dxgi, "rb").read() == b"vela interceptor 0.8.11" and json.load(open(re4_state))["dxgi_md5"] == sgsr3.md5(src_dxgi)
# 0.8.15: a missing dxgi.dll is red only until the closed-game repair puts ours back (a foreign one is never touched)
os.replace(game_dxgi, game_dxgi + ".wn")
assert s3.status("2050650")["intact"] is False and s3.refresh_dll("2050650")
assert open(game_dxgi, "rb").read() == b"vela interceptor 0.8.11" and s3.status("2050650")["intact"]
os.replace(game_dxgi + ".wn", game_dxgi)
assert s3.status("2050650")["intact"]
time.sleep(0.01)
write(os.path.join(re4, "sgsr3intercept.log"), "".join(line + chr(10) for line in (
    "[09:19:52.590] vela: calibration ok: layout fsr2.0+re16 render 960x540 (texture 960x540) display 1920x1080 jitter (0, 0)",
    "[09:19:52.618] vela: Vela AI ready 960x540 -> 1920x1080",
    "[09:19:56.449] fps 60.9 (presents 609 in 10.0 s; frames by Vela 0)",
    "[09:20:06.461] fps 79.6 (presents 797 in 10.0 s; frames by Vela 797), Vela AI GPU 2.3 ms")))
lg = s3.status("2050650")["log"]
assert lg["sgsr3"] and lg["calibrated"] and lg["mode"] == "student" and lg["render"] == "960x540" and lg["frames"] == 797, lg
# 0.8.3-re: the interceptor's live models -- FSR 4.1 Lite (the DLL's own lines), then FSR 3.1 refused -> Vela AI
assert s3.status("2050650")["live_models"] == ["fsr4", "fsr4fast", "fsr3", "sgsr2"]  # 0.8.13: no Vela AI / SR
time.sleep(0.01)
write(os.path.join(re4, "sgsr3intercept.log"), "".join(line + chr(10) for line in (
    "[09:19:52.590] vela: calibration ok: layout fsr2.0+re16 render 960x540 (texture 960x540) display 1920x1080 jitter (0, 0)",
    "[09:19:52.600] vela: live (Z:/tmp/vela-live/2050650.ini): model fsr4lite lossy 2 npu 0",
    "[09:19:52.700] vela: jitter locked (Halton phase offset 5 of 32)",
    "[09:19:52.710] Vela FSR 4 VK: FSR 4.1.2 Lite VK INT8 context 960x540 -> 1920x1080 (hdr 0, network every 2 frames)",
    "[09:19:52.720] vela: model fsr4lite runs 960x540 -> 1920x1080",
    "[09:20:06.461] fps 79.6 (presents 797 in 10.0 s; frames by Vela 797)")))
lg = s3.status("2050650")["log"]
assert lg["mode"] == "fsr4" and lg["fsr4_version"] == "4.1.2 Lite" and "lossy" not in lg and lg["jitter"] == "locked"     and lg["fsr4_fallback"] is None, lg
time.sleep(0.01)
with open(os.path.join(re4, "sgsr3intercept.log"), "a") as f:
    f.write("[09:21:00.000] vela: live (Z:/tmp/vela-live/2050650.ini): model fsr3 lossy 1 npu 0" + chr(10)
            + "[09:21:00.100] vela: FSR 3.1: amd_fidelityfx_dx12.dll not found next to the DLL" + chr(10)
            + "[09:21:00.100] vela: model fsr3 is not available here; Vela AI instead" + chr(10)
            + "[09:21:00.200] vela: model ai runs 960x540 -> 1920x1080" + chr(10))
lg = s3.status("2050650")["log"]
assert lg["mode"] == "student" and lg["fsr3_fallback"] == "failed" and lg["fsr4_fallback"] is None, lg
assert s3.status("2050650")["amax"] == {"set": None, "want": "0", "stale": False}
# 0.4.13: the game's FSR 2 at Ultra Performance (x3): amax 0.246 wanted, the panel re-applies -> VELA_AMAX=0.246
time.sleep(0.01)
write(os.path.join(re4, "sgsr3intercept.log"),
      "[09:19:52.590] vela: calibration ok: layout fsr2.0+re16 render 640x360 (texture 640x360) display 1920x1080" + chr(10))
assert s3.status("2050650")["amax"] == {"set": None, "want": "0", "stale": False}, "off: none at any ratio"
sgsr3.AMAX_BY_RATIO = True
assert s3.status("2050650")["amax"] == {"set": None, "want": "0.246", "stale": True}
r = s3.enable("2050650", {}, r["launch_options"])
assert "VELA_AMAX=0.246" in r["launch_options"] and r["launch_options"].count("AMAX") == 1, r["launch_options"]
assert not s3.status("2050650")["amax"]["stale"]
sgsr3.AMAX_BY_RATIO = False
assert s3.status("2050650")["amax"] == {"set": "0.246", "want": "0", "stale": True}, "an old key goes on re-apply"
r = s3.enable("2050650", {}, r["launch_options"])
assert "AMAX" not in r["launch_options"] and not s3.status("2050650")["amax"]["stale"], r["launch_options"]
# 0.4.11: no SDF-skip toggle (the game's Ambient Occlusion = Off instead); an old "fast" in the options is ignored
r2 = s3.enable("2050650", {"fast": True}, r["launch_options"])
assert r2["launch_options"] == r["launch_options"] and "fast" not in r2["status"]["opts"], r2
off = s3.disable("2050650", r["launch_options"])
assert off["launch_options"] == lo0 and tree(re4) == before, (off, tree(re4))
assert "2050650-sgsr3intercept.log" in os.listdir(os.path.join(settings, "sgsr3-logs"))
assert sgsr3.strip_ours("VELA_FSR2=replace VELA_FSR2_ALGO=ai RECON_LEVEL=1 VELA_AMAX=0.5 VELA_RCAS=0 VELA_SKIP_SDF=1 x %command%") == "x %command%"

print("ok")
