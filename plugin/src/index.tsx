import { addEventListener, callable, definePlugin, removeEventListener, toaster, useQuickAccessVisible } from "@decky/api";
import { DialogButton, Focusable, PanelSection, PanelSectionRow, Router, staticClasses, TextField, ToggleField } from "@decky/ui";
import { useEffect, useState } from "react";
import { LuScanEye } from "react-icons/lu";
import {
  hintText, Key, menuHint, MODEL_IDS, MODEL_NAME, modelNote, ModelId, NAME, npuReason, reasonText, REPORT_REASONS, ReportReason,
  reportWhy, t, updateError,
} from "./strings";

// Vela 0.8.15: ONE screen — the game and what runs now, then FSR 4 / FSR 3 / SGSR 2 / Off, switched live
// in the running game. Everything else is automatic: the files and launch options (NPU layer tokens always), FSR 4 on the NPU
// and exact, the window at the full 1920×1080, the render size from the game's own DLSS / FSR preset.

declare const SteamClient: any;
declare const appStore: any;

// ---- what the backend sends ------------------------------------------------------------------------------------------
type Res<T> = { ok: true; data: T } | { ok: false; error: string };
type NpuLog = { state: "on" | "fallback" | "off" | null; mode: string | null; reason: string | null };
type S3Log = {
  api?: string | null; out_format?: boolean;
  sgsr3: boolean; render: string | null; output: string | null; mode: string | null; calibrated?: boolean;
  fsr4_fallback?: string | null; fsr3_fallback?: string | null; sgsr2_fallback?: string | null;
  not_offered?: string | null; npu?: NpuLog | null;
};
type Live = { model?: string; npu?: number; npuexact?: number };
type S3State = { installed: boolean; intact: boolean | null; log: S3Log | null; menu_hint: string; static_fsr2?: object | null; live?: Live };
type Check = { kind: string; reason: string | null };
type Hint = { id: string; code?: string; got?: string; res?: string; soc?: string; chip?: string };
type GameState = {
  appid: string; running: boolean; sgsr3: S3State; check: Check; scan?: { inputs?: string[] | null };
  models: { id: ModelId; ok: boolean; why?: string | null }[]; model: ModelId | "off"; hint: Hint | null;
  llfg_on?: boolean; layer_on?: boolean; launch_fix?: boolean; window_fix?: boolean;
  report?: ReportPanel | null;
};
// 0.8.16: «Report a problem» (py_modules/report.py)
type Pending = { key: string; appid: string; why: string[] };
type ReportPanel = { configured: boolean; rate_left: number; auto_prompt: boolean; pending: Pending | null };
type Preview = { id: string; files: { name: string; bytes: number }[]; zip_bytes: number; status: string;
  report_id: string | null; configured: boolean; rate_left: number; path: string; why: string[] };
type Switched = { launch_options: string | null; resolution?: string | null };
// py_modules/updater.py
type UpdateState = { enabled: boolean; current: string; latest: string | null; available: boolean; checked_at: number | null;
  error: string | null };
type Install = { url: string; name: string; version: string; hash: string; install_type: number };

function wrap<A extends any[], T>(name: string) {
  const fn = callable<A, Res<T>>(name);
  return async (...args: A): Promise<T> => {
    const r = await fn(...args);
    if (!r.ok) throw new Error(r.error);
    return r.data;
  };
}
const api = {
  game: wrap<[appid: string, window: string | null], GameState>("game_state"),
  applyModel: wrap<[appid: string, model: string, launch: string | null, resolution: string | null], Switched>("apply_model"),
  llfgOff: wrap<[appid: string, launch: string | null], Switched>("llfg_off"),
  launchFix: wrap<[appid: string, launch: string | null], { launch_options: string | null }>("launch_fix"),
  windowFix: wrap<[appid: string], { resolution: string }>("window_fix"),
  reportPrepare: wrap<[appid: string, reason: string, text: string, launch: string | null, name: string, log: string | null], Preview>("report_prepare"),
  reportSend: wrap<[id: string], { report_id: string }>("report_send"),
  reportDismiss: wrap<[key: string, never: boolean], ReportPanel>("report_dismiss"),
  updateState: wrap<[auto: boolean], UpdateState>("update_state"),
  updateCheck: wrap<[], UpdateState>("update_check"),
  updateEnable: wrap<[on: boolean], UpdateState>("update_enable"),
  updatePrepare: wrap<[], Install>("update_prepare"),
};

