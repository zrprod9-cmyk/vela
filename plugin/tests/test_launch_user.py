"""0.8.18: the player's own launch options survive Vela on and Off (FINAL FANTASY VII REBIRTH, 2909400, needs two variables
on Proton). Rule: turning Vela off removes only the plugin's own arguments and keeps everything that was there before or
was added later, in its place and order."""
import json
import os
import re
import shutil
import sys
import tempfile
import traceback
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("VELA_TEST_ROOT") or os.path.join(HERE, "..")  # another tree's plugin (the release, for the "before")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import anygame  # noqa: E402
import gamecfg  # noqa: E402
import gamecheck  # noqa: E402
import gameutil  # noqa: E402
import latency  # noqa: E402
import live  # noqa: E402
import npu  # noqa: E402
import sgsr3  # noqa: E402
import upscale  # noqa: E402

tmp = tempfile.mkdtemp(prefix="vela_lu_")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


decky = types.ModuleType("decky")
decky.logger = types.SimpleNamespace(info=lambda *a: None, error=lambda *a: None, exception=lambda *a: None)
decky.DECKY_USER_HOME = os.path.join(tmp, "home")
decky.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "psettings")
sys.modules["decky"] = decky
sys.path.insert(0, ROOT)
import main  # noqa: E402

live.TMP_DIR = os.path.join(tmp, "vela-live")
gameutil.running_appids = lambda proc="/proc": set()
home = decky.DECKY_USER_HOME
settings = decky.DECKY_PLUGIN_SETTINGS_DIR
steam = os.path.join(home, ".local/share/Steam")
write(os.path.join(steam, "steamapps/libraryfolders.vdf"), '"libraryfolders"\n{\n "0"\n {\n  "path"  "%s"\n }\n}\n' % steam)


def ue_game(appid, name, folder, exe):
    write(os.path.join(steam, f"steamapps/appmanifest_{appid}.acf"),
          '"AppState"\n{\n "appid" "%s"\n "name" "%s"\n "installdir" "%s"\n}\n' % (appid, name, folder))
    root = os.path.join(steam, "steamapps/common", folder)
    write(os.path.join(root, f"{exe}/Binaries/Win64/{exe}-Win64-Shipping.exe"), b"x" * 2_000_000 + b"d3d12.dll")
    write(os.path.join(root, "Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll"), b"d")
    return os.path.join(root, f"{exe}/Binaries/Win64")


FF7 = "2909400"   # FINAL FANTASY VII REBIRTH: no profile, Unreal (Vela adds -dx12 when the options pass no API flag)
BMI = "1178830"   # Bright Memory: Infinite: its own Steam launch entries pass -dx11 / -dx12 (appinfo)
ML = "1363080"    # Manor Lords: the profile adds VKD3D_SHADER_MODEL=6_6 VKD3D_FEATURE_LEVEL=12_2
DIRS = {FF7: ue_game(FF7, "FINAL FANTASY VII REBIRTH", "FF7R", "End"),
        BMI: ue_game(BMI, "Bright Memory: Infinite", "BMI", "BrightMemoryInfinite"),
        ML: ue_game(ML, "Manor Lords", "ML", "ManorLords")}

# appinfo.vdf v29 with Bright Memory's two entries (the format sgsr3.appinfo_api_args reads; as tests/test_presets.py)
data = b"\x00appinfo\x00\x00config\x00\x00launch\x00" + b"".join(
    b"\x00" + str(i).encode() + b"\x00\x01executable\x00BMI.exe\x00\x01arguments\x00" + a.encode() + b"\x00\x08"
    for i, a in enumerate(["-dx11", "-dx12"])) + b"\x08\x08\x08"
body = int(BMI).to_bytes(4, "little") + len(data).to_bytes(4, "little") + data + (0).to_bytes(4, "little")
write(os.path.join(steam, "appcache", "appinfo.vdf"), (0x07564429).to_bytes(4, "little") + b"\x01\x00\x00\x00" +
      (0).to_bytes(8, "little") + body)

assets = os.path.join(tmp, "assets")
shutil.copytree(os.path.join(ROOT, "assets", "sgsr3"), os.path.join(assets, "sgsr3"),
                ignore=shutil.ignore_patterns("*.dll", "*.asi"))
for f in ("OptiScaler.dll", "OptiPatcher.asi", "amd_fidelityfx_upscaler_vk.dll", "amd_fidelityfx_dx12.dll"):
    write(os.path.join(assets, "sgsr3", f), b"fake " + f.encode())
