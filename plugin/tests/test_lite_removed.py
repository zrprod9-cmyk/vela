"""0.8.15: FSR 4 Lite is out of Vela. A saved model=fsr4lite becomes
fsr4 and a saved lossy= disappears, silently, in the json and in the /tmp ini the fork reads (live.Live.get / restore_all); neither
is ever written again (set, _tmp_line); the other keys (NPU, ratio) are kept. The NPU knobs of the full FSR 4 (npukick, npuexact)
still go to the /tmp ini. A fresh temp folder per run (nothing is deleted). Run: python -I tests/test_lite_removed.py"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import live  # noqa: E402
import presets  # noqa: E402

assert presets.MODELS == ("fsr4", "fsr4fast", "fsr3", "sgsr2") and presets.FSR4_MODELS == ("fsr4", "fsr4fast")  # 0.8.16: + FSR 4 Fast
assert presets.RETIRED_MODELS == {"fsr4lite": "fsr4"}
assert "lossy" not in live.clean({"lossy": 4, "model": "fsr4"}) and live.clean({"model": "fsr4lite"}) == {"model": "fsr4"}
assert not hasattr(live, "LOSSY") and not hasattr(live, "LOSSY_DEFAULT")

tmp = tempfile.mkdtemp(prefix="vela_lite_")
saved_tmp_dir = live.TMP_DIR
live.TMP_DIR = os.path.join(tmp, "tmp")
try:
    lv = live.Live(os.path.join(tmp, "settings"), log=lambda *_: None)
    os.makedirs(lv.dir, exist_ok=True)

    def stored(appid, values):
        with open(os.path.join(lv.dir, appid + ".json"), "w") as f:
            json.dump(values, f)

    def on_disk(appid):
        with open(os.path.join(lv.dir, appid + ".json")) as f:
            return json.load(f)

    # ---- a saved Lite game (with its network rate and the NPU) is FSR 4 after the first read; json and /tmp file both clean
    stored("1", {"model": "fsr4lite", "lossy": 5, "ratio": "2.0", "npu": 2})
    # 0.8.15: ratio retired the same way
    assert lv.get("1") == {"model": "fsr4", "npu": 2}
    assert on_disk("1") == {"model": "fsr4", "npu": 2}
    ini = open(os.path.join(live.TMP_DIR, "1.ini")).read()
    assert "fsr4lite" not in ini and "lossy" not in ini and "model=fsr4\n" in ini, ini
    # a lossy key left under another model is dropped too, the model stays
    stored("2", {"model": "sgsr2", "lossy": 4})
    assert lv.get("2") == {"model": "sgsr2"} and on_disk("2") == {"model": "sgsr2"}
    # plugin start: restore_all migrates every record and writes the clean /tmp files (the reboot case: /tmp was empty)
    stored("3", {"model": "fsr4lite"})
    stored("4", {"model": "fsr4lite", "lossy": 1})
    os.remove(os.path.join(live.TMP_DIR, "1.ini"))
    lv.restore_all()
    assert on_disk("3") == {"model": "fsr4"} and on_disk("4") == {"model": "fsr4"}
    assert open(os.path.join(live.TMP_DIR, "3.ini")).read() == "model=fsr4\n"
    assert open(os.path.join(live.TMP_DIR, "1.ini")).read() == "model=fsr4\nnpu=2\n"

    # ---- never written again: a stale panel's pick or lossy knob
    assert lv.set("5", {"model": "fsr4lite"}) == {"model": "fsr4"}
    assert lv.set("5", {"lossy": 4}) == {"model": "fsr4"} and "lossy" not in on_disk("5")
    assert lv.set("5", {"model": "fsr4lite", "lossy": 6, "npu": 2}) == {"model": "fsr4", "npu": 2}
    assert "fsr4lite" not in open(os.path.join(live.TMP_DIR, "5.ini")).read()
    # NPU on or off never touches a network rate any more
    assert lv.set("5", {"npu": None}) == {"model": "fsr4"}

    # ---- the /tmp files we have no record of (a game Vela does not know yet): Lite becomes fsr4, lossy goes, the rest stays
    d = live.TMP_DIR
    with open(os.path.join(d, "77.ini"), "w") as f:
        f.write("model=fsr4lite\nlossy=4\nnpu=2\n")
    with open(os.path.join(d, "78.ini"), "w") as f:
        f.write("lossy=5\n")
    lv.restore_all()
    assert open(os.path.join(d, "77.ini")).read() == "model=fsr4\nnpu=2\n"
    assert not os.path.exists(os.path.join(d, "78.ini"))

    # ---- 0.8.15: npukick (developer knob) retired: dropped, never written
    assert lv.set("8", {"model": "fsr4", "npu": 2, "npukick": "early"}) == {"model": "fsr4", "npu": 2}
    assert "npukick" not in open(os.path.join(live.TMP_DIR, "8.ini")).read()
    # 0.8.10: NPU mode exact/async goes to the /tmp ini (the provider DLL reads npuexact=), kept next to npukick
    assert lv.set("8", {"npuexact": 0}) == {"model": "fsr4", "npu": 2, "npuexact": 0}
    assert "npuexact=0" in open(os.path.join(live.TMP_DIR, "8.ini")).read()
    assert lv.set("8", {"npuexact": 1})["npuexact"] == 1 and "npuexact=1" in open(os.path.join(live.TMP_DIR, "8.ini")).read()
    assert lv.set("8", {"npuexact": "0"})["npuexact"] == 0
    assert "npuexact" not in lv.set("8", {"npuexact": None})
    assert all("npuexact" not in live.clean({"npuexact": v}) for v in (2, -1, "exact", 0.5, ""))
    # other models: no lossy key appears
    assert lv.set("7", {"model": "fsr4"}) == {"model": "fsr4"}
finally:
    live.TMP_DIR = saved_tmp_dir
print("test_lite_removed: ok")