// one-time fixes of a game set up by an older Vela, once per game while the plugin is loaded
let llfgFixed = "";   // 0.8.9: Vela LL / FG removed
let launchKept = "";  // 0.8.15: NPU layer tokens in, VELA_FSR2_ALGO out, our -dx12 out where the game has its own API entries
let layerFixed = "";  // 0.8.13: the any-game layer removed
let windowFixed = ""; // 0.8.15: Steam's window back to the full 1920×1080 (no «Game window» any more)

// ---- Steam -----------------------------------------------------------------------------------------------------------
function launchOptions(appid: string): Promise<string | null> {
  return new Promise((resolve) => {
    let done = false;
    let reg: any = null;
    const finish = (v: string | null) => {
      if (done) return;
      done = true;
      setTimeout(() => reg?.unregister?.(), 0);
      resolve(v);
    };
    try {
      // 0.8.17: only real details count (a callback without strLaunchOptions is not "no options": 0.8.16 took it for "" and
      // the backend wrote its own options over the player's); a slow Steam gets 10 s, then null = not read, nothing changes
      reg = SteamClient.Apps.RegisterForAppDetails(Number(appid), (d: any) => {
        if (typeof d?.strLaunchOptions === "string") finish(d.strLaunchOptions);
      });
    } catch {
      finish(null);
    }
    setTimeout(() => finish(null), 10000);
  });
}
function runningApp(): { appid: string; name: string } | null {
  const app = (Router as any).MainRunningApp;
  return app ? { appid: String(app.appid), name: app.display_name } : null;
}
function pageApp(): { appid: string; name: string } | null {
  try {
    const path: string = (Router as any).WindowStore?.GamepadUIMainWindowInstance?.m_history?.location?.pathname ?? "";
    const m = path.match(/^\/library\/app\/(\d+)/);
    if (!m) return null;
    const ov = appStore?.GetAppOverviewByAppID?.(Number(m[1]));
    return { appid: m[1], name: ov?.display_name ?? `App ${m[1]}` };
  } catch {
    return null;
  }
}
const currentApp = () => runningApp() ?? pageApp();
async function steamResolution(appid: string): Promise<string | null> {
  try {
    const r = await SteamClient.Apps.GetResolutionOverrideForApp(Number(appid));
    return r === undefined || r === null ? null : String(r);
  } catch {
    return null;
  }
}

// ---- the status card ---------------------------------------------------------------------------------------------------
const px = (s?: string | null) => (s ?? "?").replace(/x/, "×");
// what the log says runs: fsr2 = the game's own FSR 2 (RE4 interceptor), anything else unknown = an older DLL's fallback
const runName = (m?: string | null) => !m ? t("other_upscaler") : m === "fsr2" ? t("game_fsr2") : MODEL_NAME[m] ?? t("other_upscaler");

type Status = { dot: string; text: string; sub?: string; why?: string };
const GREEN = "#50c060", YELLOW = "#e0b040", RED = "#e05050", GREY = "#888", WAIT = "#8fb8e8";