write(os.path.join(assets, "npu", "libVkLayer_VELA_npu.so"), b"so")
shutil.copy(os.path.join(ROOT, "assets", "npu", "VkLayer_VELA_npu.json"), os.path.join(assets, "npu"))
main.ASSETS = assets
unix = os.path.join(tmp, "unix")
write(unix, "Num RefCount Protocol Flags Type St Inode Path\n" +
      "".join(f"0: 00000002 0 10000 0001 01 1{i} @{n}\n" for i, n in enumerate(
          ["fsr4npud", "fsr4npud_nf4_k1h", "fsr4npud_nf4_k1h_360"])))


class Nop:
    def __getattr__(self, name):
        return lambda *a, **k: {"on": False}


def plugin():
    P = main.Plugin()
    P.up = upscale.Upscaler(settings, home, log=lambda *a: None)
    P.up.refresh = lambda: None
    P.s3 = sgsr3.Sgsr3(os.path.join(assets, "sgsr3"), home, settings, log=lambda *a: None)
    P.ag = anygame.AnyGame(os.path.join(assets, "layer"), home, settings, log=lambda *a: None)
    P.lat = latency.Latency(home, settings, log=lambda *a: None)
    P.cfg = gamecfg.GameCfg(settings, log=lambda *a: None)
    P.live = live.Live(settings, log=lambda *a: None)
    P.npu = npu.Npu(os.path.join(assets, "npu"), home, settings, log=lambda *a: None, proc_net_unix=unix, unit_dirs=(),
                    systemctl=lambda: None)
    P.floor = Nop()
    P.cpu = Nop()
    P.check = gamecheck.GameCheck(settings, log=lambda *a: None)
    P._win_restart = set()
    return P


P = plugin()
TOK = re.compile(r"""(?:"[^"]*"?|'[^']*'?|[^\s"'])+""")


def toks(s):
    return TOK.findall(s or "")


def side(s, tok):
    """Where a token stands: "before" / "after" %command% (None: not there)."""
    t = toks(s)
    if tok not in t:
        return None
    return "after" if "%command%" in t and t.index(tok) > t.index("%command%") else "before"


def keeps(new, old, why):
    """Every token of old is in new on the same side of %command% and in the same order (Vela's tokens in between)."""
    t_new, t_old = toks(new), toks(old)
    has_cmd = "%command%" in t_old
    for tok in t_old:
        if tok == "%command%":
            continue
        assert tok in t_new, f"{why}: the player's {tok!r} is gone: {new!r}"
        want = side(old, tok) if has_cmd else "after"  # no %command%: Steam appends the options after the command
        assert side(new, tok) == want, f"{why}: {tok!r} moved {want} -> {side(new, tok)}: {new!r}"
    order = [t for t in t_new if t in t_old and t != "%command%"]
    assert order == [t for t in t_old if t != "%command%"], f"{why}: order changed: {order} vs {t_old}"
    for q in re.findall(r'"[^"]*"', old):
        assert q in new, f"{why}: quoted {q!r} changed: {new!r}"


def cycle(appid, user, model="fsr4"):
    """ON with the player's options -> (Vela's options). Off undone by the caller."""
    r = P._apply_model(appid, model, user, "Default")
    keeps(r["launch_options"], user, "on")
    return r["launch_options"]


def off(appid, current):
    return P._apply_model(appid, "off", current, None)["launch_options"]


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


# ---- a game that needs two variables FINAL FANTASY VII REBIRTH needs on Proton (feature level 12_2, shader model 6.6)
FF7_USER = "VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 %command%"


@case
def ff7_two_env_kept_on_and_off():
    on = cycle(FF7, FF7_USER)
    assert off(FF7, on) == FF7_USER


@case
def ff7_typed_back_after_on_kept_on_off():
    """0.8.16 took them out; the player typed them back while Vela was on; Off took them out again (the strip path)."""
    on = cycle(FF7, FF7_USER)
    edited = "VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 " + on.replace("VKD3D_FEATURE_LEVEL=12_2 ", "")\
        .replace("VKD3D_SHADER_MODEL=6_6 ", "")
    out = off(FF7, edited)
    assert out == FF7_USER, out


@case
def two_args_after_command():
    user = "%command% -dx12 -something"
    on = cycle(FF7, user)
    assert on.endswith("%command% -dx12 -something"), on  # the player's -dx12: no second one from Vela
    assert off(FF7, on) == user


@case
def env_before_and_two_args_after_with_edits():
    user = "PROTON_ENABLE_NVAPI=1 %command% -arg1 -arg2"
    on = cycle(FF7, user)
    edited = "PROTON_LOG=1 " + on + " -arg3"  # the player adds a variable in front and an argument at the end
    out = off(FF7, edited)
    assert out == "PROTON_LOG=1 PROTON_ENABLE_NVAPI=1 %command% -arg1 -arg2 -arg3", out


