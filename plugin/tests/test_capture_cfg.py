"""0.4.1: gamecfg (a game's settings keys with undo, Control's HDR). 0.8.13: the captures part left with Vela AI
(_removed/py_modules/captures.py)."""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import gamecfg  # noqa: E402

tmp = tempfile.mkdtemp(prefix="vela041_")
game = os.path.join(tmp, "Control")
os.makedirs(game)
RENDERER = ('{\n\t"objectType": "rend::UserRendererSettings",\n\t"m_iRenderResolutionX": 960,\n\t"m_eAllowHDR": 0,\n'
            '\t"m_bFilmGrain": false,\n\t"m_fHDRPaperWhiteNits": 203.0\n}\n')
ini = os.path.join(game, "renderer.ini")
open(ini, "w", newline="").write(RENDERER)
assert gamecfg.get_key(RENDERER, "m_eAllowHDR") == 0 and gamecfg.get_key(RENDERER, "m_bFilmGrain") is False
assert gamecfg.get_key(RENDERER, "m_fHDRPaperWhiteNits") == 203.0 and gamecfg.get_key(RENDERER, "nope") is None
cfg = gamecfg.GameCfg(os.path.join(tmp, "settings"), log=lambda *a: None)
C = {"file": "renderer.ini", "keys": {"m_eAllowHDR": 1}}

# game closed: written at once; off: the old value back, the rest of the file as the player left it
st = cfg.enable("870780", game, C, running=False)
assert st["on"] and not st["pending"] and '"m_eAllowHDR": 1' in open(ini).read(), open(ini).read()
s = open(ini).read().replace('"m_iRenderResolutionX": 960', '"m_iRenderResolutionX": 1280')
open(ini, "w", newline="").write(s)  # the player changes something else meanwhile
cfg.disable("870780", running=False)
assert open(ini).read() == RENDERER.replace("960", "1280") and not cfg.has_pending()

# game running: waits, the game rewrites its file on exit, then ours goes in
st = cfg.enable("870780", game, C, running=True)
assert st["pending"] and '"m_eAllowHDR": 0' in open(ini).read() and cfg.has_pending()
cfg.reconcile({"870780"})
assert '"m_eAllowHDR": 0' in open(ini).read()
cfg.reconcile(set())
assert '"m_eAllowHDR": 1' in open(ini).read() and not cfg.status("870780")["pending"]
# re-enable keeps the first original value; disable while running waits too
cfg.enable("870780", game, C, running=False)
cfg.disable("870780", running=True)
assert '"m_eAllowHDR": 1' in open(ini).read()
cfg.reconcile(set())
assert open(ini).read() == RENDERER.replace("960", "1280") and cfg.read("870780") is None
# no settings file yet (the game never ran): 0.8.16 waits for the game's first run, then the keys go in
st = cfg.enable("1", os.path.join(tmp, "none"), C, running=False)
assert st["on"] and st["pending"] and st["waiting"]
os.makedirs(os.path.join(tmp, "none"))
open(os.path.join(tmp, "none", "renderer.ini"), "w", newline="").write(RENDERER)
cfg.reconcile(set())
assert '"m_eAllowHDR": 1' in open(os.path.join(tmp, "none", "renderer.ini")).read() and not cfg.status("1")["pending"]
cfg.disable("1", running=False)
assert open(os.path.join(tmp, "none", "renderer.ini")).read() == RENDERER

print("ok")