function statusOf(gs: GameState): Status {
  const s3 = gs.sgsr3, c = gs.check;
  if (gs.layer_on) return { dot: YELLOW, text: t("st.layer"), sub: gs.running ? t("st.layerRunning") : t("st.layerOff") };
  if (c.kind === "none") return { dot: GREY, text: t("not_supported"), sub: reasonText(c.reason) };
  if (!s3.installed) return { dot: GREY, text: t("st.off") };
  if (s3.intact === false) return { dot: RED, text: t("st.filesChanged"), sub: t("st.filesChangedSub") };
  const want = s3.live?.model ?? gs.model;
  const wantName = MODEL_NAME[want] ?? want;
  if (!gs.running) return { dot: GREEN, text: t("st.onIdle", { m: wantName }), sub: t("st.onIdleSub") };
  const L = s3.log;
  if (!L) return { dot: YELLOW, text: t("st.nextStart") };
  const isStatic = !!s3.static_fsr2;
  const sizes = L.render && L.output ? `${px(L.render)} → ${px(L.output)}` : undefined;
  const runs = isStatic && !L.sgsr3 ? t("game_fsr2") : runName(L.mode);
  // the chosen model could not run: the one short reason the log gives
  if (L.not_offered) return { dot: YELLOW, text: t("st.notOffered", { key: L.not_offered, runs }), sub: sizes };
  if (L.fsr4_fallback || L.sgsr2_fallback) return { dot: YELLOW, text: t("st.failed", { want: wantName, runs }), sub: sizes };
  if (L.fsr3_fallback) return { dot: YELLOW, text: t("st.failed", { want: MODEL_NAME.fsr3, runs }), sub: sizes, why: t("st.fsr3FailedSub") };
  // the last run was DirectX 11: no upscaler of Vela's can start (the hint says what to switch)
  if (!L.sgsr3 && L.api === "dx11") return { dot: YELLOW, text: t("st.dx11") };
  // no frame through the game's upscaler yet (its menu): waiting, not an error
  if (!L.sgsr3) {
    const what = isStatic ? t("st.waitingFsr2") : t("st.waitingMenu", { hint: menuHint(gs.scan?.inputs, s3.menu_hint) });
    return { dot: WAIT, text: t("st.waiting", { m: wantName }), sub: t("st.waitingSub", { what }) };
  }
  const model = runName(L.mode);
  if (L.mode && MODEL_NAME[L.mode] && want !== L.mode) return { dot: GREEN, text: t("st.switching", { from: model, to: wantName }), sub: sizes };
  if (L.mode === "fsr4" || L.mode === "fsr4fast") {
    const n = L.npu;
    if (n?.state === "on") return { dot: GREEN, text: `${t("st.works")} · ${model} ${t("st.onNpu")}`, sub: sizes };
    if (n?.state === "fallback") return { dot: YELLOW, text: `${t("st.works")} · ${model} ${t("st.onGpu")}`, sub: sizes, why: npuReason(n.reason) };
    // the provider reports the NPU at the first frame it upscales: before it, FSR 4 is set up and waits
    if (s3.live?.npu && n?.state !== "off") return { dot: WAIT, text: t("st.waiting", { m: model }), sub: sizes };
  }
  return { dot: GREEN, text: `${t("st.works")} · ${model} ${t("st.onGpu")}`, sub: sizes };
}

const SMALL = { fontSize: 12, lineHeight: 1.4, opacity: 0.8 } as const;
const HEAD = { fontSize: 12, fontWeight: 600, opacity: 0.6, textTransform: "uppercase" as const, letterSpacing: 0.4, marginTop: 4 };
const LINE = { fontSize: 12, opacity: 0.75, marginTop: 2, lineHeight: 1.35 } as const;
const Small = ({ children }: { children: any }) => <PanelSectionRow><div style={SMALL}>{children}</div></PanelSectionRow>;

