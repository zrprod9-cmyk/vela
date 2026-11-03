"""SGSR 3 inside the game: our OptiScaler fork (Sloptiscaler + SGSR 3) put next to a Steam game's exe.

The game keeps its own DLSS / FSR 2+ / XeSS path; OptiScaler (loaded as dxgi.dll) takes those calls and runs SGSR 3
instead. What is installed into the exe folder:
  dxgi.dll                             our OptiScaler build (assets/sgsr3/OptiScaler.dll)
  OptiScaler.ini                       assets/sgsr3/OptiScaler.ini with the keys from ini_values()
  OptiScaler/plugins/OptiPatcher.asi   opens the DLSS input of UE games on a non-NVIDIA GPU without spoofing
  amd_fidelityfx_dx12.dll              only with frame generation: FidelityFX SDK 1.1.4 (MIT), whose swapchain shows
                                       the frames SGSR-FG generates (OptiScaler's FGOutput=fsrfg)
Everything that was there before is copied to <exe dir>/.upscale-sgsr3/backup first; state.json there lists what
existed, so disable() puts the old files back and removes the ones that were not there. OptiScaler.log written
while SGSR 3 was on is moved to the plugin's settings folder.

OptiScaler's own menu stays hidden (no shortcut keys, no splash): the plugin's panel is the interface.
Low latency (the game's Reflex -> fakenvapi -> LatencyFleX, all inside our OptiScaler) and the frame-rate limit are
ini keys; UE4's r.OneFrameThreadLag=0 is latency.py's.

Wine only loads a dxgi.dll from the game folder with WINEDLLOVERRIDES=dxgi=n,b; the algorithm (v1f is the DLL's
default, OPTI_SGSR3_V1CS=1 = v1, OPTI_SGSR3_STUDENT=1 = the student) and SGSR-FG (OPTI_SGSRFG=1) are environment
variables. All go into the game's Steam launch options. The frontend reads and writes those (SteamClient), this
module only computes the strings, so it stays testable.

Pure standard library: Decky's frozen Python has no html.parser etc.
"""
import hashlib
import json
import os
import re
import shutil
import time

import launchopts
import npu
import presets
import profiles

# v1f: SGSR 3 v1 rewritten for fp16 on Vulkan (fork patch 0005) with the HUD locks and the temporal box hp (0007);
# v1: the D3D12 compute v1 (patch 0003); student: the S4s4 network (0004)
ALGOS = ["v1f", "v1", "student"]
# OptiScaler [UpscaleRatio] override; "game" leaves the ratio to the quality mode picked in the game's menu
RATIOS = ["game", "1.5", "2.0", "2.25", "3.0"]
FPS_LIMITS = ["off", "30", "40"]
# "Smoothing" (0.3.3, the user's "too sharp, DLSS smooths more" from Control): fork environment per level, calibrated
# against DLSS M on Control / DRG / BMI captures.
# 0.4.0: the softness only on young history (OPTI_SGSR3_FRESH=<k>, report 31 S1), GPU replay against DLSS M
#: k 0.5 beats the global k0 0.5 of 0.3.3 everywhere
# (Control +0.23 / +0.38 dB over sharp, the global one +0.15 / +0.27; DRG +0.10 / +0.06), k 1 has DLSS M's stairs
# in Control (jag 0.99) at -0.3 dB. OPTI_SGSR3_SOFT = the global k0 of the v1f kernel (kernelbias * 2^-k0), kept
# for A/B.
SMOOTH = {"sharp": {}, "dlss": {"OPTI_SGSR3_FRESH": "0.5"}, "soft": {"OPTI_SGSR3_FRESH": "1.0"}}
SMOOTH_ENV_KEYS = ("OPTI_SGSR3_SOFT", "OPTI_SGSR3_FRESH")
# v1f switches of (fork 0007, one key each; set by game profiles, profiles.py "env"):
# SQ sqrt storage (dark flicker), FLOOR box floor, NCAP N <= 4 in motion, FRESH=<k> softness on young history only,
# DISOCC disocclusion from the previous depth (ghosting), QSTOCH stochastic history rounding
VF_ENV_KEYS = ("OPTI_SGSR3_SQ", "OPTI_SGSR3_FLOOR", "OPTI_SGSR3_NCAP", "OPTI_SGSR3_FRESH", "OPTI_SGSR3_DISOCC",
               "OPTI_SGSR3_QSTOCH")
# fg: SGSR-FG (experimental, off by default); latency / fps: set by the plugin's "low latency" switch (main.py)
# mode / model / res: what the panel shows (Vela 0.4.0: Quality / Balanced / Speed, overrides in "advanced section");
# algo / ratio are what the fork gets, resolved from them by resolve_mode(). "custom": algo / ratio as given (an
# install of 0.3.x, whose state has no mode, keeps exactly what it had).
MODES = ["quality", "balance", "speed", "custom"]
MODELS = ["auto", "student", "v1f"]
RESES = ["auto"] + RATIOS
# Vela AI (the student) is the main model since 0.4.0 (Vela SR = v1f is its fallback and the choice of game profiles
# where it is better): the mode only picks the render size. Student on the console: 1.9 ms a 540p frame.
MODE_CONFIG = {"quality": ("student", "1.5"), "balance": ("student", "2.0"), "speed": ("student", "2.25")}
DEFAULT_OPTS = {"algo": "student", "ratio": "2.0", "fg": False, "latency": False, "fps": "off", "smooth": "dlss",
                "mode": "balance", "model": "auto", "res": "auto"}


def resolve_mode(opts, appid=None):
    """algo / ratio from the panel's mode, its overrides and the game's profile (profiles.py)."""
    out = dict(opts)
    mode = out.get("mode", "custom")
    if mode not in MODE_CONFIG:
        return out
    algo, ratio = profiles.mode_config(appid, mode, MODE_CONFIG[mode])
    out["algo"] = algo if out.get("model", "auto") == "auto" else out["model"]
    out["ratio"] = ratio if out.get("res", "auto") == "auto" else out["res"]
    return out

STATE_DIR = ".upscale-sgsr3"
FG_DLL = "amd_fidelityfx_dx12.dll"
# 0.5.2: FSR 4 as a Vela live model (the fork's FFX backend): AMD's driver FidelityFX library, which the user puts on
# the console (not in the repository: an AMD driver file); with it and FG_DLL in the game folder OptiScaler finds the
# FSR 4 provider and runs its INT8 model ([FSR] Fsr4ForceModel=2). Off: AMD's driver library gives the Adreno FSR 3.1
# only, the SDK 4.1.2 upscaler offers 3.1.6 / 2.3.4 (29.10); kept so old installs get their files back
AMDXC = "amdxcffx64.dll"
AMDXC_DIR = "homebrew/data/upscale/amd"
FSR4_READY = False
# 0.6.0: FSR 4.1.1 VK INT8 -- the fsr4vk provider (github.com/dvj5411/fsr4vk, GPLv3, AMD model payloads MIT), built by
# Vela from source (tools/build_fsr4vk.sh), runs inside our SGSR 3 on vkd3d-proton's device (Fsr4VkBridge.h)
FSR4VK = "amd_fidelityfx_upscaler_vk.dll"
# 0.8.2: FSR 3.1 as a live model (live "fsr3", fork patch 0014): OptiScaler's FFX backend loads
# amd_fidelityfx_dx12.dll from the exe folder and takes its newest 3.x upscaler (SDK 1.1.4: FSR 3.1.4). The live switch
# needs the DLL there before the game starts, so with a fork that has 0014 (VERSION.json "fsr3_live") ours goes in
# whenever the game has none of its own; a game's own copy stays (its FSR 3.1 runs)
INSTALLED = ["dxgi.dll", "OptiScaler.ini", "OptiScaler/plugins/OptiPatcher.asi", FG_DLL, AMDXC, FSR4VK]
OPTIONAL = {FG_DLL, AMDXC, FSR4VK}    # only while frame generation / FSR 4 needs them; otherwise what the game had stays
RUNTIME = ["OptiScaler.log"]          # written by OptiScaler while the game runs
ASSET_OF = {"dxgi.dll": "OptiScaler.dll", "OptiScaler/plugins/OptiPatcher.asi": "OptiPatcher.asi", FG_DLL: FG_DLL}
REQUIRED_ASSETS = ("OptiScaler.dll", "OptiScaler.ini", "OptiPatcher.asi")
# 0.4.7: games whose FSR 2 is built into the exe (Resident Evil 4): our own interceptor instead of OptiScaler
#, assets/staticfsr/dxgi.dll, no ini; profiles.py "static_fsr2". Its log next to the exe.
STATIC_ASSETS = "staticfsr"
# 0.8.3-re: the interceptor runs FSR 4.1 (fsr4vk provider) and FSR 3.1 (FidelityFX SDK DX12 DLL) too, on the
# panel's live model: both go next to it while the plugin carries them (OPTIONAL: the game's own copy is backed up)
STATIC_INSTALLED = ["dxgi.dll", FSR4VK, FG_DLL]
# 0.8.11: interceptor builds earlier plugin versions shipped as assets/staticfsr/dxgi.dll (md5 of every Vela zip and
# worktree on the dev machine: 0.7.0 8791a3bf, 0.8.3-re c1b336a4, 0.8.4..0.8.10 55096b0c). An install from 0.8.11 on
# records its own hash in the state (dxgi_md5); this list is for installs made before that
# 0.8.15: + 0.8.13 / 0.8.14 532ca224 (a later build, in both zips), so the 0.8.15 interceptor replaces it even with no state
# 0.8.16: + 0.8.15 / 0.8.16 427567f6 (CF build), so the Fast-aware interceptor (model fsr4fast) replaces it the same way
OLD_STATIC_DXGI = frozenset({"8791a3bfaff0b1fad39d7a02294ebfe7", "c1b336a49d7fe0f3fc2b7a8f8b47cc4e",
                             "55096b0ce856e580c8a0ec5b12db0a02", "532ca224529ff73501dfc78164247341",
                             "427567f6d2c7abe12ca170f7bf603814"})
STATIC_RUNTIME = ["sgsr3intercept.log"]
# 0.8.15: the launch options no longer carry VELA_FSR2_ALGO (0.8.13 and 0.8.14 wrote "=ai" after Vela AI had left: the
# interceptor ignores it, the live file / its default fsr4 name the model). A game set up before has it in Steam's options
# and in the states of sgsr3 / npu; drop_legacy_env takes it out of any launch-options text
LEGACY_ENV = re.compile(r"(?<!\S)VELA_FSR2_ALGO=\S*")


