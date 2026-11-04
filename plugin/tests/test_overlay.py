"""0.8.20: the kit through vela-launch's overlay: bwrap argv, probe fallback, env merging, kit missing, Armada wrapper
detection, migration from the copy mode and a game that still starts after the plugin is uninstalled."""
import importlib.machinery
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import launchopts  # noqa: E402
import sgsr3  # noqa: E402
import vela_launch  # noqa: E402
import velakit  # noqa: E402

tmp = tempfile.mkdtemp(prefix="vela-ov-")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def tree(root):
    out = {}
    for top, _, files in os.walk(root):
        for n in files:
            p = os.path.join(top, n)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = open(p, "rb").read()
    return out


class Ran:
    def __init__(self, code=0, err=b""):
        self.returncode, self.stderr = code, err


# ---- bwrap argv: overlay + a read-only bind over every kit entry, a new folder bound as a whole
game = os.path.join(tmp, "lib", "steamapps", "common", "G", "bin")
layer = os.path.join(tmp, "kits", "10", "files")
write(os.path.join(game, "g.exe"), b"exe")
write(os.path.join(game, "amd_fidelityfx_dx12.dll"), b"the game's own")
for rel in ("dxgi.dll", "OptiScaler.ini", "amd_fidelityfx_dx12.dll", "OptiScaler/plugins/OptiPatcher.asi"):
    write(os.path.join(layer, *rel.split("/")), b"kit " + rel.encode())
work = velakit.work_dir(os.path.dirname(game), game, "10")
assert work == os.path.join(tmp, "lib", "steamapps", "common", ".vela-10"), work
argv = vela_launch.bwrap_argv(game, layer, work, "bwrap")
assert argv[:10] == ["bwrap", "--dev-bind", "/", "/", "--overlay-src", layer, "--overlay", game, work, game], argv
assert argv[-1] == "--"
binds = [argv[i:i + 3] for i in range(10, len(argv) - 1, 3)]
assert all(b[0] == "--ro-bind" for b in binds), binds
assert [os.path.relpath(b[2], game).replace(os.sep, "/") for b in binds] == \
    ["OptiScaler", "OptiScaler.ini", "amd_fidelityfx_dx12.dll", "dxgi.dll"], binds
os.makedirs(os.path.join(game, "OptiScaler", "plugins"))  # the game has the folder: its file is bound, not the folder
binds = [argv[i + 2] for i in range(10, len(vela_launch.bwrap_argv(game, layer, work)) - 1, 3)]
assert os.path.join(game, "OptiScaler", "plugins", "OptiPatcher.asi") in vela_launch.bwrap_argv(game, layer, work)
os.rmdir(os.path.join(game, "OptiScaler", "plugins"))
os.rmdir(os.path.join(game, "OptiScaler"))
write(os.path.join(game, "dxgi.dll", "x"), b"")  # a folder where the kit has a file: refused, never mounted
try:
    vela_launch.bwrap_argv(game, layer, work)
    raise AssertionError("file/folder clash accepted")
except RuntimeError:
    pass
shutil.move(os.path.join(game, "dxgi.dll"), os.path.join(tmp, "moved-dxgi"))

# ---- env: the player's value wins, WINEDLLOVERRIDES appended with ';'
env, args = vela_launch.split_options('WINEDLLOVERRIDES="dxgi=n,b" OPTI_SGSR3_STUDENT=1 VELA_NPU=1 %command% -dx12')
assert env == {"WINEDLLOVERRIDES": "dxgi=n,b", "OPTI_SGSR3_STUDENT": "1", "VELA_NPU": "1"} and args == ["-dx12"], (env, args)
m = vela_launch.merge_env({"WINEDLLOVERRIDES": "winmm=n,b", "VELA_NPU": "0"}, env)
assert m["WINEDLLOVERRIDES"] == "winmm=n,b;dxgi=n,b" and m["VELA_NPU"] == "0" and m["OPTI_SGSR3_STUDENT"] == "1", m
assert vela_launch.merge_env({"WINEDLLOVERRIDES": "dxgi=n,b"}, env)["WINEDLLOVERRIDES"] == "dxgi=n,b"
assert vela_launch.merge_env({}, env)["WINEDLLOVERRIDES"] == "dxgi=n,b"

