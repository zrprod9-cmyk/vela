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
kaanapali = tree("kaanapali", ["vendor,kaanapali", "qcom,kaanapali"], 660, "SM8850", "44050a01")  # mainline Linux kaanapali.dtsi
canoe = tree("canoe", ["vendor,canoe", "qcom,canoe"], 661, "CANOEP", "44050a01")
sm8850_sid = tree("sm8850sid", ["vendor,board"], 660, "SM8850", None)  # no gpu node, no qcom compatible: soc_id decides
tb321fu = tree("tb321fu", ["lenovo,tb321fu", "qcom,sm8650"], 577, "Lenovo Legion Y700 (2025) / TB321FU", "43051401")
sm8650_sid = tree("sm8650sid", ["vendor,board"], 557, "SM8650", None)  # no gpu node, no qcom compatible: soc_id decides
other = tree("other", ["vendor,x", "qcom,sm8450"], 457, "SM8450", "730.1")  # Linux sm8450.dtsi "qcom,adreno-730.1"
empty = os.path.join(tmp, "empty")
os.makedirs(empty)

d = device.detect(rp6)
assert d["soc"] == "SM8550" and d["gpu"] == "Adreno 740" and d["npu_arch"] == "v73" and d["soc_id"] == 603, d
d8 = device.detect(odin3)
assert d8["soc"] == "SM8750" and d8["gpu"] == "Adreno 830" and d8["npu_arch"] == "v79" and d8["gpu_chip"] == "44050001", d8
ds = device.detect(sid_only)
assert ds["soc"] == "SM8750" and ds["gpu"] == "Adreno 830" and ds["gpu_chip"] is None, ds
dk88 = device.detect(kaanapali)
assert dk88["soc"] == "SM8850" and dk88["gpu"] == "Adreno 840" and dk88["npu_arch"] == "v81" and dk88["gpu_chip"] == "44050a01", dk88
d88 = device.detect(canoe)
assert d88["soc"] == "SM8850" and d88["gpu"] == "Adreno 840" and d88["npu_arch"] == "v81" and d88["gpu_chip"] == "44050a01", d88
ds88 = device.detect(sm8850_sid)
assert ds88["soc"] == "SM8850" and ds88["gpu"] == "Adreno 840" and ds88["npu_arch"] == "v81" and ds88["gpu_chip"] is None, ds88
dt = device.detect(tb321fu)
assert dt["soc"] == "SM8650" and dt["gpu"] == "Adreno 750" and dt["npu_arch"] == "v75" and dt["soc_id"] == 577, dt
d65 = device.detect(sm8650_sid)
assert d65["soc"] == "SM8650" and d65["gpu"] == "Adreno 750" and d65["gpu_chip"] is None, d65
du = device.detect(other)
assert du["soc"] is None and du["gpu"] == "Adreno 730.1" and du["npu_arch"] is None, du
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
v81 = os.path.join(tmp, "v81")
p88_2 = device.policy(d88, (v79,))  # 2-arg caller stays isolated from host V81_DIRS
assert p88_2["npu"] and p88_2["fsr4fast"] and p88_2["npu_graphs"] == "v73" and "graph_dir" not in p88_2 and p88_2["note"] is None and p88_2["unavailable"] == [], p88_2
p88 = device.policy(d88, (v79,), (v81,))
assert p88["npu"] and p88["fsr4fast"] and p88["npu_graphs"] == "v73" and "graph_dir" not in p88 and p88["note"] is None and p88["unavailable"] == [] and not p88.get("anf"), p88
os.makedirs(v81)
assert device.policy(d88, (v79,), (v81,))["npu_graphs"] == "v73"
write(os.path.join(v81, "nf4_k1h.bin"), b"graph")
p88g = device.policy(d88, (v79,), (os.path.join(tmp, "nope"), v81))
assert p88g["npu"] and p88g["fsr4fast"] and p88g["npu_graphs"] == "v81" and p88g["graph_dir"] == v81 and not p88g.get("anf"), p88g
assert not device.policy(d, (v79,))["unavailable"]  # SM8550 ignores the v79 folder
pt = device.policy(dt, (v79,))  # SM8650 = SM8550: v73 graphs, FSR 4 Fast, no note
assert pt["npu"] and pt["fsr4"] and pt["fsr4fast"] and pt["npu_graphs"] == "v73" and pt["note"] is None and pt["unavailable"] == [], pt
pu = device.policy(du, (v79,))
assert not pu["npu"] and not pu["fsr4fast"] and "SM8450" in pu["note"], pu
pe = device.policy(de, (v79,))
assert not pe["npu"] and not pe["fsr4fast"] and "unknown chip" in pe["note"], pe

st = device.state(d8, p8)
assert st["soc"] == "SM8750" and st["gpu"] == "Adreno 830" and st["npu_arch"] == "v79" and not st["npu"], st
st88 = device.state(d88, p88)
assert st88["soc"] == "SM8850" and st88["gpu"] == "Adreno 840" and st88["npu_arch"] == "v81" and st88["npu"] and st88["npu_graphs"] == "v73" and not st88["anf"], st88
assert device.state(de, pe)["soc"] == "unknown"

