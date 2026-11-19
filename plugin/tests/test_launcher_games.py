"""0.8.23: a launcher's shortcut (Heroic, Xodus Nexus, a store client in a Proton prefix) gets Vela once its game has been
seen running (learn.py): the scan uses the learned folder, the check words what is missing, the files go into the game
folder (the overlay cannot hold a launcher's game), and the game's settings file is looked up in the launcher's prefix.
Run: python -I tests/test_launcher_games.py"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import gamecfg  # noqa: E402
import gamecheck  # noqa: E402
import sgsr3  # noqa: E402

NUL = bytes([0])


def s(key, value):
    return b"\x01" + key.encode() + NUL + value.encode() + NUL


def entry(index, appid, name, exe, start, options):
    signed = appid - 2 ** 32
    return (b"\x00" + str(index).encode() + NUL + b"\x02appid" + NUL + signed.to_bytes(4, "little", signed=True) +
            s("AppName", name) + s("Exe", f'"{exe}"') + s("StartDir", f'"{start}"') + s("LaunchOptions", options) + b"\x08")


def put(path, data=b""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


PE64 = b"MZ" + bytes(58) + (64).to_bytes(4, "little") + b"PE\0\0" + (0x8664).to_bytes(2, "little") + bytes(60)
HIFI, HEROIC, EMU, CEL, GOG = "2641705097", "2339058479", "3877193403", "3372006481", "3123456789"

with tempfile.TemporaryDirectory() as home:
    launch = "/usr/libexec/armada/armada-game-launch %command%"
    game = os.path.join(home, "Games", "Xodus Nexus", "Hi-Fi-RUSH")
    exe_dir = os.path.join(game, "Hibiki", "Binaries", "WinGDK")
    put(os.path.join(exe_dir, "Hi-Fi-RUSH.exe"), b"\x8f\x11 encrypted " * 100)       # not a PE file on disk
    put(os.path.join(game, "Engine", "Binaries", "ThirdParty", "Big-Win64-Shipping.exe"), PE64 * 9)  # scan_dir's own pick
    put(os.path.join(game, "Engine", "Plugins", "DLSS", "nvngx_dlss.dll"), b"x")
    celeste = os.path.join(home, "Games", "Heroic", "Celeste")
    put(os.path.join(celeste, "Celeste.exe"), PE64)
    gog = os.path.join(home, "Games", "Witcher")
    put(os.path.join(gog, "witcher3.exe"), PE64 + bytes(1024 * 1024))  # pick_exe takes an exe of 1 MB or more
    put(os.path.join(home, "Applications", "Ryujinx.AppImage"))
    cfg = os.path.join(home, ".local", "share", "Steam", "userdata", "10001", "config")
    put(os.path.join(cfg, "shortcuts.vdf"), b"\x00shortcuts" + NUL + b"".join([
        entry(0, int(HIFI), "Hi-Fi RUSH", "/bin/sh", "/bin/", launch + ' "/h/launch-game" "4096bd52c00d45c3"'),
        entry(1, int(HEROIC), "Heroic Games Launcher", "/usr/bin/heroic", "/usr/bin", "LD_PRELOAD= " + launch + " --console"),
        entry(2, int(EMU), "Ryujinx", os.path.join(home, "Applications", "Ryujinx.AppImage"), home, launch),
        entry(3, int(CEL), "Celeste", "/usr/lib64/heroic/heroic", "/usr/lib64/heroic",
              launch + ' --no-gui "heroic://launch?appName=Salt&runner=legendary"'),
        entry(4, int(GOG), "The Witcher 3", os.path.join(gog, "witcher3.exe"), gog, ""),
    ]) + b"\x08\x08")
    settings = os.path.join(home, "settings")
    s3 = sgsr3.Sgsr3(os.path.join(home, "no-assets"), home, settings, log=lambda m: None)
    s3.new_mode, s3.probe = "launcher", lambda *a: (True, "")  # the overlay works on this device
    check = gamecheck.GameCheck(settings, log=lambda m: None)

    def verdict(appid):
        sc = s3.scan(appid, fresh=True)
        return sc, check.check(sc, s3_assets=True, ag_assets=False, appid=appid)

    # the shortcut's own launch options come through, and what Vela adds to them later does not change what it starts
    sc0 = sgsr3.shortcut_entry(home, HIFI)
    assert sc0["options"].endswith('"4096bd52c00d45c3"') and sc0["exe"] == "/bin/sh", sc0
    sig = sgsr3.shortcut_sig(sc0)
    ours = f'VELA_NPU=1 VELA_NPU_LOG_FILE=/h/npu/layer.log {sgsr3.DLL_OVERRIDE} {sgsr3.STUDENT_ENV} OPTI_SGSR3_AMAX=0.6 '
    assert sgsr3.shortcut_sig({**sc0, "options": ours + sc0["options"] + " -dx12"}) == sig
    assert sgsr3.shortcut_sig({**sc0, "options": "~/.local/share/vela/bin/vela-launch " + sc0["options"]}) == sig
    assert sgsr3.shortcut_sig({**sc0, "options": sc0["options"].replace("4096bd52c00d45c3", "7b7e70c5f0c02444")}) != sig
    assert sgsr3.shortcut_entry(home, "1817190") is None and sgsr3.shortcut_sig(None) is None

    # nothing learned yet: "start the game once", not "a Linux game"; no model can be picked
    sc, v = verdict(HIFI)
    assert sc["pending"] == "start_once" and (v["kind"], v["reason"]) == ("none", "start_once"), (sc, v)
    assert verdict(EMU)[1]["reason"] == "start_once"
    # a shortcut that ran without a Windows game says so from then on; a launcher's own shortcut says that
    s3.learned.note_native(EMU, sgsr3.shortcut_sig(sgsr3.shortcut_entry(home, EMU)))
    sc, v = verdict(EMU)
    assert sc["pending"] == "native" and v["reason"] == "no_windows" and sc["install_dir"], (sc, v)
    # ... until a Windows game is seen under it after all (a launcher that stopped on an error the first time)
    late = {"status": "found", "certain": True, "install_dir": gog, "exe": "witcher3.exe"}
    assert s3.learned.record(EMU, late, sig=sgsr3.shortcut_sig(sgsr3.shortcut_entry(home, EMU)))["install_dir"] == gog
    s3.learned.forget(EMU)
    s3.learned.note_native(EMU, sgsr3.shortcut_sig(sgsr3.shortcut_entry(home, EMU)))
    s3.learned.record(HEROIC, {"status": "other", "appid": CEL}, sig=sgsr3.shortcut_sig(sgsr3.shortcut_entry(home, HEROIC)))
    assert verdict(HEROIC)[1]["reason"] == "launcher"

    # the game was seen running: its folder, the exe that ran (not the folder's biggest), DX12 read from the running copy
    obs = {"status": "found", "certain": True, "handoff": False, "install_dir": game,
           "exe": os.path.join("Hibiki", "Binaries", "WinGDK", "Hi-Fi-RUSH.exe"), "machine": "x64",
           "prefix": os.path.join(home, "nexus", "compatdata", "4096bd52c00d45c3", "pfx")}
    s3.learned.record(HIFI, obs, sig=sig, markers={"dx12", "dx11"})
    sc, v = verdict(HIFI)
    assert sc["install_dir"] == game and sc["exe_dir"] == exe_dir and sc["exe"] == "Hi-Fi-RUSH.exe", sc
    assert sc["name"] == "Hi-Fi RUSH" and sc["inputs"] == ["DLSS"] and "pending" not in sc, sc
    assert sc["learned"]["confirmed"] is True and sc["learned"]["folder"] == "Hi-Fi-RUSH" and sc["learned"]["by_steam"] is False
    assert (v["kind"], v["reason"]) == ("fsr4", None) and v["api"] == ["dx12", "dx11"], v
    # the launcher starts the game, so the files go into the game folder although the overlay works here
    assert s3._new_mode(HIFI, sc0["options"]) == "copy"
    assert s3._new_mode(GOG, "") == "launcher"  # a shortcut straight to the exe: as before
    # the shortcut now starts another game: learned again, nothing of the old one is used
    put(os.path.join(cfg, "shortcuts.vdf"), b"\x00shortcuts" + NUL + entry(
        0, int(HIFI), "Hi-Fi RUSH", "/bin/sh", "/bin/", launch + ' "/h/launch-game" "7b7e70c5f0c02444"') + b"\x08\x08")
    assert verdict(HIFI)[1]["reason"] == "start_once"

    # Heroic: no id of its own, a candidate until the player picks a model; a 32-bit game is not supported
    put(os.path.join(cfg, "shortcuts.vdf"), b"\x00shortcuts" + NUL + b"".join([
        entry(3, int(CEL), "Celeste", "/usr/lib64/heroic/heroic", "/usr/lib64/heroic",
              launch + ' --no-gui "heroic://launch?appName=Salt&runner=legendary"'),
        entry(4, int(GOG), "The Witcher 3", os.path.join(gog, "witcher3.exe"), gog, "")]) + b"\x08\x08")
    csig = sgsr3.shortcut_sig(sgsr3.shortcut_entry(home, CEL))
    hpfx = os.path.join(home, "Games", "Heroic", "Prefixes", "Celeste")
    cobs = {"status": "found", "certain": False, "install_dir": celeste, "exe": "Celeste.exe", "machine": "x86", "prefix": hpfx}
    s3.learned.record(CEL, cobs, sig=csig)
    sc, v = verdict(CEL)
    assert sc["learned"]["confirmed"] is False and sc["machine"] == "x86" and v["reason"] == "x86" and v["kind"] == "none", (sc, v)
    s3.learned.record(CEL, {**cobs, "machine": "x64"}, sig=csig)
    s3.learned.confirm(CEL)
    assert s3.scan(CEL, fresh=True)["learned"]["confirmed"] is True

    # a shortcut straight to the game's exe is never "learned", whatever was seen running
    gsig = sgsr3.shortcut_sig(sgsr3.shortcut_entry(home, GOG))
    s3.learned.record(GOG, {"status": "found", "certain": True, "install_dir": gog, "exe": "witcher3.exe"}, sig=gsig)
    sc = s3.scan(GOG, fresh=True)
    assert "learned" not in sc and sc["exe_dir"] == gog, sc

    # the game's settings file in the prefix: the launcher's own prefix, not Steam's compatdata/<appid>
    gc = gamecfg.GameCfg(settings, log=lambda m: None, home=home)
    e = {"root": "prefix", "file": os.path.join("AppData", "Local", "Game", "cfg.ini")}
    steam_path = gc._resolve(CEL, celeste, e)
    assert os.path.join("compatdata", CEL, "pfx") in steam_path, steam_path
    gc.prefix_of = lambda a: (s3.scan(a).get("learned") or {}).get("prefix")
    assert gc._resolve(CEL, celeste, e) == os.path.join(hpfx, "drive_c", "users", "steamuser", e["file"])
    assert gc._resolve(GOG, gog, e) == gc._resolve(GOG, gog, e) and "compatdata" in gc._resolve(GOG, gog, e)
print("test_launcher_games OK")
