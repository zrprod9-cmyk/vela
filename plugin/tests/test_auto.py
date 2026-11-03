"""0.8.16: fewer manual
steps -- presets.default_model (FSR 4 when the device can), gamecfg's ini files in the game folder or the Proton prefix, keys
only at their "off" value (when), a file the game writes on its first run, ensure() for a game set up before its profile had
keys, the old one-file state. The fixtures are the console's own files.
Run: python -I tests/test_auto.py"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import gamecfg  # noqa: E402
import presets  # noqa: E402
import profiles  # noqa: E402

# ---- the default model
NPU_ON = {"layer_found": True, "service": {"installed": True}}
NPU_OFF = {"layer_found": True, "service": {"installed": False}}
assert presets.default_model("fsr4", True, NPU_ON) == "fsr4"
assert presets.default_model("static", True, NPU_ON) == "fsr4"
assert presets.default_model("fsr4", True, NPU_OFF) == "fsr4fast" and presets.default_model("fsr4", True, None) == "fsr4fast"
assert presets.default_model("fsr4", False, NPU_ON) == "sgsr2"
assert presets.default_model("sgsr2", True, NPU_ON) == "sgsr2"  # DX11: OptiScaler's Dx11Upscaler is SGSR 2
assert presets.default_model("none") is None and presets.default_model("any") is None
for k in presets.KINDS:  # whatever default it picks, the panel offers it for that kind
    assert presets.default_model(k, True, NPU_ON) in dict(presets.models_for(k, True, True))

# ---- ini helpers: only keys that are there, line endings kept
GR = ("[ScalabilityGroups]\r\nsg.ResolutionQuality=87.000000\r\n\r\n[/Script/Ghostrunner.GhostrunnerGameUserSettings]\r\n"
      "FidelityFSREnabled=False\r\nFidelityFSRMode=3\r\nDLSSEnabled=False\r\nCurrentUpscalingQuality=2\r\nDLSSMode=0\r\n"
      "ResolutionSizeX=1920\r\n")
SEC = "/Script/Ghostrunner.GhostrunnerGameUserSettings"
assert gamecfg.ini_get(GR, SEC, "DLSSEnabled") == "False" and gamecfg.ini_get(GR, SEC, "nope") is None
t, ch = gamecfg.ini_set(GR, SEC, "DLSSEnabled", "True")
assert ch and "DLSSEnabled=True\r\n" in t and t.count("\r\n") == GR.count("\r\n")
assert gamecfg.ini_set(GR, SEC, "NewKey", 1) == (GR, False) and gamecfg.ini_set(GR, "Nope", "DLSSEnabled", 1) == (GR, False)
assert gamecfg.ini_get(GR, "ScalabilityGroups", "sg.ResolutionQuality") == "87.000000"

tmp = tempfile.mkdtemp(prefix="vela_auto_")
lib = os.path.join(tmp, "lib", "steamapps")
cfg = gamecfg.GameCfg(os.path.join(tmp, "settings"), log=lambda *a: None)

# ---- Ghostrunner: the prefix's GameUserSettings.ini, written by the game on its first run
gr_dir = os.path.join(lib, "common", "Ghostrunner")
os.makedirs(gr_dir)
gp = profiles.game_cfg("1139900")
gr_ini = os.path.join(gamecfg.prefix_user(gr_dir, "1139900"), gp["file"])
assert gr_ini.replace(os.sep, "/").endswith(
    "lib/steamapps/compatdata/1139900/pfx/drive_c/users/steamuser/AppData/Local/Ghostrunner/Saved/Config/WindowsNoEditor/"
    "GameUserSettings.ini")
st = cfg.enable("1139900", gr_dir, gp, running=False)
assert st["on"] and st["pending"] and st["waiting"]  # never ran: waits
os.makedirs(os.path.dirname(gr_ini))
open(gr_ini, "w", newline="").write(GR)
cfg.reconcile({"1139900"})  # running: still waits
assert open(gr_ini, newline="").read() == GR
cfg.reconcile(set())
t = open(gr_ini, newline="").read()
assert "DLSSEnabled=True\r\n" in t and "DLSSMode=5\r\n" in t and "FidelityFSREnabled=False\r\n" in t, t
assert not cfg.status("1139900")["pending"]
open(gr_ini, "w", newline="").write(t.replace("DLSSMode=5", "DLSSMode=3"))
cfg.reconcile(set())  # done: not written again (the player may change it in the game now)
assert "DLSSMode=3" in open(gr_ini, newline="").read()
cfg.disable("1139900", running=False)
assert open(gr_ini, newline="").read() == GR and cfg.read("1139900") is None
# the player's own DLSS on (Quality) is kept and never restored
mine = GR.replace("DLSSEnabled=False", "DLSSEnabled=True").replace("DLSSMode=0", "DLSSMode=3")
open(gr_ini, "w", newline="").write(mine)
cfg.enable("1139900", gr_dir, gp, running=False)
assert open(gr_ini, newline="").read() == mine
cfg.disable("1139900", running=False)
assert open(gr_ini, newline="").read() == mine

# ---- Control: the console's renderer.ini (output = render = 1280x720 from the old 720p window)
ctl = os.path.join(lib, "common", "Control")
os.makedirs(ctl)
R = ('{\n\t"objectType": "rend::UserRendererSettings",\n\t"objectVersion": 22,\n\t"m_iOutputResolutionX": 1280,\n'
     '\t"m_iOutputResolutionY": 720,\n\t"m_iRenderResolutionX": 1280,\n\t"m_iRenderResolutionY": 720,\n\t"m_eDLSSQuality": 1,\n'
     '\t"m_eAllowHDR": 1,\n\t"m_bDLSSLegacyModel": true\n}')
rp = os.path.join(ctl, "renderer.ini")
open(rp, "w", newline="").write(R)
cp = profiles.game_cfg("870780")
# an old (0.4.1) state of a game set up before its profile had the new keys: HDR was 0 before Vela
os.makedirs(os.path.join(tmp, "settings", "gamecfg"), exist_ok=True)
open(os.path.join(tmp, "settings", "gamecfg", "870780.json"), "w").write(json.dumps(
    {"path": rp, "orig": {"m_eAllowHDR": 0}, "want": {"m_eAllowHDR": 1}, "pending": False}))
assert cfg.status("870780")["on"] and cfg.read("870780")["files"][0]["orig"] == {"m_eAllowHDR": 0}
cfg.ensure("870780", ctl, cp, running=False)
t = open(rp).read()
assert '"m_iOutputResolutionX": 1920' in t and '"m_iOutputResolutionY": 1080' in t
# IT2: the 720p window's render (= its output, nothing upscaled) -> 960x540, FSR 4 Performance at 1080p
assert '"m_iRenderResolutionX": 960' in t and '"m_iRenderResolutionY": 540' in t, t
f = cfg.read("870780")["files"][0]
assert f["orig"] == {"m_eAllowHDR": 0, "m_iOutputResolutionX": 1280, "m_iOutputResolutionY": 720,
                     "m_iRenderResolutionX": 1280, "m_iRenderResolutionY": 720}, f["orig"]
mt = os.path.getmtime(rp)
cfg.ensure("870780", ctl, cp, running=False)  # matches: nothing written
assert os.path.getmtime(rp) == mt and cfg.read("870780")["files"][0]["orig"] == f["orig"]
cfg.disable("870780", running=False)
assert open(rp).read() == R.replace('"m_eAllowHDR": 1', '"m_eAllowHDR": 0')
# the player's own render (e.g. 1600x900) stays
R3 = R.replace('"m_iRenderResolutionX": 1280', '"m_iRenderResolutionX": 1600').replace('"m_iRenderResolutionY": 720', '"m_iRenderResolutionY": 900')
open(rp, "w", newline="").write(R3)
cfg.enable("870780", ctl, cp, running=False)
t = open(rp).read()
assert '"m_iRenderResolutionX": 1600' in t and '"m_iRenderResolutionY": 900' in t and '"m_iOutputResolutionX": 1920' in t
cfg.disable("870780", running=False)
# render = output = 1920x1080 (no upscaling) -> 960x540
R2 = R.replace("1280", "1920").replace("720", "1080")
open(rp, "w", newline="").write(R2)
cfg.enable("870780", ctl, cp, running=False)
t = open(rp).read()
assert '"m_iRenderResolutionX": 960' in t and '"m_iRenderResolutionY": 540' in t and '"m_iOutputResolutionX": 1920' in t
cfg.disable("870780", running=True)  # running: the undo waits for the exit
assert '"m_iRenderResolutionX": 960' in open(rp).read()
cfg.reconcile(set())
assert open(rp).read() == R2

# ---- RE4: local_config.ini [AppRender] AppUpScalingTypeFSR2 0 (off) -> 3; the player's mode stays
re4 = os.path.join(lib, "common", "RE4")
os.makedirs(re4)
L = "[Render]\nCapability=DirectX12\n[AppRender]\nAppResolution=18\nAppUpScalingTypeFSR1=0\nAppUpScalingTypeFSR2=0\n[Render/Adapter]\n"
lp = os.path.join(re4, "local_config.ini")
open(lp, "w", newline="").write(L)
rp4 = profiles.game_cfg("2050650")
cfg.enable("2050650", re4, rp4, running=False)
assert "AppUpScalingTypeFSR2=3\n" in open(lp, newline="").read()
cfg.disable("2050650", running=False)
assert open(lp, newline="").read() == L
open(lp, "w", newline="").write(L.replace("FSR2=0", "FSR2=1"))
cfg.enable("2050650", re4, rp4, running=False)
assert "AppUpScalingTypeFSR2=1\n" in open(lp, newline="").read()
assert profiles.static_fsr2("2050650")  # the profile's interceptor entry is still there

# ---- every profile's game_cfg is well-formed
for appid, p in profiles.PROFILES.items():
    for e in gamecfg.entries(p.get("game_cfg")):
        assert e.get("file") and e.get("keys"), appid
        assert e.get("format", "json") in ("json", "ini") and e.get("root", "install") in ("install", "prefix"), appid
        assert e.get("format", "json") == "json" or e.get("section"), appid
        assert set(e.get("when") or {}) <= set(e["keys"]), appid
print("ok")