# Npu.ready(): the chip gate comes on top of the base checks; chip=None and SM8550 the same
# 0.8.21: the NPU part installed while the plugin runs is seen at the next panel read, and removing it too
class _Npu:
    chip = None


class _Owner:
    def __init__(self, dev, chip):
        self.dev, self.chip, self.npu = dev, chip, _Npu()
        self.npu.chip = chip


v79_late = os.path.join(tmp, "v79_late")
os.makedirs(v79_late, exist_ok=True)
ow = _Owner(d8, device.policy(d8, (v79_late,)))
assert not ow.chip["npu"] and not ow.chip["fsr4fast"] and not device.refresh(ow, (v79_late,))
write(os.path.join(v79_late, "nf4_k1h.bin"), b"x")
said = []
assert device.refresh(ow, (v79_late,), log=said.append) and said
assert ow.chip["npu"] and ow.chip["fsr4fast"] and ow.chip["graph_dir"] == v79_late and ow.npu.chip is ow.chip
assert not device.refresh(ow, (v79_late,))
os.replace(os.path.join(v79_late, "nf4_k1h.bin"), os.path.join(v79_late, "nf4_k1h.old"))
assert device.refresh(ow, (v79_late,)) and not ow.chip["npu"] and not ow.npu.chip["fsr4fast"]
v81_late = os.path.join(tmp, "v81_late")
os.makedirs(v81_late, exist_ok=True)
ow88 = _Owner(d88, device.policy(d88, (), (v81_late,)))
assert ow88.chip["npu"] and ow88.chip["npu_graphs"] == "v73" and not device.refresh(ow88, (), (v81_late,))
write(os.path.join(v81_late, "nf4_k1h.bin"), b"x")
said88 = []
assert device.refresh(ow88, (), (v81_late,), log=said88.append) and said88
assert ow88.chip["npu"] and ow88.chip["npu_graphs"] == "v81" and ow88.chip["graph_dir"] == v81_late and ow88.npu.chip is ow88.chip
assert not device.refresh(ow88, (), (v81_late,))
os.replace(os.path.join(v81_late, "nf4_k1h.bin"), os.path.join(v81_late, "nf4_k1h.old"))
assert device.refresh(ow88, (), (v81_late,)) and ow88.chip["npu"] and ow88.chip["npu_graphs"] == "v73" and "graph_dir" not in ow88.npu.chip
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main.py"), encoding="utf-8") as fh:
    main_src = fh.read()
    assert "device.refresh(self, self.v79_dirs, self.v81_dirs" in main_src


def npu_with(chip):
    n = npu.Npu(os.path.join(tmp, "a"), os.path.join(tmp, "h"), os.path.join(tmp, "s"), log=lambda *a: None, chip=chip)
    n.layer_found = lambda: True
    n.service = lambda fresh=False: {"installed": True}
    return n


assert npu_with(None).ready() and npu_with(p).ready() and npu_with(p8g).ready() and npu_with(p88).ready() and npu_with(p88g).ready()
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

s3 = {"fsr4_available": True, "fsr3_available": True, "dittosr_available": True}  # DITTO: + DittoSR
ready = {"ready": True, "layer_found": True, "service": {"installed": True}}
P = main.Plugin()
offered = P._models("fsr4", s3)
assert offered == presets.models_for("fsr4", True, True) and all(ok for m, ok in offered if m != "anf"), offered  # no chip: nothing is taken away (ANF: hidden without the DLL key and the NPU package)
assert P._default_model("fsr4", s3, ready) == presets.default_model("fsr4", True, ready)
P.dev, P.chip = d, p  # SM8550
assert P._models("fsr4", s3) == offered and P._models("sgsr2", s3) == presets.models_for("sgsr2", True, True)
for kind in ("fsr4", "sgsr2", "static"):
    for nst in (ready, None, {"ready": False}):
        assert P._default_model(kind, s3, nst) == presets.default_model(kind, True, nst), (kind, nst)
P.dev, P.chip = d8, p8  # SM8750, no v79 graphs
m8 = dict(P._models("fsr4", s3))
assert m8 == {"fsr4": True, "fsr4fast": False, "anf": False, "fsr3": True, "dittosr": True, "sgsr2": True}, m8  # DITTO: any chip
assert P._default_model("fsr4", s3, {"ready": False}) == "fsr3"  # not fsr4fast
assert P._default_model("fsr4", {"fsr4_available": True, "fsr3_available": False}, None) == "sgsr2"
P.dev, P.chip = d8, p8g  # SM8750 with v79 graphs: all models
assert all(ok for m, ok in P._models("fsr4", s3) if m != "anf")
P.dev, P.chip = d88, p88  # SM8850 with default v73 graphs on V81 runtime: all models except anf
assert all(ok for m, ok in P._models("fsr4", s3) if m != "anf") and not dict(P._models("fsr4", s3))["anf"]
P.dev, P.chip = d88, p88g  # SM8850 with optional v81 graphs: all models except anf
assert all(ok for m, ok in P._models("fsr4", s3) if m != "anf") and not dict(P._models("fsr4", s3))["anf"]
P.dev, P.chip = du, pu  # unknown chip
assert dict(P._models("fsr4", s3)) == {"fsr4": False, "fsr4fast": False, "anf": False, "fsr3": True, "dittosr": True, "sgsr2": True}  # no FSR 4
print("test_device: OK")
