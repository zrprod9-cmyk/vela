"""0.8.16: Cyberpunk 2077 (1091500) gets VKD3D_CONFIG=no_upload_hvv in its launch options (the game hangs the GPU
without it, with or without Vela); the token is one of ours, so it goes away with Vela's other tokens. Run: python -I tests/test_cp4_env.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import profiles  # noqa: E402
import sgsr3  # noqa: E402

assert "VKD3D_CONFIG=no_upload_hvv" in profiles.env_tokens("1091500")
assert "VKD3D_CONFIG=no_upload_hvv" in profiles.env_tokens(1091500)
for other in ("870780", "1178830", "2239710", "1363080", ""):
    assert "VKD3D_CONFIG=no_upload_hvv" not in profiles.env_tokens(other), other
assert "VKD3D_CONFIG=no_upload_hvv" in profiles.EXTRA_ENV_TOKENS
assert "VKD3D_CONFIG=no_upload_hvv" in sgsr3.OUR_TOKENS
assert profiles.ini_overrides("1091500") == {("Spoofing", "Dxgi"): "false"}  # CP77 key kept
print("test_cp4_env OK")
