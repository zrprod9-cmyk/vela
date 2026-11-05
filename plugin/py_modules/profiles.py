"""Per-game profiles of SGSR 3 in the game: what our measurements say about a game, applied when SGSR 3 is switched on.

A profile adds environment variables of the fork to the launch options (and may name a preferred algorithm). The
player's own choice in the panel stays: the profile only changes the defaults and what the fork is told.

  lines  2: the HUD lock's line detector at threshold 2 instead of 4 (OPTI_SGSR3_LINES=2;:
            Bright Memory +0.48...0.70 dB to DLSS M, DRG 540p -0.41; unknown games keep 4
  algo   the model of every mode for the game ("v1f" = Vela SR: the student, Vela AI, is weaker there)
  speed  (algo, ratio) of the "Speed" mode for a heavy game where the student does not fit the frame
         (Vela SR 480p "2.25" or 360p "3.0"); no game measured as such yet
  nolock v1f without the thin-feature locks (OPTI_SGSR3_NOLOCK=1)
  env    more fork keys for the game (the v1f switches of, sgsr3.VF_ENV_KEYS). Vela AI is one
         model for every game since 0.4.11 (amax 0.6, mk 8, alphaK 1): no per-game model keys
  game_cfg  a settings file of the game and keys Vela sets while it is on, undone when it is off (gamecfg.py)
  static_fsr2  the game's FSR 2 is inside its exe: our interceptor DLL (assets/staticfsr) instead of OptiScaler,
         {"algo": "ai" | "sr", "env": [interceptor keys]} (sgsr3.kit)
  smooth the "smoothing" the panel picks first (BMI: "sharp", a wider kernel loses 0.5 dB to DLSS M there)
  hint   a line for the panel under the mode text (a game setting worth changing, e.g. RE4's Ambient Occlusion)
  ini    OptiScaler.ini keys for the game, {("Section", "Key"): "value"}, applied after sgsr3.ini_values()
         (Cyberpunk: upstream's own Dxgi=false, see the Cyberpunk 2077 profile below)
"""

LINES_ENV = "OPTI_SGSR3_LINES=2"
NOLOCK_ENV = "OPTI_SGSR3_NOLOCK=1"
ENV_TOKENS = (LINES_ENV, NOLOCK_ENV)
# launch keys of profiles that are not the fork's (OPTI_SGSR3_* go anyway): taken off with ours
EXTRA_ENV_TOKENS = ("VKD3D_SHADER_MODEL=6_6", "VKD3D_FEATURE_LEVEL=12_2", "VKD3D_CONFIG=no_upload_hvv")

