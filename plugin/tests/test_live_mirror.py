"""0.8.23: a launcher's runtime may hide the shortcut's appid from the game (Heroic sets SteamAppId=0, seen on the console
2026-11-16). Vela's library in the game then reads /tmp/vela-live/0.ini (fork patch 0010: the first of SteamAppId,
SteamGameId, STEAM_COMPAT_APP_ID that starts with a digit), so the panel's model never reached it. While such a game runs
its live file is kept under that id as well.
Run: python -I tests/test_live_mirror.py"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import learn  # noqa: E402
import live  # noqa: E402

# the id the library reads by, as the fork picks it
assert learn.run_id({"SteamAppId": "0", "SteamGameId": "0", "STEAM_COMPAT_APP_ID": "1dcd033c"}) == "0"       # Heroic
assert learn.run_id({"SteamAppId": "2641705097", "SteamGameId": "11345678901234567890"}) == "2641705097"  # Xodus Nexus
assert learn.run_id({"SteamGameId": "17967071543326408704"}) == "17967071543326408704"
assert learn.run_id({"SteamAppId": "", "STEAM_COMPAT_APP_ID": "abc"}) is None and learn.run_id({}) is None

with tempfile.TemporaryDirectory() as tmp:
    live.TMP_DIR = os.path.join(tmp, "vela-live")
    lv = live.Live(os.path.join(tmp, "settings"), log=lambda m: None)

    def ini(name):
        try:
            with open(os.path.join(live.TMP_DIR, f"{name}.ini"), encoding="utf-8") as f:
                return f.read()
        except OSError:
            return None

    A, B = "3372006481", "3579871069"
    lv.set(A, {"model": "fsr4", "npu": 2})
    lv.set(B, {"model": "sgsr2"})
    assert ini("0") is None and "model=fsr4" in ini(A)
    # game A runs and reads "0": the copy is there, and follows the panel
    assert lv.mirror(A, "0") is True and ini("0") == ini(A)
    assert lv.mirror(A, "0") is False  # already kept: nothing written again
    lv.set(A, {"model": "fsr4fast"})
    assert "model=fsr4fast" in ini("0") and ini("0") == ini(A)
    lv.set(B, {"model": "fsr3"})
    assert "model=fsr4fast" in ini("0")  # another game's change does not touch it
    # A stops, B (the same hidden id) starts: the copy is B's
    lv.unmirror(keep=[B])
    assert ini("0") is None and lv.mirrors == {}
    assert lv.mirror(B, "0") and "model=fsr3" in ini("0")
    lv.unmirror(keep=[B])
    assert "model=fsr3" in ini("0")
    # Off takes the game's live settings away: the copy goes with them
    lv.set(B, {"model": None})
    assert ini(B) is None and ini("0") is None
    lv.unmirror()
    # a game that reads its own appid needs no copy; a non-numeric id is never a file name
    assert lv.mirror(A, A) is False and lv.mirror(A, "../x") is False and lv.mirrors == {}
print("test_live_mirror OK")
