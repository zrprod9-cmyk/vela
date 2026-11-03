"""0.8.14: FSR 4 Ultra Performance on the NPU needs the sysext v9 graphs (nf4up_k1h*): the hint asks for v9 once.
0.8.15: Ultra is read from the sizes the log names (the game's own preset sets the render; no ratio of ours)."""
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "py_modules"))
import presets  # noqa: E402

assert presets.is_ultra("640x360", "1920x1080") and not presets.is_ultra("960x540", "1920x1080") and not presets.is_ultra(None, "1920x1080")
up = {"1920x1080": True, "1600x900": True, "1280x720": True}
v8 = {"installed": True, "running": True, "k1h_running": True, "k1h360_running": True, "k1h450_running": True,
      "k1h450_graph": True, "up_graph": False, "up_running": {}}
v9 = dict(v8, up_graph=True, up_running=up)
RENDER = {"3": "640x360", "2": "960x540"}


def hint(ratio, svc):
    p = presets.plan("fsr4", {"model": "fsr4", "npu": 2})
    st = {"npu": {"layer_found": True, "service": svc}, "running": True,
          "log": {"sgsr3": True, "mode": "fsr4", "render": RENDER[ratio], "output": "1920x1080"}}
    return (presets.hint(p, st) or {}).get("id")


assert hint("3", v8) == "npu_update_up"   # Ultra on v8: install v9
assert hint("3", v9) is None and hint("2", v8) is None  # v9 has Ultra; perf: v8 is enough
assert hint("3", dict(v9, up_running={"1920x1080": False})) == "npu_service"  # v9 in, the Ultra daemon down
# 0.8.16: the panel points at sysext v12 (v11 layout + the fastest device-exact graphs: UP = BU all26, P/exact = BP all);
# v10 stays only as the rollback, never as the hint
with open(os.path.join(os.path.dirname(__file__), "..", "src", "strings.ts"), encoding="utf-8") as fh:
    ts = fh.read()
# the public installer (release asset), never a path that exists only on the developers' console
assert "releases/download/v0.8.16/install-vela-npu.sh | sudo sh" in ts and ("fsr4" + "bench") not in ts
with open(os.path.join(os.path.dirname(__file__), "..", "src", "i18n", "en.json"), encoding="utf-8") as fh:
    ts += fh.read()
assert "(package 12)" in ts
assert "install_v10" not in ts and "install_v11" not in ts and "install_v9" not in ts and "(package 10)" not in ts
# installed but not running -> why: no Qualcomm runtime / a failed unit / not started
import presets  # noqa: E402
P = {"model": "fsr4", "kind": "fsr4"}
base = {"installed": True, "running": False, "up_graph": True, "k1h450_graph": True, "up_running": {}}
H = lambda svc: presets.hint(P, {"npu": {"layer_found": True, "service": svc}})["id"]
assert H(dict(base, qnn=False)) == "npu_qnn" and H(dict(base, qnn=True, unit_state="failed")) == "npu_failed"
assert H(dict(base, qnn=True, unit_state="inactive")) == "npu_service" and H(base) == "npu_service"
assert presets.hint(P, {"npu": {"layer_found": True, "service": {"installed": False}}}) == {"id": "npu_install"}
print("test_up_npu ok")
