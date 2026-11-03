"""0.8.16: per-game OptiScaler.ini keys from profiles.py "ini". Cyberpunk 2077 (1091500) gets upstream
OptiScaler's own Dxgi=false (the explicit Dxgi=true skipped the fork's DisableDxgiSpoofing quirk, dllmain.cpp:1376, and the game
picked its NVIDIA path, then a GPU hang); every other game keeps Dxgi=true. Run: python -I tests/test_cp77_ini.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "py_modules"))
import profiles  # noqa: E402
import sgsr3  # noqa: E402

with open(os.path.join(ROOT, "assets", "sgsr3", "OptiScaler.ini"), encoding="utf-8", newline="") as f:
    TPL = f.read()


assert profiles.ini_overrides("1091500") == {("Spoofing", "Dxgi"): "false"}
assert profiles.ini_overrides(1091500) == {("Spoofing", "Dxgi"): "false"}  # int appid too
assert profiles.ini_overrides("870780") == {} and profiles.ini_overrides("") == {}
assert sgsr3.ini_values({"appid": "1091500"})[("Spoofing", "Dxgi")] == "false"
assert sgsr3.ini_values({"appid": "1091500"})[("Spoofing", "StreamlineSpoofing")] == "true"
for other in ("870780", "1178830", "2239710", ""):
    assert sgsr3.ini_values({"appid": other})[("Spoofing", "Dxgi")] == "true", other
assert sgsr3.ini_values({})[("Spoofing", "Dxgi")] == "true"
# the written ini text: [Spoofing] Dxgi=false only for Cyberpunk


def section_value(text, section, key):
    cur = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            cur = s[1:-1].lower()
        elif cur == section.lower() and "=" in s and s.split("=", 1)[0].strip().lower() == key.lower():
            return s.split("=", 1)[1].strip()
    return None


assert section_value(sgsr3.make_ini(TPL, {"appid": "1091500"}), "Spoofing", "Dxgi") == "false"
assert section_value(sgsr3.make_ini(TPL, {"appid": "870780"}), "Spoofing", "Dxgi") == "true"
print("test_cp77_ini: ok")