@case
def quotes_and_spacing_kept():
    user = 'WINEDLLOVERRIDES="xinput1_3=n,b" DXVK_HUD="fps,  gpuload" %command% -name "Two  Words" -x'
    on = cycle(FF7, user)
    assert 'DXVK_HUD="fps,  gpuload"' in on and '-name "Two  Words"' in on, on
    assert off(FF7, on) == user
    on = cycle(FF7, user)
    assert off(FF7, on + " -late") == user + " -late"


@case
def own_dxgi_override_kept():
    """The player's own WINEDLLOVERRIDES="dxgi=n,b" (ReShade, an earlier OptiScaler) is his: not Vela's to take."""
    user = 'WINEDLLOVERRIDES="dxgi=n,b" %command% -windowed'
    on = cycle(FF7, user)
    out = off(FF7, on + " -x")
    assert out == user + " -x", out


@case
def options_without_command():
    user = "-dx11 -nosplash"
    on = cycle(FF7, user)
    assert toks(on)[-2:] == ["-dx11", "-nosplash"] and "-dx12" not in toks(on), on
    assert off(FF7, on) == user
    on = cycle(FF7, user)
    assert off(FF7, on + " -x") == user + " -x"


@case
def user_dx12_with_own_api_entries_kept():
    """Bright Memory's entries pass -dx11 / -dx12: Vela adds no flag, and the player's own -dx12 stays (on and off)."""
    user = "%command% -dx12 -nosplash"
    on = cycle(BMI, user)
    assert toks(on).count("-dx12") == 1, on
    fixed = P._launch_fix(BMI, on)["launch_options"]
    keeps(fixed, user, "launch_fix")
    assert off(BMI, fixed) == user
    on = cycle(BMI, user)
    assert off(BMI, on + " -x") == user + " -x"


@case
def user_dx11_not_dropped_by_dx12_fix():
    user = "PROTON_NO_ESYNC=1 %command% -dx11"
    on = cycle(BMI, user)
    assert not P.s3.dx12_fix_pending(BMI), "the player's flag is not Vela's to fix"
    fixed = P._launch_fix(BMI, on)["launch_options"]
    keeps(fixed, user, "launch_fix")
    assert off(BMI, fixed) == user


@case
def profile_token_the_player_had():
    """Manor Lords' profile adds VKD3D_SHADER_MODEL / VKD3D_FEATURE_LEVEL: the player had one of them himself."""
    user = "VKD3D_SHADER_MODEL=6_6 %command% -windowed"
    on = cycle(ML, user)
    assert toks(on).count("VKD3D_SHADER_MODEL=6_6") == 1 and "VKD3D_FEATURE_LEVEL=12_2" in toks(on), on
    out = off(ML, on + " -x")
    assert out == user + " -x", out


@case
def model_switches_keep_user():
    user = "VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 %command% -a -b"
    on = cycle(FF7, user)
    for model in ("fsr3", "sgsr2", "fsr4fast", "fsr4"):
        on = P._apply_model(FF7, model, on, None)["launch_options"]
        keeps(on, user, model)
    assert off(FF7, on) == user


@case
def steam_not_read_changes_nothing():
    """The panel could not read Steam's options (null after its wait): no setup, no rewrite, no Off on a guess."""
    user = "VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 %command%"
    try:
        r = P._apply_model(FF7, "fsr4", None, "Default")
    except RuntimeError:
        r = None
    assert r is None or r["launch_options"] is None, f"options written blind: {r and r['launch_options']!r}"
    assert not P.s3.status(FF7)["installed"], "set up without the player's options"
    on = cycle(FF7, user)
    try:
        r = P._apply_model(FF7, "off", None, None)
    except RuntimeError:
        r = None
    assert r is None or r["launch_options"] is None, f"Off wrote options blind: {r and r['launch_options']!r}"
    assert P.s3.status(FF7)["installed"]
    assert off(FF7, on) == user


@case
def llfg_startup_cleanup_then_panel():
    """0.8.9's LL / FG cleanup at plugin start runs with no Steam options (None), the panel then sends the real ones."""
    user = "VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 %command% -a"
    on = cycle(FF7, user)
    P._llfg_off(FF7, None)
    out = P._llfg_off(FF7, on)["launch_options"]
    keeps(out, user, "llfg")
    assert off(FF7, out) == user
    on = cycle(FF7, user)
    P._llfg_off(FF7, None)
    assert off(FF7, on + " -x") == user + " -x"


@case
def on_off_on_off_exact():
    user = "  ENV_A=1   %command%   -a  -b  "
    for _ in range(2):
        on = cycle(FF7, user)
        assert off(FF7, on) == user