PROFILES = {
    # Bright Memory: Infinite: thin HUD lines and text gain from the lower threshold; the student model loses to v1f there
    # (-0.6...-1.5 dB on this game)
    "1178830": {"name": "Bright Memory: Infinite", "lines": 2, "algo": "v1f", "smooth": "sharp",
                "note": "Vela SR instead of Vela AI (0.6-1.5 dB better), line threshold 2, «Sharp»"},
    # Control: the HUD locks cost v1f 0.26 / 0.15 dB to DLSS M (0.18 / 0.19 in motion; whitish spots and halo).
    # Its input: RGBA16F with the game's HDR on, R10G10B10A2 (shadows already 10-bit) with it off.
    # + SQ (sqrt storage of the working colour and history): GPU replay of Control 540p / 480p
    # +0.07 / +0.12 dB, flicker of dark pixels x0.80; it loses on DRG (-0.3 dB) and BMI, so Control only.
    # The game's HDR on: its colour then comes as RGBA16F instead of R10G10B10A2, and the dark flickers 2-3x less
    # with both models.
    # alphaK 0.75 for Vela AI (Control 540p / 480p +0.22 / +0.27 dB, flicker -15 %, jag 1.95 -> 1.53; Ghostrunner -0.27)
    # was dropped in 0.4.5: with the lock by min(history state, current) it strengthens the ghost behind a moving hero
    # (-0.9 dB on third-person scenes).
    # 0.8.16: the console's renderer.ini kept output 1280x720 = render 1280x720 from the old 720p window pick
    # (the log showed "Initializing SGSR3 1280x720 -> 1280x720", nothing upscaled); the window is always the full
    # screen since 0.8.15, so the game's output follows it, and a render = the full output (no upscaling) becomes 960x540
    # (FSR 4 Performance, the size Control ran FSR 4 at before); a smaller render stays (the console's
    # 1280x720 is then FSR 4 Quality 1.5x at the 1080p output)
    "870780": {"name": "Control", "nolock": True, "env": ["OPTI_SGSR3_SQ=1"],
               "game_cfg": {"file": "renderer.ini",
                            "keys": {"m_eAllowHDR": 1, "m_iOutputResolutionX": 1920, "m_iOutputResolutionY": 1080,
                                     "m_iRenderResolutionX": 960, "m_iRenderResolutionY": 540},
                            # 1280x720 too -- the render the game writes itself after a 720p window
                            # (renderer.ini rewritten by the game to 1280x720 / 1280x720, FSR 4 context
                            # 1280x720 -> 1280x720, nothing upscaled); 960x540 -> 1920x1080 is the intended case
                            "when": {"m_iRenderResolutionX": [1920, 1280], "m_iRenderResolutionY": [1080, 720]}},
               "note": "the game's HDR on (shadows flicker 2-3 times less), no HUD line locks, dark tones in sqrt encoding"},
    # Manor Lords: its upscaler input is FSR 3.1 / DLSS (UE5)
    # DX12 under vkd3d-proton needs shader model 6.6 / feature level 12_2 or the game says "DX12 is not supported";
    # the game's anti-aliasing must be FSR 3.1 (DLSS does not work there).
    # Vela AI knobs per game (alphaK 0.75, mk 4) were dropped in 0.4.11: one Vela AI for all games with amax 0.6.
    "1363080": {"name": "Manor Lords", "env": ["VKD3D_SHADER_MODEL=6_6", "VKD3D_FEATURE_LEVEL=12_2"],
                "note": "choose FSR anti-aliasing in the game"},
    # Resident Evil 4 (0.4.7): its FSR 2 is built into re4.exe -- our interceptor replaces it
    # by Vela AI (Vela SR shows fireflies there); the game's FSR 2 must be on (any mode: it sets the render size)
    # amax 0.6 comes as for every game (sgsr3.AMAX -> VELA_AMAX). The interceptor's VELA_SKIP_SDF is not offered: the
    # game's own Ambient Occlusion = Off turns the same Global Distance Field pass off (title 14.5 -> 39 FPS)
    # 0.8.16: the interceptor needs the game's FSR 2 on; local_config.ini [AppRender] AppUpScalingTypeFSR2 (default 0 =
    # off, local_config_default.ini); 3 gave 640x360 -> 1280x720 = 2.0x (Performance) on the console
    "2050650": {"name": "Resident Evil 4", "static_fsr2": {"algo": "ai", "env": ["VELA_RCAS=0"]},
                "game_cfg": {"file": "local_config.ini", "format": "ini", "section": "AppRender",
                             "keys": {"AppUpScalingTypeFSR2": 3}, "when": {"AppUpScalingTypeFSR2": [0]}},
                # 0.4.15: x2 (FSR 2 "Performance", 960x540) instead of the user's x3 (Ultra Performance) -- less grain
                "hint": "For FPS turn Ambient Occlusion off in the game (about 2.5 times faster). "
                        "Less noise: FSR 2 «Performance» in the game (960×540)",
                "note": "turn FSR 2 on in the game (any mode), Vela replaces it"},
    # 0.8.16: Ghostrunner's own settings had DLSS off (GameUserSettings.ini in the prefix: DLSSEnabled=False, DLSSMode=0);
    # with DLSS off the game never calls an upscaler and Vela gets no frame. DLSSMode is the UE DLSS plugin's UDLSSMode
    # (exe strings in this order: Off, Auto, UltraQuality, Quality, Balanced, Performance, UltraPerformance -> 5 = Performance);
    # FidelityFSR is the game's spatial FSR 1 (off with DLSS). Each key only when it is at its "off" value
    "1139900": {"name": "Ghostrunner",
                "game_cfg": {"root": "prefix", "format": "ini",
                             "file": "AppData/Local/Ghostrunner/Saved/Config/WindowsNoEditor/GameUserSettings.ini",
                             "section": "/Script/Ghostrunner.GhostrunnerGameUserSettings",
                             "keys": {"DLSSEnabled": "True", "DLSSMode": 5, "FidelityFSREnabled": "False"},
                             "when": {"DLSSEnabled": ["False"], "DLSSMode": [0], "FidelityFSREnabled": ["True"]}}},
    # Deep Rock Galactic: the lower threshold loses 0.41 dB at 540p (Vela SR); the mk 4 knob for Vela AI went in 0.4.11
    # 0.8.16: DRG's own upscaler setting is FSD/Saved/Config/WindowsNoEditor/GameUserSettings.ini in the game folder,
    # UpscalingType=0 (off) on the console; 3 = FSR 2 (the log then shows "ffxFsr2ContextCreate_Dx12 context created",
    # then "Vela FSR 4 VK ... 1130x636 -> 1920x1080", LowResMV true); only when it is off, the player's choice stays
    "548430": {"name": "Deep Rock Galactic", "lines": 4, "note": "line threshold 4 (Vela SR)",
               "game_cfg": {"file": "FSD/Saved/Config/WindowsNoEditor/GameUserSettings.ini", "format": "ini",
                            "section": "/Script/FSD.FSDGameUserSettings",
                            "keys": {"UpscalingType": 3}, "when": {"UpscalingType": [0]}}},
    # Cyberpunk 2077: with DXGI spoofing the game sees "NVIDIA GeForce RTX 4090" and the Adreno
    # hangs (hangcheck, vkd3d_queue) 2-6 s after the intro; upstream OptiScaler turns DXGI spoofing off for this exe
    # (Streamline spoofing alone opens DLSS); FSR 3.1 / XeSS inputs need no spoofing.
    # The GPU hang (hangcheck, vkd3d_queue, status 00EF0087, 20-66 s in, during the intro videos) happens
    # WITHOUT Vela too (Turnip + vkd3d-proton of Proton 11 ARM64); VKD3D_CONFIG=no_upload_hvv (UPLOAD heap not in host-visible
    # VRAM) reached the title screen and held 150 s. Proton appends its own
    # force_bindless_texel_buffer for this game to VKD3D_CONFIG, so ours is kept next to it.
    "1091500": {"name": "Cyberpunk 2077", "ini": {("Spoofing", "Dxgi"): "false"}, "env": ["VKD3D_CONFIG=no_upload_hvv"],
                "note": "no GPU spoofing (as OptiScaler does for this game)"},
}


