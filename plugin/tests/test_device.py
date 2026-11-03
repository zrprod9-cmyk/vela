"""0.8.16: chip detection (py_modules/device.py) on mocked device trees, and what it changes: SM8550 (the values the
Retroid Pocket 6 console reports) = everything as in 0.8.16; SM8750 with / without a v79 graph set;
an unknown chip. Run: python tests/test_device.py"""
import os
import sys
import tempfile
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import device  # noqa: E402
import npu  # noqa: E402
import presets  # noqa: E402

tmp = tempfile.mkdtemp(prefix="vela_dev_")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def tree(name, compatible, soc_id, machine, gpu):
    r = os.path.join(tmp, name)
    write(os.path.join(r, "proc/device-tree/compatible"), "\0".join(compatible) + "\0")
    if soc_id is not None:
        write(os.path.join(r, "sys/devices/soc0/soc_id"), f"{soc_id}\n")
        write(os.path.join(r, "sys/devices/soc0/machine"), machine + "\n")
    if gpu:
        write(os.path.join(r, "proc/device-tree/soc@0/gpu@3d00000/compatible"), f"qcom,adreno-{gpu}\0qcom,adreno\0")
    return r


rp6 = tree("rp6", ["retroidpocket,rp6", "qcom,qcs8550", "qcom,sm8550"], 603, "QCS8550", "43050a01")
odin3 = tree("odin3", ["ayntec,odin3", "qcom,sm8750"], 618, "SM8750", "44050001")
sid_only = tree("sid", ["vendor,board"], 618, "SM8750", None)  # no gpu node, no qcom compatible: soc_id decides
other = tree("other", ["vendor,x", "qcom,sm8650"], 557, "SM8650", "43051401")
empty = os.path.join(tmp, "empty")
os.makedirs(empty)

d = device.detect(rp6)
assert d["soc"] == "SM8550" and d["gpu"] == "Adreno 740" and d["npu_arch"] == "v73" and d["soc_id"] == 603, d
d8 = device.detect(odin3)
assert d8["soc"] == "SM8750" and d8["gpu"] == "Adreno 830" and d8["npu_arch"] == "v79" and d8["gpu_chip"] == "44050001", d8
ds = device.detect(sid_only)
assert ds["soc"] == "SM8750" and ds["gpu"] == "Adreno 830" and ds["gpu_chip"] is None, ds
du = device.detect(other)
assert du["soc"] is None and du["gpu"] == "Adreno 43051401" and du["npu_arch"] is None, du
de = device.detect(empty)  # a PC / no device tree: unknown
assert de["soc"] is None and de["gpu"] is None and de["npu_arch"] is None, de

# policy
v79 = os.path.join(tmp, "v79")
p = device.policy(d, (v79,))
assert p["npu"] and p["fsr4fast"] and p["npu_graphs"] == "v73" and p["note"] is None and p["unavailable"] == [], p
p8 = device.policy(d8, (v79,))
assert not p8["npu"] and not p8["fsr4fast"] and p8["npu_graphs"] is None and "v79" in p8["note"], p8
os.makedirs(v79)
assert not device.policy(d8, (v79,))["npu"]  # an empty folder is no graph set
write(os.path.join(v79, "nf4_k1h.bin"), b"graph")
p8g = device.policy(d8, (os.path.join(tmp, "nope"), v79))
assert p8g["npu"] and p8g["fsr4fast"] and p8g["npu_graphs"] == "v79" and p8g["graph_dir"] == v79, p8g
assert not device.policy(d, (v79,))["unavailable"]  # SM8550 ignores the v79 folder
pu = device.policy(du, (v79,))
assert not pu["npu"] and not pu["fsr4fast"] and "SM8650" in pu["note"], pu
pe = device.policy(de, (v79,))
assert not pe["npu"] and not pe["fsr4fast"] and "unknown chip" in pe["note"], pe

st = device.state(d8, p8)
assert st["soc"] == "SM8750" and st["gpu"] == "Adreno 830" and st["npu_arch"] == "v79" and not st["npu"], st
assert device.state(de, pe)["soc"] == "unknown"

# Npu.ready(): the chip gate comes on top of the base checks; chip=None and SM8550 the same
def npu_with(chip):
    n = npu.Npu(os.path.join(tmp, "a"), os.path.join(tmp, "h"), os.path.join(tmp, "s"), log=lambda *a: None, chip=chip)
    n.layer_found = lambda: True
    n.service = lambda fresh=False: {"installed": True}
    return n


assert npu_with(None).ready() and npu_with(p).ready() and npu_with(p8g).ready()
assert not npu_with(p8).ready() and not npu_with(pu).ready() and not npu_with(pe).ready()
n0 = npu_with(p)
n0.service = lambda fresh=False: {"installed": False}
assert not n0.ready()  # SM8550 without the component: as without a chip gate

# Plugin: the models list and the auto model per chip (fake decky module, no __init__ run)
dk = types.ModuleType("decky")
dk.logger = types.SimpleNamespace(info=lambda *a: None, error=lambda *a: None, exception=lambda *a: None)
dk.DECKY_USER_HOME = os.path.join(tmp, "home")
dk.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "ps")
sys.modules["decky"] = dk
sys.path.insert(0, ROOT)
import main  # noqa: E402

s3 = {"fsr4_available": True, "fsr3_available": True}
ready = {"ready": True, "layer_found": True, "service": {"installed": True}}
P = main.Plugin()
offered = P._models("fsr4", s3)
assert offered == presets.models_for("fsr4", True, True) and all(ok for _, ok in offered), offered  # no chip: nothing is taken away
assert P._default_model("fsr4", s3, ready) == presets.default_model("fsr4", True, ready)
P.dev, P.chip = d, p  # SM8550
assert P._models("fsr4", s3) == offered and P._models("sgsr2", s3) == presets.models_for("sgsr2", True, True)
for kind in ("fsr4", "sgsr2", "static"):
    for nst in (ready, None, {"ready": False}):
        assert P._default_model(kind, s3, nst) == presets.default_model(kind, True, nst), (kind, nst)
P.dev, P.chip = d8, p8  # SM8750, no v79 graphs
m8 = dict(P._models("fsr4", s3))
assert m8 == {"fsr4": True, "fsr4fast": False, "fsr3": True, "sgsr2": True}, m8
assert P._default_model("fsr4", s3, {"ready": False}) == "fsr3"  # not fsr4fast
assert P._default_model("fsr4", {"fsr4_available": True, "fsr3_available": False}, None) == "sgsr2"
P.dev, P.chip = d8, p8g  # SM8750 with v79 graphs: all models
assert all(dict(P._models("fsr4", s3)).values())
P.dev, P.chip = du, pu  # unknown chip
assert dict(P._models("fsr4", s3)) == {"fsr4": False, "fsr4fast": False, "fsr3": True, "sgsr2": True}  # no FSR 4
print("test_device: OK")
