import asyncio
import os
import sys

import decky

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "py_modules"))
import anygame  # noqa: E402
import cpuboost  # noqa: E402
import device  # noqa: E402
import gamecfg  # noqa: E402
import gamecheck  # noqa: E402
import gpufloor  # noqa: E402
import profiles  # noqa: E402
import gameutil  # noqa: E402
import latency  # noqa: E402
import live  # noqa: E402
import npu  # noqa: E402
import presets  # noqa: E402
import report  # noqa: E402
import sgsr3  # noqa: E402
import upscale  # noqa: E402

ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")


def suggest(scan, s3_status, ag_status):
    """Which branch fits the game: "ingame" (its DLSS / FSR / XeSS -> SGSR 3), "anygame" (our layer) or None."""
    if scan.get("anticheat") or not scan.get("install_dir"):
        return None
    if profiles.static_fsr2(scan.get("appid", "")) and s3_status["available"]:
        return "ingame"  # FSR 2 inside the exe: no input library to find, our interceptor takes it
    if s3_status["available"] and scan.get("inputs"):
        return "ingame"
    if ag_status["available"]:
        return "anygame"
    return "ingame" if s3_status["available"] else None


def carry_settings(settings):
    """0.4.0 renamed the plugin (Upscale -> Vela, folder "upscale" kept). Should Decky name the settings folder after
    the new name, the old folder's files (Gamescope profiles, layer / Engine.ini restore records) come along once."""
    import shutil
    try:
        if os.path.isdir(settings) and os.listdir(settings):
            return
        for old in ("upscale", "Upscale"):
            src = os.path.join(os.path.dirname(settings), old)
            if os.path.normcase(os.path.abspath(src)) != os.path.normcase(os.path.abspath(settings)) and os.path.isdir(src):
                shutil.copytree(src, settings, dirs_exist_ok=True)
                decky.logger.info(f"settings carried over from {src}")
                return
    except OSError as e:
        decky.logger.error(f"settings carry-over: {e}")