// ---- 0.8.16: «Report a problem»: reason + short text -> the file list -> «Send» (never sent by itself) ----
const appName = (appid: string, fallback?: string) => {
  try {
    return appStore?.GetAppOverviewByAppID?.(Number(appid))?.display_name ?? fallback ?? `App ${appid}`;
  } catch {
    return fallback ?? `App ${appid}`;
  }
};
function sendError(e: any): string {
  const m = String(e?.message ?? e);
  if (m === "not_configured") return "";
  if (m.startsWith("rate_limited:")) return t("rp.rateLimited", { min: Math.max(1, Math.ceil(Number(m.split(":")[1]) / 60)) });
  return t("rp.failed", { why: m.replace(/^(relay|network):/, "") });
}
type Target = { appid: string; name: string; log: string | null };
function ReportBox({ game, rp, onDone }: { game: { appid: string; name: string }; rp: ReportPanel | null | undefined; onDone: () => void }) {
  const [open, setOpen] = useState(false);
  const [target, setTarget] = useState<Target | null>(null);
  const [reason, setReason] = useState<ReportReason>("crash");
  const [text, setText] = useState("");
  const [phase, setPhase] = useState<"form" | "collecting" | "preview" | "sending" | "done">("form");
  const [pv, setPv] = useState<Preview | null>(null);
  const [msg, setMsg] = useState<string | null>(null);
  const reset = () => { setOpen(false); setTarget(null); setPhase("form"); setPv(null); setMsg(null); setText(""); };
  const start = (tg: Target, r: ReportReason) => {
    setTarget(tg); setReason(r); setPhase("form"); setPv(null); setMsg(null); setOpen(true);
  };
  const collect = async () => {
    const tg = target ?? { appid: game.appid, name: game.name, log: null };
    setPhase("collecting"); setMsg(null);
    try {
      const launch = await launchOptions(tg.appid);
      const p = await api.reportPrepare(tg.appid, reason, text.slice(0, 500), launch, tg.name, tg.log);
      setPv(p); setPhase("preview");
      if (!p.configured) setMsg(t("rp.notConfigured", { path: p.path }));
    } catch (e: any) {
      setMsg(String(e?.message ?? e)); setPhase("form");
    }
  };
  const send = async () => {
    if (!pv) return;
    setPhase("sending"); setMsg(null);
    try {
      const r = await api.reportSend(pv.id);
      setMsg(t("rp.sent", { id: r.report_id })); setPhase("done"); onDone();
    } catch (e: any) {
      setMsg(sendError(e) || t("rp.notConfigured", { path: pv.path })); setPhase("preview");
    }
  };
  const p = rp?.pending;
  if (!open && p) {
    const name = appName(p.appid);
    const answer = async (never: boolean) => {
      try { await api.reportDismiss(p.key, never); } catch { /* the next refresh shows it again */ }
      onDone();
    };
    const crashy = p.why.some((w) => w === "gpu_hang" || w === "short_run");
    return (
      <PanelSectionRow>
        <div style={{ borderRadius: 6, padding: "10px 12px", background: "rgba(224,176,64,0.12)", marginTop: 6 }}>
          <div style={{ fontSize: 13, fontWeight: 600 }}>{t("rp.askTitle")}: {name}</div>
          <div style={LINE}>{p.why.map(reportWhy).join(" · ")}</div>
          <div style={{ fontSize: 13, marginTop: 6 }}>{t("rp.ask")}</div>
          <Focusable style={{ display: "flex", flexDirection: "column", marginTop: 6 }} flow-children="vertical">
            <DialogButton style={{ marginBottom: 6 }} onClick={() => start({ appid: p.appid, name, log: p.key }, crashy ? "crash" : "fsr")}>
              {t("rp.askYes")}
            </DialogButton>
            <DialogButton style={{ marginBottom: 6 }} onClick={() => answer(false)}>{t("rp.askNo")}</DialogButton>
            <DialogButton onClick={() => answer(true)}>{t("rp.askNever")}</DialogButton>
          </Focusable>
        </div>
      </PanelSectionRow>
    );
  }
  if (!open) return (
    <PanelSectionRow>
      <DialogButton style={{ marginTop: 8, padding: "6px 12px", fontSize: 13 }}
        onClick={() => start({ appid: game.appid, name: game.name, log: null }, "crash")}>{t("rp.button")}</DialogButton>
    </PanelSectionRow>
  );
  const busy = phase === "collecting" || phase === "sending";
  return (<>
    <PanelSectionRow><div style={HEAD}>{t("rp.title")} · {target?.name ?? game.name}</div></PanelSectionRow>
    {phase === "form" || phase === "collecting" ? (<>
      <PanelSectionRow>
        <Focusable style={{ display: "flex", flexDirection: "column" }} flow-children="vertical">
          {REPORT_REASONS.map((r) => (
            <DialogButton key={r} disabled={busy} onClick={() => setReason(r)} style={{ minWidth: 0, padding: "6px 12px",
              textAlign: "left", marginBottom: 4, ...(reason === r ? { background: "#1a9fff", color: "white" } : {}) }}>
              {reason === r ? "✓ " : ""}{t(`rp.reason.${r}` as Key)}
            </DialogButton>
          ))}
        </Focusable>
      </PanelSectionRow>
      <PanelSectionRow>
        <TextField label={t("rp.textLabel")} value={text} disabled={busy}
          onChange={(e: any) => setText(String(e?.target?.value ?? "").slice(0, 500))} />
      </PanelSectionRow>
      <PanelSectionRow>
        <Focusable style={{ display: "flex", gap: 6 }} flow-children="horizontal">
          <DialogButton disabled={busy} onClick={collect}>{phase === "collecting" ? t("rp.collecting") : t("rp.collect")}</DialogButton>
          <DialogButton disabled={busy} onClick={reset}>{t("rp.cancel")}</DialogButton>
        </Focusable>
      </PanelSectionRow>
    </>) : (<>
      {pv && phase !== "done" && (
        <PanelSectionRow>
          <div style={SMALL}>
            <div>{t("rp.files", { n: pv.files.length, kb: Math.max(1, Math.round(pv.zip_bytes / 1024)) })}</div>
            {pv.files.map((f) => <div key={f.name}>• {f.name} · {Math.max(1, Math.round(f.bytes / 1024))} KB</div>)}
          </div>
        </PanelSectionRow>
      )}
      <PanelSectionRow>
        <Focusable style={{ display: "flex", gap: 6 }} flow-children="horizontal">
          {phase !== "done" && pv?.configured && (
            <DialogButton disabled={busy} onClick={send}>{phase === "sending" ? t("rp.sending") : msg ? t("rp.retry") : t("rp.send")}</DialogButton>)}
          <DialogButton disabled={busy} onClick={reset}>{phase === "done" ? "OK" : t("rp.cancel")}</DialogButton>
        </Focusable>
      </PanelSectionRow>
    </>)}
    {msg && <Small>{msg}</Small>}
  </>);
}

