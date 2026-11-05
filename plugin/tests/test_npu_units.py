"""0.8.21: unit names per NPU package, from the service to the panel's hint. Package 14: one daemon, vela-npu.service. Package 13
and older: one unit per graph, and vela-npu.service is the old Lite daemon or nothing. A graph file tells them apart. The stub
answers like systemctl: "inactive" for a unit it does not know.
Run: python -I tests/test_npu_units.py"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import npu  # noqa: E402
import presets  # noqa: E402

HDR = "Num       RefCount Protocol Flags    Type St Inode Path\n"
LINE = "0000000000000000: 00000002 00000000 00010000 0005 01 12345 @%s\n"
FILES_13 = ["nf4_exact.bin", "nf4_k1h.bin", "nf4_k1h_360.bin", "nf4_k1h_450.bin", "nf4up_k1h.bin", npu.QNN_LIB]
FILES_14 = FILES_13 + [npu.SIZES_GRAPH]
SOCKETS_13 = [npu.FULL_SOCKET, npu.K1H_SOCKET, npu.K1H360_SOCKET, npu.K1H450_SOCKET,
              *[npu.UP_SOCKETS[o] for o in ("1920x1080", "1600x900", "1280x720")]]
SOCKETS_14 = list(npu.FULL_SOCKETS)
K1H_13 = "vela-npu@nf4_k1h.service"


def service(sockets, files, units=None):
    """npu.Npu.service() on a fake /proc/net/unix and graph folder, and the units systemctl was asked for."""
    calls = []

    def systemctl(unit):
        calls.append(unit)
        return (units or {}).get(unit, "inactive")

    with tempfile.TemporaryDirectory(prefix="vela_units_") as tmp:
        proc = os.path.join(tmp, "unix")
        graphs = os.path.join(tmp, "graphs")
        os.makedirs(graphs)
        for f in files:
            open(os.path.join(graphs, f), "w").close()
        with open(proc, "w") as fh:
            fh.write(HDR + "".join(LINE % s for s in sockets))
        return npu.Npu(tmp, tmp, tmp, log=lambda m: None, proc_net_unix=proc, systemctl=systemctl,
                       graph_dir=graphs).service(fresh=True), calls


F = presets.plan("fsr4", {"model": "fsr4", "npu": 2})
RENDER = {"1920x1080": ("960x540", "640x360"), "1600x900": ("800x450", "533x300"), "1280x720": ("640x360", "426x240"),
          "1600x904": ("800x452", "533x301"), "960x540": ("480x270", "320x180"), "1366x768": ("683x384", "455x256")}


def hint(svc, output, ultra=False):
    log = {"sgsr3": True, "mode": "fsr4", "render": RENDER[output][ultra], "output": output}
    return (presets.hint(F, {"running": True, "npu": {"layer_found": True, "service": svc}, "log": log}) or {}).get("id")


def both(svc, output):
    return hint(svc, output, False), hint(svc, output, True)


# ---- package 14, running: no systemctl, no hint; 1366x768 has no graph and keeps "set 1920x1080"
sv, calls = service(SOCKETS_14, FILES_14)
assert sv["installed"] and sv["running"] and sv["unit_state"] == "active" and not calls, sv
assert {sv[k] for k in ("full_unit", "k1h_unit", "k1h360_unit", "k1h450_unit")} == {"vela-npu.service"}, sv
for o in ("1920x1080", "1600x900", "1280x720", "1600x904", "960x540"):
    assert both(sv, o) == (None, None), o
assert both(sv, "1366x768") == ("game_res", "game_res")
assert "1366x768" not in presets.NPU_OUTPUTS and "1366x768" not in npu.UP_SOCKETS and "1366x768" not in npu.SIZE_SOCKETS

# ---- package 14, the daemon failed: no socket is listed, the panel says npu_failed at every size
sv, calls = service([], FILES_14, {"vela-npu.service": "failed"})
assert sv["installed"] and not sv["running"] and sv["unit_state"] == "failed" and calls == ["vela-npu.service"], (sv, calls)
for o in ("1920x1080", "1600x900", "1280x720", "1600x904", "960x540", "1366x768"):
    assert both(sv, o) == ("npu_failed", "npu_failed"), o
# restarting after exit 75, or stopped: "not running", not "failed"
for state in ("activating", "inactive"):
    sv, _ = service([], FILES_14, {"vela-npu.service": state})
    assert sv["unit_state"] == state and both(sv, "1920x1080") == ("npu_service", "npu_service"), state
# no Qualcomm runtime: the installer again, even with a failed unit
sv, _ = service([], [f for f in FILES_14 if f != npu.QNN_LIB], {"vela-npu.service": "failed"})
assert hint(sv, "1920x1080") == "npu_qnn", sv

# ---- package 13, running: still fine at its own outputs; the new sizes ask for the update, never "set 1920x1080"
sv, calls = service(SOCKETS_13, FILES_13)
assert sv["installed"] and sv["running"] and not sv["sizes_graph"] and not calls, sv
assert sv["full_unit"] == "vela-npu@nf4_exact.service" and sv["k1h_unit"] == K1H_13, sv
assert sv["k1h360_unit"] == "vela-npu@nf4_k1h_360.service" and sv["k1h450_unit"] == "vela-npu@nf4_k1h_450.service", sv
for o in ("1920x1080", "1600x900", "1280x720"):
    assert both(sv, o) == (None, None), o
for o in ("1600x904", "960x540"):
    assert both(sv, o) == ("npu_update_sizes", "npu_update_sizes"), o
assert both(sv, "1366x768") == ("game_res", "game_res")

# ---- package 13, its unit failed: it reads failed. vela-npu.service is unknown there and would answer "inactive" first
sv, calls = service([], FILES_13, {K1H_13: "failed"})
assert sv["unit_state"] == "failed" and calls == [K1H_13] and "vela-npu.service" not in calls, (sv, calls)
assert both(sv, "1920x1080") == ("npu_failed", "npu_failed")
assert both(sv, "1600x904") == ("npu_update_sizes", "npu_update_sizes"), "the update comes first, it replaces the units"
sv, _ = service([], FILES_13)
assert sv["unit_state"] == "inactive" and hint(sv, "1920x1080") == "npu_service", sv
# an old package whose vela-npu.service is the Lite daemon: active Lite does not make the full network "active"
sv, calls = service(["fsr4npud"], FILES_13, {"vela-npu.service": "active"})
assert sv["unit_state"] == "inactive" and not sv["running"] and "vela-npu.service" not in calls, (sv, calls)
assert hint(sv, "1920x1080") == "npu_service"

# ---- older still (v8, no Ultra Performance graph): npu_update_up
V8 = [f for f in FILES_13 if f != "nf4up_k1h.bin"]
sv, _ = service([npu.FULL_SOCKET, npu.K1H_SOCKET, npu.K1H360_SOCKET, npu.K1H450_SOCKET], V8)
assert both(sv, "1920x1080") == (None, "npu_update_up"), sv
assert hint(sv, "1600x904", True) == "npu_update_up" and hint(sv, "1600x904", False) == "npu_update_sizes"

# ---- vela-npu.service and the Lite socket alone, no graph file: not installed, systemctl not even asked
sv, calls = service(["fsr4npud"], [], {"vela-npu.service": "active"})
assert not sv["installed"] and not sv["running"] and sv["unit_state"] is None and not calls, (sv, calls)
assert hint(sv, "1920x1080") == "npu_install"
# the same unit together with a graph file: installed
sv, calls = service([], [npu.SIZES_GRAPH, npu.QNN_LIB], {"vela-npu.service": "failed"})
assert sv["installed"] and sv["unit_state"] == "failed", sv

assert npu.STATE_UNITS[0] == npu.UNIT == "vela-npu.service"

# systemctl runs with a clean environment and a time limit Decky's process under FEX can meet
import os as _os, subprocess as _sp, types as _types
_seen = {}
def _fake(args, **kw):
    _seen.update(kw, args=args)
    return _types.SimpleNamespace(stdout="failed\n")
_real, _sp.run = _sp.run, _fake
_os.environ["LD_LIBRARY_PATH"] = "/bundled"
try:
    assert npu.unit_active("x.service") == "failed"
finally:
    _sp.run = _real
    del _os.environ["LD_LIBRARY_PATH"]
assert "LD_LIBRARY_PATH" not in _seen["env"] and _seen["timeout"] >= 3 and _seen["args"][-1] == "x.service", _seen
print("test_npu_units OK")