# The one Vela AI setting for every game: the current sample's weight w <= amax, a function of the upscale ratio
# alone (0.4.13), the formula the Vela AI training uses: amax = min(0.9, 0.6 (2 / r)^2.2) -- x1.5 0.9, x2 0.6 (0.4.11,
# 8 game captures vs DLSS M), x2.25 0.463, x3 0.246 (fewer render samples per display pixel; RE4 x3 at 0.25: grain
# 92 -> 67, flicker 16.9 -> 12.1;. Future recipes bake it into their resolve; for L6acw the
# same formula sits in the fork (SGSR3Feature_Dx12, from the real sizes when OPTI_SGSR3_AMAX is not given). The plugin
# passes the value for the ratio it knows: OPTI_SGSR3_AMAX for the fork's modes, VELA_AMAX for the FSR 2 interceptor
# (its ratio is the game's FSR 2 mode: taken from the interceptor's last calibration, x2 before the first run)
# OFF for now (coordinator, 26.10): against DLSS M the formula loses with L6acw, which was trained without the cap
# (x3 -0.95 dB, x1.5 worse in every metric; 0.4.13) -- amax stays 0.6 as in 0.4.11 / 0.4.12. One switch turns it
# on (here and AMAX_BY_RATIO in the fork's SGSR3Feature_Dx12.h), for a model trained with it; the cap C <= 0.6
AMAX_BY_RATIO = False
AMAX_C, AMAX_K, AMAX_P = 0.6, 0.6, 2.2
# with the formula off: 0.4.14's Vela AI (S4s4P2b) limits the current sample inside (alpha = 0.75 pp.x), so no extra
# cut in the fork (its AMAX_FIXED = 0, no key). The FSR 2 interceptor carries P2b too since 0.4.15 (its SPIR-V header
# is built into the DLL; 0.4.14 had L6acw there with 0.6): no VELA_AMAX, one setting for every game
AMAX_FIXED = 0.0
STATIC_AMAX_FIXED = 0.0


def amax_for(ratio, fixed=AMAX_FIXED):
    if not AMAX_BY_RATIO:
        return fixed
    r = max(float(ratio), 1.0)
    return round(min(AMAX_C, AMAX_K * (2.0 / r) ** AMAX_P), 3)


def amax_str(ratio, fixed=AMAX_FIXED):
    return f"{amax_for(ratio, fixed):g}"


def static_env(cfg, ratio=None):
    """Launch keys of the interceptor: replace the game's FSR 2 by Vela, no frame snapshots. No model key: the panel's live
    file names the model, the interceptor's own default (FSR 4) runs until it exists (0.8.15: no VELA_FSR2_ALGO=ai)."""
    a = amax_str(ratio or 2.0, STATIC_AMAX_FIXED)
    return ["VELA_FSR2=replace", "RECON_LEVEL=1"] + ([f"VELA_AMAX={a}"] if float(a) > 0 else []) + \
        list(cfg.get("env", ()))  # amax 0: the interceptor's own (none)


def drop_legacy_env(options):
    """Launch options (or None) without VELA_FSR2_ALGO=...; everything else exactly as it was (0.8.17: the spacing too)."""
    if not options or not LEGACY_ENV.search(options):
        return options
    old = [t for t in launchopts.words(options) if LEGACY_ENV.fullmatch(t)]
    return launchopts.remove(options, {"env": old}) if old else options


_RX_VAMAX = re.compile(r"(?:^|\s)(?:VELA|OPTI_SGSR3)_AMAX=([0-9.]+)")


def log_ratio(summary):
    """display / render width of the interceptor's last calibration, None without one."""
    try:
        return int(summary["output"].split("x")[0]) / int(summary["render"].split("x")[0])
    except (TypeError, KeyError, ValueError, ZeroDivisionError, AttributeError):
        return None


_RX_CAL = re.compile(r"vela: calibration ok:.*?render (\d+)x(\d+).*?display (\d+)x(\d+)")
# 0.8.3-re: which model made the interceptor's frames (the last line wins), what the live file asked for
_RX_SRUNS = re.compile(r"vela: model (\w+) runs")
_RX_SWANT = re.compile(r"vela: live \([^\n]*?\): model (\S+)")
# 0.8.13: sgsr2 = SGSR 2 inside the interceptor
# 0.8.13: fsr2 = the game's own FSR 2 (a model that cannot run here, or a key the DLL does not offer)
# 0.8.15: FSR 4 Lite left Vela. The parsers stay tolerant of the Lite lines an older run (or a dormant Lite path of the DLLs)
# wrote: a Lite model / label is FSR 4 (a lighter FSR 4.1 network), its size line is a refusal like any other, "lossy" is gone.
STATIC_MODE = {"ai": "student", "sr": "v1f", "fsr4": "fsr4", "fsr4fast": "fsr4fast", "fsr4lite": "fsr4", "fsr3": "fsr3", "sgsr2": "sgsr2",
               "fsr2": "fsr2"}
_RX_FRAMES = re.compile(r"frames by Vela (\d+)")
# 0.8.13 (RS d23aa8e): an old / unknown live key -> "vela: model 'ai' is not offered (models: ...) -- the game's FSR 2 runs",
# then the live line says "model fsr2"
_RX_SNOTOFF = re.compile(r"vela: model '([^'\n]*)' is not offered")
# 0.8.13: the 10 s fps line names what ran: "fps 39.4 (presents 395 in 10.0 s; frames by Vela 395), SGSR 2 GPU
# 1.60 ms" / ", FSR 3.1" / ", the game's FSR 2" (0.8.12 and older: no suffix, or a wrong "Vela AI GPU" while FSR 3.1 ran)
_RX_SFPS = re.compile(r"fps ([\d.]+) \(presents \d+ in [\d.]+ s; frames by Vela (\d+)\)(?:, ([^\r\n]+?)(?: GPU ([\d.]+) ms)?)?\r?$",
                      re.M)
STATIC_LABEL = {"FSR 4": "fsr4", "FSR 4 Fast": "fsr4fast", "FSR 4 Lite": "fsr4", "FSR 3.1": "fsr3", "SGSR 2": "sgsr2", "the game's FSR 2": "fsr2"}
STATIC_MODELS = ("fsr4", "fsr4fast", "fsr3", "sgsr2")


def static_log_summary(path, since=0.0):
    """What the interceptor did in the last run (sgsr3intercept.log): calibration, which Vela, frames it made."""
    try:
        st = os.stat(path)
    except OSError:
        return None
    if st.st_mtime < since:
        return None
    with open(path, "rb") as f:
        if st.st_size > LOG_TAIL:
            f.seek(st.st_size - LOG_TAIL)
        text = f.read().decode("utf-8", "replace")
    cal = _RX_CAL.findall(text)
    fpsl = list(_RX_SFPS.finditer(text))
    frames = [int(n) for n in _RX_FRAMES.findall(text)]
    ai, sr = "Vela AI ready" in text, "Vela SR ready" in text
    wants = list(_RX_SWANT.finditer(text))
    want = wants[-1].group(1) if wants else None
    want = presets.RETIRED_MODELS.get(want, want)
    seg_at = wants[-1].start() if wants else 0
    # 0.8.13: the key the live file had when the DLL does not offer it (the line right before its "model fsr2" live line)
    offs = [m for m in _RX_SNOTOFF.finditer(text, wants[-2].end() if len(wants) > 1 else 0) if m.start() < seg_at] \
        if want == "fsr2" else []
    not_offered = offs[-1].group(1) if offs else None
    if offs:
        seg_at = offs[-1].start()
    seg = text[seg_at:]
    # which model made the frames: the last "vela: model X runs" since the last live change; none yet: the newest fps
    # line's label (0.8.13 DLL: names the model that ran); else the last run line of the whole log (older DLLs, or the
    # first frame of a switch not there yet: the old model still runs, the panel says "switching")
    runs = _RX_SRUNS.findall(seg)
    fps_seg = [m for m in fpsl if m.start() >= seg_at]
    if runs:
        mode = STATIC_MODE.get(runs[-1])
    elif fps_seg and STATIC_LABEL.get(fps_seg[-1].group(3) or ""):
        mode = STATIC_LABEL[fps_seg[-1].group(3)]
    else:
        allruns = _RX_SRUNS.findall(text)
        mode = STATIC_MODE.get(allruns[-1]) if allruns else "student" if ai else "v1f" if sr else None
    made = bool(frames) and frames[-1] > 0 and mode != "fsr2"
    refused = any(k in seg for k in ("is not available here", "frame failed", "FSR 4.1 Lite runs only "))
    # 0.8.13: the chosen model did not run -> "model fsr2 runs ... (the game's own FSR 2)"
    to_fsr2 = want in STATIC_MODELS and mode == "fsr2"
    fsr4 = _RX_FSR4.findall(seg)
    jit = "not_fsr2" if "is not FSR 2's Halton sequence" in text else "locked" if "jitter locked" in seg else \
        "waiting" if want in STATIC_MODELS else None
    last_fps = fpsl[-1] if fpsl else None
    return {"time": st.st_mtime, "sgsr3": made, "calibrated": bool(cal),
            "render": f"{cal[-1][0]}x{cal[-1][1]}" if cal else None,
            "output": f"{cal[-1][2]}x{cal[-1][3]}" if cal else None,
            "mode": mode, "frames": frames[-1] if frames else 0,
            "fsr4_version": fsr4[-1][0] if mode in presets.FSR4_MODELS and fsr4 else None,
            "fsr3_version": "3.1" if mode == "fsr3" else None,
            "fsr4_fallback": "failed" if want in presets.FSR4_MODELS and mode not in presets.FSR4_MODELS and (refused or to_fsr2) else None,
            "fsr3_fallback": "failed" if want == "fsr3" and mode != "fsr3" and (refused or to_fsr2) else None,
            "sgsr2_fallback": "failed" if want == "sgsr2" and mode != "sgsr2" and (refused or to_fsr2) else None,
            "not_offered": not_offered,
            "fps": float(last_fps.group(1)) if last_fps else None,
            "fps_model": STATIC_LABEL.get(last_fps.group(3) or "", last_fps.group(3)) if last_fps else None,
            "gpu_ms": float(last_fps.group(4)) if last_fps and last_fps.group(4) else None,
            "jitter": jit, "npu": npu.parse_fork(seg),
            "hp": False, "lines2": False, "student_fallback": False, "v1f_fallback": False, "fg": None,
            "low_latency": None, "errors": len(re.findall(r"failed|error", text, re.I))}

DLL_OVERRIDE = 'WINEDLLOVERRIDES="dxgi=n,b"'
STUDENT_ENV = "OPTI_SGSR3_STUDENT=1"
V1_ENV = "OPTI_SGSR3_V1CS=1"
FG_ENV = "OPTI_SGSRFG=1"
# with Vela FG: Turnip's autotuner optimised for preemption no. 3), only while FG is on
FG_TU_ENV = "TU_AUTOTUNE_FLAGS=preempt_optimize"  # no longer set (not supported on A740); still taken off
ALGO_ENV = {"v1f": None, "v1": V1_ENV, "student": STUDENT_ENV}
OUR_TOKENS = (DLL_OVERRIDE, STUDENT_ENV, V1_ENV, FG_ENV, FG_TU_ENV) + profiles.ENV_TOKENS + profiles.EXTRA_ENV_TOKENS
DX12_ARG = "-dx12"
# 0.8.17: the panel could not read Steam's launch options: nothing is set up or undone on a guess
NOT_READ = "Steam did not give this game's launch options, nothing was changed: try again"
# 0.8.15: Bright Memory: Infinite (1178830): a game's own Steam launch entries (appinfo: config/launch/<n>/
# arguments) may pass their own API flag ("StartGame（DX11）" = -dx11, another = -dx12); our -dx12 on top of the DX11 entry made UE
# exit with "-d3d12, -d3d11, -vulkan, and -opengl are mutually exclusive". Such a game gets no -dx12 from Vela: the player picks
# the DX12 entry when Steam asks (the panel says so).
_RX_API_ARG = re.compile(rb"(?<![\w-])-(dx11|dx12|d3d11|d3d12|vulkan|opengl)(?![\w-])", re.I)
_APPINFO_MAGICS = (0x07564427, 0x07564428, 0x07564429)
_appinfo_cache = {}


