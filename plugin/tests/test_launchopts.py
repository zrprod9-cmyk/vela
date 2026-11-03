"""0.8.17: launchopts.py, the token-exact edit of Steam launch options (Vela adds its own tokens and takes out exactly those).
Run: python -I tests/test_launchopts.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import launchopts as L  # noqa: E402
import sgsr3  # noqa: E402

DLL = 'WINEDLLOVERRIDES="dxgi=n,b"'
# tokens: quotes keep their spaces, an unclosed quote runs to the end
assert L.words('A="x  y" %command% -n "Two  Words" \'q r\'') == ['A="x  y"', "%command%", "-n", '"Two  Words"', "'q r'"]
assert L.words('-a "unclosed  x') == ["-a", '"unclosed  x'] and L.words(None) == [] and L.words("") == []
assert L.env_name("VKD3D_CONFIG=dxr") == "VKD3D_CONFIG" and L.env_name("-dx12") is None and L.env_name("~/x=1") is None

# add + remove = the very same text, whatever the spacing and quotes
for user in ("", "%command%", "-dx11 -nosplash", "VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 %command%",
             "PROTON_ENABLE_NVAPI=1 %command% -arg1 -arg2", '  A="x  y"   %command%  -n "Two  Words"  ',
             "~/lsfg /usr/libexec/armada/armada-game-launch %command% -fullscreen", "%command% -dx12 -something"):
    new, rec = L.add(user, [DLL, "OPTI_SGSR3_STUDENT=1"], ["-dx12"])
    assert L.remove(new, rec) == (user if user.strip() or user == "" else ""), (user, new, rec)
    assert "%command%" in L.words(new) and new.startswith(DLL + " OPTI_SGSR3_STUDENT=1 "), new
    assert L.words(new).count("-dx12") == 1, new  # the player's -dx12 is not doubled

# where things go: env in front (the given order), args at the very end, %command% in front of options without it
new, rec = L.add("-dx11 -nosplash", ["A=1", "B=2"], ["-z"])
assert new == "A=1 B=2 %command% -dx11 -nosplash -z" and rec == {"env": ["A=1", "B=2"], "args": ["-z"], "cmd": True}
# a variable the player sets (any value) is not added: his wins; WINEDLLOVERRIDES as before
new, rec = L.add('WINEDLLOVERRIDES="xinput1_3=n,b" VKD3D_CONFIG=dxr %command%', [DLL, "VKD3D_CONFIG=no_upload_hvv", "X=1"])
assert new == 'X=1 WINEDLLOVERRIDES="xinput1_3=n,b" VKD3D_CONFIG=dxr %command%' and rec["env"] == ["X=1"], new
# a variable named after %command% is an argument, not the environment: added in front all the same
assert L.add("%command% A=1", ["A=1"])[0] == "A=1 %command% A=1"

# remove: only the recorded tokens, once each, the player's copies and edits stay
rec = {"env": [DLL], "args": ["-dx12"], "cmd": False}
assert L.remove(DLL + " " + DLL + " %command% -dx12 -dx12", rec) == DLL + " %command% -dx12"
assert L.remove("MANGOHUD=1 " + DLL + " %command% -dx12 -late", rec) == "MANGOHUD=1 %command% -late"
assert L.remove("%command% -dx12 " + DLL, rec) == "%command% " + DLL, "an env token after %command% is the player's argument"
assert L.remove(None, rec) is None and L.remove("x %command%", None) == "x %command%"
# our %command% goes only while it is the first token (a variable the player put in front keeps it)
crec = {"env": ["A=1"], "args": [], "cmd": True}
assert L.remove("A=1 %command% -x", crec) == "-x"
assert L.remove("P=1 A=1 %command% -x", crec) == "P=1 %command% -x"
assert L.remove("A=1 %command%", crec) == ""

# replace keeps Vela's %command% Vela's when the player put a token in front of it
new, rec = L.replace("P=1 A=1 %command% -x", crec, ["B=2"])
assert new == "B=2 P=1 %command% -x" and rec["cmd"], (new, rec)
assert L.remove(new, rec) == "P=1 %command% -x"

# derive (a setup made before 0.8.17): Vela's look AND not in the options before it; -dx12 when the state says so
user = "VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6 %command%"
old_set = DLL + " OPTI_SGSR3_STUDENT=1 %command% -dx12"  # what 0.8.16 wrote (the two variables stripped)
rec = sgsr3.legacy_record(user, old_set, True)
assert rec == {"env": [DLL, "OPTI_SGSR3_STUDENT=1"], "args": ["-dx12"], "cmd": False}, rec
ml_user = "VKD3D_SHADER_MODEL=6_6 %command%"  # Manor Lords: the profile adds it too, the player had it: his
ml_set = DLL + " OPTI_SGSR3_STUDENT=1 VKD3D_SHADER_MODEL=6_6 VKD3D_FEATURE_LEVEL=12_2 %command% -dx12"
assert sgsr3.legacy_record(ml_user, ml_set, True)["env"] == [DLL, "OPTI_SGSR3_STUDENT=1", "VKD3D_FEATURE_LEVEL=12_2"]
assert sgsr3.legacy_record("%command% -dx12", DLL + " %command% -dx12", False)["args"] == []  # the player's -dx12
assert sgsr3.legacy_record("", DLL + " %command%", False)["cmd"] is True
assert sgsr3.legacy_record(None, DLL + " %command%", False) == {"env": [DLL], "args": [], "cmd": False}
assert sgsr3.ours_token(DLL) and sgsr3.ours_token("VELA_RCAS=0") and not sgsr3.ours_token("XOPTI_SGSRFG=1")
assert not sgsr3.ours_token("PROTON_LOG=1") and not sgsr3.ours_token("-dx12")

# the legacy VELA_FSR2_ALGO cleanup takes the token and one space, the rest of the spacing stays
assert sgsr3.drop_legacy_env('A="x  y"  VELA_FSR2_ALGO=ai  %command%') == 'A="x  y"   %command%'  # the token and one space
print("test_launchopts: ok")
