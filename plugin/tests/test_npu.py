"""Checks for npu.py (Vela NPU, 0.8.0): the live key, the service check, the layer manifest in the user's layer dir, the
launch options with exact restore, and the "Vela NPU:" log lines. Temp folders only.
Run: python -I tests/test_npu.py"""
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import live  # noqa: E402
import npu  # noqa: E402
import sgsr3  # noqa: E402

# ---- live key npu
assert live.clean({"npu": 1}) == {"npu": 1} and live.clean({"npu": "2"}) == {"npu": 2} and live.clean({"npu": 0}) == {"npu": 0}
assert live.clean({"npu": 3}) == {} and live.clean({"npu": "x"}) == {} and live.clean({"npu": -1}) == {}
assert live.clean({"npu": None, "model": "fsr4"}) == {"model": "fsr4"}

# ---- fork lines (F1-F4)
assert npu.parse_fork("nothing here") is None
f = npu.parse_fork("Vela NPU: fallback nolayer, GPU path\nVela NPU: on, exact\nVela NPU: 600 frames, HTP 2.47 ms, fallbacks 0\n")
assert f == {"state": "on", "mode": "exact", "reason": None, "render": None, "output": None, "frames": 600, "htp_ms": 2.47, "fallbacks": 0}, f
# 0.8.10: the provider prints the schedule it runs; a live switch adds a line, the last one counts
assert npu.parse_fork("Vela NPU: on, exact\nVela NPU: zero-copy tensors\nVela NPU: on, async\n")["mode"] == "async"
assert npu.parse_fork("Vela NPU: on, async\nVela NPU: 600 frames, HTP 11.20 ms, fallbacks 0\nVela NPU: on, exact\n")["mode"] == "exact"
f = npu.parse_fork("Vela NPU: on, async\nVela NPU: fallback nodaemon, GPU path\n")
assert f["state"] == "fallback" and f["reason"] == "nodaemon" and f["mode"] is None, f
assert npu.parse_fork("Vela NPU: fallback weird, GPU path")["reason"] == "error"
assert npu.parse_fork("Vela NPU: on, async\nVela NPU: off\n")["state"] == "off"
# ---- layer lines (L1-L4)
assert npu.parse_layer("") is None
l = npu.parse_layer("Vela NPU layer: loaded v1\nVela NPU layer: daemon not reachable (Connection refused)\n")
assert l["version"] == "1" and l["daemon"] is False and l["daemon_error"] == "Connection refused", l
l = npu.parse_layer("Vela NPU layer: loaded v1\nVela NPU layer: daemon connected\nVela NPU layer: 1200 jobs, HTP 2.50 ms, timeouts 0, errors 1\n")
assert l["daemon"] is True and l["jobs"] == 1200 and l["htp_ms"] == 2.5 and l["errors"] == 1, l