def _appinfo_paths(home, roots=None):
    roots = roots or [os.path.join(home, ".local/share/Steam"), os.path.join(home, ".steam/steam")]
    return [os.path.join(root, "appcache", "appinfo.vdf") for root in roots]


def _appinfo_entry(path, want):
    """(magic, the app's bytes, offset of the key string table or None) for appid want; None when the file has no such
    app. File: magic u32, universe u32 (v29: + string table offset i64), then per app: appid u32, size u32, size bytes;
    appid 0 ends the list. OSError goes to the caller."""
    with open(path, "rb") as f:
        head = f.read(8)
        magic = int.from_bytes(head[:4], "little") if len(head) == 8 else 0
        if magic not in _APPINFO_MAGICS:
            return None
        table = None
        if magic == 0x07564429:
            table = int.from_bytes(f.read(8), "little", signed=True)
        while True:
            h = f.read(8)
            if len(h) < 8:
                return None
            a, size = int.from_bytes(h[:4], "little"), int.from_bytes(h[4:], "little")
            if a == 0:
                return None
            if a == want:
                return magic, f.read(size), table
            f.seek(size, 1)


def appinfo_api_args(home, appid, roots=None):
    """The API flags the game's own Steam launch entries pass ({"-dx11", "-dx12"}), read from Steam's appcache/appinfo.vdf
    (the entry's strings are inline in any version). Empty: none or not readable. Cached per (file, mtime)."""
    want = int(appid) if str(appid).isdigit() else -1
    for path in _appinfo_paths(home, roots):
        try:
            mt = os.stat(path).st_mtime
        except OSError:
            continue
        key = (path, mt, want)
        if key in _appinfo_cache:
            return set(_appinfo_cache[key])
        found = set()
        try:
            hit = _appinfo_entry(path, want)
            if hit:
                found = {"-" + m.group(1).lower().decode() for m in _RX_API_ARG.finditer(hit[1])}
        except OSError:
            found = set()
        _appinfo_cache[key] = set(found)
        return found
    return set()


# 0.8.19: what Steam itself records about a game's anti-cheat, from the same appinfo.vdf. Read on two real files (an
# armadaOS console's and a Windows PC's, Steam of 2026-11, about 500 apps each):
#   extended/anti_cheat_support_url        the anti-cheat's ban / support page (EasyAntiCheat games: www.easy.ac/...)
#   */steam_deck_compatibility/.../token   Valve's review result "#SteamDeckVerified_TestResult_UnsupportedAntiCheat..."
#   config/launch/<n>/executable           the anti-cheat's own launcher (EAAntiCheat.GameServiceLauncher.exe, ...)
APPINFO_HEADER = 60  # per app before the key-values: infostate, last updated, PICS token, SHA-1, change number, SHA-1
ANTICHEAT_GENERIC = "anti-cheat"
_anticheat_cache = {}


def _appinfo_strings(blob, magic, path, table_off):
    """[(lower-case key path, value)] of the string values in one app's binary key-values. v29 keys are indexes into the
    string table at table_off; v28 keys are inline. Anything malformed ends the walk (what was read so far is kept)."""
    table = None
    if magic == 0x07564429:
        with open(path, "rb") as f:
            f.seek(table_off)
            data = f.read()
        count = int.from_bytes(data[:4], "little")
        table = data[4:].split(b"\0")[:count]
    out, stack, p, b = [], [], APPINFO_HEADER, blob
    try:
        while p < len(b):
            t = b[p]
            p += 1
            if t == 8:
                if not stack:
                    break
                stack.pop()
                continue
            if table is not None:
                key = table[int.from_bytes(b[p:p + 4], "little", signed=True)]
                p += 4
            else:
                e = b.index(b"\0", p)
                key, p = b[p:e], e + 1
            key = key.decode("utf-8", "replace").lower()
            if t == 0:
                stack.append(key)
            elif t == 1:
                e = b.index(b"\0", p)
                out.append(("/".join(stack + [key]), b[p:e].decode("utf-8", "replace")))
                p = e + 1
            elif t in (2, 3):
                p += 4
            elif t in (7, 10):
                p += 8
            else:
                break
    except (IndexError, ValueError):
        pass
    return out


def appinfo_anticheat(home, appid, roots=None):
    """The anti-cheat Steam's appinfo.vdf names for the game: a set of labels ("EasyAntiCheat", "EA AntiCheat", ...,
    or ANTICHEAT_GENERIC when Steam does not say which). Empty: none, or the file is not readable. Cached per (file,
    mtime)."""
    want = int(appid) if str(appid).isdigit() else -1
    for path in _appinfo_paths(home, roots):
        try:
            mt = os.stat(path).st_mtime
        except OSError:
            continue
        key = (path, mt, want)
        if key in _anticheat_cache:
            return set(_anticheat_cache[key])
        found = set()
        try:
            hit = _appinfo_entry(path, want)
            kvs = _appinfo_strings(hit[1], hit[0], path, hit[2]) if hit else []
        except (OSError, ValueError):
            kvs = []
        for k, v in kvs:
            low = v.lower()
            if k.endswith("extended/anti_cheat_support_url") and low.strip():
                found.add("EasyAntiCheat" if "easy.ac/" in low else ANTICHEAT_GENERIC)
            elif "_compatibility/" in k and k.endswith("/token") and "_testresult_unsupportedanticheat" in low:
                found.add(ANTICHEAT_GENERIC)
            elif "/launch/" in k and k.endswith("/executable"):
                name = low.replace("\\", "/").rsplit("/", 1)[-1]
                found |= {label for m, label in ANTICHEAT_MARKERS.items() if m in name}
        _anticheat_cache[key] = set(found)
        return found
    return set()


def anticheat_labels(found, from_steam=()):
    """scan()["anticheat"]: the anti-cheat found in the game folder and in Steam's appinfo (a named one replaces the
    generic label). At most 4."""
    out = sorted(set(found) | set(from_steam))
    if len(out) > 1 and ANTICHEAT_GENERIC in out:
        out.remove(ANTICHEAT_GENERIC)
    return out[:4]


def has_dx12_entry(args):
    return bool({"-dx12", "-d3d12"} & set(args or ()))


# a standalone API flag in launch options (Steam puts the options' tokens after %command% on the game's command line)
_RX_API_TOKEN = re.compile(r"(?<!\S)-(dx12|d3d12|dx11|d3d11)(?!\S)", re.I)


def has_api_flag(options):
    return isinstance(options, str) and bool(_RX_API_TOKEN.search(options))


def drop_api_flags(options):
    """Options without a standalone -dx12 / -d3d12 / -dx11 / -d3d11. None stays None. 0.8.17: no longer used on Steam's
    options (it took the player's own flag too); Vela takes out only the -dx12 it added (launchopts record)."""
    if not isinstance(options, str):
        return options
    return re.sub(r"\s+", " ", _RX_API_TOKEN.sub(" ", options)).strip()


# marker of the Decky OptiScaler plugin's own install in the game folder: ours takes its place while on
OTHER_OPTISCALER = ".optiscaler.json"

# the game's own temporal upscalers OptiScaler can take over (file name in the game folder -> label)
INPUT_LIBS = {
    "nvngx_dlss.dll": "DLSS", "sl.dlss.dll": "DLSS", "nvunityplugin.dll": "DLSS",
    "libxess.dll": "XeSS",
    "ffx_fsr2_api_x64.dll": "FSR 2", "ffx_fsr2_api_dx12_x64.dll": "FSR 2",
    "ffx_fsr3upscaler_x64.dll": "FSR 3", "ffx_fsr3_x64.dll": "FSR 3",
    "amd_fidelityfx_dx12.dll": "FSR 3.1", "amd_fidelityfx_upscaler_dx12.dll": "FSR 3.1",
}
# name fragment of a file or folder -> anti-cheat
ANTICHEAT_MARKERS = {"easyanticheat": "EasyAntiCheat", "start_protected_game": "EasyAntiCheat",
                     "eac_launcher": "EasyAntiCheat", "battleye": "BattlEye", "beservice": "BattlEye",
                     "xigncode": "XIGNCODE", "nprotect": "nProtect", "vanguard": "Vanguard", "equ8": "EQU8",
                     "anticheatexpert": "ACE", "ace-base": "ACE",
                     # EA AntiCheat: EAAntiCheat.GameServiceLauncher.exe / EAAntiCheat.Installer.exe (in Apex Legends'
                     # Steam launch entries)
                     "eaanticheat": "EA AntiCheat"}
# online games whose anti-cheat is not visible in the install folder (or only after an update)
ANTICHEAT_APPIDS = {"393380": "Squad", "578080": "PUBG", "1172470": "Apex Legends", "252490": "Rust",
                    "359550": "Rainbow Six Siege", "230410": "Warframe", "1665460": "eFootball"}
SKIP_DIRS = {"_commonredist", "commonredist", "redist", "directx", "dotnet", "vcredist", "__installer",
             "installer", "crashreportclient", "_backup", "backup", "saves", STATE_DIR}
SKIP_EXE = re.compile(r"(unins|setup|vcredist|dxsetup|dotnet|redist|crash|report|easyanticheat|battleye|beservice"
                      r"|anticheat|helper|launcher|start_protected_game|ue4prereq|ueprereq|7z|python)", re.I)
MAX_ENTRIES = 60000
MAX_DEPTH = 10                  # UE plugins: Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64
LOG_TAIL = 2 * 1024 * 1024


# ---- Steam library ------------------------------------------------------------------------------------------

_VDF_TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|([{}])')


def parse_vdf(text):
    root, stack, pending = {}, [], None
    stack.append(root)
    for m in _VDF_TOKEN.finditer(text):
        quoted, brace = m.group(1), m.group(2)
        if brace == "{":
            child = {}
            if pending is not None:
                stack[-1][pending] = child
                pending = None
            stack.append(child)
        elif brace == "}":
            if len(stack) > 1:
                stack.pop()
        else:
            val = quoted.replace("\\\\", "\\").replace('\\"', '"')
            if pending is None:
                pending = val
            else:
                stack[-1][pending] = val
                pending = None
    return root


def steam_libraries(home):
    """Every Steam library folder (the one with steamapps/), main install first."""
    libs = []

    def add(p):
        try:
            real = os.path.realpath(p)
        except OSError:
            return
        if os.path.isdir(os.path.join(real, "steamapps")) and real not in libs:
            libs.append(real)

    roots = [os.path.join(home, ".local/share/Steam"), os.path.join(home, ".steam/steam")]
    for root in roots:
        add(root)
    for root in roots:
        for vdf in (os.path.join(root, "steamapps", "libraryfolders.vdf"), os.path.join(root, "config", "libraryfolders.vdf")):
            try:
                with open(vdf, encoding="utf-8", errors="replace") as f:
                    data = parse_vdf(f.read())
            except OSError:
                continue
            for entry in (data.get("libraryfolders") or {}).values():
                if isinstance(entry, dict) and entry.get("path"):
                    add(entry["path"])
    return libs