# ---- plan: kit missing -> None (plain exec); probe failure -> raises (main() then runs the command untouched)
kits = os.path.join(tmp, "kits")
base = {"SteamAppId": "10"}
assert vela_launch.plan(["/g"], base, kits) is None, "no launch.json"
assert vela_launch.plan(["/g"], {}, kits) is None, "no appid"
kit = {"on": True, "game_dir": game, "layer": layer, "work_dir": work, "files": ["dxgi.dll", "OptiScaler.ini"],
       "options": 'WINEDLLOVERRIDES="dxgi=n,b" %command% -dx12'}
write(os.path.join(kits, "10", "launch.json"), json.dumps({**kit, "on": False}))
assert vela_launch.plan(["/g"], base, kits) is None, "kit off"
write(os.path.join(kits, "10", "launch.json"), json.dumps(kit))
calls = []
cmd, penv = vela_launch.plan(["/g", "-x"], {**base, "WINEDLLOVERRIDES": "a=b"}, kits,
                             run=lambda a, **k: calls.append(a) or Ran())
assert calls and calls[0][-1] == "true" and calls[0][:-1] == cmd[:len(calls[0]) - 1], calls
assert cmd[-3:] == ["/g", "-x", "-dx12"] and penv["WINEDLLOVERRIDES"] == "a=b;dxgi=n,b" and penv[vela_launch.DONE] == "1"
try:
    vela_launch.plan(["/g"], base, kits, run=lambda a, **k: Ran(1, b"bwrap: Can't mount overlay\n"))
    raise AssertionError("failed probe accepted")
except RuntimeError as e:
    assert "overlay probe failed" in str(e) and "mount overlay" in str(e), e
ok, why = vela_launch.probe(["/nonexistent/bwrap", "--"])
assert not ok and why, why
write(os.path.join(kits, "10", "launch.json"), json.dumps({**kit, "files": ["dxgi.dll", "gone.dll"]}))
try:
    vela_launch.plan(["/g"], base, kits, run=lambda a, **k: Ran())
    raise AssertionError("incomplete kit accepted")
except RuntimeError as e:
    assert "gone.dll" in str(e)

# ---- main(): never in the way of the game
real_execvpe, real_execvp = os.execvpe, os.execvp
seen = []
os.execvpe = lambda f, a, e: seen.append(("kit", a, e))
os.execvp = lambda f, a: seen.append(("plain", a, dict(os.environ)))
saved_env = dict(os.environ)
try:
    vela_launch.KITS = kits
    os.environ.update({"STEAM_COMPAT_APP_ID": "10"})  # upper case: Windows folds env names
    os.environ.pop(vela_launch.DONE, None)
    real_run = subprocess.run
    subprocess.run = lambda a, **k: Ran(1, b"no overlay")
    try:
        vela_launch.main(["FOO=1", "/g", "-x"])  # the player's FOO=1 in front: set, not run as a command
    finally:
        subprocess.run = real_run
    assert seen[-1][0] == "plain" and seen[-1][1] == ["/g", "-x"] and seen[-1][2]["FOO"] == "1", seen[-1][:2]
    log = open(os.path.join(kits, "10", "launch.log"), encoding="utf-8").read()
    assert "Vela left out of this launch" in log, log
    os.environ[vela_launch.DONE] = "1"  # second pass (options + Armada wrapper): nothing added
    vela_launch.main(["/g"])
    assert seen[-1][0] == "plain" and seen[-1][1] == ["/g"]
finally:
    os.environ.clear()
    os.environ.update(saved_env)

# ---- Armada PR 839: detection and plugin.json
alaunch = os.path.join(tmp, "armada-game-launch")
write(alaunch, "def main():\n    pass\n")
assert not velakit.wrapper_supported(alaunch) and not velakit.wrapper_supported(os.path.join(tmp, "none"))
write(alaunch, "def plugin_wrappers(plugins_dir=DECKY_PLUGINS):\n    armada = data.get(\"armada\")\n")
assert velakit.wrapper_supported(alaunch)
plugin_dir = os.path.join(HERE, "..")
pj = json.load(open(os.path.join(plugin_dir, "plugin.json"), encoding="utf-8"))
assert pj["armada"]["launch"]["wrapper"] == "~/" + velakit.LAUNCHER_REL and velakit.plugin_declares_wrapper(plugin_dir)
assert all(k in pj for k in ("name", "author", "flags", "api_version")), "Decky's keys stay"

