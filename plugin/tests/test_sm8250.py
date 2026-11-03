"""The SM8250 / Adreno 650 has no HTP NPU; FSR 4 on its GPU alone is an estimated 50-80 ms per
frame): on every chip other than SM8550 / SM8750 the plugin does not offer FSR 4 or FSR 4 Fast. The models list marks them
off with why = "chip_no_fsr4" (the panel shows en.json model.why.chip_no_fsr4), auto picks FSR 3 (else SGSR 2), a hand pick of
FSR 4 is refused (main._apply_model checks the same _models), and a game that was set to FSR 4 before is moved to the chip's model. SM8550 / SM8750 stay as before.
Run: python tests/test_sm8250.py"""
import json
import os
import sys
import tempfile
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import device  # noqa: E402
import presets  # noqa: E402

tmp = tempfile.mkdtemp(prefix="vela_8250_")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data.encode())


# the Retroid Pocket 5 (SM8250): Linux qcom,ids.h QCOM_ID_SM8250 = 356, sm8250.dtsi gpu "qcom,adreno-650.2"
rp5 = os.path.join(tmp, "rp5")
write(os.path.join(rp5, "proc/device-tree/compatible"), "retroidpocket,rp5\0qcom,sm8250\0")
write(os.path.join(rp5, "sys/devices/soc0/soc_id"), "356\n")
write(os.path.join(rp5, "sys/devices/soc0/machine"), "SM8250\n")
write(os.path.join(rp5, "proc/device-tree/soc@0/gpu@3d00000/compatible"), "qcom,adreno-650.2\0qcom,adreno\0")
d5 = device.detect(rp5)
assert d5["soc"] is None and d5["machine"] == "SM8250" and d5["gpu"] == "Adreno 650" and d5["npu_arch"] is None, d5
en = json.load(open(os.path.join(ROOT, "src", "i18n", "en.json"), encoding="utf-8"))
NOTE = "FSR 4 needs the Snapdragon 8 Gen 2 or newer NPU; use FSR 3 or SGSR 2"
assert device.NO_FSR4 == NOTE == en["model.why.chip_no_fsr4"]

for dev, name in ((d5, "SM8250"), (device.detect(os.path.join(tmp, "none")), "unknown chip"),
                  ({"soc": None, "gpu": "Adreno 660"}, "Adreno 660")):
    p = device.policy(dev, ())
    assert not p["npu"] and not p["fsr4"] and not p["fsr4fast"], p
    assert p["note"] == f"Chip {name}: {NOTE}" == en["hint.chip_other"].replace("{chip}", name), p["note"]
    assert any("FSR 4" in u and NOTE in u for u in p["unavailable"]), p
    st = device.state(dev, p)
    assert st["fsr4"] is False and st["fsr4fast"] is False, st
# SM8550 / SM8750 keep FSR 4
for soc in ("SM8550", "SM8750"):
    p = device.policy({"soc": soc}, ())
    assert p["fsr4"] and device.state({"soc": soc}, p)["fsr4"], (soc, p)
assert device.policy({"soc": "SM8550"}, ())["fsr4fast"] and not device.policy({"soc": "SM8750"}, ())["fsr4fast"]

# the plugin (fake decky module, no __init__ run)
dk = types.ModuleType("decky")
dk.logger = types.SimpleNamespace(info=lambda *a: None, error=lambda *a: None, exception=lambda *a: None)
dk.DECKY_USER_HOME = os.path.join(tmp, "home")
dk.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "ps")
sys.modules["decky"] = dk
sys.path.insert(0, ROOT)
import main  # noqa: E402

s3 = {"fsr4_available": True, "fsr3_available": True}
P = main.Plugin()
P.dev, P.chip = d5, device.policy(d5, ())
for kind in ("fsr4", "static"):
    assert dict(P._models(kind, s3)) == {"fsr4": False, "fsr4fast": False, "fsr3": True, "sgsr2": True}, kind
    for nst in (None, {"ready": False}, {"ready": True, "layer_found": True, "service": {"installed": True}}):
        assert P._default_model(kind, s3, nst) == "fsr3", (kind, nst)
    assert P._default_model(kind, {"fsr4_available": True, "fsr3_available": False}, None) == "sgsr2"
assert dict(P._models("sgsr2", s3)) == {"sgsr2": True}
assert P._chip_off("fsr4") == P._chip_off("fsr4fast") == "chip_no_fsr4"
assert P._chip_off("fsr3") is None and P._chip_off("sgsr2") is None and P._chip_off("off") is None


# _model_state: the panel's list carries why; a game set to FSR 4 before is moved to FSR 3 (live model)
class Live:
    def __init__(self, lv):
        self.lv, self.sets = dict(lv), []

    def set(self, appid, want):
        self.sets.append(dict(want))
        self.lv.update(want)
        return dict(self.lv)


P.check = types.SimpleNamespace(check=lambda *a, **k: {"kind": "fsr4", "reason": None})
P.s3 = types.SimpleNamespace(set_ini=lambda *a, **k: None, api_args=lambda appid: "")
P._launch_pending = lambda appid, s3: False
P._win_restart = set()
P.up = types.SimpleNamespace(cfg={})
P.lat = types.SimpleNamespace(wanted=lambda appid: {})
for before in ("fsr4", "fsr4fast"):
    P.live = Live({"model": before})
    gs = {"scan": {}, "running": False, "anygame": {},
          "sgsr3": {**s3, "installed": True, "npu": None, "live": {"model": before}, "opts": {"ratio": "game"}}}
    out = P._model_state("1", gs)
    ms = {m["id"]: m for m in out["models"]}
    assert ms["fsr4"] == {"id": "fsr4", "ok": False, "why": "chip_no_fsr4"} and ms["fsr4fast"]["why"] == "chip_no_fsr4", ms
    assert ms["fsr3"] == {"id": "fsr3", "ok": True, "why": None} and ms["sgsr2"]["ok"], ms
    assert out["auto_model"] == "fsr3" and out["model"] == "fsr3", out
    assert P.live.sets and P.live.sets[-1]["model"] == "fsr3", P.live.sets

# SM8550: unchanged (FSR 4 offered, why None everywhere, an FSR 4 game stays FSR 4)
P.dev, P.chip = {"soc": "SM8550"}, device.policy({"soc": "SM8550"}, ())
P.live = Live({"model": "fsr4"})
gs = {"scan": {}, "running": False, "anygame": {},
      "sgsr3": {**s3, "installed": True, "npu": None, "live": {"model": "fsr4"}, "opts": {"ratio": "game"}}}
out = P._model_state("1", gs)
assert all(m["ok"] and m["why"] is None for m in out["models"]) and out["model"] == "fsr4", out
print("test_sm8250: OK")
