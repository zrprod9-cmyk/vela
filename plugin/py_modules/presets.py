"""Vela 0.8.13: the panel's model choice -- the player picks the upscaler by hand (no modes), and the ONE thing the player still has to do, if any.

The model is the live key model= the DLLs read (/tmp/vela-live/<appid>.ini, live.py): fsr4, fsr3, sgsr2. 0.8.15: FSR 4 Lite
is out (a saved fsr4lite becomes fsr4, RETIRED_MODELS). 0.8.15: the NPU switch is gone (on by default), there is no second upscale, and the performance level is
chosen through the game; the plugin only lets the model change in game, in real time:
  window  always the full screen, 1920x1080 (Steam's per-app resolution override "Default"); no window pick, no Gamescope stretch
  render  the game's own DLSS / FSR quality preset (no live ratio=, no OptiScaler UpscaleRatioOverride)
  npu     FSR 4's network always on the NPU when the system component is installed, always exact (live npu=2, npuexact=1)
Pure functions; main.py applies them.
"""

# the live models the panel offers, in the panel's order
# 0.8.16: fsr4fast = "FSR 4 Fast" -- the same FSR 4 provider in its fast mode (q3kb prepass + rha K2, slim NPU frames;
# measured: RE4 720p P 1.74 -> 0.92 ms, 1080p UP 3.67 -> 1.90); the provider reads model=fsr4fast at FSR 4 context create,
# the fork / the RE4 interceptor run it like fsr4 and re-create the context on fsr4 <-> fsr4fast. Same NPU graphs as fsr4.
MODELS = ("fsr4", "fsr4fast", "fsr3", "sgsr2")
FSR4_MODELS = ("fsr4", "fsr4fast")  # the NPU, exact mode
# 0.8.15: a model the panel no longer has -> the one that replaces it (stored settings, a stale panel's pick); never written again
RETIRED_MODELS = {"fsr4lite": "fsr4"}
# Vela AI / Vela SR / Vela AI P2b / the any-game layer (0.8.12 and older): out of the panel. A game that had one of them
# gets FSR 4 when its FSR 2 is inside the exe (RE4: the interceptor), SGSR 2 in an OptiScaler game (always there)
LEGACY_MODELS = ("ai", "p2b", "sr", "sany", "student", "v1f", "v1")
OUT_1080 = "1920x1080"
OUT_720 = "1280x720"
OUT_900 = "1600x900"
# 0.8.21: outputs with graphs from NPU package 14 on (npu.SIZE_SOCKETS, npu.UP_SOCKETS)
SIZE_OUTPUTS = ("1600x904", "1440x810", "1280x800", "1152x648", "960x540")
# the game kinds (gamecheck.verdict) the panel can set up
KINDS = ("fsr4", "sgsr2", "static")


def legacy_model(static):
    """The model an old setting (Vela AI / SR, no model) becomes: FSR 4 for FSR 2 inside the exe, else SGSR 2."""
    return "fsr4" if static else "sgsr2"


def default_model(kind, fsr4=True, npu_state=None):
    """0.8.16: the model a
    game gets when nobody picked one (the panel's apply_model "auto"): FSR 4 when the plugin carries it -- on the NPU when it is
    ready, else FSR 4 Fast (the GPU path at about half the ms: RE4 720p P 1.74 -> 0.92, 1080p UP 3.67 -> 1.90);
    DX11 (kind sgsr2) and a build without FSR 4: SGSR 2; None for a game Vela cannot set up."""
    if kind not in KINDS:
        return None
    if kind == "sgsr2" or not fsr4:
        return "sgsr2"
    return "fsr4" if npu_ready(npu_state) else "fsr4fast"


def models_for(kind, fsr4=True, fsr3=True):
    """[(model, offered)] for a game of this kind, in the panel's order. DX11 (kind sgsr2): OptiScaler's Dx11Upscaler is
    SGSR 2 whatever the live model says. fsr4 / fsr3: the plugin carries what the model needs (sgsr3.py fsr4_available,
    fsr3_available)."""
    if kind not in KINDS:
        return []
    if kind == "sgsr2":
        return [("sgsr2", True)]
    return [("fsr4", bool(fsr4)), ("fsr4fast", bool(fsr4)), ("fsr3", bool(fsr3)), ("sgsr2", True)]


def is_ultra(render, output):
    """0.8.14: the render is 1/3 of the window (FSR 4 Ultra Performance, its own NPU graphs), from the sizes the log names."""
    try:
        return int(output.split("x")[1]) >= 2.99 * int(render.split("x")[1])
    except (AttributeError, IndexError, ValueError):
        return False


def npu_graph_up(output, service, ultra=False):
    """The daemon of the full FSR 4 graph for this output listens (npu.Npu.service()); ultra: the Ultra Performance graph."""
    service = service or {}
    if ultra:
        return bool((service.get("up_running") or {}).get(output))
    if output == OUT_720:
        return bool(service.get("k1h360_running"))
    if output == OUT_900:
        return bool(service.get("k1h450_running"))
    if output in SIZE_OUTPUTS:
        return bool((service.get("size_running") or {}).get(output))
    return bool(service.get("k1h_running") or service.get("full_running"))


# the outputs the NPU graphs run (FSR 4 and FSR 4 Fast); any other output runs FSR 4 on the GPU
NPU_OUTPUTS = (OUT_1080, OUT_900, OUT_720) + SIZE_OUTPUTS