def find_install(home, appid):
    """(install dir, game name) from appmanifest_<appid>.acf, or (None, None)."""
    for lib in steam_libraries(home):
        acf = os.path.join(lib, "steamapps", f"appmanifest_{appid}.acf")
        try:
            with open(acf, encoding="utf-8", errors="replace") as f:
                st = parse_vdf(f.read()).get("AppState") or {}
        except OSError:
            continue
        inst = st.get("installdir")
        path = os.path.join(lib, "steamapps", "common", inst) if inst else None
        if path and os.path.isdir(path):
            return path, st.get("name") or ""
    return None, None


# ---- game folder ----------------------------------------------------------------------------------------------

def scan_dir(root, appid=""):
    """One bounded walk: where the game's exe is, which upscaler inputs it ships, anti-cheat, our own install."""
    exes, inputs, anticheat, unity_dirs = [], [], [], []
    budget = MAX_ENTRIES
    for current, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(current, root)
        depth = 0 if rel == "." else rel.count(os.sep) + 1
        # folders by name; files only if they are binaries (game assets may be called "vanguard_tank" etc.)
        for name in dirnames + [f for f in filenames if f.lower().endswith((".exe", ".dll", ".sys"))]:
            low = name.lower()
            anticheat += [label for m, label in ANTICHEAT_MARKERS.items() if m in low]
        dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIRS and depth < MAX_DEPTH]
        budget -= len(dirnames) + len(filenames)
        if budget <= 0:
            break
        for name in filenames:
            low = name.lower()
            if low in INPUT_LIBS and INPUT_LIBS[low] not in inputs:
                inputs.append(INPUT_LIBS[low])
            if low == "unityplayer.dll":
                unity_dirs.append(current)
            if low.endswith(".exe") and not SKIP_EXE.search(low):
                try:
                    size = os.path.getsize(os.path.join(current, name))
                except OSError:
                    size = 0
                exes.append((current, name, size))
    engine, exe = pick_exe(exes, unity_dirs)
    if exe and not engine and godot_signs(exe[0], exe[1]):
        engine = "Godot"
    if appid in ANTICHEAT_APPIDS and not anticheat:
        anticheat.append(ANTICHEAT_APPIDS[appid])
    return {"engine": engine, "exe_dir": exe[0] if exe else None, "exe": exe[1] if exe else None,
            "inputs": inputs, "anticheat": sorted(set(anticheat))[:4]}


GODOT_PCK_MAGIC = b"GDPC"


def godot_signs(exe_dir, exe):
    """0.8.16: a Godot export (e.g. "Sir, We Have an Orc Problem" = swhaop.exe + libgodotsteam*): its FSR 2.2 is the
    engine's own code on Vulkan, OptiScaler's pattern search does not catch it, so Vela has nothing to replace. Signs, each
    cheap: a .pck next to the exe, a GodotSteam / GDExtension library, or the pack embedded in the exe (Godot's
    PackedSourcePCK: the file ends with the pack size (uint64) and the magic "GDPC"; only the last 4 bytes are read)."""
    signs = []
    try:
        names = os.listdir(exe_dir)
    except OSError:
        names = []
    low = [n.lower() for n in names]
    if any(n.endswith(".pck") for n in low):
        signs.append("pck")
    if any(n.startswith("libgodot") or n.startswith("godotsteam") for n in low):
        signs.append("godot_lib")
    try:
        with open(os.path.join(exe_dir, exe), "rb") as f:
            f.seek(-4, os.SEEK_END)
            if f.read(4) == GODOT_PCK_MAGIC:
                signs.append("embedded_pck")
    except OSError:
        pass
    return signs


def pick_exe(exes, unity_dirs):
    """The real game binary: UE's *-Win64-Shipping.exe (not the stub at the root), Unity's player exe, else the biggest."""
    ue = [e for e in exes if re.search(r"-(win64|wingdk)-shipping\.exe$", e[1], re.I)]
    if ue:
        ue.sort(key=lambda e: ("/engine/" in e[0].replace(os.sep, "/").lower() + "/", -e[2]))
        return "Unreal", ue[0]
    for d in unity_dirs:
        here = [e for e in exes if e[0] == d]
        if here:
            return "Unity", max(here, key=lambda e: e[2])
    big = [e for e in exes if e[2] >= 1024 * 1024]
    if big:
        return "", max(big, key=lambda e: e[2])
    return "", None


# ---- OptiScaler.ini -------------------------------------------------------------------------------------------

def ini_values(opts):
    """Keys we set in the template; the rest stays at OptiScaler's defaults ("auto")."""
    fg = bool(opts.get("fg"))
    lowlat = bool(opts.get("latency"))
    fps = opts.get("fps", "off")
    vals = {
        ("Upscalers", "Dx12Upscaler"): "sgsr3",
        ("Upscalers", "Dx11Upscaler"): "sgsr2_12",
        # Vulkan spoofing turns Adreno into "NVIDIA" inside vkd3d-proton and D3D12CreateDevice fails;
        # DXGI spoofing and OptiPatcher are what open the DLSS input
        ("Spoofing", "Dxgi"): "true",
        ("Spoofing", "StreamlineSpoofing"): "true",
        ("Spoofing", "Vulkan"): "false",
        ("Spoofing", "VulkanExtensionSpoofing"): "false",
        ("Plugins", "LoadAsiPlugins"): "true",
        ("Log", "LogToFile"): "true",
        ("Log", "LogLevel"): "2",
        ("UpscaleRatio", "UpscaleRatioOverrideEnabled"): "false",
        ("UpscaleRatio", "UpscaleRatioOverrideValue"): "auto",
        # OptiScaler's overlay stays (it carries FG), its menu, FPS overlay and FG hotkeys and the splash do not
        ("Menu", "ShortcutKey"): "-1",
        ("Menu", "FpsShortcutKey"): "-1",
        ("Menu", "FpsCycleShortcutKey"): "-1",
        ("Menu", "FGShortcutKey"): "-1",
        ("Menu", "DisableSplash"): "true",
        # frame generation: our SGSR-FG on the upscaler's inputs, shown through FidelityFX's swapchain, HUD-less
        # frame found by HUDfix
        ("FrameGen", "Enabled"): "true" if fg else "false",
        ("FrameGen", "FGInput"): "upscaler" if fg else "auto",
        ("FrameGen", "FGOutput"): "fsrfg" if fg else "auto",
        ("OptiFG", "HUDFix"): "true" if fg else "auto",
        # every input the game may offer goes to SGSR 3: whatever the player picks in the game's menu (DLSS, FSR 2,
        # FSR 3 / FidelityFX API, XeSS) is replaced. FSR built into the exe (no ffx_* DLL) needs OptiScaler's pattern
        # search (slower game start), so only when the folder has no FSR DLL
        ("FSR", "Fsr4ForceModel"): "2" if opts.get("fsr4_driver") else "auto",
        ("Inputs", "EnableDlssInputs"): "true",
        ("Inputs", "EnableXeSSInputs"): "true",
        ("Inputs", "EnableFsr2Inputs"): "true",
        ("Inputs", "UseFsr2Inputs"): "true",
        ("Inputs", "EnableFsr3Inputs"): "true",
        ("Inputs", "UseFsr3Inputs"): "true",
        ("Inputs", "EnableFfxInputs"): "true",
        ("Inputs", "UseFfxInputs"): "true",
        ("Inputs", "Fsr2Pattern"): "true" if opts.get("fsr_pattern") else "auto",
        ("Inputs", "Fsr3Pattern"): "true" if opts.get("fsr_pattern") else "auto",
        # low latency: the game's Reflex -> fakenvapi -> LatencyFleX
        ("fakenvapi", "ForceReflex"): "2" if lowlat else "auto",
        ("fakenvapi", "ForceLatencyFlex"): "true" if lowlat else "auto",
        # frame-rate limit through Reflex / LatencyFleX: sleeps before the input is read (not a FIFO queue)
        ("Framerate", "FramerateLimit"): fps if fps in FPS_LIMITS and fps != "off" else "auto",
    }
    # 0.8.15: no UpscaleRatioOverride whatever the stored
    # ratio: the game's own DLSS / FSR quality preset sets the render. LogLevel stays 2 (Info: the fork's Vela lines are [I];
    # Debug wrote ~23k lines in 90 s in Bright Memory when another plugin's ini was in the folder)
    # per-game OptiScaler keys (profiles.py "ini"), e.g. Cyberpunk's Dxgi=false
    vals.update(profiles.ini_overrides(opts.get("appid", "")))
    return vals


def fsr_pattern(scan):
    """OptiScaler's FSR 2/3 pattern search: when the folder ships no FSR DLL (FSR, if any, is built into the exe)."""
    return not any(i.startswith("FSR") for i in scan.get("inputs") or [])


def menu_hint(scan):
    """Which item to pick in the game's graphics menu, from the upscaler DLLs found in the game folder (= en.json menu.*)."""
    found = scan.get("inputs") or []
    fsr = [i for i in found if i.startswith("FSR")]
    if fsr:
        return f"choose {fsr[0]}" + (" or DLSS" if "DLSS" in found else "")
    if "DLSS" in found:
        return "choose DLSS" + (" or XeSS" if "XeSS" in found else "")
    if "XeSS" in found:
        return "choose XeSS (or FSR, if there is one)"
    return "choose DLSS or FSR — whichever the menu has"


def ini_set(text, section, key, value):
    """Set key=value inside [section]; adds the key (or the section) if it is missing. Keeps the line endings."""
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == f"[{section.lower()}]"), None)
    if start is None:
        return text.rstrip(nl) + f"{nl}{nl}[{section}]{nl}{key}={value}{nl}"
    end = next((i for i in range(start + 1, len(lines)) if lines[i].lstrip().startswith("[")), len(lines))
    rx = re.compile(rf"^\s*{re.escape(key)}\s*=", re.I)
    for i in range(start + 1, end):
        if rx.match(lines[i]):
            lines[i] = f"{key}={value}"
            return nl.join(lines)
    lines.insert(start + 1, f"{key}={value}")
    return nl.join(lines)


def ini_get(text, section, key):
    cur = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("["):
            cur = s.strip("[]").lower()
        elif cur == section.lower() and "=" in s and not s.startswith(";"):
            k, v = s.split("=", 1)
            if k.strip().lower() == key.lower():
                return v.strip()
    return None


def make_ini(template, opts):
    for (section, key), value in ini_values(opts).items():
        template = ini_set(template, section, key, value)
    return template


# ---- launch options -------------------------------------------------------------------------------------------

def smooth_env(level):
    return [f"{k}={v}" for k, v in SMOOTH.get(level, {}).items()]


