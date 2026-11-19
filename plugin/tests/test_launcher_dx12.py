"""0.8.23: a launcher's Unreal game is asked for DirectX 12 through its own Engine.ini, not with -dx12 on Steam's launch
options (those go to the launcher: Ghostrunner under Heroic started in DX11 with it, console 2026-11-16). The line is put
in while Vela is on and taken out exactly on Off.
Run: python -I tests/test_launcher_dx12.py"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import gamecfg  # noqa: E402
import sgsr3  # noqa: E402

SEC, KEY, VAL = sgsr3.UE_RHI["section"], "DefaultGraphicsRHI", "DefaultGraphicsRHI_DX12"

# ---- the line goes in and comes out exactly, whatever the file looks like -------------------------------------------------
for text in ("[Core.System]\r\nPaths=../../../Engine/Content\r\nPaths=%GAMEDIR%Content\r\n",   # Ghostrunner's, CRLF
             "[Core.System]\nPaths=a\n\n[Other]\nx=1\n",
             "[Core.System]\nPaths=a",                                                           # no newline at the end
             ""):
    added = gamecfg.ini_add(text, SEC, KEY, VAL)
    assert gamecfg.ini_get(added, SEC, KEY) == VAL, repr(added)
    assert added.startswith(text) and gamecfg.ini_get(added, "Core.System", "Paths") == gamecfg.ini_get(text, "Core.System", "Paths")
    back = gamecfg.ini_remove(added, SEC, KEY)
    assert back.rstrip("\r\n") == text.rstrip("\r\n") and gamecfg.ini_get(back, SEC, KEY) is None, (repr(text), repr(back))
    if text.endswith("\n"):
        assert back == text, (repr(text), repr(back))
    if "\r\n" in text:
        assert "\n[" not in added.replace("\r\n", "") and added.endswith("\r\n")  # the file's own line ending
# the section is already there (another key in it): the line joins it, and only that line leaves again
have = f"[A]\na=1\n\n[{SEC}]\nTargetedRHIs=PCD3D_SM5\n\n[B]\nb=2\n"
added = gamecfg.ini_add(have, SEC, KEY, VAL)
assert added == f"[A]\na=1\n\n[{SEC}]\nTargetedRHIs=PCD3D_SM5\n{KEY}={VAL}\n\n[B]\nb=2\n", repr(added)
assert gamecfg.ini_remove(added, SEC, KEY) == have
assert gamecfg.ini_remove(have, SEC, KEY) == have and gamecfg.ini_remove("x=1\n", SEC, KEY) == "x=1\n"

with tempfile.TemporaryDirectory() as tmp:
    # ---- where an Unreal game keeps its Engine.ini in a launcher's prefix ---------------------------------------------------
    pfx = os.path.join(tmp, "Prefixes", "shared")
    cfgdir = os.path.join(pfx, "drive_c", "users", "steamuser", "AppData", "Local", "Ghostrunner", "Saved", "Config",
                          "WindowsNoEditor")
    os.makedirs(cfgdir)
    eng = os.path.join(cfgdir, "Engine.ini")
    orig = "[Core.System]\r\nPaths=../../../Engine/Content\r\n"
    with open(eng, "w", newline="") as f:
        f.write(orig)
    with open(os.path.join(cfgdir, "GameUserSettings.ini"), "w") as f:
        f.write("[/Script/Ghostrunner.GhostrunnerGameUserSettings]\nDLSSEnabled=False\n")
    assert sgsr3.ue_engine_ini(pfx, "Ghostrunner-Win64-Shipping.exe") == eng
    assert sgsr3.ue_engine_ini(pfx, "ghostrunner-WinGDK-Shipping.exe") == eng           # the folder's name, any case
    assert sgsr3.ue_engine_ini(pfx, "Other-Win64-Shipping.exe") is None                # a game that has no folder there
    assert sgsr3.ue_engine_ini(pfx, "Celeste.exe") is None and sgsr3.ue_engine_ini(None, "X-Win64-Shipping.exe") is None
    assert sgsr3.ue_engine_ini(os.path.join(tmp, "nowhere"), "Ghostrunner-Win64-Shipping.exe") is None

    # ---- only a launcher's Unreal game gets the entry ---------------------------------------------------------------------
    s3 = sgsr3.Sgsr3.__new__(sgsr3.Sgsr3)
    scans = {}
    s3.scan = lambda appid, fresh=False: scans[appid]
    launcher = {"engine": "Unreal", "exe": "Ghostrunner-Win64-Shipping.exe", "learned": {"by_steam": False, "prefix": pfx}}
    scans.update({"heroic": launcher, "steam": {"engine": "Unreal", "exe": "Ghostrunner-Win64-Shipping.exe"},
                  "store-in-proton": {**launcher, "learned": {"by_steam": True, "prefix": pfx}},
                  "unity": {**launcher, "engine": "Unity"},
                  "never-ran": {**launcher, "exe": "Other-Win64-Shipping.exe"}})
    entry = s3.dx12_cfg("heroic")
    assert entry == {"root": "abs", "file": eng, "format": "ini", "add": True, "section": SEC, "keys": {KEY: VAL}}, entry
    for other in ("steam", "store-in-proton", "unity", "never-ran"):
        assert s3.dx12_cfg(other) is None, other
    assert s3._launcher_game(launcher) and not s3._launcher_game(scans["steam"]) and not s3._launcher_game(scans["store-in-proton"])

    # ---- on while Vela is on, gone exactly after Off; with the game running it waits for the exit ---------------------------
    gc = gamecfg.GameCfg(os.path.join(tmp, "settings"), log=lambda m: None, home=tmp)
    A = "3044195315"
    profile = {"root": "abs", "file": os.path.join(cfgdir, "GameUserSettings.ini"), "format": "ini",
               "section": "/Script/Ghostrunner.GhostrunnerGameUserSettings", "keys": {"DLSSEnabled": "True"},
               "when": {"DLSSEnabled": ["False"]}}
    read = lambda p: open(p, newline="").read()  # noqa: E731
    gc.enable(A, tmp, [profile, entry], running=True)
    assert read(eng) == orig and gc.status(A)["pending"]            # the game runs: nothing is written under it
    gc.reconcile(set())
    assert gamecfg.ini_get(read(eng), SEC, KEY) == VAL and read(eng).startswith(orig) and "DLSSEnabled=True" in read(profile["file"])
    once = read(eng)
    gc.enable(A, tmp, [profile, entry], running=False)             # set up again: not a second line, the record stays
    gc.ensure(A, tmp, [profile, entry], running=False)
    assert read(eng) == once and read(eng).count(KEY + "=") == 1
    gc.disable(A, running=False)
    assert read(eng) == orig and "DLSSEnabled=False" in read(profile["file"]), repr(read(eng))
    # a game whose Engine.ini already names an API: the player's value is kept in the record and comes back on Off
    with open(eng, "w", newline="") as f:
        f.write(orig + f"\r\n[{SEC}]\r\n{KEY}=DefaultGraphicsRHI_DX11\r\n")
    mine = read(eng)
    gc2 = gamecfg.GameCfg(os.path.join(tmp, "settings2"), log=lambda m: None, home=tmp)
    gc2.enable(A, tmp, [entry], running=False)
    assert gamecfg.ini_get(read(eng), SEC, KEY) == VAL
    gc2.disable(A, running=False)
    assert read(eng) == mine

# ---- Steam's launch options of a launcher's game carry no -dx12 ------------------------------------------------------------
opts = '/usr/libexec/armada/armada-game-launch %command% --no-gui "heroic://launch?appName=x&runner=legendary"'
steam_game, rec = sgsr3.launch_build(opts, None, "student", True)
assert steam_game.endswith(" -dx12") and rec["args"] == ["-dx12"]
launcher_game, rec = sgsr3.launch_build(opts, None, "student", False)
assert "-dx12" not in launcher_game and rec["args"] == [] and launcher_game.endswith(opts), launcher_game
with open(os.path.join(HERE, "..", "py_modules", "sgsr3.py"), encoding="utf-8") as f:
    assert 'sc["engine"] == "Unreal" and not self._launcher_game(sc)' in f.read()
print("test_launcher_dx12 OK")
