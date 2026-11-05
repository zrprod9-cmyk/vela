"""Every game's answer must reach the panel: Decky sends it with json.dumps, and one it cannot encode gets no reply at all,
so the panel sits on Loading (Cyberpunk 2077: the profile's "ini" is keyed by tuples)."""
import asyncio, json, os, sys, tempfile, types
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
tmp = tempfile.mkdtemp()
import profiles  # noqa: E402

# the raw profile cannot be sent: this is the reason public() exists
try:
    json.dumps(profiles.profile("1091500"))
    raise SystemExit("the Cyberpunk profile can be sent now: public() is no longer needed")
except TypeError:
    pass
for appid in profiles.PROFILES:
    json.dumps(profiles.public(appid))
assert profiles.public("1091500")["ini"] == {"Spoofing.Dxgi": "false"}
assert profiles.ini_overrides("1091500") == {("Spoofing", "Dxgi"): "false"}  # the OptiScaler.ini writer keeps its keys
assert profiles.public("no such game") is None

# status() is what really goes to the panel
import sgsr3  # noqa: E402
src = open(os.path.join(ROOT, "py_modules", "sgsr3.py"), encoding="utf-8").read()
assert '"profile": profiles.public(sc["appid"])' in src and '"profile": profiles.profile(' not in src

# any other answer that cannot be sent becomes an error the panel shows
dk = types.ModuleType("decky")
dk.logger = types.SimpleNamespace(info=lambda *a: None, error=lambda *a: None, exception=lambda *a: None)
dk.DECKY_USER_HOME = os.path.join(tmp, "home")
dk.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "ps")
sys.modules["decky"] = dk
sys.path.insert(0, ROOT)
import main  # noqa: E402
P = main.Plugin()
r = asyncio.run(P._run(lambda: {("a", "b"): 1}))
assert r["ok"] is False and "keys must be" in r["error"], r
r = asyncio.run(P._run(lambda: {"a": 1}))
assert r == {"ok": True, "data": {"a": 1}}, r
print("test_panel_reply: OK")
