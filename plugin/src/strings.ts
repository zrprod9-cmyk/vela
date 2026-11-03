// Vela 0.8.13: every text of the panel in one place. The backend sends ids (presets.py, gamecheck.py); the words are here.
// 0.8.16: the texts live in
// src/i18n/en.json, English only; the panel no longer follows Steam's language. tests/test_strings.py checks the keys,
// the {placeholders} and that no Cyrillic reaches the panel (en.json, strings.ts, index.tsx, dist/index.js).
// 0.8.15: one screen — the game, what runs now, four model buttons. "The plugin itself just lets
// you change the model in the game in real time"; speed = the game's own DLSS / FSR preset; no second upscale.
import en from "./i18n/en.json";

export const NAME = "Vela";
// = package.json "version" (build_zip.py refuses a zip where they differ)
export const VERSION = "0.8.16";
// the public installer of the Vela NPU system part (REL/repo/npu, release asset of the same tag); zrprod9-cmyk = the GitHub
// account, set by tools/set_release_repo.py before the public build
export const NPU_INSTALL_CMD = "curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/download/v0.8.16/install-vela-npu.sh | sudo sh";

export type Key = keyof typeof en;
// one text: {name} placeholders filled from p
export function t(key: Key, p: Record<string, string | number> = {}): string {
  const s = en[key] ?? key;
  return s.replace(/\{(\w+)\}/g, (m, k) => (k in p ? String(p[k]) : m));
}
const has = (key: string): key is Key => key in en;
// a dynamic key (backend id) or the fallback text
export const tOr = (key: string, fallback: string, p?: Record<string, string | number>) => has(key) ? t(key, p) : fallback;

// 0.8.16: «FSR 4 Fast» (live model=fsr4fast) = the same AMD network in the provider's fast mode, about twice as fast
export type ModelId = "fsr4" | "fsr4fast" | "fsr3" | "sgsr2";
export const MODEL_IDS: ModelId[] = ["fsr4", "fsr4fast", "fsr3", "sgsr2"];
export const MODEL_NAME: Record<string, string> = {
  fsr4: "FSR 4", fsr4fast: "FSR 4 Fast", fsr3: "FSR 3", sgsr2: "SGSR 2",
};
// one short line under each model card: what it is (the status card says what it does in this game)
export const modelNote = (m: ModelId | "off") => t(`model.note.${m}` as Key);

// ---- the game check (gamecheck.verdict): why a game cannot be upscaled
export const reasonText = (r?: string | null) => r ? tOr(`reason.${r}`, r) : "";
// which item to pick in the game's graphics menu (= py_modules/sgsr3.py menu_hint, the same English words as the fallback)
export function menuHint(inputs: string[] | undefined | null, fallback: string): string {
  if (!inputs) return fallback;
  const fsr = inputs.filter((i) => i.startsWith("FSR"));
  if (fsr.length) return t(inputs.includes("DLSS") ? "menu.fsr_dlss" : "menu.fsr", { fsr: fsr[0] });
  if (inputs.includes("DLSS")) return t(inputs.includes("XeSS") ? "menu.dlss_xess" : "menu.dlss");
  if (inputs.includes("XeSS")) return t("menu.xess");
  return t("menu.any");
}

// ---- the one thing to do (presets.hint ids; main.py chip_no_npu)
export function hintText(h: { id: string; code?: string; got?: string; res?: string; soc?: string; chip?: string }): string {
  const px = (s?: string) => (s ?? "").replace("x", "×");
  if (h.code === "chip_no_npu") {
    // 0.8.16: the id is the backend's sentence (an older panel prints it as it is); the words come from here
    return h.soc === "SM8750" ? t("hint.chip_v79") : t("hint.chip_other", { chip: h.chip || t("hint.chip_unknown") });
  }
  if (h.id === "game_res") return t("hint.game_res", { got: px(h.got), res: px(h.res) });
  return tOr(`hint.${h.id}`, h.id, { cmd: NPU_INSTALL_CMD });
}

// why FSR 4's network is on the GPU (the fork's "Vela NPU: fallback <reason>")
export const npuReason = (r?: string | null) => tOr(`npu_reason.${r ?? ""}`, t("npu_reason.other", { r: r ?? "?" }));

// ---- 0.8.16: «Report a problem» (py_modules/report.py)
export const REPORT_REASONS = ["crash", "fsr", "image", "slow", "other"] as const;
export type ReportReason = typeof REPORT_REASONS[number];
export const reportWhy = (w: string) => tOr(`rp.why.${w}`, w);
