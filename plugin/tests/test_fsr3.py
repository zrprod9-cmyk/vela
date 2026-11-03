"""Checks for the FSR 3.1 live model (0.8.2): live.py takes model=fsr3; the panel offers it only with a fork that has
patch 0014 (VERSION.json "fsr3_live") and the FidelityFX DLL in assets; OptiScaler.log lines of the FFX backend give the
status (FSR 3.1.x running, or failed). A fresh temp folder per run (left in the system temp dir, nothing is deleted).
Run: python -I tests/test_fsr3.py  (or python -m unittest discover -s tests)"""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import live  # noqa: E402
import sgsr3  # noqa: E402

TMP = tempfile.mkdtemp(prefix="vela_fsr3_")

HEAD = "Poll Vela live (Z:\\tmp\\vela-live\\2239710.ini): model fsr4lite ratio 2\n" \
       "Initializing SGSR3 960x540 -> 1920x1080\nSGSR3: Vela live model fsr4lite\n"
SWITCH = "Vela live: handle 1000003 model fsr3 -> FSR 3.1.4 (the game draws 960x540)\n"
VERS = "Vela: FFX upscaler versions: 0=3.1.4, 1=2.3.2\n"


def summary(text):
    p = os.path.join(TMP, "OptiScaler.log")
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    return sgsr3.log_summary(p)


class Fsr3(unittest.TestCase):
    def test_live_model(self):
        self.assertIn("fsr3", live.MODELS)
        self.assertEqual(live.clean({"model": "fsr3", "ratio": "1.5"}), {"model": "fsr3"})  # 0.8.15: no ratio key
        saved = live.TMP_DIR
        live.TMP_DIR = os.path.join(TMP, "tmp")
        try:
            lv = live.Live(os.path.join(TMP, "settings"), log=lambda *_: None)
            lv.set("7", {"model": "fsr4lite"})  # Lite N=5 by default
            v = lv.set("7", {"model": "fsr3", "lossy": None, "ratio": "2.0"})
            self.assertEqual(v, {"model": "fsr3"})
            with open(os.path.join(live.TMP_DIR, "7.ini"), encoding="utf-8") as f:
                self.assertEqual(f.read(), "model=fsr3\n")
        finally:
            live.TMP_DIR = saved

    def test_available(self):
        assets = os.path.join(TMP, "assets")
        os.makedirs(assets, exist_ok=True)
        s = sgsr3.Sgsr3(assets, os.path.join(TMP, "home"), os.path.join(TMP, "settings"), log=lambda *_: None)
        s.version = {"fsr3_live": True}
        self.assertFalse(s.fsr3_available())  # no FidelityFX DLL in assets
        with open(os.path.join(assets, sgsr3.FG_DLL), "wb") as f:
            f.write(b"MZ")
        self.assertTrue(s.fsr3_available())
        s.version = {}
        self.assertFalse(s.fsr3_available())  # a fork without patch 0014

    def test_log_running(self):
        lg = summary(HEAD + SWITCH + VERS + "Vela: FFX upscaler index 0 (3.1.4) for FSR 3\n")
        self.assertEqual((lg["mode"], lg["fsr3_version"], lg["render"], lg["fsr3_fallback"]),
                         ("fsr3", "3.1.4", "960x540", None), lg)
        self.assertTrue(lg["sgsr3"])
        # from the game's start (no SGSR init at all) it still counts as running
        lg = summary(SWITCH + VERS + "Vela: FFX upscaler index 0 (3.1.4) for FSR 3\n")
        self.assertEqual((lg["mode"], lg["sgsr3"]), ("fsr3", True), lg)

    def test_log_failed_and_back(self):
        lg = summary(HEAD + SWITCH + "can't load amd_fidelityfx_dx12.dll methods!\n")
        self.assertEqual((lg["fsr3_fallback"], lg["fsr3_version"]), ("failed", None), lg)
        self.assertNotEqual(lg["mode"], "fsr3")
        lg = summary(HEAD + SWITCH + VERS + "Vela: FFX upscaler index 0 (3.1.4) for FSR 3\n"
                     "_createContext error: FFX_API_RETURN_ERROR\n")
        self.assertEqual(lg["fsr3_fallback"], "failed", lg)
        # switched back to Lite: the FSR 3 lines before the switch do not count
        lg = summary(SWITCH + "Vela: FFX upscaler index 0 (3.1.4) for FSR 3\n"
                     "Vela live: handle 1000003 model fsr4lite -> SGSR 3 (the game draws 960x540)\n"
                     "Initializing SGSR3 960x540 -> 1920x1080\n")
        self.assertNotEqual(lg["mode"], "fsr3")
        self.assertIsNone(lg["fsr3_fallback"])


if __name__ == "__main__":
    unittest.main()