def strip_ours(options, dx12_ours=False):
    """Launch options without every token that looks like this module's (0.8.16 and older cleaned Steam's options with it,
    and took the player's own VKD3D_* / WINEDLLOVERRIDES="dxgi=n,b" / OPTI_SGSR3_* too). 0.8.17: not used on Steam's options;
    ours_token is the same test for one token, for setups made before the record of what Vela added (launchopts.derive)."""
    out = options or ""
    for tok in OUR_TOKENS:
        out = re.sub(rf"(?<!\S){re.escape(tok)}(?!\S)", " ", out)
    # every fork key (0.4.1): ours, the profiles', and A/B keys set by hand (OPTI_SGSR3_NOHP=1 ...) go together
    out = re.sub(r"(?<!\S)OPTI_SGSR3?_[A-Z0-9_]+=\S*", " ", out)
    out = re.sub(r"(?<!\S)(VELA_[A-Z0-9_]+|RECON_LEVEL)=\S*", " ", out)
    if dx12_ours:
        out = re.sub(r"(?<!\S)-dx12(?!\S)", " ", out)
    return re.sub(r"\s+", " ", out).strip()


_RX_OURS = re.compile(r"(?:OPTI_SGSR3?_[A-Z0-9_]+|VELA_[A-Z0-9_]+|RECON_LEVEL)=\S*")


def ours_token(tok):
    """Whether a token looks like one this module adds (the test strip_ours applies): only for setups made before 0.8.17,
    together with the options before Vela (launchopts.derive)."""
    return tok in OUR_TOKENS or bool(_RX_OURS.fullmatch(tok))


def legacy_record(before, set_by_us, dx12_added):
    """What a setup made before 0.8.17 added (its state has no record): Vela's tokens of the options it wrote that the
    options before it did not have; its -dx12 when the state says so."""
    return launchopts.derive(before, set_by_us, ours_token, [DX12_ARG] if dx12_added else [])


def launch_build(options, record, algo, unreal, fg=False, extra_env=(), own_api=False):
    """0.8.17: Steam's options with SGSR 3: (new options, the record of what Vela added). record: what Vela added before
    (taken out first, exactly); every other token stays as it is, where it is. extra_env: the game profile's. A variable
    the player sets himself (same NAME=) is not added: his value wins (WINEDLLOVERRIDES as before). -dx12 only for Unreal
    when the player's options pass no API flag, and never when the game's Steam launch entries pass their own (own_api,
    appinfo_api_args; 0.8.15: -dx12 on top of the DX11 entry's -dx11 is a fatal UE error); the player's own flag is his."""
    base = launchopts.remove(options or "", record)
    add_dx = unreal and not own_api and not _RX_API_TOKEN.search(base)
    env = [DLL_OVERRIDE]
    if ALGO_ENV.get(algo):
        env.append(ALGO_ENV[algo])
    if fg:
        env += [FG_ENV]  # (0.4.11-0.4.15 added FG_TU_ENV: A740 supports no preempt_optimize, "supported flags: 0x6")
    env += list(extra_env)
    return launchopts.replace(options or "", record, env, [DX12_ARG] if add_dx else [])


def launch_on(options, algo, unreal, dx12_ours=False, fg=False, extra_env=(), own_api=False):
    """launch_build for options this module wrote itself with no record of them (tests, old callers): every token that
    looks like ours counts as ours, -dx12 when dx12_ours. Returns (new options, whether -dx12 was added by us)."""
    rec = launchopts.derive(None, options or "", ours_token, [DX12_ARG] if dx12_ours else [])
    new, rec = launch_build(options, rec, algo, unreal, fg, extra_env, own_api)
    return new, DX12_ARG in rec["args"]


def launch_off(current, before, set_by_us, dx12_ours, record=None):
    """Launch options after disabling: exactly what was there before Vela if nobody touched them since, else the options
    as they are now without exactly the tokens Vela added (record; None: legacy_record). The player's tokens, the ones
    from before and his later edits, stay where they are. current None (Steam's options not read): None, write nothing."""
    if current is None:
        return None
    if current == set_by_us and isinstance(before, str):
        return before
    if record is None:
        record = legacy_record(before, set_by_us, dx12_ours)
    return launchopts.remove(current, record)


# ---- OptiScaler.log -------------------------------------------------------------------------------------------

_RX_INIT = re.compile(r"Initializing SGSR([23]) (\d+)x(\d+) -> (\d+)x(\d+)")
# 0.7.1: a live model change or a re-create at the game's subrect starts a new upscaler in the same log (patch 0010/0012):
# the status describes the last one, from the line that asked for it on
_RX_RECREATE = re.compile(r"Vela live: (?:handle \d+ model|the game draws)")
# 0.8.2: FSR 3.1 through the FFX backend (patch 0014): the switch line, then the version it took
_RX_LIVE = re.compile(r"Vela live: handle \d+ model (\w+) -> [^\n]*?(?:\(the game draws (\d+)x(\d+)\))?\r?$", re.M)
_RX_FSR3 = re.compile(r"Vela: FFX upscaler index \d+ \(([\d.]+)\) for FSR 3")
# 0.8.13: "Vela live: fsr4 cannot run in this game; SGSR 2 instead" (provider refused, a frame
# failed); the handles are then re-created on SGSR 2 until the panel picks another model
_RX_DOWN = re.compile(r"Vela live: (\w+) cannot run in this game; SGSR 2 instead")
_RX_FSR4 = re.compile(r"Vela FSR 4 VK: FSR ([\d.]+(?: Lite)?) VK INT8 context \d+x\d+ -> \d+x\d+"
                      r"(?: \(hdr \w+(?:, network every (\d+) frames)?\))?")


def log_summary(path, since=0.0):
    """What the last game run did, from OptiScaler.log: SGSR 3 (or SGSR 2, a Vela live model) created (sizes), which
    algorithm really ran (v1f, the student, v1 when the Vulkan interop is missing, FSR 4.1 inside SGSR 3), the temporal box, SGSR-FG, the low-latency path. Live model changes re-create the upscaler
    while the game runs: the model, sizes and fallbacks are the last upscaler's."""
    try:
        st = os.stat(path)
    except OSError:
        return None
    if st.st_mtime < since:
        return None
    with open(path, "rb") as f:
        if st.st_size > LOG_TAIL:
            f.seek(st.st_size - LOG_TAIL)
        text = f.read().decode("utf-8", "replace")
    inits = list(_RX_INIT.finditer(text))
    seg = text
    if inits:
        start = 0
        for m in _RX_RECREATE.finditer(text, 0, inits[-1].start()):
            start = m.start()
        seg = text[start:]
    fallback = any(k in seg for k in ("student (Vulkan interop) is not available", "the student failed; v1f",
                                      "frame sizes differ from the student's", "v1f started as the student's fallback"))
    student = "SGSR3 student:" in seg and not fallback
    v1f_fail = "v1f (Vulkan interop) is not available" in seg
    v1f = "SGSR3 v1f: v1f" in seg and not v1f_fail
    # FSR 4.1 (4.1.1, or 4.0.2 by VELA_FSR4_VER) runs inside SGSR 3 (Fsr4VkBridge.h); refused, the student (Vela AI) runs.
    # 0.8.15: an older run's Lite line ("FSR 4.1.2 Lite VK INT8 context", "FSR 4.1 Lite runs only ...") is read as FSR 4
    # and as a refusal, never as a model of its own
    fsr4 = _RX_FSR4.findall(seg)
    fsr4_fail = "FSR 4.1 Lite runs only " in seg or "FSR 4 VK is not available here" in seg
    mode = None
    if inits and inits[-1].group(1) == "2":
        mode = "sgsr2"
    elif inits:
        mode = "student" if student else "v1f" if v1f else "v1"
        if fsr4 and not fsr4_fail and not fallback:
            mode = "fsr4"
    want = re.findall(r"SGSR3: Vela live model (\w+)", seg)
    # 0.8.16 (fork patch 0018): FSR 4 Fast is the same FSR 4 VK context line; the live model the feature was made for names it
    if mode == "fsr4" and want and want[-1] == "fsr4fast":
        mode = "fsr4fast"
    # FSR 3.1 (live fsr3): the last switch asked for it; it runs when the FFX backend took a 3.x upscaler after it
    lives = list(_RX_LIVE.finditer(text))
    fsr3_want = bool(lives) and lives[-1].group(1) == "fsr3"
    seg3 = text[lives[-1].start():] if fsr3_want else ""
    fsr3 = _RX_FSR3.findall(seg3)
    fsr3_ok = bool(fsr3) and not any(k in seg3 for k in ("_createContext error", "can't load amd_fidelityfx_dx12"))
    if fsr3_ok:
        mode = "fsr3"
    # 0.8.13: SGSR 2 runs because the chosen FSR 4 cannot run here (no other model picked after that line)
    downs = list(_RX_DOWN.finditer(text))
    down = None
    if downs and mode == "sgsr2":
        d = downs[-1]
        if all(m.group(1) == d.group(1) for m in _RX_LIVE.finditer(text, d.end())):
            down = presets.RETIRED_MODELS.get(d.group(1), d.group(1))
    if down in presets.FSR4_MODELS:
        fsr4_fail = True
        want = [down]
    fg = None
    if "SGSR-FG: init failed" in text:
        fg = "failed"
    elif "SGSR-FG: on" in text:
        fg = "on"
    m = re.findall(r"LowLatency algo: (\w+)", text)
    last = inits[-1].groups() if inits else None
    return {
        "time": st.st_mtime,
        "sgsr3": bool(inits) or fsr3_ok,
        "render": (f"{lives[-1].group(2)}x{lives[-1].group(3)}" if lives[-1].group(2) else None) if fsr3_ok else
        f"{last[1]}x{last[2]}" if last else None,
        "output": f"{last[3]}x{last[4]}" if last else None,
        "mode": mode,
        "fsr4_version": fsr4[-1][0] if mode in presets.FSR4_MODELS else None,
        "fsr3_version": fsr3[-1] if fsr3_ok else None,
        "fsr3_fallback": "failed" if fsr3_want and not fsr3_ok else None,
        # the live model asked for FSR 4.1 and SGSR 3 ran Vela AI instead: "failed"
        "fsr4_fallback": "failed" if fsr4_fail and want and want[-1].startswith("fsr4") else None,
        "hp": mode == "v1f" and "temporal box hp" in seg,
        "lines2": mode == "v1f" and "line threshold 2" in seg,
        "student_fallback": fallback and "SGSR3 student" in seg or "student (Vulkan interop) is not available" in seg,
        "v1f_fallback": v1f_fail,
        "sgsr2_instead": down,
        "fg": fg,
        "low_latency": m[-1] if m else None,
        "errors": len(re.findall(r"SGSR3[^\n]*(failed|missing inputs)", seg)),
        # 0.8.0: the network on the NPU (the fork's "Vela NPU:" log lines)
        "npu": npu.parse_fork(seg),
    }


# ---- install / restore ----------------------------------------------------------------------------------------

def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _atomic_write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".upscale-tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def _atomic_copy(src, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".upscale-tmp"
    shutil.copyfile(src, tmp)
    shutil.copymode(src, tmp)
    os.replace(tmp, path)


