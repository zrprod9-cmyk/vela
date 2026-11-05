"""0.8.21: a non-Steam shortcut (Heroic, Lutris ...; appid >= 2^31) says that Vela does not support it yet, instead of "the
game is not installed". A Steam game that is not installed keeps its own reason.
Run: python -I tests/test_nonsteam.py"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import gamecheck  # noqa: E402

# the appid as the panel sends it: a decimal string (String(app.appid), or the digits of the /library/app/<id> route)
for shortcut in ("2147483648", "3123456789", "4294967295", 3123456789, "-1171510507"):
    assert gamecheck.is_shortcut(shortcut), shortcut
for steam in ("2147483647", "1817190", "4594150", 1817190, "0", "", None, "abc"):
    assert not gamecheck.is_shortcut(steam), steam

SCAN = {"install_dir": None, "exe_dir": None, "inputs": []}
assert gamecheck.verdict(SCAN, set(), appid="3123456789")["reason"] == "non_steam"
assert gamecheck.verdict(SCAN, set(), appid="3123456789")["kind"] == "none"
assert gamecheck.verdict(SCAN, set(), appid="1817190")["reason"] == "not_installed"
assert gamecheck.verdict(SCAN, set())["reason"] == "not_installed"
# anti-cheat is still read first, and a shortcut with a folder is judged by its files
assert gamecheck.verdict({**SCAN, "anticheat": True}, set(), appid="3123456789")["reason"] == "anticheat"
assert gamecheck.verdict({**SCAN, "install_dir": "/g"}, {"opengl"}, appid="3123456789")["reason"] == "opengl"

# the backend passes the appid, the panel has the text and offers no Steam FSR 1 line for it (nothing can be judged)
with open(os.path.join(ROOT, "main.py"), encoding="utf-8") as fh:
    assert "ag_assets=False, appid=appid)" in fh.read()
with open(os.path.join(ROOT, "src", "i18n", "en.json"), encoding="utf-8") as fh:
    en = json.load(fh)
assert en["reason.non_steam"] == "Vela works with Steam games for now; non-Steam shortcuts (Heroic, Lutris…) are not supported yet"
assert en["reason.not_installed"] == "the game is not installed"
with open(os.path.join(ROOT, "src", "index.tsx"), encoding="utf-8") as fh:
    assert re.search(r'const fsr1 = .*"not_installed", "non_steam"', fh.read())
print("test_nonsteam OK")