class Plugin:
    async def _main(self):
        home, settings = decky.DECKY_USER_HOME, decky.DECKY_PLUGIN_SETTINGS_DIR
        carry_settings(settings)
        self.up = upscale.Upscaler(settings, home, log=decky.logger.info)
        self.s3 = sgsr3.Sgsr3(os.path.join(ASSETS, "sgsr3"), home, settings, log=decky.logger.info)
        self.ag = anygame.AnyGame(os.path.join(ASSETS, "layer"), home, settings, log=decky.logger.info)
        self.lat = latency.Latency(home, settings, log=decky.logger.info)
        self.cfg = gamecfg.GameCfg(settings, log=decky.logger.info)
        # 0.5.0: settings the panel changes while the game runs (the fork reads /tmp/vela-live/<appid>.ini)
        self.live = live.Live(settings, log=decky.logger.info)
        self.live.restore_all()
        # 0.8.0: the FSR 4 network on the NPU (our layer assets/npu + the system's fsr4npud, py_modules/npu.py)
        # 0.8.16: the chip (device-tree / socinfo) decides whether the NPU and FSR 4 Fast are offered at all
        self.dev = device.detect()
        self.chip = device.policy(self.dev, device.V79_DIRS + (os.path.join(ASSETS, "npu", "v79"),))
        decky.logger.info(f"Vela device: {device.state(self.dev, self.chip)}")
        self.npu = npu.Npu(os.path.join(ASSETS, "npu"), home, settings, log=decky.logger.info, chip=self.chip)
        try:
            self.npu.sync_manifest()
        except (OSError, ValueError) as e:
            decky.logger.error(f"npu: layer manifest: {e}")
        # 0.8.2: GPU floor while a game Vela upscales runs (py_modules/gpufloor.py: Armada has no per-game GPU minimum)
        self.floor = gpufloor.GpuFloor(settings, log=decky.logger.info)
        # 0.8.3: "CPU for the game": cpu7's cap raised inside Balanced while a chosen game runs (py_modules/cpuboost.py)
        self.cpu = cpuboost.CpuBoost(settings, log=decky.logger.info)
        # 0.8.16: "Report a problem" -- the run's logs in one anonymised zip, sent to our relay only on "Send"
        self.rep = report.Reporter(settings, home, os.path.dirname(os.path.abspath(__file__)), s3=self.s3, npu=self.npu,
                                   live=self.live, log=decky.logger.info,
                                   plugin_log_dir=getattr(decky, "DECKY_PLUGIN_LOG_DIR", None))
        # 0.8.9: what the game allows (gamecheck.py); 0.8.13: the model by hand, the window pick (presets.py)
        self.check = gamecheck.GameCheck(settings, log=decky.logger.info)
        # 0.8.15: the panel lost the window pick, the stretch, the render pick, the NPU toggle, the NPU mode and the advanced
        # section; the clocks they left go back to Vela's defaults once (no control for them any more)
        self._win_restart = set()  # games whose Steam window the panel put back to 1920x1080 while they ran
        try:
            self._clocks_default()
        except Exception:
            decky.logger.exception("clocks to defaults")
        # 0.8.9: LL / FG / their frame limit are out of the panel; a game that had them gets them off now (ini, FG DLL,
        # Engine.ini); its Steam launch options (OPTI_SGSRFG=1) are cleaned when the panel shows the game (only the frontend
        # can write them): llfg_pending
        try:
            self._llfg_migrate()
        except Exception:
            decky.logger.exception("LL/FG off")
        await asyncio.to_thread(self.up.restore_steam_filter)  # once: Steam's own Gamescope filter back (upscale.py)
        # 0.8.13: the training captures (frames for Vela AI) and their back-button chord left with Vela AI
        aio = asyncio.get_event_loop()
        self.loop = aio.create_task(self._reconcile_loop())
        self.cpu_loop = aio.create_task(self._cpuboost_loop())
        decky.logger.info("Vela ready")

    async def _reconcile_loop(self):
        # undo that waits for a game to quit (UE4 rewrites Engine.ini on exit, Wine writes user.reg on exit) also
        # happens with the panel closed, before the game can be started again
        while True:
            await asyncio.sleep(10)
            try:
                # 0.5.4: our dxgi.dll in each closed game Vela is on in follows the plugin's build
                running = await asyncio.to_thread(gameutil.running_appids)
                await asyncio.to_thread(self.floor.reconcile, running)
                await asyncio.to_thread(self.cpu.reconcile, running)  # also the restore after a crash (no game runs)
                self.rep.observe(running)  # 0.8.16: how long each run lasted (the bad-run question)
                if self.up.reset_pending:  # Gamescope was not up at plugin start
                    await asyncio.to_thread(self.up.restore_steam_filter)
                for appid in list(self.up.cfg.get("sgsr3", [])):
                    if appid not in running:
                        # 0.8.13: the last run's log first (the next launch overwrites it), then the DLL update
                        kept = await asyncio.to_thread(self.s3.keep_runs, appid, False)
                        await self._report_run_end(appid, kept)
                        await asyncio.to_thread(self.s3.refresh_dll, appid)
                if self.lat.has_pending() or self.ag.has_pending() or self.cfg.has_pending():
                    running = await asyncio.to_thread(gameutil.running_appids)
                    await asyncio.to_thread(self.lat.reconcile, running)
                    await asyncio.to_thread(self.ag.reconcile, running)
                    await asyncio.to_thread(self.cfg.reconcile, running)
            except Exception:
                decky.logger.exception("reconcile failed")

    async def _report_run_end(self, appid, kept):
        # 0.8.16: a run just ended (its log was kept): when it looks bad the panel asks once (opt-in, never sent alone)
        if not kept:
            return
        try:
            entry = await asyncio.to_thread(self.rep.on_run_end, appid, kept)
            if entry:
                await decky.emit("vela_report_prompt", entry)
        except Exception:
            decky.logger.exception("report: run end")

    async def _cpuboost_loop(self):
        # the temperature / battery / powerd guard of a raised cpu7 cap: sysfs reads only (no /proc scan) while applied
        while True:
            await asyncio.sleep(2)
            try:
                await asyncio.to_thread(self.cpu.guard, gameutil.running_appids)
            except Exception:
                decky.logger.exception("cpu boost guard")

    def _clocks_default(self):
        """0.8.15: the GPU minimum-frequency and "CPU for the game" controls left the panel: every game's GPU floor goes back to Vela's
        default (gpufloor.VELA_GPU_MIN_MHZ, set up with the game) and the CPU boost off."""
        running = gameutil.running_appids()
        for appid, g in list(self.floor.data["games"].items()):
            if g.get("mhz") != gpufloor.VELA_GPU_MIN_MHZ:
                self.floor.set_mhz(appid, gpufloor.VELA_GPU_MIN_MHZ, running)
        for appid in list(self.cpu.data["games"]):
            self.cpu.set_mode(appid, "off", running)

    async def _unload(self):
        for t in ("loop", "cpu_loop"):
            if getattr(self, t, None):
                getattr(self, t).cancel()
        if getattr(self, "floor", None):
            self.floor.release()
        if getattr(self, "cpu", None):
            self.cpu.release()
        decky.logger.info("Vela unloading")

    async def _run(self, fn, *args):
        try:
            return {"ok": True, "data": await asyncio.to_thread(fn, *args)}
        except Exception as e:
            decky.logger.exception(f"{fn.__name__} failed")
            return {"ok": False, "error": str(e) or e.__class__.__name__}

    def _sync_gamescope(self, appid, s3_on, ag_on):
        # the game's frame arrives at the panel resolution already: Gamescope must not upscale it again
        self.up.set_sgsr3(appid, s3_on or ag_on)

    def _game_state(self, appid, window=None):
        """window: Steam's resolution override for the game as the panel read it (GetResolutionOverrideForApp; None = not
        read): the steps say when it is not the window the mode needs (0.8.11)."""
        running = gameutil.running_appids()
        if getattr(self, "rep", None):
            self.rep.observe(running)
        self.lat.reconcile(running)
        self.ag.reconcile(running)
        self.cfg.reconcile(running)
        s3 = self.s3.status(appid)
        s3["game_cfg"] = self.cfg.status(appid)
        s3["live"] = self.live.get(appid)
        s3["npu"] = self.npu.status(appid)
        # 0.8.2: games Vela was on in before 0.8.2 get the floor on the first look
        if s3["installed"] and not self.floor.known(appid):
            self.floor.enable(appid)
        elif not s3["installed"] and self.floor.status(appid)["on"]:
            self.floor.disable(appid, running)  # Vela gone from the game folder (reinstall, verify)
        self.floor.reconcile(running)
        s3["gpu_floor"] = self.floor.status(appid)
        self.cpu.reconcile(running)
        s3["cpu_boost"] = self.cpu.status(appid)
        scan = s3["scan"]
        ag = self.ag.status(appid, scan, appid in running)
        lat = self.lat.status(appid, scan, appid in running)
        # the game folder is the truth (the game may have been reinstalled or verified); keep Gamescope in step
        self._sync_gamescope(appid, s3["installed"], ag["installed"])
        if s3["installed"]:
            kept = self.s3.keep_runs(appid, appid in running)
            if kept:
                try:
                    self.rep.on_run_end(appid, kept)
                except Exception:
                    decky.logger.exception("report: run end")
            # 0.8.16: the profile's game settings (its upscaler on, the output at the full screen) in a game that is
            # on, also one set up before its profile had them; a file the game has not written yet waits for its first run
            cfg = profiles.game_cfg(appid)
            if cfg:
                self.cfg.ensure(appid, scan.get("install_dir"), cfg, appid in running)
                s3["game_cfg"] = self.cfg.status(appid)
        out = {"appid": appid, "running": appid in running, "scan": scan, "sgsr3": s3, "anygame": ag,
               "latency": lat, "suggest": suggest(scan, s3, ag)}
        out.update(self._model_state(appid, out, window))
        try:
            out["report"] = self.rep.panel() if getattr(self, "rep", None) else None
        except Exception:
            decky.logger.exception("report: panel")
            out["report"] = None
        chip = getattr(self, "chip", None)
        if chip is not None:  # 0.8.16: device: {soc, gpu, npu_arch, ...}
            out["device"] = device.state(self.dev, chip)
        return out

    # ---- 0.8.16: "Report a problem" (py_modules/report.py) ----

    def _report_prepare(self, appid, reason, text, launch_options, game_name, log_name):
        running = appid in gameutil.running_appids()
        try:
            gs = self._game_state(appid)
            gs.pop("report", None)
        except Exception as e:
            gs = {"error": str(e)}
        return self.rep.build(appid, reason, text or "", launch_options, gs, game_name, log_name or None, running,
                              "prompt" if log_name else "button")

    async def report_prepare(self, appid, reason, text, launch_options, game_name, log_name=None):
        return await self._run(self._report_prepare, str(appid), str(reason), text, launch_options, game_name, log_name)

    async def report_send(self, local_id):
        return await self._run(self.rep.send, str(local_id))

    async def report_dismiss(self, key, never=False):
        return await self._run(self.rep.dismiss, str(key), bool(never))

    async def report_saved(self):
        return await self._run(self.rep.saved)

    # ---- 0.8.13: the model by hand ----

    def _check(self, appid, scan, s3, ag):
        # 0.8.13: no any-game layer (ag_assets=False): a game without DLSS / FSR / XeSS is "Not supported"
        v = self.check.check(scan, static=bool(profiles.static_fsr2(appid)),
                             s3_assets=os.path.isfile(os.path.join(ASSETS, "sgsr3", "OptiScaler.dll")),
                             ag_assets=False)
        if s3.get("installed") and v["kind"] not in presets.KINDS and v["reason"] != "anticheat":
            # installed by hand / an older version: keep what is in the folder
            v = {**v, "kind": "static" if s3.get("static_fsr2") else "fsr4", "reason": None}
        return v

    def _models(self, kind, s3):
        models = presets.models_for(kind, s3.get("fsr4_available"), s3.get("fsr3_available"))
        chip = getattr(self, "chip", None)
        if chip is not None and not chip["fsr4fast"]:  # 0.8.16: FSR 4 Fast is tuned for the A740 + v73 NPU
            models = [(m, ok and m != "fsr4fast") for m, ok in models]
        if chip is not None and not chip.get("fsr4", True):  # no FSR 4 at all without a supported NPU
            models = [(m, ok and m not in presets.FSR4_MODELS) for m, ok in models]
        return models

    def _chip_off(self, m):
        """Why the chip (not the build) takes model m away: "chip_no_fsr4" (the panel words it), else None."""
        chip = getattr(self, "chip", None)
        if chip is not None and not chip.get("fsr4", True) and m in presets.FSR4_MODELS:
            return "chip_no_fsr4"
        return None

    def _default_model(self, kind, s3, npu_st):
        """presets.default_model, but never a model this chip does not offer (0.8.16: without the NPU and FSR 4 Fast
        the cheap GPU path is FSR 3, then SGSR 2)."""
        model = presets.default_model(kind, s3.get("fsr4_available"), npu_st)
        ok = dict(self._models(kind, s3))
        if not ok.get(model):
            model = next((m for m in ("fsr3", "sgsr2") if ok.get(m)), model)
        return model

    def _model_state(self, appid, gs, window=None):
        s3, ag = gs["sgsr3"], gs["anygame"]
        v = self._check(appid, gs["scan"], s3, ag)
        on = bool(s3.get("installed"))
        npu_st = s3.get("npu")
        models = self._models(v["kind"], s3)
        win = presets.window_size(window)
        out = {"check": v, "models": [{"id": m, "ok": ok, "why": None if ok else self._chip_off(m)} for m, ok in models],
               "model": "off", "plan": None, "hint": None,
               # 0.8.16: what apply_model "auto" would set up (None: Vela cannot set this game up)
               "auto_model": self._default_model(v["kind"], s3, npu_st),
               "window": win, "window_fix": False,
               "layer_on": bool(ag.get("installed")),  # 0.8.12's any-game layer: the panel turns it off
               "launch_fix": self._launch_pending(appid, s3)}
        if not gs["running"]:
            self._win_restart.discard(appid)
        if on and v["kind"] in presets.KINDS:
            lv = self._live_migrate(appid, v["kind"], s3, npu_st)
            p = presets.plan(v["kind"], lv)
            # 0.8.15: the window is always the full screen; the panel puts Steam's override back to Default
            out["window_fix"] = win is not None and win != presets.SCREEN
            st = {"running": gs["running"], "log": s3.get("log"), "npu": npu_st,
                  "window_restart": appid in self._win_restart,
                  "dx12_choice": sgsr3.has_dx12_entry(self.s3.api_args(appid))}
            out.update({"model": p["model"], "plan": p, "hint": presets.hint(p, st)})
            chip = getattr(self, "chip", None)
            if chip is not None and not chip["npu"] and p["model"] in presets.FSR4_MODELS and                     (out["hint"] is None or str(out["hint"].get("id", "")).startswith("npu_")):
                # 0.8.16: no "install Vela NPU" on a chip the NPU graphs are not built for; the panel prints an
                # unknown hint id as it is, so the id is the sentence
                out["hint"] = {"id": chip["note"], "code": "chip_no_npu", "soc": self.dev.get("soc") or "",
                               "chip": self.dev.get("machine") or self.dev.get("gpu") or ""}  # the panel words it
        o = s3.get("opts") or {}
        out["llfg_on"] = appid in self.up.cfg.get("llfg_pending", []) or bool(self.lat.wanted(appid).get("on")) or \
            bool(s3.get("installed") and (o.get("fg") or o.get("latency") or o.get("fps", "off") != "off"))
        return out

    def _live_migrate(self, appid, kind, s3, npu_st):
        """0.8.15, silently: the live values of a set-up game as the panel sets them now -- a model of its own (an old
        Vela AI / SR setting: FSR 4 in RE4, SGSR 2 elsewhere), FSR 4 on the NPU and exact whenever the NPU is ready (an old
        NPU off / fast setting goes), no ratio / npukick (live.py drops them), and OptiScaler.ini without our
        UpscaleRatioOverride (the render size is the game's DLSS / FSR preset again)."""
        lv = s3.get("live") or {}
        model = presets.plan(kind, lv)["model"] if lv.get("model") in presets.MODELS else presets.legacy_model(kind == "static")
        if self._chip_off(model):  # a game set to FSR 4 before (or by an old setting) gets this chip's model
            model = self._default_model(kind, s3, npu_st)
        want = presets.live_wanted(model, npu_st)
        if any(lv.get(k) != v for k, v in want.items()):
            lv = self.live.set(appid, want)
        s3["live"] = lv
        if kind != "static" and (s3.get("opts") or {}).get("ratio", "game") != "game":
            self.s3.set_ini(appid, {"mode": "custom", "ratio": "game", "res": "auto"})
            decky.logger.info(f"{appid}: OptiScaler.ini UpscaleRatioOverride off (the game's preset sets the render)")
        return lv

    # ---- 0.8.9: Vela LL / Vela FG removed; what they left in a game goes off exactly as their off-switches did ----

    def _llfg_active(self, appid):
        st = self.s3.status(appid)
        o = st.get("opts") or {}
        return bool(self.lat.wanted(appid).get("on")) or \
            bool(st["installed"] and (o.get("fg") or o.get("latency") or o.get("fps", "off") != "off"))

    def _llfg_pending(self, appid, add):
        with self.up.lock:
            p = set(self.up.cfg.get("llfg_pending", []))
            p = p | {appid} if add else p - {appid}
            if sorted(p) != self.up.cfg.get("llfg_pending", []):
                self.up.cfg["llfg_pending"] = sorted(p)
                self.up._save()

    def _llfg_off(self, appid, launch_options):
        """LL off (UE4 Engine.ini back, latency.py), FG / LL / frame limit off in OptiScaler.ini, the FG DLL out, the
        launch options without OPTI_SGSRFG=1 (None: the backend's own record; the panel sends Steam's real ones)."""
        running = gameutil.running_appids()
        scan = self.s3.scan(appid, fresh=True)
        if self.lat.read(appid) is not None or self.lat.wanted(appid).get("on"):
            self.lat.set(appid, False, "off", scan, appid in running)
        st = self.s3.status(appid)
        if st["installed"]:
            res = self._s3_enable(appid, {**st["opts"], "fg": False, "latency": False, "fps": "off"}, launch_options, None)
            if launch_options is not None:
                launch_options = res["launch_options"]
        if launch_options is not None:
            self._llfg_pending(appid, False)
        return {"launch_options": launch_options}

    def _llfg_migrate(self):
        games = set(self.up.cfg.get("sgsr3", []))
        try:
            d = os.path.dirname(self.lat._state_path("0"))
            games |= {f[:-5] for f in os.listdir(d) if f.endswith(".json")}
        except OSError:
            pass
        for appid in sorted(games):
            if self._llfg_active(appid):
                self._llfg_off(appid, None)
                self._llfg_pending(appid, True)

    async def llfg_off(self, appid, launch_options):
        return await self._run(self._llfg_off, str(appid), launch_options)

    def _launch_keep(self, appid, launch_options):
        """0.8.15: the launch options of a game Vela is set up in, as they are meant to be: the NPU layer's tokens in
        (whenever the layer is in the plugin and the system component is installed; a console without them gets none) and no
        VELA_FSR2_ALGO (0.8.13 / 0.8.14 wrote =ai after Vela AI had left). The states of sgsr3 / npu forget the token too,
        so the undo (exact restore) still matches. Returns the options to set (None stays None)."""
        self.npu.rewrite_state(appid, sgsr3.drop_legacy_env)
        self.s3.rewrite_launch_set(appid, sgsr3.drop_legacy_env)
        launch = sgsr3.drop_legacy_env(launch_options)
        # 0.8.15: a game whose Steam launch entries pass their own API flag gets none in its options (UE: "-d3d12,
        # -d3d11 ... mutually exclusive" when our -dx12 meets the DX11 entry's -dx11); the npu state remembers them too
        if self.s3.drop_our_dx12(appid):
            launch = sgsr3.drop_api_flags(launch)
            self.npu.rewrite_state(appid, sgsr3.drop_api_flags)
        if self.npu.ready():
            return self.npu.enable(appid, launch)
        return self.npu.disable(appid, launch)

    def _launch_pending(self, appid, s3):
        """Whether Steam's launch options of this game still need _launch_keep (the panel calls launch_fix once when it
        shows the game): the layer's tokens are not in, or VELA_FSR2_ALGO is in what Vela recorded."""
        if not s3.get("installed"):
            return False
        st = self.npu.read(appid) or {}
        if self.npu.ready() and not st.get("on"):
            return True
        if self.s3.dx12_fix_pending(appid):
            return True
        return any(sgsr3.LEGACY_ENV.search(t) for t in (st.get("launch_prev"), st.get("launch_set"),
                                                        self.s3.launch_set(appid)) if isinstance(t, str))

    def _launch_fix(self, appid, launch_options):
        """The panel's one-time migration of a game set up by an older Vela (0.8.15): see _launch_keep."""
        if launch_options is None or not self.s3.status(appid)["installed"]:
            return {"launch_options": launch_options}  # Steam's options not read: nothing to rewrite
        return {"launch_options": self._launch_keep(appid, launch_options)}

    async def launch_fix(self, appid, launch_options):
        return await self._run(self._launch_fix, str(appid), launch_options)

    def _apply_model(self, appid, model, launch_options, resolution):
        """The model picked by hand: fsr4, fsr3, sgsr2 or "off". The first pick puts the files in (resolution =
        Steam's override as it is, for the undo); a change in a game that is on is live (model=) plus the NPU layer in
        the launch options. FSR 4 gets the NPU when it is ready (picking it again keeps the choice);
        FSR 3 / SGSR 2 keep the layer's tokens (0.8.15: the live key alone turns the NPU off). Returns the launch options for the frontend to set and Steam's override (None =
        leave it)."""
        scan = self.s3.scan(appid, fresh=True)
        s3 = self.s3.status(appid)
        ag = self.ag.status(appid, scan, appid in gameutil.running_appids())
        model = presets.RETIRED_MODELS.get(model, model)  # 0.8.15: a stale panel's fsr4lite is fsr4; fsr4lite is never written
        if model == "off":
            res = {"launch_options": launch_options, "resolution": None}
            if s3["installed"]:
                res = self._s3_disable(appid, launch_options)
            elif ag["installed"]:
                res = self._ag_disable(appid, launch_options)
            return {"launch_options": res["launch_options"], "resolution": res.get("resolution"), "model": "off"}
        if model not in presets.MODELS and model != "auto":
            raise ValueError(f"unknown model {model}")
        if ag["installed"]:
            raise RuntimeError("the old any-game layer is on for this game: pick «Off» first")
        v = self._check(appid, scan, s3, ag)
        if v["kind"] not in presets.KINDS:
            raise RuntimeError(f"not supported: {v['reason']}")
        if model == "auto":  # 0.8.16: the device's best model for this game (presets.default_model)
            model = self._default_model(v["kind"], s3, self.npu.status(appid))
        if not dict(self._models(v["kind"], s3)).get(model):
            raise RuntimeError(f"{model} is not available for this game")
        override = None
        if not s3["installed"]:
            # 0.8.15: no ratio override: the game's own DLSS / FSR preset sets the render
            opts = {**s3["opts"], "mode": "custom", "model": "auto", "res": "auto", "algo": "student", "ratio": "game"}
            res = self._s3_enable(appid, opts, launch_options, resolution)
            launch = res["launch_options"]
            override = res.get("resolution")
        else:
            launch = launch_options
        # 0.8.15: FSR 4 always on the NPU when it is ready, exact; the layer's tokens stay in the launch options of a
        # set-up game whatever the model, so FSR 3 -> FSR 4 needs no restart
        launch = self._launch_keep(appid, launch)
        self.live.set(appid, presets.live_wanted(model, self.npu.status(appid)))
        # the window is always the full screen (resolution = Steam's override as the panel read it)
        win = presets.window_size(resolution)
        if win is not None and win != presets.SCREEN:
            override = "Default"
            if appid in gameutil.running_appids():
                self._win_restart.add(appid)
        return {"launch_options": launch, "resolution": override, "model": model}

    async def apply_model(self, appid, model, launch_options, resolution):
        return await self._run(self._apply_model, str(appid), str(model), launch_options, resolution)

    def _window_fix(self, appid):
        """0.8.15: the panel puts Steam's window of a set-up game back to the full screen (an old window pick of 720p / 900p
        or Steam's own setting). A running game takes it at its next start: the hint then says to restart it."""
        if appid in gameutil.running_appids():
            self._win_restart.add(appid)
        return {"resolution": "Default"}

    async def window_fix(self, appid):
        return await self._run(self._window_fix, str(appid))

    def _s3_enable(self, appid, opts, launch_options, resolution):
        if self.ag.status(appid, self.s3.scan(appid)).get("installed"):
            raise RuntimeError("the layer upscale is on for this game: switch it off first")
        want = self.lat.wanted(appid)
        # the NPU layer's tokens sit on top of SGSR 3's: off before, on again after, so each restores exactly
        npu_on = bool((self.npu.read(appid) or {}).get("on"))
        if npu_on:
            launch_options = self.npu.disable(appid, launch_options)
        res = self.s3.enable(appid, {**(opts or {}), "latency": want["on"], "fps": want["fps"]}, launch_options,
                             resolution)
        if npu_on:
            res["launch_options"] = self.npu.enable(appid, res["launch_options"])
        self._sync_gamescope(appid, True, False)
        cfg = profiles.game_cfg(appid)
        if cfg:
            self.cfg.enable(appid, res["status"]["scan"].get("install_dir"), cfg, appid in gameutil.running_appids())
        self.floor.enable(appid)
        self.floor.reconcile(gameutil.running_appids())
        return res

    def _s3_disable(self, appid, launch_options):
        if (self.npu.read(appid) or {}).get("on"):
            launch_options = self.npu.disable(appid, launch_options)
            self.live.set(appid, {"npu": None})
        res = self.s3.disable(appid, launch_options)
        self.floor.disable(appid, gameutil.running_appids())
        self._sync_gamescope(appid, False, False)
        self.cfg.disable(appid, appid in gameutil.running_appids())
        return res

    def _ag_disable(self, appid, launch_options):
        scan = self.s3.scan(appid)
        res = self.ag.disable(appid, launch_options, scan, appid in gameutil.running_appids())
        self._sync_gamescope(appid, False, False)
        return res

    async def game_state(self, appid, window=None):
        return await self._run(self._game_state, str(appid), None if window is None else str(window))