def profile(appid):
    return PROFILES.get(str(appid))


def public(appid):
    """The profile as the panel gets it: the backend's answer goes through json.dumps, and an "ini" keyed by tuples made it
    fail with no reply at all, so the panel sat on Loading for that game (Cyberpunk 2077)."""
    p = profile(appid)
    if not p or "ini" not in p:
        return p
    return dict(p, ini={".".join(k): v for k, v in p["ini"].items()})


def env_tokens(appid):
    p = profile(appid) or {}
    out = []
    if p.get("lines") == 2:
        out.append(LINES_ENV)
    if p.get("nolock"):
        out.append(NOLOCK_ENV)
    out += list(p.get("env", ()))
    return out


def defaults(appid):
    """The profile's first choices for the panel options (the player's own choice wins)."""
    p = profile(appid) or {}
    return {k: p[k] for k in ("smooth",) if k in p}


def static_fsr2(appid):
    """{"algo": "ai" | "sr", "env": [...]} for games whose FSR 2 is built into the exe (our interceptor), else None."""
    return (profile(appid) or {}).get("static_fsr2")


def game_cfg(appid):
    return (profile(appid) or {}).get("game_cfg")


def ini_overrides(appid):
    """OptiScaler.ini keys of the game's profile: {(section, key): value}."""
    return dict((profile(appid) or {}).get("ini") or {})


def mode_config(appid, mode, base):
    """(algo, ratio) of a panel mode for the game: the plugin's (base) with the profile's model / heavy-game speed."""
    p = profile(appid) or {}
    algo, ratio = base
    if mode == "speed" and p.get("speed"):
        return tuple(p["speed"])
    if p.get("algo"):
        algo = p["algo"]
    return algo, ratio