def npu_runs(p, log, npu_state, output):
    """0.8.21: FSR 4 of this game runs on the NPU, so its output size needs no change: the run says so (the fork's NPU
    line is "on"), or, before that line, the output is one the NPU runs and its graph is up. A run that fell back for the
    frame size (state "fallback", reason "size") does not count."""
    if p["model"] not in FSR4_MODELS:
        return False
    run = (log or {}).get("npu") or {}
    if run.get("state") == "on":
        return True
    if run.get("state") == "fallback" and run.get("reason") == "size":
        return False
    svc = (npu_state or {}).get("service")
    return output in NPU_OUTPUTS and npu_ready(npu_state) and npu_graph_up(output, svc, is_ultra(_norm((log or {}).get("render")), output))


def npu_ready(npu_state):
    """The NPU can go on: the plugin has the layer and the system component is installed."""
    st = npu_state or {}
    return bool(st.get("layer_found") and (st.get("service") or {}).get("installed"))


def _norm(size):
    return (size or "").replace("×", "x").strip() or None


# 0.8.11: Steam's per-app resolution override (SteamClient.Apps.GetResolutionOverrideForApp) = the window Gamescope
# gives the game; the game sees it as the screen and offers no bigger resolution (seen in Into the Dead). "Default" /
# "Native" = the panel (1920x1080).
SCREEN = OUT_1080
_SCREEN_WORDS = ("", "default", "native")


def window_size(override):
    """'1280x720' for Steam's override '1280x720'; SCREEN for 'Default' / 'Native' / ''; None when unknown (no value read
    or not a size)."""
    if override is None:
        return None
    o = str(override).replace("×", "x").strip()
    if o.lower() in _SCREEN_WORDS:
        return SCREEN
    try:
        w, h = (int(v) for v in o.lower().split("x"))
    except ValueError:
        return None
    return f"{w}x{h}" if w > 0 and h > 0 else None


def _height(size):
    try:
        return int(size.split("x")[1])
    except (AttributeError, IndexError, ValueError):
        return 0


def plan(kind, live):
    """What the game's settings mean: {kind, model, npu}. live: the game's live values (live.py)."""
    live = live or {}
    model = live.get("model")
    model = RETIRED_MODELS.get(model, model)
    if model not in MODELS:
        model = legacy_model(kind == "static")
    if kind == "sgsr2":
        model = "sgsr2"
    return {"kind": kind, "model": model, "npu": bool(live.get("npu")) and model in FSR4_MODELS}


def live_wanted(model, npu_state):
    """0.8.15: the live values of a model: FSR 4 always on the NPU when it is ready (exact), nothing else."""
    on = model in FSR4_MODELS and npu_ready(npu_state)
    return {"model": model, "npu": 2 if on else None, "npuexact": 1 if on else None}


def hint(p, st):
    """The one thing the player has to do for plan p, or None (0.8.15: no list of steps). {"id", ...params}.
    st: running, log (the fork's / the interceptor's summary), npu (npu.Npu.status()), window_restart (the panel put Steam's
    window back to 1920x1080 while this game ran), dx12_choice (the game's own Steam launch entries offer DX12)."""
    log = st.get("log") or {}
    running = bool(st.get("running"))
    got = _norm(log.get("output"))
    if running and st.get("window_restart") and got and got != SCREEN:
        return {"id": "restart_window"}
    # the game's own Steam launch entries pick the API (Bright Memory: "StartGame (DX11)" / DX12): before the first frame through
    # the game's upscaler, and when SGSR 2 runs instead of the model picked (OptiScaler's DX11 path = the DX11 entry)
    dx11_run = log.get("sgsr3") and log.get("mode") == "sgsr2" and p["model"] != "sgsr2"
    # 0.8.20: the last run was DirectX 11 (OptiScaler's lines): the game's DX12 entry when Steam offers one, else its setting.
    # No -dx12 is added here: Vela adds it only for Unreal games without their own API entries (sgsr3.launch_build)
    if p["kind"] != "static" and log.get("api") == "dx11":
        return {"id": "dx12_choice"} if st.get("dx12_choice") else {"id": "dx11_run"}
    if p["kind"] != "static" and st.get("dx12_choice") and (not log.get("sgsr3") or dx11_run):
        return {"id": "dx12_choice"}
    if p["model"] in FSR4_MODELS:
        npu = st.get("npu") or {}
        svc = npu.get("service") or {}
        out = got or SCREEN
        ultra = is_ultra(_norm(log.get("render")), out)
        if not npu.get("layer_found"):
            return {"id": "npu_missing_layer"}
        if not svc.get("installed"):
            return {"id": "npu_install"}
        if ultra and not svc.get("up_graph") and not npu_graph_up(out, svc, True):
            return {"id": "npu_update_up"}
        # 0.8.21: a size with graphs only in a newer package: update it, the resolution can stay
        if out in SIZE_OUTPUTS and not svc.get("sizes_graph") and not npu_graph_up(out, svc, ultra):
            return {"id": "npu_update_sizes"}
        # an output with no graph at all runs on the GPU (game_res below): the service is judged by the screen's graph
        if not npu_graph_up(out if out in NPU_OUTPUTS else SCREEN, svc, ultra):
            # without the Qualcomm runtime (missing = an incomplete install of the NPU image) the
            # daemons cannot start -> the installer again; a failed unit -> reboot, then reinstall; else not started yet
            if svc.get("qnn") is False:
                return {"id": "npu_qnn"}
            if svc.get("unit_state") == "failed":
                return {"id": "npu_failed"}
            return {"id": "npu_service"}
    if log.get("out_format") and p["model"] in FSR4_MODELS:
        return {"id": "out_format"}
    if running and log.get("sgsr3") and got and got != SCREEN and not npu_runs(p, log, st.get("npu"), got):
        return {"id": "game_res", "got": got, "res": SCREEN}
    return None