# ---- installed launcher: the plugin's kits folder written in; it runs with the plugin gone (uninstall)
home = os.path.join(tmp, "home")
fake_plugin = os.path.join(tmp, "plugin", "py_modules")
os.makedirs(fake_plugin)
shutil.copy(os.path.join(HERE, "..", "py_modules", "vela_launch.py"), fake_plugin)
dst = velakit.install_launcher(home, kits, os.path.join(fake_plugin, "vela_launch.py"))
assert os.path.normpath(dst) == os.path.join(home, ".local", "share", "vela", "bin", "vela-launch")
assert velakit.install_launcher(home, kits, os.path.join(fake_plugin, "vela_launch.py")) == dst  # unchanged: kept
shutil.move(os.path.join(tmp, "plugin"), os.path.join(tmp, "plugin-uninstalled"))
mod = importlib.machinery.SourceFileLoader("vela_launch_installed", dst).load_module()
assert mod.KITS == os.path.abspath(kits), mod.KITS
seen.clear()
saved_env = dict(os.environ)
try:
    os.environ["STEAM_COMPAT_APP_ID"] = "77"  # a game with no kit
    os.environ.pop(mod.DONE, None)
    mod.main(["/usr/bin/game", "--arg"])
    assert seen == [("plain", ["/usr/bin/game", "--arg"], seen[0][2])], seen
    shutil.move(kits, kits + "-gone")  # the whole settings folder gone too
    os.environ["STEAM_COMPAT_APP_ID"] = "10"
    mod.main(["/usr/bin/game"])
    assert seen[-1][:2] == ("plain", ["/usr/bin/game"]), seen
    shutil.move(kits + "-gone", kits)
finally:
    os.environ.clear()
    os.environ.update(saved_env)
    os.execvpe, os.execvp = real_execvpe, real_execvp

# ---- sgsr3: an overlay setup writes nothing into the game folder; Off leaves it as it was
assets = os.path.join(tmp, "assets", "sgsr3")
for a in ("OptiScaler.dll", "OptiPatcher.asi"):
    write(os.path.join(assets, a), b"asset " + a.encode())
shutil.copy(os.path.join(HERE, "..", "assets", "sgsr3", "OptiScaler.ini"), assets)
shutil.copy(os.path.join(HERE, "..", "assets", "sgsr3", "VERSION.json"), assets)
settings = os.path.join(tmp, "settings")
lib = os.path.join(home, ".local", "share", "Steam")
write(os.path.join(lib, "steamapps", "libraryfolders.vdf"), '"libraryfolders" { "0" { "path" "%s" } }' % lib.replace("\\", "/"))
write(os.path.join(lib, "steamapps", "appmanifest_4242.acf"), '"AppState" { "appid" "4242" "name" "Ov Game" "installdir" "OvGame" }')
groot = os.path.join(lib, "steamapps", "common", "OvGame")
gexe = os.path.join(groot, "OvGame", "Binaries", "Win64")
write(os.path.join(groot, "OvGame.exe"), b"stub" * 100)
write(os.path.join(gexe, "OvGame-Win64-Shipping.exe"), b"x" * 2000)
write(os.path.join(gexe, "dxgi.dll"), b"the game's dxgi")
write(os.path.join(groot, "Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll"), b"dlss")
before = tree(groot)
s3 = sgsr3.Sgsr3(assets, home, settings, log=lambda *a: None)
probes = []
s3.probe = lambda exe, w, scratch: probes.append(exe) or (True, "")
s3.new_mode = "launcher"
assert s3.setup_mode("4242", "%command%") == "launcher" and probes == [gexe], probes
r = s3.enable("4242", {}, launchopts.CMD)
assert tree(groot) == before, "nothing written into the game folder"
assert s3.install_mode("4242") == "launcher" and s3.status("4242")["install_mode"] == "launcher"
kitroot = os.path.join(settings, "kits", "4242")
lj = json.load(open(os.path.join(kitroot, "launch.json"), encoding="utf-8"))
assert lj["on"] and lj["game_dir"] == gexe and lj["layer"] == os.path.join(kitroot, "files"), lj
assert "dxgi.dll" in lj["files"] and "OptiScaler.ini" in lj["files"] and 'WINEDLLOVERRIDES="dxgi=n,b"' in lj["options"]
ini = open(os.path.join(kitroot, "files", "OptiScaler.ini"), encoding="utf-8").read()
assert "LogFileName=Z:" in ini and "OptiScaler.log" in ini, "log outside the game folder"
assert s3.status("4242")["intact"] is True
steam = s3.steam_options("4242", "FOO=1 /usr/libexec/armada/armada-game-launch %command%", True)
assert steam == velakit.LAUNCHER_TOKEN + " FOO=1 /usr/libexec/armada/armada-game-launch %command%", steam
assert s3.steam_options("4242", steam, True) == steam
off = s3.disable("4242", s3.kit_options("4242"))
assert off["launch_options"] == launchopts.CMD and tree(groot) == before and s3.install_mode("4242") is None
assert not json.load(open(os.path.join(kitroot, "launch.json"), encoding="utf-8"))["on"]
assert s3.steam_options("4242", steam, False) == "FOO=1 /usr/libexec/armada/armada-game-launch %command%"
assert s3.steam_options("4242", "%command%", False) == "%command%"