# ---- OptiScaler.log: the npu field rides on the last upscaler's segment
tmp = tempfile.mkdtemp(prefix="vela_npu_")
try:
    log = os.path.join(tmp, "OptiScaler.log")
    with open(log, "w") as fh:
        fh.write("Initializing SGSR3 960x540 -> 1920x1080\nVela FSR 4 VK: FSR 4.1.1 VK INT8 context 960x540 -> 1920x1080 (hdr off)\n"
                 "Vela NPU: on, exact\n")
    s = sgsr3.log_summary(log)
    assert s["mode"] == "fsr4" and s["npu"]["state"] == "on" and s["npu"]["mode"] == "exact", s
    with open(log, "a") as fh:  # a live model change re-creates the upscaler: the old NPU line is not the state
        fh.write("Vela live: handle 1 model ai\nInitializing SGSR3 960x540 -> 1920x1080\nSGSR3 student: on\n")
    assert sgsr3.log_summary(log)["npu"] is None

    # ---- the service: /proc/net/unix and the unit file; cached
    proc = os.path.join(tmp, "unix")
    units = os.path.join(tmp, "units")
    os.makedirs(units)
    hdr = "Num       RefCount Protocol Flags    Type St Inode Path\n"
    with open(proc, "w") as fh:
        fh.write(hdr + "0000000000000000: 00000002 00000000 00010000 0001 01 12345 @fsr4npud-other\n")
    calls = []
    assets = os.path.join(tmp, "assets", "npu")
    os.makedirs(assets)
    home, settings = os.path.join(tmp, "home"), os.path.join(tmp, "settings")
    graphs = os.path.join(tmp, "graphs")
    os.makedirs(graphs)
    n = npu.Npu(assets, home, settings, log=lambda *_: None, proc_net_unix=proc, unit_dirs=(units,),
                systemctl=lambda u: calls.append(u) or "inactive", graph_dir=graphs)
    sv = n.service()
    assert sv["installed"] is False and sv["running"] is False and not calls, sv  # nothing there: no systemctl
    # 0.8.15: the Lite daemon alone (unit vela-npu.service, socket @fsr4npud) is not "the NPU is there"
    with open(os.path.join(units, "vela-npu.service"), "w") as fh:
        fh.write("[Service]\n")
    with open(proc, "w") as fh:
        fh.write(hdr + "0000000000000000: 00000002 00000000 00010000 0001 01 12345 @fsr4npud\n")
    sv = n.service(fresh=True)
    assert sv["installed"] is False and sv["running"] is False and not calls and not n.ready(), sv
    # the template unit of the full daemons: installed, none listens yet: systemctl says why
    with open(os.path.join(units, npu.TEMPLATE_UNIT), "w") as fh:
        fh.write("[Service]\n")
    assert n.service()["installed"] is False, "cached for 30 s"
    sv = n.service(fresh=True)
    assert sv["installed"] and not sv["running"] and sv["unit_state"] == "inactive" and calls[0] == npu.K1H_UNIT, sv
    # a full daemon listens: running, whatever the Lite daemon does (it is gone here) and without any unit file
    os.remove(os.path.join(units, npu.TEMPLATE_UNIT))
    os.remove(os.path.join(units, "vela-npu.service"))
    with open(proc, "w") as fh:
        fh.write(hdr + "0000000000000000: 00000002 00000000 00010000 0001 01 12345 @fsr4npud_nf4_k1h\n")
    calls.clear()
    sv = n.service(fresh=True)
    assert sv["installed"] and sv["running"] and sv["unit_state"] == "active" and sv["k1h_running"] and not calls, sv
    assert not sv["k1h360_running"] and not sv["k1h450_running"] and not any(sv["up_running"].values()), sv
    # each full socket alone is enough; a graph file of the full network alone says "installed"
    for sock in npu.FULL_SOCKETS:
        with open(proc, "w") as fh:
            fh.write(hdr + "0000000000000000: 00000002 00000000 00010000 0001 01 12345 @" + sock + "\n")
        assert n.service(fresh=True)["running"], sock
    with open(proc, "w") as fh:
        fh.write(hdr)
    assert not n.service(fresh=True)["installed"]
    with open(os.path.join(graphs, "nf4_k1h.bin"), "wb") as fh:
        fh.write(b"g")
    assert n.service(fresh=True)["installed"] and not n.service()["running"]
    os.remove(os.path.join(graphs, "nf4_k1h.bin"))
    with open(proc, "w") as fh:
        fh.write(hdr + "0000000000000000: 00000002 00000000 00010000 0001 01 12345 @fsr4npud_nf4_k1h\n")
    assert n.service(fresh=True)["running"]
    assert not n.layer_found() and not n.ready()
    try:
        n.enable("100", "%command%")
        raise AssertionError("enable without the layer files")
    except RuntimeError:
        pass

    # ---- the layer files, the manifest in the user's layer dir while a game has it on
    shutil.copy(os.path.join(HERE, "..", "assets", "npu", npu.MANIFEST), assets)
    with open(os.path.join(assets, npu.LIB), "wb") as fh:
        fh.write(b"\x7fELF")
    assert n.layer_found() and n.ready()
    inst = os.path.join(home, ".local", "share", "vulkan", "implicit_layer.d", npu.MANIFEST)
    lo0 = "PROTON_LOG=1 %command% -dx12"
    lo1 = n.enable("100", lo0)
    assert lo1.startswith("VELA_NPU=1 VELA_NPU_LOG_FILE=") and lo1.endswith(lo0), lo1
    assert n.enable("100", lo1) == lo1, "on again with the same options: unchanged"
    m = json.load(open(inst))["layer"]
    assert m["name"] == "VK_LAYER_VELA_npu" and os.path.isabs(m["library_path"]) and m["library_path"].endswith(npu.LIB)
    assert m["enable_environment"] == {"VELA_NPU": "1"} and m["disable_environment"] == {"DISABLE_VELA_NPU": "1"}
    st = n.status("100")
    assert st["on"] and st["ready"] and st["layer"] is None
    with open(n.layer_log("100"), "w") as fh:
        fh.write("Vela NPU layer: loaded v1\nVela NPU layer: daemon connected\n")
    assert n.status("100")["layer"]["daemon"] is True
    lo2 = n.enable("200", "")
    assert lo2.endswith("%command%") and "layer-200.log" in lo2, lo2
    # off: exact restore when untouched; the manifest stays while another game has it on
    assert n.disable("100", lo1) == lo0 and os.path.exists(inst)
    assert n.disable("100", lo0) == lo0, "off twice: the options as they are"
    # options edited by hand meanwhile: only ours go
    edited = lo2.replace("%command%", "MANGOHUD=1 %command%")
    assert n.disable("200", edited) == "MANGOHUD=1 %command%" and not os.path.exists(inst)
    # SGSR 3's own strip removes the NPU tokens too (VELA_*), so turning SGSR 3 off never leaves the layer on
    assert sgsr3.strip_ours(n.tokens("300")[0] + " " + n.tokens("300")[1] + " %command%") == "%command%"
    lo3 = n.enable("300", None)
    assert lo3.endswith("%command%") and n.disable("300", lo3) == ""
finally:
    pass  # the temp folder stays, like the other tests' (no recursive delete)

print("ok")

# 0.8.3: the size line names render and output; the old form still parses
f = npu.parse_fork("Vela NPU: fallback size 640x360->1920x1080, GPU path")
assert f["state"] == "fallback" and f["reason"] == "size" and f["render"] == "640x360" and f["output"] == "1920x1080", f
f = npu.parse_fork("Vela NPU: fallback size, GPU path")
assert f["reason"] == "size" and f["render"] is None and f["output"] is None, f
print("SZ size line OK")
