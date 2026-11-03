"""Vela 0.8.16: model fsr4fast ("FSR 4 Fast"). The panel offers it next to FSR 4 wherever FSR 4 can run, writes
model=fsr4fast (the FSR 4 provider reads it at context create), keeps FSR 4's NPU setup, and reads the logs of the
RE4 interceptor (Model::Fsr4Fast: "model fsr4fast", fps label "FSR 4 Fast") and of the fork (patch 0018: the FSR 4 VK context
line under "SGSR3: Vela live model fsr4fast"). Run: python -I tests/test_fsr4fast.py"""
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import live  # noqa: E402
import presets  # noqa: E402
import sgsr3  # noqa: E402

TMP = tempfile.mkdtemp(prefix="vela_fsr4fast_")
NPU_ON = {"layer_found": True, "service": {"installed": True, "running": True}}

assert "fsr4fast" in presets.MODELS and "fsr4fast" in presets.FSR4_MODELS
assert presets.models_for("fsr4", fsr4=False)[1] == ("fsr4fast", False)  # no provider: neither FSR 4 nor Fast
assert presets.models_for("sgsr2") == [("sgsr2", True)]  # DX11: SGSR 2 only
assert presets.plan("static", {"model": "fsr4fast", "npu": 2}) == {"kind": "static", "model": "fsr4fast", "npu": True}
assert presets.plan("sgsr2", {"model": "fsr4fast"})["model"] == "sgsr2"
w = presets.live_wanted("fsr4fast", NPU_ON)
assert w["model"] == "fsr4fast" and (w["npu"], w["npuexact"]) == (presets.live_wanted("fsr4", NPU_ON)["npu"], 1), w
assert live.clean({"model": "fsr4fast"}) == {"model": "fsr4fast"}
assert live._tmp_line("model=fsr4fast\n") == "model=fsr4fast\n"

# the hidden opt-in fastup15=1 (Fast at 1080p UP as P720 + separate 1.5x; the provider reads it) survives clean / set / the /tmp file
assert live.clean({"model": "fsr4fast", "fastup15": 1}) == {"model": "fsr4fast", "fastup15": 1}
assert live.clean({"fastup15": "1"}) == {"fastup15": 1} and live.clean({"fastup15": 0}) == {"fastup15": 0}
assert all("fastup15" not in live.clean({"fastup15": v}) for v in (2, -1, "on", 1.0, True, ""))
assert live._tmp_line("fastup15=1\n") == "fastup15=1\n"
_ub = tempfile.mkdtemp(prefix="vela_ub_"); _tmp0 = live.TMP_DIR; live.TMP_DIR = os.path.join(_ub, "tmp")
_lv = live.Live(os.path.join(_ub, "s"), log=lambda *a: None); os.makedirs(_lv.dir)
with open(_lv._json("9"), "w", encoding="utf-8") as _f:
    _f.write('{"model": "fsr4fast", "npu": 2, "fastup15": 1}')
assert _lv.set("9", {"npuexact": 1}) == {"model": "fsr4fast", "npu": 2, "npuexact": 1, "fastup15": 1}  # a panel change keeps it
assert "fastup15=1\n" in open(os.path.join(live.TMP_DIR, "9.ini"), encoding="utf-8").read()
assert "fastup15" not in presets.live_wanted("fsr4fast", NPU_ON)  # the panel never writes it
# mode 3 is the provider's default; the hidden off-switch fastup15=0 survives clean / _tmp_line / set / restore_all and reaches the /tmp file
assert live._tmp_line("fastup15=0\n") == "fastup15=0\n" and live.clean({"model": "fsr4fast", "fastup15": "0"}) == {"model": "fsr4fast", "fastup15": 0}
with open(_lv._json("9"), "w", encoding="utf-8") as _f:
    _f.write('{"model": "fsr4fast", "npu": 2, "fastup15": 0}')
assert _lv.set("9", {"model": "fsr4"}) == {"model": "fsr4", "npu": 2, "fastup15": 0}  # a panel model change keeps the off-switch
assert _lv.set("9", {"model": "fsr4fast"}) == {"model": "fsr4fast", "npu": 2, "fastup15": 0}
assert "fastup15=0\n" in open(os.path.join(live.TMP_DIR, "9.ini"), encoding="utf-8").read()
os.remove(os.path.join(live.TMP_DIR, "9.ini")); _lv.restore_all()  # after a reboot (empty /tmp) the off-switch comes back
assert "fastup15=0\n" in open(os.path.join(live.TMP_DIR, "9.ini"), encoding="utf-8").read()
assert "fastup15" not in presets.live_wanted("fsr4fast", NPU_ON) and "fastup15" not in presets.live_wanted("fsr4", NPU_ON)
live.TMP_DIR = _tmp0


def write(name, text):
    p = os.path.join(TMP, name)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return p


