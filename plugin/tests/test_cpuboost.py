"""Checks for cpuboost.py (Vela 0.8.3, "CPU for the game"): policy7 cap per game on a fake sysfs, saved original,
thermal guard 85 / 88 / 78 C + 20 s, battery < 20 %, powerd re-write and profile change, crash restore, sg route,
helpers' placement on uid-1000 processes only. Temp folders only. Run: python -I tests/test_cpuboost.py"""
import os
import shutil
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import cpuboost  # noqa: E402

FREQS = ("595200 729600 864000 998400 1132800 1248000 1363200 1478400 1593600 1708800 1843200 1977600 2092800 "
         "2227200 2342400 2476800 2592000 2726400 2841600 2956800")
fails = []


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)


tmp = tempfile.mkdtemp(prefix="vela_cpu_")
try:
    pol = os.path.join(tmp, "policy7")
    th = os.path.join(tmp, "thermal")
    bat = os.path.join(tmp, "battery")
    proc = os.path.join(tmp, "proc")
    state = os.path.join(tmp, "powerd.state")
    settings = os.path.join(tmp, "settings")
    for d in (pol, th, bat, proc):
        os.makedirs(d)

    def put(path, v):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(f"{v}\n")

    def cap():
        with open(os.path.join(pol, "scaling_max_freq")) as f:
            return int(f.read())

    put(os.path.join(pol, "scaling_available_frequencies"), FREQS)
    put(os.path.join(pol, "scaling_max_freq"), 2092800)
    put(os.path.join(pol, "scaling_cur_freq"), 2092800)
    put(state, "profile=balanced\ngpu_level=auto")
    put(os.path.join(bat, "capacity"), 80); put(os.path.join(bat, "status"), "Discharging")
    zones = {"thermal_zone13": "cpu7-top-thermal", "thermal_zone14": "cpu7-middle-thermal",
             "thermal_zone1": "cpuss0-thermal", "thermal_zone29": "gpuss-0-thermal"}
    for z, t in zones.items():
        put(os.path.join(th, z, "type"), t); put(os.path.join(th, z, "temp"), 60000)
    put(os.path.join(th, "thermal_zone29", "temp"), 99000)   # GPU zone: never counted
    put(os.path.join(th, "cooling_device2", "type"), "cpufreq-cpu7"); put(os.path.join(th, "cooling_device2", "cur_state"), 0)

    def temp(c, zone="thermal_zone14"):
        put(os.path.join(th, zone, "temp"), int(c * 1000))

    # fake processes: helpers of uid 1000, one Xwayland of another user, the game
    aff = {}

    def mkproc(pid, comm, uid, tids):
        put(os.path.join(proc, str(pid), "comm"), comm)
        put(os.path.join(proc, str(pid), "status"), f"Name:\t{comm}\nUid:\t{uid}\t{uid}\t{uid}\t{uid}")
        for t in tids:
            os.makedirs(os.path.join(proc, str(pid), "task", str(t)), exist_ok=True)
            aff[t] = set(range(8))

    mkproc(2391, "Xwayland", 1000, [2391, 2392])
    mkproc(2334, "gamescope-wl", 1000, [2334, 2340])
    mkproc(2487, "mangoapp", 1000, [2487])
    mkproc(3000, "Xwayland", 0, [3000])
    mkproc(4000, "IntoTheDead.exe", 1000, [4000])
    now = [0.0]
    logs = []

    def make():
        return cpuboost.CpuBoost(settings, log=logs.append, policy=pol, thermal=th, battery=bat, powerd_state=state,
                                 proc=proc, clock=lambda: now[0], sg="",
                                 getaff=lambda t: set(aff[t]), setaff=lambda t, c: aff.__setitem__(t, set(c)))

    cb = make()
    check("temperature = max of cpu7-* / cpuss*, GPU zone ignored", cb.temp_c() == 60.0)
    st = cb.set_mode("100", "mild", set())
    check("mode saved, nothing applied without the game", st["mode"] == "mild" and cap() == 2092800)
    check("thread placement untouched without the game", aff[2391] == set(range(8)))

    cb.reconcile({"100"})
    check("mild: cap 2092800 -> 2476800", cap() == 2476800)
    check("original saved before the write", cb.data["applied"]["orig"] == 2092800)
    check("Xwayland / gamescope-wl threads -> cpu3-6", aff[2391] == aff[2392] == aff[2334] == aff[2340] == {3, 4, 5, 6})
    check("mangoapp -> cpu0-2", aff[2487] == {0, 1, 2})
    check("other user's Xwayland and the game untouched", aff[3000] == set(range(8)) and aff[4000] == set(range(8)))
    check("status: applied, 2477 cap", cb.status("100")["applied"] and cb.status("100")["cap_mhz"] == 2476)

    # thermal guard
    temp(85.5); cb.guard(lambda: {"100"})
    check("85 C: one step down (2342400)", cap() == 2342400)
    temp(88.0); cb.guard(lambda: {"100"})
    check("88 C: original cap", cap() == 2092800)
    temp(80); now[0] = 100; cb.guard(lambda: {"100"})
    check("78..85 C: hold", cap() == 2092800)
    temp(70); now[0] = 110; cb.guard(lambda: {"100"})
    now[0] = 125; cb.guard(lambda: {"100"})
    check("< 78 C for 15 s: still held", cap() == 2092800)
    now[0] = 131; cb.guard(lambda: {"100"})
    check("< 78 C for 20 s: one step up (2227200)", cap() == 2227200)
    for k in range(1, 6):
        now[0] = 131 + 21 * k; cb.guard(lambda: {"100"})
    check("re-raise stops at the target", cap() == 2476800)

    # battery
    put(os.path.join(bat, "capacity"), 19); cb.guard(lambda: {"100"})
    check("battery 19 % discharging: original cap", cap() == 2092800)
    put(os.path.join(bat, "status"), "Charging"); now[0] = 400; cb.guard(lambda: {"100"})
    now[0] = 421; cb.guard(lambda: {"100"})
    check("charging: raised again step by step", cap() == 2227200)
    put(os.path.join(bat, "capacity"), 80); put(os.path.join(bat, "status"), "Discharging")

    # powerd re-wrote the cap (resume / Reload): apply again while the game runs and Balanced
    put(os.path.join(pol, "scaling_max_freq"), 2092800)
    cb.guard(lambda: {"100"})
    check("powerd re-write: applied again", cap() == 2476800 and cb.data["applied"]["orig"] == 2092800)
    put(os.path.join(pol, "scaling_max_freq"), 2092800)
    cb.guard(lambda: set())
    check("powerd re-write, game gone: not applied", cap() == 2092800 and cb.data["applied"] is None)
    cb.reconcile({"100"})
    check("game back: applied", cap() == 2476800)

    # kernel thermal throttle (cooling device cpufreq-cpu7 active) lowers the read: not powerd, held; exit writes back
    put(os.path.join(th, "cooling_device2", "cur_state"), 3)
    put(os.path.join(pol, "scaling_max_freq"), 1977600)
    cb.guard(lambda: {"100"})
    check("throttled read: record kept", cb.data["applied"] and cb.data["applied"]["hz"] == 2476800)
    cb.reconcile(set())
    check("throttled at exit: original written", cap() == 2092800 and cb.data["applied"] is None)
    put(os.path.join(th, "cooling_device2", "cur_state"), 0)
    cb.reconcile({"100"})

    # profile change to Performance (powerd writes hw max): never written back
    cb.set_mode("100", "max", {"100"})
    check("max: cap 2956800", cap() == 2956800)
    put(state, "profile=performance")
    cb.guard(lambda: {"100"})
    cb.reconcile(set())
    check("Performance: record dropped, cap left to powerd", cap() == 2956800 and cb.data["applied"] is None)
    check("game exit: helpers back to 0-7", aff[2391] == aff[2334] == aff[2487] == set(range(8)))
    cb.reconcile({"100"})
    check("not Balanced: nothing applied", cb.data["applied"] is None)
    put(state, "profile=balanced"); put(os.path.join(pol, "scaling_max_freq"), 2092800)

    # game exit restores
    cb.reconcile({"100"})
    cb.reconcile(set())
    check("game exit: original cap", cap() == 2092800 and cb.data["applied"] is None and not cb.data["threads"])

    # crash: a fresh instance restores from the file on its first pass with no game
    cb.reconcile({"100"})
    check("before crash: raised", cap() == 2956800)
    cb2 = make()
    cb2.reconcile(set())
    check("next start after a crash: original cap + helpers back", cap() == 2092800 and aff[2391] == set(range(8)))
    # crash, then reboot: powerd's value differs from Vela's: left alone
    cb.reconcile({"100"})
    put(os.path.join(pol, "scaling_max_freq"), 1977600)
    cb3 = make(); cb3.reconcile(set())
    check("after reboot: someone else's cap kept", cap() == 1977600 and cb3.data["applied"] is None)
    put(os.path.join(pol, "scaling_max_freq"), 2092800)

    # cap already higher than the mode (underclock "small" = 2476800): nothing to raise
    cb4 = make(); cb4.set_mode("100", "mild", set())
    put(os.path.join(pol, "scaling_max_freq"), 2476800)
    cb4.reconcile({"100"})
    check("cap already at the mode: no record, no write", cb4.data["applied"] is None and cap() == 2476800)
    cb4.reconcile(set())
    put(os.path.join(pol, "scaling_max_freq"), 2092800)

    # hot at start: original held until it cools
    temp(86); cb5 = make(); cb5.reconcile({"100"})
    check("hot at start: not raised", cap() == 2092800 and cb5.data["applied"]["orig"] == 2092800)
    cb5.reconcile(set()); temp(60)

    # sg route: the plain write is refused -> `sg wheel -c printf ... > path`
    calls = []

    def runner(cmd, **kw):
        calls.append(cmd)
        return types.SimpleNamespace(returncode=0, stderr=b"")

    cb6 = cpuboost.CpuBoost(os.path.join(tmp, "s6"), log=logs.append, policy=pol, thermal=th, battery=bat,
                            powerd_state=state, proc=proc, runner=runner, sg="/usr/bin/sg", sg_group=lambda p: "wheel",
                            getaff=lambda t: set(aff[t]), setaff=lambda t, c: None)
    real_open = open

    def refusing_open(path, mode="r", *a, **kw):
        if "w" in mode and str(path).endswith("scaling_max_freq"):
            raise PermissionError(13, "Permission denied")
        return real_open(path, mode, *a, **kw)

    cpuboost.open = refusing_open
    try:
        ok = cb6._write(2476800)
    finally:
        del cpuboost.open
    check("EACCES -> sg wheel printf", ok and calls and calls[0][:3] == ["/usr/bin/sg", "wheel", "-c"]
          and "2476800" in calls[0][3] and "scaling_max_freq" in calls[0][3])
    check("writable through sg", cb6.writable())
    try:
        cb6.set_mode("100", "turbo", set())
        check("unknown mode refused", False)
    except ValueError:
        check("unknown mode refused", True)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print("FAILED: " + ", ".join(fails) if fails else "all ok")
sys.exit(1 if fails else 0)
