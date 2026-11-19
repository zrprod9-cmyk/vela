"""The game of a launcher's shortcut, learned from a running launch (learn.py). The process trees are the ones seen on the
Armada console on 2026-11-16: Xodus Nexus (a temporary exe folder, its own id kept), Heroic (SteamAppId=0), Ubisoft Connect
in a Proton prefix, and each launcher already open when the game starts.
Run: python -I tests/test_learn.py"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import learn  # noqa: E402

NUL = b"\0"


def pe(machine=0x8664):
    head = bytearray(128)
    head[:2] = b"MZ"
    head[60:64] = (64).to_bytes(4, "little")
    head[64:68] = b"PE\0\0"
    head[68:70] = machine.to_bytes(2, "little")
    return bytes(head)


def put(path, data=b""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


class Proc:
    """A /proc in a temp folder."""

    def __init__(self, root):
        self.root = os.path.join(root, "proc")
        os.makedirs(self.root)
        self.next = 100

    def add(self, argv, ppid=1, cwd=None, env=None, rss=10, maps=()):
        pid = self.next
        self.next += 1
        d = os.path.join(self.root, str(pid))
        os.makedirs(d)
        put(os.path.join(d, "cmdline"), NUL.join(a.encode() for a in argv) + NUL)
        fields = ["S", str(ppid)] + ["0"] * 17 + [str(pid * 10), "0", str(rss)]
        put(os.path.join(d, "stat"), f"{pid} (some name) {' '.join(fields)}\n".encode())
        put(os.path.join(d, "environ"), NUL.join(f"{k}={v}".encode() for k, v in (env or {}).items()) + NUL)
        put(os.path.join(d, "maps"), "".join(f"7f00-7f10 r--p 00000000 00:2a 1234 {m}\n" for m in maps).encode())
        if cwd:
            os.symlink(cwd, os.path.join(d, "cwd"))
        return pid

    def reaper(self, appid, *cmd):
        return self.add(["/steam/steamrtarm64/reaper", "SteamLaunch", f"AppId={appid}", "--", *cmd])


SECRET = "-AUTH_PASSWORD=0123456789abcdef-not-a-real-code"

with tempfile.TemporaryDirectory() as tmp:
    # ---- Xodus Nexus: the exe runs from a temporary folder of links, the install's own exe is encrypted --------------
    nexus = os.path.join(tmp, "Games", "Xodus Nexus")
    hifi = os.path.join(nexus, "Hi-Fi-RUSH")
    put(os.path.join(hifi, "Hibiki", "Binaries", "WinGDK", "Hi-Fi-RUSH.exe"), b"\x8f\x11encrypted")
    put(os.path.join(hifi, "Engine", "Binaries", "Hi-Fi-RUSH.exe"), b"\x8f\x11other")  # the same name twice
    view = os.path.join(nexus, ".xodus-chain-Us77u5")
    run = put(os.path.join(view, "Hibiki", "Binaries", "WinGDK", "Hi-Fi-RUSH.exe"), pe())
    pfx = os.path.join(tmp, "nexus-data", "compatdata", "4096bd52", "pfx")
    os.makedirs(os.path.join(pfx, "drive_c"))
    A = "2641705097"
    p = Proc(os.path.join(tmp, "p1"))
    r = p.reaper(A, "/bin/sh", "launch-game", "4096bd52")
    n = p.add(["/tmp/.mount_Xodus/xodus-nexus.bin", "--launch-game", "4096bd52"], r)
    g = p.add(["Z:" + run.replace(os.sep, "/"), SECRET], n, hifi, {"SteamAppId": A, "WINEPREFIX": pfx + "/", "TOKEN": "x"},
              rss=2000, maps=[run, os.path.join(hifi, "Hibiki", "Binaries", "WinGDK", "XCurl.dll")])
    p.add(["Z:" + view.replace(os.sep, "/") + "/UnityCrashHandler64.exe"], n, hifi, {"SteamAppId": A}, rss=5000)  # a heavy helper
    p.add(["C:\\windows\\system32\\services.exe"], 1, pfx, {"SteamAppId": A}, rss=9000)
    o = learn.observe(A, p.root)
    assert o["status"] == "found" and o["certain"] and not o["handoff"], o
    assert o["install_dir"] == os.path.realpath(hifi), o
    assert o["exe"] == os.path.join("Hibiki", "Binaries", "WinGDK", "Hi-Fi-RUSH.exe"), o  # the one the running exe names
    assert o["running_exe"] == run and o["machine"] == "x64" and o["prefix"] == os.path.realpath(pfx), o
    assert learn.pe_machine(os.path.join(hifi, o["exe"])) is None  # encrypted on disk: read from the running copy
    # nothing of the command line or the environment but the three names leaves the module
    snap = learn.snapshot(p.root)
    assert SECRET not in repr(snap) and "TOKEN" not in repr(learn.candidates(snap, p.root)), "arguments or env kept"

    # ---- Nexus was already open: the game sits under the LAUNCHER's Steam launch with its own id -----------------------
    L = "2834316520"
    p = Proc(os.path.join(tmp, "p2"))
    rl = p.reaper(L, "/bin/sh", "launch-nexus")
    ui = p.add(["/tmp/.mount_Xodus/xodus-nexus.bin"], rl)
    p.add(["Z:" + run.replace(os.sep, "/")], ui, hifi, {"SteamAppId": A, "WINEPREFIX": pfx}, rss=2000, maps=[run])
    p.reaper(A, "/bin/sh", "launch-game", "4096bd52")  # the game's own launch: nothing under it
    o = learn.observe(A, p.root)
    assert o["status"] == "found" and o["certain"] and o["handoff"] and o["install_dir"] == os.path.realpath(hifi), o
    assert learn.observe(L, p.root) == {"status": "other", "appid": A}  # never the launcher's game

    # ---- Heroic: the runtime sets SteamAppId=0 --------------------------------------------------------------------------
    celeste = os.path.join(tmp, "Games", "Heroic", "Celeste")
    cexe = put(os.path.join(celeste, "Celeste.exe"), pe(0x014C))
    circuit = os.path.join(tmp, "Games", "Heroic", "GravityCircuit")
    gexe = put(os.path.join(circuit, "GravityCircuit.exe"), pe())
    hp = os.path.join(tmp, "Games", "Heroic", "Prefixes", "Celeste")
    os.makedirs(os.path.join(hp, "pfx", "drive_c"))
    C = "3372006481"
    p = Proc(os.path.join(tmp, "p3"))
    r = p.reaper(C, "/usr/lib64/heroic/heroic", "--no-gui", "heroic://launch?appName=Salt")
    pv = p.add(["srt-bwrap"], p.add(["python3", "umu_run.py", cexe, SECRET], r))
    p.add(["c:\\windows\\system32\\umu.exe", cexe], pv, celeste, {"SteamAppId": "0"}, rss=40)
    p.add(["X:\\Games\\Heroic\\Celeste\\Celeste.exe", SECRET], pv, celeste,
          {"SteamAppId": "0", "STEAM_COMPAT_DATA_PATH": hp}, rss=640, maps=["/run/host/x/opengl32.dll", cexe])
    o = learn.observe(C, p.root)
    assert o["status"] == "found" and not o["certain"] and not o["handoff"], o
    assert o["install_dir"] == os.path.realpath(celeste) and o["exe"] == "Celeste.exe" and o["machine"] == "x86", o
    assert o["prefix"] == os.path.realpath(os.path.join(hp, "pfx")), o
    assert learn.observe("1817190", p.root) == {"status": "none"}

    # the store: a candidate until the player confirms; the folder and the shortcut must still be what they were
    st = learn.Store(os.path.join(tmp, "settings"), log=lambda m: None)
    e = st.record(C, o, sig="heroic|Salt")
    assert e["confirmed"] is False and st.get(C, "heroic|Salt")["install_dir"] == os.path.realpath(celeste)
    st.confirm(C)
    assert learn.Store(os.path.join(tmp, "settings")).get(C)["confirmed"] is True  # written
    assert st.get(C, "heroic|Other") is None  # the shortcut points elsewhere now: learn again
    assert st.record(C, o, sig="heroic|Salt")["confirmed"] is True  # seen again: stays confirmed

    # ---- Heroic's own shortcut (the launcher's window): the first game is a candidate, a second one says "launcher" ------
    H = "2339058479"
    p = Proc(os.path.join(tmp, "p4"))
    r = p.reaper(H, "/usr/bin/heroic")
    hw = p.add(["/usr/lib64/heroic/heroic"], r)
    p.add(["X:\\Games\\Heroic\\Celeste\\Celeste.exe"], hw, celeste, {"SteamAppId": "0"}, rss=640, maps=[cexe])
    o1 = learn.observe(H, p.root)
    assert o1["status"] == "found" and not o1["certain"], o1
    assert st.record(H, o1, sig="heroic")["confirmed"] is False
    p = Proc(os.path.join(tmp, "p5"))
    hw = p.add(["/usr/lib64/heroic/heroic"], p.reaper(H, "/usr/bin/heroic"))
    p.add(["X:\\Games\\Heroic\\GravityCircuit\\GravityCircuit.exe"], hw, circuit, {"SteamAppId": "0"}, rss=300, maps=[gexe])
    assert st.record(H, learn.observe(H, p.root), sig="heroic") is None
    assert st.is_launcher(H) and st.get(H, "heroic") is None
    assert st.record(H, o1, sig="heroic") is None  # stays a launcher
    assert st.get(C) is not None  # the game's own shortcut is untouched

    # a game's own shortcut whose game moved (the old folder is gone) follows it, and is no launcher
    moved = os.path.join(tmp, "Games", "Moved")
    mexe = put(os.path.join(moved, "Celeste.exe"), pe(0x014C))
    st2 = learn.Store(os.path.join(tmp, "settings2"), log=lambda m: None)
    gone = {**o, "install_dir": os.path.join(tmp, "Games", "Gone")}
    st2.record(C, gone)
    assert st2.get(C) is None  # its exe is not there
    assert st2.record(C, {**o, "install_dir": moved})["install_dir"] == moved and not st2.is_launcher(C)

    # ---- Ubisoft Connect in a Proton prefix: the launcher's own programs run next to the game ---------------------------
    ubi = os.path.join(tmp, "pfx", "drive_c", "Program Files (x86)", "Ubisoft", "Ubisoft Game Launcher")
    put(os.path.join(ubi, "upc.exe"), pe())
    put(os.path.join(ubi, "BigTool.exe"), pe())
    pop = os.path.join(ubi, "games", "Prince of Persia The Lost Crown")
    pexe = put(os.path.join(pop, "TheLostCrown.exe"), pe())
    U = "2709351965"
    p = Proc(os.path.join(tmp, "p6"))
    adv = p.add(["pv-adverb"], p.add(["srt-bwrap"], p.reaper(U, "proton", "waitforexitandrun", "upc.exe")))
    env = {"SteamAppId": U, "STEAM_COMPAT_DATA_PATH": os.path.join(tmp)}
    p.add(["C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\upc.exe", "uplay://launch/6145/0"], adv, ubi, env, rss=900)
    p.add(["C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\UplayWebCore.exe"], adv, ubi, env, rss=1500)
    p.add(["C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\BigTool.exe"], adv, ubi, env, rss=3000)
    assert learn.observe(U, p.root, own_dirs=[ubi]) == {"status": "none"}  # only the launcher runs yet
    p.add(["C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\games\\Prince of Persia The Lost Crown\\TheLostCrown.exe"],
          adv, pop, env, rss=500, maps=[pexe])
    o = learn.observe(U, p.root, own_dirs=[ubi])
    assert o["status"] == "found" and o["certain"] and o["install_dir"] == os.path.realpath(pop), o
    assert o["exe"] == "TheLostCrown.exe" and o["prefix"] == os.path.realpath(os.path.join(tmp, "pfx")), o

    # ---- small parts ------------------------------------------------------------------------------------------------------
    assert learn.pe_machine(cexe) == "x86" and learn.pe_machine(pexe) == "x64"
    assert learn.pe_machine(os.path.join(tmp, "missing.exe")) is None
    assert learn.observe(A, os.path.join(tmp, "no-proc")) == {"status": "none"}
    assert learn.Store(os.path.join(tmp, "empty")).get("1") is None
    put(os.path.join(tmp, "bad", "learned.json"), b"[1, 2")
    assert learn.Store(os.path.join(tmp, "bad")).get("1") is None  # a cut file: nothing learned, no exception
print("test_learn OK")