// ---- updates: a newer release on GitHub -> «Vela X.Y.Z is available» + Update; Decky installs it after its own confirm ----
type Updates = ReturnType<typeof useUpdates>;
function useUpdates(visible: boolean) {
  const [st, setSt] = useState<UpdateState | null>(null);
  // the in-place answer of «Check now»: null until pressed in this panel
  const [manual, setManual] = useState<"checking" | "done" | null>(null);
  const [phase, setPhase] = useState<"idle" | "preparing" | "confirm">("idle");
  const [msg, setMsg] = useState<string | null>(null);
  useEffect(() => {
    if (!visible) return;
    api.updateState(true).then(setSt).catch(() => undefined);
  }, [visible]);
  const checkNow = async () => {
    setManual("checking"); setMsg(null);
    try {
      setSt(await api.updateCheck());
    } catch (e: any) {
      setMsg(updateError(e?.message ?? String(e)));
    }
    setManual("done");
  };
  const enable = async (on: boolean) => {
    try { setSt(await api.updateEnable(on)); } catch { /* the switch shows the stored value on the next open */ }
  };
  const install = async () => {
    if (phase === "preparing") return;
    setPhase("preparing"); setMsg(null);
    try {
      const p = await api.updatePrepare();
      // Decky's own install-from-URL: it shows its confirm window, checks the sha256, replaces and reloads Vela
      const backend = (window as any).DeckyBackend;
      if (!backend?.call) throw new Error("no_decky");
      await backend.call("utilities/install_plugin", p.url, p.name, p.version, p.hash, p.install_type);
      setPhase("confirm");
    } catch (e: any) {
      setMsg(updateError(e?.message ?? String(e))); setPhase("idle");
    }
  };
  return { st, manual, phase, msg, checkNow, enable, install };
}

function UpdateButton({ up }: { up: Updates }) {
  return (
    <DialogButton disabled={up.phase === "preparing"} onClick={up.install}
      style={{ minWidth: 0, padding: "4px 14px", fontSize: 13, width: "auto", flexShrink: 0 }}>
      {up.phase === "preparing" ? t("up.preparing") : t("up.update")}
    </DialogButton>
  );
}
function UpdateOffer({ up }: { up: Updates }) {
  return (<>
    <Focusable style={{ display: "flex", alignItems: "center", gap: 8 }} flow-children="horizontal">
      <div style={{ fontSize: 13, fontWeight: 600, flexGrow: 1 }}>{t("up.available", { v: up.st?.latest ?? "" })}</div>
      <UpdateButton up={up} />
    </Focusable>
    {up.phase === "confirm" && <div style={LINE}>{t("up.confirm")}</div>}
    {up.msg && <div style={LINE}>{up.msg}</div>}
  </>);
}
// the line at the top, after the automatic check found a newer version
function UpdateLine({ up }: { up: Updates }) {
  if (!up.st?.available || !up.st.enabled || up.manual) return null;
  return (
    <PanelSection>
      <PanelSectionRow>
        <div style={{ borderRadius: 6, padding: "8px 12px", background: "rgba(26,159,255,0.14)" }}><UpdateOffer up={up} /></div>
      </PanelSectionRow>
    </PanelSection>
  );
}
// the switch and «Check now» at the bottom; the manual answer shows here
function UpdateSection({ up }: { up: Updates }) {
  const st = up.st;
  let result: any = null;
  if (up.manual === "checking") result = <div style={SMALL}>{t("up.checking")}</div>;
  else if (up.manual === "done") {
    const err = st?.error ? updateError(st.error) : null;
    result = (<>
      {err && <div style={SMALL}>{err}</div>}
      {st?.available ? <UpdateOffer up={up} /> : !err && !up.msg && <div style={SMALL}>{t("up.latest", { v: st?.current ?? "" })}</div>}
      {!st?.available && up.msg && <div style={SMALL}>{up.msg}</div>}
    </>);
  }
  return (
    <PanelSection title={t("up.title")}>
      <PanelSectionRow>
        <ToggleField label={t("up.auto")} description={t("up.autoDesc")} checked={st?.enabled ?? true} disabled={!st}
          onChange={(on: boolean) => up.enable(on)} />
      </PanelSectionRow>
      <PanelSectionRow>
        <DialogButton disabled={up.manual === "checking"} onClick={up.checkNow}
          style={{ padding: "6px 12px", fontSize: 13 }}>{up.manual === "checking" ? t("up.checking") : t("up.checkNow")}</DialogButton>
      </PanelSectionRow>
      {result && <PanelSectionRow>{result}</PanelSectionRow>}
    </PanelSection>
  );
}

