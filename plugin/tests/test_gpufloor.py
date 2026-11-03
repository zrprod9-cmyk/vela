"""Checks for gpufloor.py (Vela 0.8.2): per-game GPU floor on a fake devfreq node; raise only, restore only Vela's own
value, the profile's max wins, userspace governor left alone. Temp folders only. Run: python -I tests/test_gpufloor.py"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import gpufloor  # noqa: E402

FREQS = "220000000 295000000 348000000 401000000 475000000 550000000 615000000 680000000"
tmp = tempfile.mkdtemp(prefix="vela_floor_")
try:
    node = os.path.join(tmp, "devfreq", "3d00000.gpu")
    os.makedirs(node)

    def put(name, v):
        with open(os.path.join(node, name), "w") as f:
            f.write(f"{v}\n")

    def get(name):
        with open(os.path.join(node, name)) as f:
            return int(f.read().strip())

    put("available_frequencies", FREQS); put("min_freq", 220000000); put("max_freq", 680000000)
    put("cur_freq", 295000000); put("governor", "simple_ondemand")
    settings = os.path.join(tmp, "settings")
    fl = gpufloor.GpuFloor(settings, log=lambda *_: None, devfreq_glob=os.path.join(tmp, "devfreq", "*.gpu"))

    assert gpufloor.at_least([1, 5, 9], 4) == 5 and gpufloor.at_least([1, 5, 9], 10) == 9
    # not enabled: nothing happens even while the game runs
    fl.reconcile({"620"}); assert get("min_freq") == 220000000
    fl.enable("620")
    assert fl.status("620")["on"] and fl.status("620")["mhz"] == gpufloor.VELA_GPU_MIN_MHZ
    # enabled, game not running: no floor
    fl.reconcile(set()); assert get("min_freq") == 220000000
    # set 400 MHz -> snapped up to the 401 MHz step while 620 runs
    fl.set_mhz("620", 400, {"620"}); assert get("min_freq") == 401000000, get("min_freq")
    assert fl.status("620")["applied"]
    # powerd applies a profile (min back to 220): the next reconcile puts the floor back
    put("min_freq", 220000000); fl.reconcile({"620"}); assert get("min_freq") == 401000000
    # game quits: Vela's value goes back to the one before it
    fl.reconcile(set()); assert get("min_freq") == 220000000 and not fl.status("620")["applied"]
    # a higher minimum is never lowered (Performance profile / pinned clock), and not "restored" over
    put("min_freq", 680000000); fl.reconcile({"620"}); assert get("min_freq") == 680000000
    fl.reconcile(set()); assert get("min_freq") == 680000000
    # the profile's max caps the floor (battery saver)
    put("min_freq", 220000000); put("max_freq", 348000000); fl.set_mhz("620", 680, {"620"})
    assert get("min_freq") == 348000000, get("min_freq")
    # powerd changed min to its own value meanwhile: not restored over it
    put("min_freq", 295000000); put("max_freq", 680000000); fl.reconcile(set()); assert get("min_freq") == 295000000
    # Armada's manual GPU clock (userspace): left alone
    put("governor", "userspace"); put("min_freq", 220000000); fl.reconcile({"620"}); assert get("min_freq") == 220000000
    put("governor", "simple_ondemand")
    # slider 0 = off; disable lifts it and keeps the value
    fl.set_mhz("620", 0, {"620"}); assert get("min_freq") == 220000000
    fl.set_mhz("620", 550, {"620"}); assert get("min_freq") == 550000000
    fl.disable("620", {"620"}); assert get("min_freq") == 220000000 and fl.status("620")["mhz"] == 550
    # persisted across a reload; the highest of two running games wins
    fl2 = gpufloor.GpuFloor(settings, log=lambda *_: None, devfreq_glob=os.path.join(tmp, "devfreq", "*.gpu"))
    assert fl2.status("620")["mhz"] == 550 and not fl2.status("620")["on"]
    fl2.enable("620"); fl2.enable("700"); fl2.set_mhz("700", 615, {"620", "700"})
    assert get("min_freq") == 615000000
    fl2.release(); assert get("min_freq") == 220000000
    # no GPU node: harmless
    gpufloor.GpuFloor(settings, log=lambda *_: None, devfreq_glob=os.path.join(tmp, "none", "*")).reconcile({"620"})
    print("test_gpufloor OK")
finally:
    shutil.rmtree(tmp)
