"""Vela live settings (0.5.0): what the panel sets for a game while it runs.

The fork reads /tmp/vela-live/<appid>.ini every 15 frames (fork patch 0010, VelaLive.h): the
model (OptiScaler's SGSR 2, FSR 4.1, FSR 3.1) re-creates the upscaler, the render ratio goes
to the DLSS optimal-settings answer. 0.8.13: Vela AI / Vela SR (ai, p2b, sr) and their knobs (amax, alphak, mk, fresh,
sq, ncap, disocc) left the panel; a stored old model becomes FSR 4 in a game
with FSR 2 inside the exe (RE4) and SGSR 2 elsewhere (presets.legacy_model), the old knobs are dropped. 0.8.15: FSR 4 Lite
left Vela: a stored model=fsr4lite becomes fsr4 and a stored lossy= (Lite's network rate) is dropped, in the
json and in the /tmp file at once, silently, whenever the settings are read (plugin start: restore_all; a game shown: get); the
key lossy and the model fsr4lite are never written again. The values are kept in
<settings>/live/<appid>.json too, so a reboot (which empties /tmp) or the next launch keeps them: the plugin writes
the /tmp files again when it loads. A key that is not set leaves the launch settings (OptiScaler.ini, launch options).
"""
import json
import os

import presets
import profiles

TMP_DIR = "/tmp/vela-live"
# 0.8.2: "fsr3" = FSR 3.1, OptiScaler's FFX backend with amd_fidelityfx_dx12.dll (fork patch 0014)
MODELS = presets.MODELS
# 0.8.15: keys of FSR 4 Lite that are gone; a stored one is dropped (get). The fork / interceptor keep their Lite code dormant.
# 0.8.15: ratio (render size: the game's DLSS / FSR preset now)
# and npukick (developer knob) are gone the same way; the fork restores the ini's own ratio when the key goes
RETIRED_KEYS = ("lossy", "ratio", "npukick")
# 0.8.0: the network on the Hexagon NPU (py_modules/npu.py): 0 off, 1 exact, 2 async (+1 frame). Absent = 0
# 0.8.3: for model fsr4 = the full FSR 4.1.1 network on the NPU (graph nf4_exact; the provider runs async)
NPU = (0, 1, 2)
# 0.8.6: NPU start for the full FSR 4.1.1 on the NPU: where the provider kicks the HTP in the frame. late (the provider's default
# when the key is absent) = after FSR's post: K2 runs without the HTP's DDR traffic (GPU time 4.07 -> 3.86 ms), the HTP overlaps
# what follows (in game the next frame); early = 0.8.5, before the network. The provider DLL reads it from the same /tmp ini every
# 120 frames (fv_npu.hpp live_kick); the fork ignores the key.
NPU_KICK = ("early", "late")
# 0.8.10: NPU mode for the full FSR 4.1.1 on the NPU: 1 exact (the provider's default when the key is absent: K2 of frame N-1
# with its own network, true FSR 4.1.1, +1 frame latency), 0 async (K2(N) on the network of N-1: no extra latency, fine detail can
# shimmer). The provider DLL reads npuexact= from the same /tmp ini every 120 frames (fv_npu.hpp exact_setting) and reports the
# schedule it runs as "Vela NPU: on, exact|async" (npu.parse_fork mode). Kept when the model changes (main._apply_model).
NPU_EXACT = (0, 1)
# 0.8.16: "FSR 4 Fast" at 1080p UP (output 3 x render) runs as Fast Performance at
# 2 x render + one separate cubic 1.5x pass to the game's image BY DEFAULT (provider fv_npu.hpp u15_setting, read once per FSR 4
# context; on the console 1080p UP takes ~1.5 ms instead of ~2.1). Hidden off-switch, not in the panel:
# fastup15=0 set by hand in <settings>/live/<appid>.json ("fastup15": 0) = plain Fast UP (env VELA_FAST_UP15=0 does the same and wins).
# fastup15=1 (the old opt-in) = the default, kept as it is. The panel never writes the key, a panel change keeps it.
FAST_UP15 = (0, 1)


def _tmp_line(line):
    """One line of a /tmp ini we have no record of: model=fsr4lite -> model=fsr4, lossy= out (0.8.15), a model that is not offered
    out (0.8.15: model=ai), the rest as it is."""
    key, _, val = line.strip().partition("=")
    if key == "model":
        new = presets.RETIRED_MODELS.get(val.strip(), val.strip())
        return (line if new == val.strip() else f"model={new}") if new in MODELS else None
    if key in RETIRED_KEYS:
        return None
    return line


def clean(values):
    """Only known keys with valid values; None or "" drops a key."""
    out = {}
    for k, v in (values or {}).items():
        if v is None or v == "":
            continue
        if k == "model" and presets.RETIRED_MODELS.get(v, v) in MODELS:
            out[k] = presets.RETIRED_MODELS.get(v, v)
        elif k == "npu" and str(v).isdigit() and int(v) in NPU:
            out[k] = int(v)
        elif k == "npuexact" and str(v) in ("0", "1") and not isinstance(v, float):
            out[k] = int(v)
        elif k == "fastup15" and str(v) in ("0", "1") and not isinstance(v, (float, bool)):
            out[k] = int(v)
    return out


