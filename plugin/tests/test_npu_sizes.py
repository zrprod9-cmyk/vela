"""0.8.21: the outputs of NPU package 14 (1600x904, 1440x810, 1280x800, 1152x648, 960x540): the service reads their sockets
and tells package 14 from 13 by a graph file; the panel asks to change the resolution only for an output with no graph, and
asks to update the NPU part when the installed package lacks the graph.
Run: python -I tests/test_npu_sizes.py"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import npu  # noqa: E402
import presets  # noqa: E402

SIZES = ("1600x904", "1440x810", "1280x800", "1152x648", "960x540")
RENDER_P = {"1600x904": "800x452", "1440x810": "720x405", "1280x800": "640x400", "1152x648": "576x324", "960x540": "480x270"}
RENDER_UP = {"1600x904": "533x301", "1440x810": "480x270", "1280x800": "426x266", "1152x648": "384x216", "960x540": "320x180"}

# the tables agree
assert presets.SIZE_OUTPUTS == SIZES == tuple(npu.SIZE_SOCKETS)
assert all(s in npu.UP_SOCKETS for s in SIZES) and all(s in presets.NPU_OUTPUTS for s in SIZES)
assert "1366x768" not in presets.NPU_OUTPUTS and "1366x768" not in npu.UP_SOCKETS and "1600x900" in presets.NPU_OUTPUTS
for s in npu.SIZE_SOCKETS.values():
    assert s in npu.FULL_SOCKETS and s.startswith("fsr4npud_nf4_k1h_")
for s in SIZES:
    assert presets.is_ultra(RENDER_UP[s], s) and not presets.is_ultra(RENDER_P[s], s), s
assert not presets.is_ultra("536x304", "1600x904")  # Spider-Man: Performance

# ---- the service, on a fake /proc/net/unix and graph folder
HDR = "Num       RefCount Protocol Flags    Type St Inode Path\n"
LINE = "0000000000000000: 00000002 00000000 00010000 0005 01 12345 @%s\n"


def service(sockets, files):
    with tempfile.TemporaryDirectory(prefix="vela_sizes_") as tmp:
        proc = os.path.join(tmp, "unix")
        graphs = os.path.join(tmp, "graphs")
        os.makedirs(graphs)
        for f in files:
            open(os.path.join(graphs, f), "w").close()
        with open(proc, "w") as fh:
            fh.write(HDR + "".join(LINE % s for s in sockets))
        return npu.Npu(tmp, tmp, tmp, log=lambda m: None, proc_net_unix=proc, systemctl=lambda u: "inactive",
                       graph_dir=graphs).service(fresh=True)


OLD = [npu.K1H_SOCKET, npu.K1H360_SOCKET, npu.K1H450_SOCKET, npu.FULL_SOCKET,
       *[npu.UP_SOCKETS[o] for o in ("1920x1080", "1600x900", "1280x720")]]
v13 = service(OLD, ["nf4_k1h.bin", "nf4up_k1h.bin", "nf4_k1h_450.bin"])
assert v13["installed"] and v13["running"] and not v13["sizes_graph"] and not any(v13["size_running"].values())
assert not any(v13["up_running"][s] for s in SIZES)
NEW = OLD + list(npu.SIZE_SOCKETS.values()) + [npu.UP_SOCKETS[s] for s in SIZES]
v14 = service(NEW, ["nf4_k1h.bin", "nf4up_k1h.bin", "nf4_k1h_450.bin", "nf4_k1h_452.bin"])
assert v14["sizes_graph"] and all(v14["size_running"][s] and v14["up_running"][s] for s in SIZES)
# a graph file of package 14 alone says "installed"; a size socket alone says "running"
assert service([], ["nf4_k1h_452.bin"])["installed"] and service([npu.SIZE_SOCKETS["960x540"]], [])["running"]

# ---- the panel's hint
F = presets.plan("fsr4", {"model": "fsr4", "npu": 2})


def hint(svc, output, render=None, run=None, plan=F):
    log = {"sgsr3": True, "mode": plan["model"], "render": render, "output": output, "npu": run}
    return (presets.hint(plan, {"running": True, "npu": {"layer_found": True, "service": svc}, "log": log}) or {}).get("id")


for s in SIZES:
    # package 14, graph up: nothing to do, in both modes, before and after the NPU line
    assert hint(v14, s, RENDER_P[s]) is None and hint(v14, s, RENDER_UP[s]) is None, s
    assert hint(v14, s, RENDER_P[s], {"state": "on", "mode": "exact"}) is None, s
    # package 13: update the NPU part, never "set 1920x1080"
    assert hint(v13, s, RENDER_P[s]) == "npu_update_sizes" and hint(v13, s, RENDER_UP[s]) == "npu_update_sizes", s
    assert hint(v13, s, RENDER_P[s], {"state": "fallback", "reason": "size"}) == "npu_update_sizes", s
    # package 14 with the socket gone: the service is down
    down = dict(v14, size_running={}, up_running={})
    assert hint(down, s, RENDER_P[s]) == "npu_service" and hint(dict(down, unit_state="failed"), s, RENDER_UP[s]) == "npu_failed", s
    # a run that still fell back for the size: the hint stays
    assert hint(v14, s, RENDER_P[s], {"state": "fallback", "reason": "size"}) == "game_res", s
# a model off the NPU: the resolution hint at any size, on either package
sg = presets.plan("fsr4", {"model": "sgsr2"})
assert hint(v14, "1600x904", "800x452", plan=sg) == "game_res" and hint(v13, "1600x904", "800x452", plan=sg) == "game_res"
# FSR 4 Fast uses the same graphs
fast = presets.plan("fsr4", {"model": "fsr4fast"})
assert hint(v14, "1600x904", "536x304", plan=fast) is None and hint(v13, "1600x904", "536x304", plan=fast) == "npu_update_sizes"
# an output with no graph: the resolution hint on either package, in both modes; never "the service is not running"
for svc in (v13, v14):
    assert hint(svc, "1366x768", "683x384") == "game_res" and hint(svc, "1366x768", "455x256") == "game_res"
# today's outputs are untouched on package 13
for s in ("1920x1080", "1600x900", "1280x720"):
    assert hint(v13, s) is None, s
# the texts
with open(os.path.join(ROOT, "src", "i18n", "en.json"), encoding="utf-8") as fh:
    en = json.load(fh)
assert "(package 14)" in en["hint.npu_update_up"] and "(package 14)" in en["hint.npu_update_sizes"] and "{cmd}" in en["hint.npu_update_sizes"]
print("test_npu_sizes OK")