class Sgsr3:
    def __init__(self, assets_dir, home, settings_dir, log=print):
        self.assets = assets_dir
        self.home = home
        self.logs_dir = os.path.join(settings_dir, "sgsr3-logs")
        self.log = log
        self.cache = {}      # appid -> (time, scan)
        self.md5_cache = {}  # path -> ((size, mtime), md5)
        self.log_cache = {}  # path -> ((size, mtime, since), summary); the UI polls status every few seconds
        try:
            with open(os.path.join(assets_dir, "VERSION.json"), encoding="utf-8") as f:
                self.version = json.load(f)
        except (OSError, ValueError):
            self.version = {}

    # -- what is where

    def scan(self, appid, fresh=False):
        appid = str(appid)
        hit = self.cache.get(appid)
        if hit and not fresh and time.time() - hit[0] < 120:
            return hit[1]
        root, name = find_install(self.home, appid)
        res = {"appid": appid, "install_dir": root, "name": name, "engine": "", "exe_dir": None, "exe": None,
               "inputs": [], "anticheat": [ANTICHEAT_APPIDS[appid]] if appid in ANTICHEAT_APPIDS else []}
        if root:
            res.update(scan_dir(root, appid))
        res["anticheat"] = anticheat_labels(res["anticheat"], appinfo_anticheat(self.home, appid))
        self.cache[appid] = (time.time(), res)
        return res

    def _md5(self, path):
        try:
            st = os.stat(path)
        except OSError:
            return None
        key = (st.st_size, st.st_mtime)
        hit = self.md5_cache.get(path)
        if hit and hit[0] == key:
            return hit[1]
        value = md5(path)
        self.md5_cache[path] = (key, value)
        return value

    def kit(self, appid):
        """What goes into the game folder: OptiScaler (files, assets) or, for a static_fsr2 profile, the
        interceptor DLL alone."""
        cfg = profiles.static_fsr2(appid)
        if cfg:
            return {"static": cfg, "installed": STATIC_INSTALLED, "runtime": STATIC_RUNTIME,
                    "src": {"dxgi.dll": os.path.join(os.path.dirname(self.assets), STATIC_ASSETS, "dxgi.dll"),
                            FSR4VK: os.path.join(self.assets, FSR4VK), FG_DLL: os.path.join(self.assets, FG_DLL)},
                    "log": "sgsr3intercept.log"}
        src = {rel: os.path.join(self.assets, a) for rel, a in ASSET_OF.items()}
        src[AMDXC] = os.path.join(self.home, AMDXC_DIR, AMDXC)
        src[FSR4VK] = os.path.join(self.assets, FSR4VK)
        return {"static": None, "installed": INSTALLED, "runtime": RUNTIME, "src": src, "log": "OptiScaler.log"}

    def static_ratio(self, appid, exe_dir):
        """The upscale ratio of the interceptor's last calibration: its log in the game folder, else the one kept when
        Vela was last switched off (sgsr3-logs); None before the first run."""
        for path in (os.path.join(exe_dir, STATIC_RUNTIME[0]), os.path.join(self.logs_dir, f"{appid}-{STATIC_RUNTIME[0]}")):
            r = log_ratio(static_log_summary(path))
            if r:
                return r
        return None

    def dll_md5(self):
        """md5 of the DLL the plugin carries, from the file itself: a new build dropped into assets/sgsr3 needs no
        VERSION.json edit to be recognised in the games."""
        return self._md5(os.path.join(self.assets, "OptiScaler.dll")) or self.version.get("md5")

    def fg_available(self):
        return os.path.isfile(os.path.join(self.assets, FG_DLL))

    def fsr4_available(self):
        """FSR 4 can be a model: AMD's driver library is on the console and the FidelityFX SDK DLL is in the plugin.
        Off (0.5.3): on the console the fork reports "fsr4: false" for the Adreno even with Fsr4ForceModel=2 (the
        driver library is never loaded), and its FFX backend crashed Into the Dead when the game released a feature
        (FFXFeatureDx12::~FFXFeatureDx12 on the delayed-destroy thread, 29.10)."""
        return os.path.isfile(os.path.join(self.assets, FSR4VK))

    def fsr3_available(self):
        """0.8.2: FSR 3.1 can be a live model: the fork has patch 0014 (VERSION.json "fsr3_live") and the
        FidelityFX SDK DLL is in the plugin."""
        return self.fg_available() and bool(self.version.get("fsr3_live"))

    def fsr4_driver(self):
        """AMD's driver FSR 4 route (FFX backend + amdxcffx64.dll): off, see FSR4_READY."""
        return FSR4_READY and os.path.isfile(os.path.join(self.home, AMDXC_DIR, AMDXC)) and self.fg_available()

    def _log(self, path, since):
        try:
            st = os.stat(path)
        except OSError:
            return None
        key = (st.st_size, st.st_mtime, since)
        hit = self.log_cache.get(path)
        if not hit or hit[0] != key:
            hit = (key, log_summary(path, since))
            self.log_cache[path] = hit
        return hit[1]

    @staticmethod
    def _state_path(exe_dir):
        return os.path.join(exe_dir, STATE_DIR, "state.json")

    def read_state(self, exe_dir):
        try:
            with open(self._state_path(exe_dir), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError, TypeError):
            return None

    def _write_state(self, exe_dir, st):
        _atomic_write(self._state_path(exe_dir), json.dumps(st, indent=2).encode("utf-8"))

    def launch_set(self, appid):
        """The launch options this module recorded for the game (None: not set up)."""
        d = self.scan(appid)["exe_dir"]
        st = self.read_state(d) if d else None
        return None if st is None else st.get("launch_set")

    def api_args(self, appid):
        """0.8.15: the API flags of the game's own Steam launch entries (appinfo_api_args)."""
        return appinfo_api_args(self.home, appid)

    @staticmethod
    def record(st):
        """What Vela added to the game's launch options (0.8.17 states keep it; older ones: legacy_record)."""
        if isinstance((st or {}).get("added"), dict):
            return {**launchopts.empty_record(), **st["added"]}
        st = st or {}
        return legacy_record(st.get("launch_prev"), st.get("launch_set"), st.get("dx12_added", False))

    def launch_legacy(self, appid):
        """0.8.17: the game was set up by an older Vela (no record of what it added): the panel's launch_fix migrates it."""
        d = self.scan(appid)["exe_dir"]
        st = self.read_state(d) if d else None
        return bool(st) and not isinstance(st.get("added"), dict)

    def migrate_launch(self, appid, launch):
        """0.8.17: a setup made before 0.8.17 gets its record. When Steam's options are still the ones Vela wrote, they are
        rebuilt from the player's options before Vela with the same tokens of Vela's: what 0.8.16 and older took out of
        them (VKD3D_FEATURE_LEVEL=12_2, VKD3D_SHADER_MODEL=6_6, the player's -dx12 ...) comes back. Options the player edited
        since are kept as they are. launch: Steam's options without the NPU layer's tokens. Returns the options to set."""
        d = self.scan(appid)["exe_dir"]
        st = self.read_state(d) if d else None
        if not st or isinstance(st.get("added"), dict):
            return launch
        rec = self.record(st)
        prev, mine = st.get("launch_prev"), st.get("launch_set")
        if launch is not None and launch == mine and isinstance(prev, str):
            new, rec = launchopts.add(prev, rec["env"], rec["args"])
            st["launch_set"] = launch = new
            st["dx12_added"] = DX12_ARG in rec["args"]
        st["added"] = rec
        self._write_state(d, st)
        self.log(f"sgsr3: {appid}: launch options of an older Vela: Vela's own tokens {rec}, launch: {launch}")
        return launch

    def dx12_fix_pending(self, appid):
        """0.8.15: a -dx12 Vela added is still in the options of a game whose Steam launch entries pass their own API flag.
        0.8.17: only Vela's own -dx12 (the record); a flag the player had (Bright Memory's -dx12 / -dx11) is his."""
        d = self.scan(appid)["exe_dir"]
        st = self.read_state(d) if d else None
        if not st or st.get("kind", "optiscaler") != "optiscaler":
            return False
        return DX12_ARG in self.record(st)["args"] and bool(self.api_args(appid))

    def drop_our_dx12(self, appid):
        """dx12_fix_pending: Vela's -dx12 out of the record and of the recorded options. True: done."""
        if not self.dx12_fix_pending(appid):
            return False
        d = self.scan(appid)["exe_dir"]
        st = self.read_state(d)
        rec = self.record(st)
        rec["args"] = [a for a in rec["args"] if a != DX12_ARG]
        st["launch_set"] = launchopts.remove(st.get("launch_set"), {"args": [DX12_ARG]})
        st["added"], st["dx12_added"] = rec, False
        self._write_state(d, st)
        self.log(f"sgsr3: {appid}: API flag out of the launch options (the game's Steam launch entries pass "
                 f"{sorted(self.api_args(appid))})")
        return True

    def rewrite_launch_set(self, appid, fn):
        """0.8.15: the recorded launch options through fn (the undo compares Steam's options with them). True: changed."""
        d = self.scan(appid)["exe_dir"]
        st = self.read_state(d) if d else None
        if st is None or not isinstance(st.get("launch_set"), str):
            return False
        new = fn(st["launch_set"])
        if new == st["launch_set"]:
            return False
        st["launch_set"] = new
        if isinstance(st.get("added"), dict):  # a token fn took out is not Vela's to take out again
            left = launchopts.words(new)
            st["added"] = {**st["added"], "env": [t for t in st["added"].get("env", []) if t in left],
                           "args": [t for t in st["added"].get("args", []) if t in left]}
        self._write_state(d, st)
        return True

    def static_dxgi(self, d, st, kit):
        """0.8.11: the interceptor dxgi.dll in a static_fsr2 game: "ours" (the plugin's build), "old" (an older build of
        ours: the hash recorded at install, a build an earlier version shipped, or -- an install before 0.8.11 recorded
        none -- not written since our last install), "foreign" (anything else: a game update, a verify, another mod),
        "missing"."""
        target = os.path.join(d, "dxgi.dll")
        cur = self._md5(target)
        if not cur:
            return "missing"
        if cur == self._md5(kit["src"]["dxgi.dll"]):
            return "ours"
        orig = (st.get("files") or {}).get("dxgi.dll") or {}
        if orig.get("existed") and cur == orig.get("md5"):
            return "foreign"  # the game's own file is back (verify)
        if cur == st.get("dxgi_md5") or cur in OLD_STATIC_DXGI:
            return "old"
        if st.get("dxgi_md5"):
            return "foreign"
        try:
            written = os.path.getmtime(target)
        except OSError:
            return "foreign"
        return "old" if written <= float(st.get("updated_at") or st.get("installed_at") or 0) + 2 else "foreign"

    def _refresh_static(self, appid, d, st, kit):
        """0.8.11: the static_fsr2 kit follows the plugin like OptiScaler's does (game closed: the caller checks): an older
        interceptor of ours and the FSR 4 provider we put there are replaced; a foreign dxgi.dll is never touched (the
        panel shows "Game files changed"). The game's own files stay in the backup. Returns True when copied."""
        ours = self._md5(kit["src"]["dxgi.dll"])
        state = self.static_dxgi(d, st, kit)
        target = os.path.join(d, "dxgi.dll")
        old_md5 = self._md5(target)
        vk = os.path.join(d, FSR4VK)
        src_vk = kit["src"][FSR4VK]
        vk_old = FSR4VK in st.get("files", {}) and os.path.isfile(src_vk) and self._md5(vk) != self._md5(src_vk)
        if state == "missing" and ours:
            state = "old"  # 0.8.15: a missing interceptor goes back like an older one (a foreign one never is touched)
        if state == "old":
            _atomic_copy(kit["src"]["dxgi.dll"], target)
        if vk_old:
            _atomic_copy(src_vk, vk)
        if state in ("ours", "old") and st.get("dxgi_md5") != ours:
            st["dxgi_md5"] = ours  # an install before 0.8.11: record the hash once
            if state == "ours" and not vk_old:
                self._write_state(d, st)
                return False
        if state != "old" and not vk_old:
            return False
        st["updated_at"] = time.time()
        self._write_state(d, st)
        self.log(f"sgsr3: {appid} interceptor updated (game closed): "
                 + (f"dxgi.dll {(old_md5 or '')[:8]} -> {(ours or '')[:8]}" if state == "old" else "dxgi.dll current")
                 + (f", {FSR4VK} -> {(self._md5(src_vk) or '')[:8]}" if vk_old else ""))
        return True

    def _kit_wanted(self, kit, clean, st):
        """The kit files (besides OptiScaler.ini) Vela puts into the game folder for these options: the DLL and OptiPatcher
        always, the FidelityFX DLL / AMD's driver library / the FSR 4 provider while a model needs them (enable() and the
        repair use the same list)."""
        fsr4 = not kit["static"] and self.fsr4_driver()
        fsr3 = self.fg_available() if kit["static"] else self.fsr3_available()
        out = set()
        for rel in kit["installed"]:
            if rel == "OptiScaler.ini":
                continue
            if rel not in OPTIONAL or (rel == FG_DLL and (clean.get("fg") or fsr4)) or \
                    (rel == FG_DLL and fsr3 and not (st.get("files") or {}).get(rel, {}).get("existed")) or \
                    (rel == AMDXC and fsr4) or (rel == FSR4VK and self.fsr4_available()):
                out.add(rel)
        return out

    def _ini_text(self, sc, opts):
        with open(os.path.join(self.assets, "OptiScaler.ini"), encoding="utf-8", newline="") as f:
            return make_ini(f.read(), {**opts, "appid": sc["appid"], "fsr_pattern": fsr_pattern(sc), "fsr4_driver": self.fsr4_driver()})

    def refresh_dll(self, appid):
        """The kit in a closed game Vela is on in follows the plugin and is whole (the caller checks the game is closed; the
        user restarts the game himself; the game's own files stay in the backup). Returns True when anything was written.
        0.5.4: an older dxgi.dll of ours is replaced. 0.8.11: the static_fsr2 interceptor too (_refresh_static).
        0.8.15 (seen with Bright Memory: Infinite): EVERY kit file is checked, each written atomically (tmp + rename), and
        nothing is ever deleted: a missing or changed dxgi.dll / OptiPatcher.asi / FSR 4 provider / FidelityFX DLL goes back,
        and OptiScaler.ini is rewritten when it is missing or not the one Vela writes for the game's options (another plugin's
        ini had LogLevel=1, Debug: ~23k lines in 90 s). Before, only dxgi.dll and the provider came back and the ini stayed
        gone. What removed them that night: the Decky OptiScaler plugin's install + uninstall in the same folder (its log
        00:07:01, its profile and manifest gone 00:07:09; its uninstall deletes the files its marker lists), then Vela's own
        "Off" with the game running restored the backup (that plugin's dxgi.dll and ini) and stopped on the restore check of
        the running game's OptiScaler.log, leaving Vela on with those files (disable(): runtime files are not checked now)."""
        sc = self.scan(appid)
        d = sc["exe_dir"]
        st = self.read_state(d) if d else None
        kit = self.kit(appid)
        if st is not None and kit["static"] and st.get("kind") == "static_fsr2":
            return self._refresh_static(appid, d, st, kit)
        if st is None or kit["static"] or st.get("kind", "optiscaler") != "optiscaler":
            return False
        opts = dict(DEFAULT_OPTS)
        opts.update(self.migrate(st.get("opts")))
        st.setdefault("files", {})
        bdir = os.path.join(d, STATE_DIR, "backup")
        fixed = []
        for rel in sorted(self._kit_wanted(kit, opts, st)):
            src = kit["src"].get(rel)
            want = self._md5(src) if src else None
            target = os.path.join(d, *rel.split("/"))
            if not want or self._md5(target) == want:
                continue
            if rel not in st["files"]:
                st["files"][rel] = self._backup_one(d, bdir, rel)  # an install that did not list it: the game's file first
            _atomic_copy(src, target)
            fixed.append(rel)
        try:
            ini = self._ini_text(sc, opts)
        except OSError:
            ini = None  # the plugin's template is missing: leave the game's ini as it is
        if ini is not None:
            ini_path = os.path.join(d, "OptiScaler.ini")
            try:
                with open(ini_path, encoding="utf-8", newline="") as f:
                    cur = f.read()
            except (OSError, UnicodeDecodeError):
                cur = None
            if cur != ini:
                _atomic_write(ini_path, ini.encode("utf-8"))
                fixed.append("OptiScaler.ini")
        if not fixed:
            return False
        st.update({"version": self.version.get("dll", ""), "updated_at": time.time()})
        self._write_state(d, st)
        self.log(f"sgsr3: {appid} kit repaired (game closed): {', '.join(fixed)}"
                 + (f"; dxgi.dll updated to {self.version.get('dll', '')}" if "dxgi.dll" in fixed else ""))
        return True

    def status(self, appid):
        sc = self.scan(appid)
        st = self.read_state(sc["exe_dir"]) if sc["exe_dir"] else None
        other = bool(sc["exe_dir"]) and os.path.isfile(os.path.join(sc["exe_dir"], OTHER_OPTISCALER))
        out = {"scan": sc, "available": bool(sc["exe_dir"]) and not sc["anticheat"], "installed": st is not None,
               "opts": dict(DEFAULT_OPTS), "version": self.version.get("dll", ""), "intact": None, "log": None,
               "fg_available": self.fg_available(), "fsr4_available": self.fsr4_available(), "fsr3_available": self.fsr3_available(), "other_optiscaler": other, "menu_hint": menu_hint(sc),
               "profile": profiles.profile(sc["appid"])}
        out["opts"] = resolve_mode({**DEFAULT_OPTS, **profiles.defaults(sc["appid"])}, sc["appid"])
        out["modes"] = {m: dict(zip(("algo", "ratio"), profiles.mode_config(sc["appid"], m, MODE_CONFIG[m])))
                        for m in MODE_CONFIG}
        kit = self.kit(sc["appid"])
        out["static_fsr2"] = kit["static"]
        if kit["static"]:
            out["opts"] = {**out["opts"], "algo": "student" if kit["static"].get("algo", "ai") == "ai" else "v1f"}
            out["fg_available"] = False
            # 0.8.3-re: the interceptor's live models; FSR 3.1 needs only the SDK DLL (no fork patch)
            out["fsr3_available"] = self.fg_available()
            out["live_models"] = ["fsr4", "fsr4fast", "fsr3", "sgsr2"]  # 0.8.13: no Vela AI / SR in the panel
        if st:
            opts = dict(DEFAULT_OPTS)
            opts.update(self.migrate(st.get("opts")))
            out["opts"] = opts
            out["installed_version"] = st.get("version", "")
            if kit["static"]:
                # 0.8.11: an older interceptor of ours is not "changed" (refresh_dll replaces it while the game is closed)
                sd = self.static_dxgi(sc["exe_dir"], st, kit) if os.path.isfile(kit["src"]["dxgi.dll"]) else None
                out["intact"] = None if sd is None else sd in ("ours", "old")
                out["dxgi_old"] = sd == "old"
            else:
                ours = self.dll_md5()
                out["intact"] = self._md5(os.path.join(sc["exe_dir"], "dxgi.dll")) == ours if ours else None
            path = os.path.join(sc["exe_dir"], kit["log"])
            if kit["static"]:
                out["log"] = static_log_summary(path, st.get("installed_at", 0))
                # amax follows the game's FSR 2 mode: when the last run had another ratio than the launch options
                # were made for, the panel re-applies them (game closed) -- amax_stale
                m = _RX_VAMAX.search(st.get("launch_set") or "")
                want = amax_str(self.static_ratio(sc["appid"], sc["exe_dir"]) or 2.0, STATIC_AMAX_FIXED)
                # no key = the interceptor's own 0; launch options of 0.4.11-0.4.14 carry VELA_AMAX=0.6 -> re-applied
                out["amax"] = {"set": m.group(1) if m else None, "want": want,
                               "stale": float(m.group(1) if m else 0) != float(want)}
            else:
                out["log"] = self._log(path, st.get("installed_at", 0))
        return out

    # -- enable / reconfigure

    @staticmethod
    def migrate(opts):
        """Options saved by 0.3.x (no mode): "custom", so their algorithm and ratio stay as the player chose."""
        opts = dict(opts or {})
        if opts and "mode" not in opts:
            opts["mode"] = "custom"
        return opts

    @staticmethod
    def clean_opts(opts, base=None):
        clean = dict(DEFAULT_OPTS)
        clean.update(base or {})
        clean.update({k: (opts or {})[k] for k in DEFAULT_OPTS if k in (opts or {})})
        if clean["algo"] not in ALGOS:
            raise ValueError(f"unknown algorithm {clean['algo']}")
        if clean["ratio"] not in RATIOS:
            raise ValueError(f"unknown ratio {clean['ratio']}")
        if clean["fps"] not in FPS_LIMITS:
            raise ValueError(f"unknown frame-rate limit {clean['fps']}")
        if clean["smooth"] not in SMOOTH:
            raise ValueError(f"unknown smoothing {clean['smooth']}")
        if clean["mode"] not in MODES:
            raise ValueError(f"unknown mode {clean['mode']}")
        if clean["model"] not in MODELS:
            raise ValueError(f"unknown model {clean['model']}")
        if clean["res"] not in RESES:
            raise ValueError(f"unknown resolution {clean['res']}")
        clean["fg"] = bool(clean["fg"])
        clean["latency"] = bool(clean["latency"])
        return clean

    def enable(self, appid, opts, launch_options, resolution=None):
        """Install (or update the options of) SGSR 3 for the game. Returns the launch options to set."""
        sc = self.scan(appid, fresh=True)
        if sc["anticheat"]:
            raise RuntimeError(f"anti-cheat in the game ({', '.join(sc['anticheat'])}), SGSR 3 is not installed")
        if not sc["exe_dir"]:
            raise RuntimeError("the game's Windows exe was not found" if sc["install_dir"] else "the game is not installed")
        kit = self.kit(appid)
        if kit["static"]:
            if not os.path.isfile(kit["src"]["dxgi.dll"]):
                raise RuntimeError(f"plugin file assets/{STATIC_ASSETS}/dxgi.dll is missing")
        else:
            for asset in REQUIRED_ASSETS:
                if not os.path.isfile(os.path.join(self.assets, asset)):
                    raise RuntimeError(f"plugin file assets/sgsr3/{asset} is missing")
        d = sc["exe_dir"]
        st = self.read_state(d)
        if st is None and launch_options is None:
            # 0.8.17: never set up a game on options Vela could not read (0.8.16 wrote its own over the player's)
            raise RuntimeError(NOT_READ)
        clean = resolve_mode(self.clean_opts(opts, {**profiles.defaults(appid), **self.migrate((st or {}).get("opts"))}),
                             appid)
        if kit["static"]:  # the game's FSR 2 sets the render size; Vela AI / SR as the profile says, no FG / smoothing
            clean.update({"algo": "student" if kit["static"].get("algo", "ai") == "ai" else "v1f", "fg": False,
                          "ratio": "game"})
        if clean["fg"] and not self.fg_available():
            raise RuntimeError(f"plugin file assets/sgsr3/{FG_DLL} is missing, frame generation is not available")
        if st is None:
            st = self._backup(d, launch_options, resolution, kit)
        else:
            self._backup_missing(d, st, kit)
        rec = self.record(st)
        if launch_options is None:
            # Steam's options not read (the LL / FG cleanup at plugin start): the files change, the launch options and
            # the record stay; the panel sends Steam's options later (llfg_pending) and they get the same exact edit
            new_launch, dx12_added = st.get("launch_set"), st.get("dx12_added", False)
        elif kit["static"]:
            new_launch, rec = launch_build(launch_options, rec, "v1f", False, False,
                                           static_env(kit["static"], self.static_ratio(appid, d)))
        else:
            # "game" ratio: the fork takes amax from the real sizes itself
            amax = [f"OPTI_SGSR3_AMAX={amax_str(clean['ratio'])}"] \
                if AMAX_BY_RATIO and clean["algo"] == "student" and clean["ratio"] != "game" else []
            new_launch, rec = launch_build(launch_options, rec, clean["algo"], sc["engine"] == "Unreal", clean["fg"],
                                           profiles.env_tokens(appid) + amax +
                                           (smooth_env(clean["smooth"]) if clean["algo"] == "v1f" else []),
                                           bool(self.api_args(appid)))
        if launch_options is not None:
            dx12_added = DX12_ARG in rec["args"]
        ini = None
        if not kit["static"]:
            with open(os.path.join(self.assets, "OptiScaler.ini"), encoding="utf-8", newline="") as f:
                ini = make_ini(f.read(), {**clean, "appid": sc["appid"], "fsr_pattern": fsr_pattern(sc), "fsr4_driver": self.fsr4_driver()})
        bdir = os.path.join(d, STATE_DIR, "backup")
        wanted = self._kit_wanted(kit, clean, st)
        for rel in kit["installed"]:
            target = os.path.join(d, *rel.split("/"))
            if rel == "OptiScaler.ini":
                _atomic_write(target, ini.encode("utf-8"))
            elif rel in wanted:
                _atomic_copy(kit["src"][rel], target)
            else:
                # not wanted now: whatever the game had there before us
                info = st["files"].get(rel, {"existed": False})
                if info.get("existed"):
                    if self._md5(target) != info.get("md5"):
                        _atomic_copy(os.path.join(bdir, *rel.split("/")), target)
                elif os.path.isfile(target):
                    os.remove(target)
        if kit["static"]:
            st["dxgi_md5"] = self._md5(kit["src"]["dxgi.dll"])  # 0.8.11: ours, to tell an older build from a foreign file
        st.update({"opts": clean, "version": "static-fsr2" if kit["static"] else self.version.get("dll", ""),
                   "launch_set": new_launch, "kind": "static_fsr2" if kit["static"] else "optiscaler",
                   "dx12_added": dx12_added, "added": rec, "updated_at": time.time()})
        self._write_state(d, st)
        self.log(f"sgsr3: {appid} on in {d}: {clean}, launch: {new_launch}, Vela's tokens: {rec}")
        return {"launch_options": new_launch if launch_options is not None else None, "status": self.status(appid)}

    def set_ini(self, appid, opts):
        """Rewrite only OptiScaler.ini (low latency / frame-rate limit changed): no launch option change."""
        sc = self.scan(appid)
        d = sc["exe_dir"]
        st = self.read_state(d) if d else None
        if st is None or self.kit(appid)["static"]:
            return None  # the interceptor has no ini (low latency / FPS limit are OptiScaler's)
        clean = resolve_mode(self.clean_opts(opts, self.migrate(st.get("opts"))), appid)
        with open(os.path.join(self.assets, "OptiScaler.ini"), encoding="utf-8", newline="") as f:
            _atomic_write(os.path.join(d, "OptiScaler.ini"),
                          make_ini(f.read(), {**clean, "appid": sc["appid"], "fsr_pattern": fsr_pattern(sc), "fsr4_driver": self.fsr4_driver()}).encode("utf-8"))
        st.update({"opts": clean, "updated_at": time.time()})
        self._write_state(d, st)
        return self.status(appid)

    def _backup_one(self, d, bdir, rel):
        src = os.path.join(d, *rel.split("/"))
        if os.path.isfile(src):
            dst = os.path.join(bdir, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            return {"existed": True, "md5": md5(src), "size": os.path.getsize(src)}
        return {"existed": False}

    def _backup(self, d, launch_options, resolution, kit=None):
        kit = kit or self.kit("")
        bdir = os.path.join(d, STATE_DIR, "backup")
        os.makedirs(bdir, exist_ok=True)
        files = {rel: self._backup_one(d, bdir, rel) for rel in kit["installed"] + kit["runtime"]}
        dirs_created = [] if kit["static"] else \
            [rel for rel in ("OptiScaler", "OptiScaler/plugins") if not os.path.isdir(os.path.join(d, *rel.split("/")))]
        st = {"files": files, "dirs_created": dirs_created, "installed_at": time.time(),
              "launch_prev": launch_options, "resolution_prev": resolution, "dx12_added": False}
        # state first: if anything below fails, disable() can still put things back
        self._write_state(d, st)
        return st

    def _backup_missing(self, d, st, kit=None):
        """An install from an older plugin version did not list every file this one may write: back those up now,
        before they are touched (they are still the game's)."""
        kit = kit or self.kit("")
        missing = [rel for rel in kit["installed"] + kit["runtime"] if rel not in st["files"]]
        if missing:
            bdir = os.path.join(d, STATE_DIR, "backup")
            for rel in missing:
                st["files"][rel] = self._backup_one(d, bdir, rel)
            self._write_state(d, st)

    # -- disable

    def disable(self, appid, launch_options):
        """Put back what was there before. Returns the launch options and resolution override to restore."""
        sc = self.scan(appid, fresh=True)
        d = sc["exe_dir"]
        st = self.read_state(d) if d else None
        if st is None:
            raise RuntimeError("SGSR 3 is not installed in this game")
        bdir = os.path.join(d, STATE_DIR, "backup")
        kit = self.kit(appid)
        self._keep_log(d, appid, st, kit["log"])
        # every file the state lists (an install of the other kind before a profile change is undone as well)
        for rel in list(dict.fromkeys(kit["installed"] + kit["runtime"] + list(st["files"]))):
            info = st["files"].get(rel)
            if info is None:
                continue  # an older plugin version's install never touched this file
            path = os.path.join(d, *rel.split("/"))
            if info.get("existed"):
                _atomic_copy(os.path.join(bdir, *rel.split("/")), path)
            elif os.path.isfile(path):
                os.remove(path)
        for rel in sorted(st.get("dirs_created", []), key=len, reverse=True):
            try:
                os.rmdir(os.path.join(d, *rel.split("/")))
            except OSError:
                pass  # not empty: something else was put there, leave it
        restored = launch_off(launch_options, st.get("launch_prev"), st.get("launch_set"), st.get("dx12_added", False),
                              self.record(st))
        # 0.8.15: only the files Vela put in are checked; a runtime file (OptiScaler.log) is written by a running game
        # (Bright Memory 00:15:41: the check failed on it after every file was restored, and Vela stayed on half-undone)
        runtime = set(kit["runtime"]) | set(RUNTIME) | set(STATIC_RUNTIME)
        changed = [rel for rel, info in st["files"].items() if info.get("existed") and rel not in runtime
                   and md5(os.path.join(d, *rel.split("/"))) != info.get("md5")]
        if changed:
            raise RuntimeError(f"restore check failed for {', '.join(changed)}; backup kept in {STATE_DIR}")
        shutil.rmtree(os.path.join(d, STATE_DIR), ignore_errors=True)
        self.log(f"sgsr3: {appid} off, files restored in {d}, launch: {restored}")
        return {"launch_options": restored, "resolution": st.get("resolution_prev"), "status": self.status(appid)}

    # 0.8.13: every run's log is kept (the next launch overwrites the game folder's one): the last KEEP_RUNS per
    # game in <settings>/sgsr3-logs/<appid>-<YYYYmmdd-HHMMSS>.log (the log's own time = the end of that run), older ones
    # moved into sgsr3-logs/_old (never deleted)
    KEEP_RUNS = 5

    def keep_runs(self, appid, running):
        """Copy the game's last log (sgsr3intercept.log / OptiScaler.log) while the game is closed, once per run.
        Returns the new file's path or None. The reconcile loop calls it every 10 s, so the log of a run is saved
        before the game can start again and overwrite it."""
        if running:
            return None
        sc = self.scan(appid)
        d = sc.get("exe_dir")
        st = self.read_state(d) if d else None
        if st is None:
            return None
        path = os.path.join(d, self.kit(appid)["log"])
        try:
            mtime = os.path.getmtime(path)
            if mtime < st.get("installed_at", 0) or os.path.getsize(path) == 0:
                return None
        except OSError:
            return None
        name = f"{appid}-{time.strftime('%Y%m%d-%H%M%S', time.localtime(mtime))}.log"
        dst = os.path.join(self.logs_dir, name)
        if os.path.exists(dst) or os.path.exists(os.path.join(self.logs_dir, "_old", name)):
            return None
        try:
            os.makedirs(self.logs_dir, exist_ok=True)
            shutil.copyfile(path, dst + ".part")
            os.replace(dst + ".part", dst)
        except OSError as e:
            self.log(f"sgsr3: {appid} log not kept: {e}")
            return None
        self._trim_runs(appid)
        return dst

    def _trim_runs(self, appid):
        rx = re.compile(re.escape(str(appid)) + r"-\d{8}-\d{6}\.log$")
        try:
            runs = sorted(n for n in os.listdir(self.logs_dir) if rx.match(n))
        except OSError:
            return
        if len(runs) <= self.KEEP_RUNS:
            return
        old = os.path.join(self.logs_dir, "_old")
        os.makedirs(old, exist_ok=True)
        for n in runs[:-self.KEEP_RUNS]:
            try:
                os.replace(os.path.join(self.logs_dir, n), os.path.join(old, n))
            except OSError as e:
                self.log(f"sgsr3: {appid} log {n} not moved: {e}")

    def _keep_log(self, d, appid, st, name="OptiScaler.log"):
        """The log of the runs (OptiScaler.log or the interceptor's) goes to the settings folder instead of being lost."""
        path = os.path.join(d, name)
        try:
            if os.path.getmtime(path) < st.get("installed_at", 0):
                return
            os.makedirs(self.logs_dir, exist_ok=True)
            shutil.copyfile(path, os.path.join(self.logs_dir, f"{appid}-{name}"))
        except OSError:
            pass