# ---- probe failure: the copy mode, as before
s3._probes.clear()
s3.probe = lambda exe, w, scratch: (False, "bwrap: Can't mount overlay")
assert s3.setup_mode("4242") == "copy"
r = s3.enable("4242", {}, "%command%")
assert s3.install_mode("4242") == "copy" and os.path.isfile(os.path.join(gexe, ".upscale-sgsr3", "state.json"))
assert open(os.path.join(gexe, "dxgi.dll"), "rb").read() == b"asset OptiScaler.dll"

# ---- migration copy -> overlay: files back (md5-checked), player's tokens exact, kit on
s3._probes.clear()
s3.probe = lambda exe, w, scratch: (True, "")
assert s3.migrate_pending("4242")
player = "FOO=1 /usr/libexec/armada/armada-game-launch %command% -novid"
s3.disable("4242", r["launch_options"])
r = s3.enable("4242", {}, player, mode="copy")  # the copy setup with the player's own tokens
st = s3.read_state(gexe)
copy_opts = r["launch_options"]
assert copy_opts != player and "FOO=1" in copy_opts
res = s3.disable("4242", copy_opts)
assert res["launch_options"] == player and tree(groot) == before, "restored exactly"
r = s3.enable("4242", st.get("opts"), s3.kit_options("4242"), res.get("resolution"), mode="launcher")
assert s3.install_mode("4242") == "launcher" and tree(groot) == before
assert s3.steam_options("4242", res["launch_options"], True) == velakit.LAUNCHER_TOKEN + " " + player
# a changed game file blocks the restore: the copy setup stays
s3.disable("4242", s3.kit_options("4242"))
s3._probes.clear()
s3.probe = lambda exe, w, scratch: (False, "no")
r = s3.enable("4242", {}, player)
write(os.path.join(gexe, ".upscale-sgsr3", "backup", "dxgi.dll"), b"tampered backup")
try:
    s3.disable("4242", r["launch_options"])
    raise AssertionError("bad backup accepted")
except RuntimeError as e:
    assert "restore check failed" in str(e)
# ---- a 0.8.19 record with launch_prev == launch_set: Vela's tokens out of Steam's options
bmi_steam = ("VELA_NPU=1 VELA_NPU_LOG_FILE=/var/home/armada/homebrew/settings/upscale/npu/layer-1178830.log "
             "WINEDLLOVERRIDES=dxgi=n,b OPTI_SGSR3_STUDENT=1 OPTI_SGSR3_LINES=2 /usr/libexec/armada/armada-game-launch %command%")
bmi_kit = ("VELA_NPU=1 VELA_NPU_LOG_FILE=/var/home/armada/homebrew/settings/upscale/npu/layer-1178830.log "
           'WINEDLLOVERRIDES="dxgi=n,b" OPTI_SGSR3_STUDENT=1 OPTI_SGSR3_LINES=2 %command%')
assert sgsr3.drop_kit_env(bmi_steam, bmi_kit) == "/usr/libexec/armada/armada-game-launch %command%"
# the player's own tokens and anything after %command% stay byte for byte; a value that differs is his
mine = "FOO=1  WINEDLLOVERRIDES=winmm=n,b OPTI_SGSR3_LINES=3 /usr/libexec/armada/armada-game-launch %command% VELA_NPU=1 -novid"
assert sgsr3.drop_kit_env(mine, bmi_kit) == mine
assert sgsr3.drop_kit_env("VELA_NPU=1 FOO=1 %command%", bmi_kit) == "FOO=1 %command%"
assert sgsr3.drop_kit_env(None, bmi_kit) is None and sgsr3.drop_kit_env("%command%", "%command%") == "%command%"
print("test_overlay OK")
