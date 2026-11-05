"""0.8.16: English-only panel texts, src/i18n/en.json.
- one language: en.json only (no ru.json, no fixture of the old Russian texts), the panel does not follow Steam's language;
- no empty text; every key the panel code uses (t("..."), and the dynamic families model.note / reason / hint / st /
  npu_reason / rp.*) exists; every hint id the backend can send has a text;
- the menu hint = py_modules/sgsr3.py menu_hint for the same inputs (the backend fallback is the same English text);
- no Cyrillic reaches the panel: en.json, strings.ts, index.tsx, dist/index.js (when built), and every string constant of
  main.py and py_modules/*.py that is not a docstring (hint ids, chip notes, menu hints, errors shown as toasts).
Run: python -I tests/test_strings.py"""
import ast
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
SRC = os.path.join(ROOT, "src")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import device  # noqa: E402
import sgsr3  # noqa: E402

CYR = re.compile("[\u0400-\u04ff]")
read = lambda *p: open(os.path.join(*p), encoding="utf-8").read()

# ---- one language
assert sorted(os.listdir(os.path.join(SRC, "i18n"))) == ["en.json"], os.listdir(os.path.join(SRC, "i18n"))
assert not os.path.exists(os.path.join(HERE, "fixtures", "ru_rc3_snapshot.json"))
en = json.load(open(os.path.join(SRC, "i18n", "en.json"), encoding="utf-8"))
for k, v in en.items():
    assert v.strip(), k
    assert not CYR.search(v), ("Cyrillic in en.json", k)

ts, tsx = read(SRC, "strings.ts"), read(SRC, "index.tsx")
code = tsx + ts
assert "ru.json" not in code and "LocalizationManager" not in code and "navigator.language" not in code, "a language switch is back"
assert 'import en from "./i18n/en.json";' in ts and "export type Key = keyof typeof en;" in ts
used = set(re.findall(r'\bt\("([\w.]+)"', code))
assert used and used <= set(en), used - set(en)
for fam, ids in {"model.note": ["fsr4", "fsr4fast", "fsr3", "sgsr2", "off"], "model.why": ["chip_no_fsr4"], "speed": ["fsr4", "sgsr2", "static"],
                 "rp.reason": ["crash", "fsr", "image", "slow", "other"],
                 "reason": ["anticheat", "not_installed", "non_steam", "opengl", "vulkan_native", "no_inputs", "no_exe", "godot"],
                 "hint": ["restart_window", "dx12_choice", "npu_missing_layer", "npu_install", "npu_update_up", "npu_update_sizes", "npu_qnn",
                          "npu_service", "npu_failed", "game_res", "chip_v79", "chip_other", "chip_unknown"]}.items():
    for i in ids:
        assert f"{fam}.{i}" in en, f"{fam}.{i}"
# every hint id the backend can send has a text
for i in re.findall(r'\{"id": "(\w+)"', read(ROOT, "py_modules", "presets.py")):
    assert f"hint.{i}" in en, i
# the English texts are the ones the panel had before the Russian language was removed: spot checks
assert en["rp.button"] == "Report a problem" and en["off_title"] == "Off" and en["hint.chip_unknown"] == "unknown chip"
assert len(en) == 130, len(en)  # 0.8.21: hint.npu_update_sizes, reason.non_steam; 0.8.20: hint.dx11_run, hint.out_format, st.dx11; incl. model.why.chip_no_fsr4 and the 20 update texts (up.*)
# every reason py_modules/updater.py can give the panel has a text (network / github have their own wording)
upd_src = read(ROOT, "py_modules", "updater.py")
codes = set(re.findall(r'UpdateError\("(\w+)', upd_src)) | {"network", "github"}
assert codes >= {"bad_answer", "no_zip", "no_checksum", "hash_mismatch", "bad_zip", "not_newer"}, codes
for c in codes | {"no_decky", "other"}:
    assert f"up.err.{c}" in en, c

# ---- the menu hint: panel text = backend fallback
for inputs, key, p in ((["FSR 2", "DLSS"], "menu.fsr_dlss", {"fsr": "FSR 2"}), (["FSR 3"], "menu.fsr", {"fsr": "FSR 3"}),
                       (["DLSS", "XeSS"], "menu.dlss_xess", {}), (["DLSS"], "menu.dlss", {}), (["XeSS"], "menu.xess", {}),
                       ([], "menu.any", {})):
    assert re.sub(r"\{(\w+)\}", lambda m: p[m.group(1)], en[key]) == sgsr3.menu_hint({"inputs": inputs}), key

# ---- the chip note (sent as the hint id of chip_no_npu, logged, in game_state["device"]) = the panel's English text
v79 = device.policy({"soc": "SM8750"}, ())
assert v79["note"] == en["hint.chip_v79"], v79["note"]
other = device.policy({"soc": "X", "machine": "Foo"}, ())
assert other["note"] == en["hint.chip_other"].replace("{chip}", "Foo"), other["note"]
for pol in (v79, other, device.policy({}, ())):
    assert not any(CYR.search(s) for s in pol["unavailable"] + [pol["note"]]), pol

# ---- no Cyrillic reaches the panel
for name, s in (("strings.ts", ts), ("index.tsx", tsx)):
    bad = [l for l in s.splitlines() if CYR.search(l)]
    assert not bad, (name, bad[:3])
dist = os.path.join(ROOT, "dist", "index.js")
if os.path.isfile(dist):
    d = read(dist)
    assert not CYR.search(d), ("Cyrillic in dist/index.js", CYR.search(d).start())
for p in [os.path.join(ROOT, "main.py")] + sorted(glob.glob(os.path.join(ROOT, "py_modules", "*.py"))):
    tree = ast.parse(read(p))
    docs = {id(n.body[0].value) for n in ast.walk(tree)
            if isinstance(n, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and n.body
            and isinstance(n.body[0], ast.Expr) and isinstance(n.body[0].value, ast.Constant)}
    for n in ast.walk(tree):
        if isinstance(n, ast.Constant) and isinstance(n.value, str) and id(n) not in docs:
            assert not CYR.search(n.value), (os.path.basename(p), n.lineno, n.value[:60])

DEV_DIR = "fsr4" + "bench"  # a developer-only folder name; it must never reach the panel
assert DEV_DIR not in ts and DEV_DIR not in json.dumps(en) and "install-vela-npu.sh | sudo sh" in ts
print("test_strings OK:", len(en), "texts, English only, no Cyrillic in the panel, the backend strings or the bundle")