function Content() {
  const visible = useQuickAccessVisible();
  const up = useUpdates(visible);
  return (<>
    <UpdateLine up={up} />
    <GameContent visible={visible} />
    <UpdateSection up={up} />
  </>);
}

function GameContent({ visible }: { visible: boolean }) {
  const [game, setGame] = useState(currentApp());
  const [res, setRes] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [gs, setGs] = useState<GameState | null>(null);
  const [busy, setBusy] = useState(false);

  const load = async () => {
    const g = currentApp();
    setGame(g);
    if (!g) return;
    const r = await steamResolution(g.appid);
    setRes(r);
    try {
      setGs(await api.game(g.appid, r));
      setErr(null);
    } catch (e: any) {
      setErr(e?.message ?? String(e));
      setGs(null);
    }
  };
  useEffect(() => {
    if (!visible) return;
    load();
    const t = setInterval(load, 1500);
    return () => clearInterval(t);
  }, [visible]);

  if (!game) return <PanelSection><Small>{t("t.noGame")}</Small></PanelSection>;
  if (!gs) return <PanelSection><PanelSectionRow>{err ?? t("t.loading")}</PanelSectionRow></PanelSection>;

  const toast = (e: any) => toaster.toast({ title: NAME, body: e?.message ?? String(e) });
  const s3 = gs.sgsr3;
  const on = !!s3.installed;
  const kind = gs.check.kind;
  const supported = kind !== "none" && !gs.layer_on;
  const model: ModelId | "off" = on ? gs.model : "off";

  const setLaunch = (o: string) => SteamClient.Apps.SetAppLaunchOptions(Number(game.appid), o);
  const setOverride = async (r: string | null | undefined) => {
    if (!r) return;
    await SteamClient.Apps.SetAppResolutionOverride(Number(game.appid), r);
    setRes(r);
  };
  const refresh = async () => {
    const r = await steamResolution(game.appid);
    setRes(r);
    setGs(await api.game(game.appid, r));
  };
  const run = async (fn: () => Promise<void>) => {
    if (busy) return;
    setBusy(true);
    try {
      await fn();
      await refresh();
    } catch (e: any) {
      toast(e);
    } finally {
      setBusy(false);
    }
  };
  // the one click: the backend sets the files (first time), launch options, the live model and the NPU
  const pick = (m: ModelId | "off") => run(async () => {
    const launch = await launchOptions(game.appid);
    const r = await api.applyModel(game.appid, m, launch, res);
    if (typeof r.launch_options === "string" && r.launch_options !== launch) await setLaunch(r.launch_options);
    await setOverride(r.resolution);
  });

  // ---- silent one-time fixes of what an older Vela left ----
  if (gs.llfg_on && !busy && llfgFixed !== game.appid) {
    llfgFixed = game.appid;
    setTimeout(() => run(async () => {
      const launch = await launchOptions(game.appid);
      const r = await api.llfgOff(game.appid, launch);
      if (r.launch_options !== null && r.launch_options !== launch) await setLaunch(r.launch_options);
    }), 0);
  }
  if (gs.launch_fix && !busy && launchKept !== game.appid) {
    launchKept = game.appid;
    setTimeout(() => run(async () => {
      const launch = await launchOptions(game.appid);
      const r = await api.launchFix(game.appid, launch);
      if (r.launch_options !== null && r.launch_options !== launch) await setLaunch(r.launch_options);
    }), 0);
  }
  if (gs.window_fix && !busy && windowFixed !== game.appid) {
    windowFixed = game.appid;
    setTimeout(() => run(async () => setOverride((await api.windowFix(game.appid)).resolution)), 0);
  }
  if (gs.layer_on && !gs.running && !busy && layerFixed !== game.appid) {
    layerFixed = game.appid;
    setTimeout(() => pick("off"), 0);
  }

  const status = statusOf(gs);
  const speed = ["fsr4", "sgsr2", "static"].includes(kind) ? t(`speed.${kind}` as Key) : null;
  const fsr1 = kind === "none" && !["not_installed", "non_steam"].includes(gs.check.reason ?? "");
  const offered = new Map(gs.models.map((m) => [m.id, m.ok]));
  const why = new Map(gs.models.map((m) => [m.id, m.why])); // "chip_no_fsr4" = the chip, not the build
  const modelButton = (m: ModelId | "off") => {
    const lit = model === m;
    const ok = m === "off" || !!offered.get(m);
    return (
      <DialogButton key={m} disabled={busy || !ok} onClick={() => !lit && pick(m)}
        style={{ minWidth: 0, padding: "8px 12px", textAlign: "left", marginBottom: 6,
          ...(lit ? { background: "#1a9fff", color: "white" } : {}) }}>
        <div style={{ fontSize: 14, fontWeight: 600 }}>{lit ? "✓ " : ""}{m === "off" ? t("off_title") : MODEL_NAME[m]}</div>
        <div style={{ fontSize: 12, opacity: lit ? 0.95 : 0.7, marginTop: 2 }}>{ok ? modelNote(m) : why.get(m) === "chip_no_fsr4" ? t("model.why.chip_no_fsr4") : t("not_in_build")}</div>
      </DialogButton>
    );
  };

  return (
    <PanelSection>
      <PanelSectionRow>
        <div style={{ borderRadius: 6, padding: "10px 12px", background: "rgba(255,255,255,0.06)" }}>
          <div style={{ fontSize: 14, fontWeight: 600, marginBottom: 4 }}>{game.name}</div>
          <div style={{ fontSize: 13 }}><span style={{ color: status.dot }}>●</span> {status.text}</div>
          {status.sub && <div style={LINE}>{status.sub}</div>}
          {status.why && <div style={LINE}>{status.why}</div>}
          {on && gs.hint && <div style={{ ...LINE, opacity: 1, color: YELLOW, marginTop: 6 }}>→ {hintText(gs.hint)}</div>}
          {fsr1 && <div style={{ ...LINE, marginTop: 6 }}>{t("steam_fsr1")}</div>}
        </div>
      </PanelSectionRow>
      {(supported || fsr1) && (<>
        <PanelSectionRow><div style={HEAD}>{t("t.sectionModel")}</div></PanelSectionRow>
        <PanelSectionRow>
          <Focusable style={{ display: "flex", flexDirection: "column" }} flow-children="vertical">
            {supported && MODEL_IDS.filter((m) => offered.has(m)).map(modelButton)}
            {modelButton("off")}
          </Focusable>
        </PanelSectionRow>
        {busy && <Small>{t("t.busy")}</Small>}
        {supported && !busy && (model === "off" ? <Small>{t("t.pickHint")}</Small> : speed && <Small>{speed}</Small>)}
      </>)}
      <ReportBox game={game} rp={gs.report} onDone={() => { refresh().catch(() => undefined); }} />
    </PanelSection>
  );
}

export default definePlugin(() => {
  // 0.8.16: a run that looked bad -> one toast; the question itself is in the panel (opt-in, nothing is sent alone)
  const onPrompt = (p: Pending) => toaster.toast({ title: `${NAME} · ${appName(p?.appid ?? "")}`, body: t("rp.toast") });
  addEventListener<[Pending]>("vela_report_prompt", onPrompt);
  return {
    name: NAME,
    titleView: <div className={staticClasses.Title}>{NAME}</div>,
    content: <Content />,
    icon: <LuScanEye />,
    onDismount: () => removeEventListener("vela_report_prompt", onPrompt),
  };
});