class Live:
    def __init__(self, settings, log=print):
        self.dir = os.path.join(settings, "live")
        self.log = log

    def _json(self, appid):
        return os.path.join(self.dir, f"{appid}.json")

    def get(self, appid):
        try:
            with open(self._json(appid), encoding="utf-8") as f:
                raw = json.load(f)
        except (OSError, ValueError):
            return {}
        if not isinstance(raw, dict):
            return {}
        values = clean(raw)
        model = raw.get("model")
        if model in presets.LEGACY_MODELS or model in presets.RETIRED_MODELS or any(k in raw for k in RETIRED_KEYS):
            values = self._migrate(appid, raw)
        return values

    def _migrate(self, appid, raw):
        """0.8.13: a Vela AI / SR model stored by 0.8.12 or older: FSR 4 (FSR 2 inside the exe) or SGSR 2, stored and
        handed to the game at once (no error, no Vela model left in the /tmp file). 0.8.15: model fsr4lite -> fsr4 and the
        key lossy gone, the same way (silently: the log line is the only trace)."""
        model = raw.get("model")
        if model in presets.RETIRED_MODELS:
            model = presets.RETIRED_MODELS[model]
        elif model in presets.LEGACY_MODELS:
            model = presets.legacy_model(bool(profiles.static_fsr2(appid)))
        values = clean({**raw, **({"model": model} if model else {})})
        try:
            os.makedirs(self.dir, exist_ok=True)
            with open(self._json(appid), "w", encoding="utf-8") as f:
                json.dump(values, f, indent=1)
            self._write_tmp(appid, values)
            self.log(f"live {appid}: old settings {({k: raw[k] for k in ('model',) + RETIRED_KEYS if k in raw})} -> {values}")
        except OSError as e:
            self.log(f"live {appid}: migrate: {e}")
        return values

    def _write_tmp(self, appid, values):
        os.makedirs(TMP_DIR, exist_ok=True)
        path = os.path.join(TMP_DIR, f"{appid}.ini")
        if not values:
            if os.path.exists(path):
                os.remove(path)
            return
        text = "".join(f"{k}={v}\n" for k, v in sorted(values.items()))
        tmp = path + ".new"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)

    def set(self, appid, patch):
        """Merge the panel's change (a key with None goes away), store it and hand it to the game. Returns the values."""
        values = self.get(appid)
        patch = {k: v for k, v in dict(patch or {}).items() if k not in RETIRED_KEYS}  # 0.8.15: lossy is never written
        if patch.get("model") in presets.RETIRED_MODELS:
            patch["model"] = presets.RETIRED_MODELS[patch["model"]]  # a stale panel's fsr4lite pick: fsr4
        for k, v in patch.items():
            values.pop(k, None)
            if v is not None and v != "":
                values[k] = v
        values = clean(values)
        os.makedirs(self.dir, exist_ok=True)
        if values:
            with open(self._json(appid), "w", encoding="utf-8") as f:
                json.dump(values, f, indent=1)
        elif os.path.exists(self._json(appid)):
            os.remove(self._json(appid))
        self._write_tmp(appid, values)
        self.log(f"live {appid}: {values or 'launch settings'}")
        return values

    def restore_all(self):
        """The /tmp files again after a reboot or a plugin update."""
        try:
            names = os.listdir(self.dir)
        except OSError:
            names = []
        for name in names:
            if name.endswith(".json"):
                appid = name[:-5]
                try:
                    self._write_tmp(appid, self.get(appid))
                except OSError as e:
                    self.log(f"live {appid}: {e}")
        self._purge_tmp(names)

    def _purge_tmp(self, names):
        """0.8.15: a /tmp file we have no record of that names a model the interceptor does not offer (model=ai from
        0.8.12 or older; /tmp survives a plugin update) makes it log "model 'ai' is not offered" at the game's start: the
        model line goes (the other keys stay), an empty file goes."""
        try:
            files = [f for f in os.listdir(TMP_DIR) if f.endswith(".ini") and f[:-4] + ".json" not in names]
        except OSError:
            return
        for name in files:
            path = os.path.join(TMP_DIR, name)
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    lines = f.read().splitlines()
                keep = [_tmp_line(ln) for ln in lines]
                keep = [ln for ln in keep if ln is not None]
                if keep == lines:
                    continue
                if any(ln.strip() for ln in keep):
                    with open(path + ".new", "w", encoding="utf-8") as f:
                        f.write(chr(10).join(keep) + chr(10))
                    os.replace(path + ".new", path)
                else:
                    os.remove(path)
                self.log(f"live {name[:-4]}: model line of an old Vela removed from {path}")
            except OSError as e:
                self.log(f"live {name[:-4]}: {e}")