# RE4 interceptor: ModelName "fsr4fast", ModelLabel "FSR 4 Fast" ext_upscalers.cpp)
RE4 = (r"[20:44:43.312] vela: live (Z:\tmp\vela-live\2050650.ini): model fsr4fast lossy 1 npu 2" "\n"
       r"[20:44:43.338] vela: fsr4 context retired (released after 16 presents)" "\n"
       r"[20:44:43.366] Vela FSR 4 VK: FSR 4.1.1 VK INT8 context 640x360 -> 1920x1080 (hdr 0, network every 1 frames)" "\n"
       r"[20:44:43.833] vela: model fsr4fast runs 640x360 -> 1920x1080" "\n"
       r"[20:44:51.800] fps 59.8 (presents 598 in 10.0 s; frames by Vela 598), FSR 4 Fast GPU 1.90 ms" "\n")
s = sgsr3.static_log_summary(write("sgsr3intercept.log", RE4))
assert (s["mode"], s["fsr4_fallback"], s["fsr4_version"]) == ("fsr4fast", None, "4.1.1"), s
s = sgsr3.static_log_summary(write("sgsr3intercept2.log", RE4.split("[20:44:43.833]")[0].replace(
    "Vela FSR 4 VK", "vela: FSR 4.1 Lite runs only x").replace("context 640x360", "y") +
    r"[20:44:43.835] vela: model fsr2 runs 640x360 -> 1920x1080" "\n"))
assert s["mode"] == "fsr2" and s["fsr4_fallback"] == "failed", s

# fork (OptiScaler.log): patch 0018 logs the Fast feature exactly like FSR 4; the live model names it
HEAD = (r"[20:04:50.201242] [I] InitInternal Initializing SGSR3 960x540 -> 1920x1080" "\n"
        r"[20:04:50.226999] [I] Poll Vela live (Z:\tmp\vela-live\870780.ini): model fsr4fast ratio -1 amax -1 alphak -1 mk -1 "
        r"fresh -1 sq -1 ncap -1 disocc -1 lossy 1 npu 2" "\n"
        r"[20:04:50.227074] [I] CreatePipelines SGSR3: Vela live model fsr4fast" "\n")
CTX = r"[20:04:50.300000] [I] Init Vela FSR 4 VK: FSR 4.1.1 VK INT8 context 960x540 -> 1920x1080 (hdr true)" "\n"
o = sgsr3.log_summary(write("OptiScaler.log", HEAD + CTX))
assert (o["mode"], o["fsr4_fallback"], o["fsr4_version"]) == ("fsr4fast", None, "4.1.1"), o
o = sgsr3.log_summary(write("OptiScaler2.log", HEAD.replace("fsr4fast", "fsr4") + CTX))
assert o["mode"] == "fsr4", o  # exact FSR 4 stays fsr4
DOWN = (r"[20:05:00.000000] [W] Evaluate SGSR3: FSR 4 VK is not available here; SGSR 2 instead" "\n"
        r"[20:05:00.010000] [W] TryEvaluateOptiFeature Vela live: fsr4fast cannot run in this game; SGSR 2 instead" "\n"
        r"[20:05:00.010100] [I] TryEvaluateOptiFeature Vela live: handle 1 model fsr4fast -> SGSR 2 (the game draws 960x540)" "\n"
        r"[20:05:00.020000] [I] InitInternal Initializing SGSR2 960x540 -> 1920x1080" "\n")
o = sgsr3.log_summary(write("OptiScaler3.log", HEAD + DOWN))
assert (o["mode"], o["fsr4_fallback"], o["sgsr2_instead"]) == ("sgsr2", "failed", "fsr4fast"), o

# panel: the fourth card, name + note (English only since 0.8.16), NPU status for Fast like FSR 4
ts = open(os.path.join(HERE, "..", "src", "strings.ts"), encoding="utf-8").read()
assert 'MODEL_IDS: ModelId[] = ["fsr4", "fsr4fast", "fsr3", "sgsr2"]' in ts
en = open(os.path.join(HERE, "..", "src", "i18n", "en.json"), encoding="utf-8").read()  # EN: English texts only
assert 'fsr4fast: "FSR 4 Fast"' in ts and re.search(r'"model.note.fsr4fast": "AMD neural network · [^"]+"', en)
tsx = open(os.path.join(HERE, "..", "src", "index.tsx"), encoding="utf-8").read()
assert 'L.mode === "fsr4" || L.mode === "fsr4fast"' in tsx
print("test_fsr4fast OK")
assert "427567f6d2c7abe12ca170f7bf603814" in sgsr3.OLD_STATIC_DXGI  # the 0.8.15 RE4 interceptor is replaced by the Fast-aware one
print("test_fsr4fast OLD_STATIC_DXGI OK")