@case
def edited_wrapper_kept():
    user = "~/lsfg /usr/libexec/armada/armada-game-launch %command% -fullscreen"
    on = cycle(FF7, user)
    edited = on.replace("~/lsfg ", "")  # the player removed a wrapper: his edit stays
    assert off(FF7, edited) == "/usr/libexec/armada/armada-game-launch %command% -fullscreen"


@case
def hand_set_vela_keys_are_players():
    """A key the player typed himself after Vela set the game up (A/B tests) is his edit: Off keeps it."""
    user = "%command% -a"
    on = cycle(FF7, user)
    out = off(FF7, "OPTI_SGSR3_SQ=1 " + on)
    assert out == "OPTI_SGSR3_SQ=1 %command% -a", out


def legacy_0816(appid, user, old_s3_set):
    """Turn the game's states into what 0.8.16 left (no record, Vela's options without the player's stripped tokens);
    returns Steam's options as 0.8.16 wrote them."""
    d = DIRS[appid]
    st = P.s3.read_state(d)
    st.pop("added", None)
    st.update({"launch_prev": user, "launch_set": old_s3_set, "dx12_added": "-dx12" in toks(old_s3_set)})
    P.s3._write_state(d, st)
    n = P.npu.read(appid)
    n.pop("added", None)
    n.update({"launch_prev": old_s3_set, "launch_set": " ".join(n["tokens"] + [old_s3_set])})
    P.npu._write(appid, n)
    return n["launch_set"]


@case
def ff7_set_up_by_0816_gets_its_args_back():
    """A player's state after 0.8.16 had taken the two variables out. 0.8.17's launch_fix (the panel, once) rebuilds the
    options from his own (they were not edited since), then Off is exact."""
    on = cycle(FF7, FF7_USER)
    old = on.split("%command%")[0].split(" VKD3D_FEATURE_LEVEL")[0]  # only to find the npu tokens' end
    s3_set = P.s3.launch_set(FF7).replace("VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 ", "")
    steam = legacy_0816(FF7, FF7_USER, s3_set)
    assert "VKD3D" not in steam and old
    assert P._game_state(FF7)["launch_fix"] is True
    fixed = P._launch_fix(FF7, steam)["launch_options"]
    keeps(fixed, FF7_USER, "launch_fix of a 0.8.16 setup")
    assert "VELA_NPU=1" in fixed and 'WINEDLLOVERRIDES="dxgi=n,b"' in fixed, fixed
    assert P._game_state(FF7)["launch_fix"] is False
    assert off(FF7, fixed) == FF7_USER


@case
def ff7_set_up_by_0816_edited_then_off():
    """0.8.16 setup, the player typed his two variables back himself: launch_fix keeps his options, Off takes only Vela's."""
    on = cycle(FF7, FF7_USER)
    s3_set = P.s3.launch_set(FF7).replace("VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 ", "")
    steam = legacy_0816(FF7, FF7_USER, s3_set)
    typed = "VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 " + steam
    fixed = P._launch_fix(FF7, typed)["launch_options"]
    assert off(FF7, fixed) == FF7_USER, (on, fixed)


@case
def bmi_0815_dx12_of_vela_still_goes():
    """0.8.14 added -dx12 to Bright Memory (no entries known then): 0.8.15's fix still takes Vela's -dx12 out."""
    user = "%command% -nosplash"
    on = cycle(FF7, user)
    assert toks(on)[-1] == "-dx12"
    d = DIRS[FF7]
    st = P.s3.read_state(d)
    assert st["added"]["args"] == ["-dx12"] and st["dx12_added"]
    P.s3.api_args = lambda appid: {"-dx11", "-dx12"}  # as if FF7 had Bright Memory's two entries
    try:
        assert P.s3.dx12_fix_pending(FF7)
        fixed = P._launch_fix(FF7, on)["launch_options"]
        assert "-dx12" not in toks(fixed) and fixed.endswith("%command% -nosplash"), fixed
        assert off(FF7, fixed) == user
    finally:
        del P.s3.api_args


failed = []
for fn in CASES:
    for appid in (FF7, BMI, ML):  # each case from a clean game
        if P.s3.status(appid)["installed"]:
            try:
                P._apply_model(appid, "off", P.s3.launch_set(appid), None)
            except Exception:
                pass
    try:
        fn()
        print(f"ok   {fn.__name__}")
    except Exception as e:  # noqa: BLE001
        failed.append(fn.__name__)
        print(f"FAIL {fn.__name__}: {e}" if isinstance(e, AssertionError) else f"FAIL {fn.__name__}: "
              f"{traceback.format_exc().strip().splitlines()[-1]}")
print(f"test_launch_user: {len(CASES) - len(failed)}/{len(CASES)} ok" + (f", failed: {', '.join(failed)}" if failed else ""))
sys.exit(1 if failed else 0)
