"""Checks for the gpufloor group-write fix: EACCES -> sg route (member only, error logged once), boost-aware floor check and
boost-free "prev". Fake devfreq node in a temp folder; open() of the module is wrapped to simulate the plugin process
without groups and the msm boost QoS. Run: python -I tests/test_gpufloor_ig.py"""
import builtins
import os
import shutil
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import gpufloor  # noqa: E402

FREQS = "220000000 295000000 348000000 401000000 475000000 550000000 615000000 680000000"
tmp = tempfile.mkdtemp(prefix="ig_floor_")
state = {"locked": False, "boost": []}   # locked: direct write -> EACCES; boost: values the next reads see as a floor
real_open = builtins.open


def fake_open(path, mode="r", *a, **k):
    if str(path).endswith("min_freq"):
        if "w" in mode and state["locked"]:
            raise PermissionError(13, "Permission denied", str(path))
        if "r" in mode and state["boost"]:
            b = state["boost"].pop(0)
            with real_open(path, *a, **k) as f:
                v = int(f.read().strip())
            import io
            return io.StringIO(f"{max(v, b)}\n")
    return real_open(path, mode, *a, **k)


gpufloor.open = fake_open   # module global shadows the builtin for gpufloor's own code only

try:
    node = os.path.join(tmp, "devfreq", "3d00000.gpu")
    os.makedirs(node)

    def put(name, v):
        with real_open(os.path.join(node, name), "w") as f:
            f.write(f"{v}\n")

    def get(name):
        with real_open(os.path.join(node, name)) as f:
            return int(f.read().strip())

    put("available_frequencies", FREQS); put("min_freq", 220000000); put("max_freq", 680000000)
    put("cur_freq", 680000000); put("governor", "simple_ondemand")
    calls, logs = [], []

    def runner(cmd, **kw):   # stands in for `sg wheel -c "printf ... > path"`: performs the write as the group would
        calls.append(cmd)
        assert cmd[:3] == ["/usr/bin/sg", "wheel", "-c"] and kw.get("stdin") is not None, (cmd, kw)
        hz, path = cmd[3].split(" > ")
        put("min_freq", int(hz.split()[-1]))
        return types.SimpleNamespace(returncode=0, stderr=b"")

    def make(member=True):
        fl = gpufloor.GpuFloor(os.path.join(tmp, "settings"), log=logs.append, runner=runner, sg="/usr/bin/sg",
                               devfreq_glob=os.path.join(tmp, "devfreq", "*.gpu"), sleep=lambda s: None)
        fl._sg_group = (lambda p: "wheel") if member else (lambda p: "")
        return fl

    # 1. plugin process without groups (console 22:21: EACCES): the floor goes on through sg
    state["locked"] = True
    fl = make(member=True); fl.enable("2239710")
    fl.reconcile({"2239710"})
    assert get("min_freq") == 680000000, get("min_freq")
    assert len(calls) == 1 and calls[0][3] == "printf '%s\\n' 680000000 > " + gpufloor.shlex.quote(os.path.join(node, "min_freq")), calls
    assert fl.status("2239710")["applied"] and fl.writable()
    # held: no new write on the next reconciles
    fl.reconcile({"2239710"}); fl.reconcile({"2239710"}); assert len(calls) == 1
    # game quits: restored through sg to the value before (220)
    fl.reconcile(set()); assert get("min_freq") == 220000000 and len(calls) == 2

    # 2. not a member: no sg call, the same error logged once over many reconciles
    calls.clear(); logs.clear()
    fl2 = make(member=False); fl2.reconcile({"2239710"}); fl2.reconcile({"2239710"}); fl2.reconcile({"2239710"})
    assert not calls and get("min_freq") == 220000000
    assert len([m for m in logs if "cannot write" in m]) == 1, logs
    # (writable() of fl2 is os.access on the real temp file here: the EACCES is simulated, so not asserted)

    # 3. powerd reset the floor to 220 while the driver's boost makes one read show 680: still re-applied
    state["locked"] = False; calls.clear()
    fl3 = make(); fl3.reconcile({"2239710"}); assert get("min_freq") == 680000000
    put("min_freq", 220000000); state["boost"] = [680000000]   # first read boosted, next ones the real 220
    fl3.reconcile({"2239710"}); assert get("min_freq") == 680000000, get("min_freq")
    fl3.reconcile(set()); assert get("min_freq") == 220000000

    # 4. "prev" is boost-free: the user floor is 220 but the first reads see the boost -> restore goes to 220, not 680
    state["boost"] = [680000000, 680000000]
    fl4 = make(); fl4.data["applied"] = None
    fl4.reconcile({"2239710"}); assert get("min_freq") == 680000000
    assert fl4.data["applied"]["prev"] == 220000000, fl4.data["applied"]
    fl4.reconcile(set()); assert get("min_freq") == 220000000, get("min_freq")

    # 5. boost seen on every read while the floor is held: no needless write
    calls.clear(); state["locked"] = True
    fl5 = make(); fl5.reconcile({"2239710"}); n = len(calls)
    state["boost"] = [680000000] * 8; fl5.reconcile({"2239710"}); assert len(calls) == n
    state["boost"] = []; fl5.reconcile(set()); assert get("min_freq") == 220000000
    print("test_gpufloor_ig OK")
finally:
    gpufloor.open = builtins.open
    pass   # temp folder left to the OS temp cleaner (no deletes)
