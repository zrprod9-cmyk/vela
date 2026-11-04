"""0.8.20: a run in DirectX 11 explains itself (panel hint, report flag). The fixture follows a game in DX11 mode: DLSS
probed on D3D12 at start, then a DX11 swapchain, no upscaler created."""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import presets  # noqa: E402
import report  # noqa: E402
import sgsr3  # noqa: E402

LOG = os.path.join(HERE, "fixtures", "dx11_run_optiscaler.log")
s = sgsr3.log_summary(LOG)
assert s["api"] == "dx11" and not s["sgsr3"] and not s["out_format"], s

p = {"kind": "optiscaler", "model": "fsr4"}
assert presets.hint(p, {"log": s}) == {"id": "dx11_run"}
assert presets.hint(p, {"log": s, "dx12_choice": True}) == {"id": "dx12_choice"}, "Steam's DX12 entry first"
assert presets.hint({"kind": "static", "model": "fsr4"}, {"log": s}) != {"id": "dx11_run"}

flags = report.assess(open(LOG, encoding="utf-8").read(), s)
assert flags["dx11"] and not flags["out_format"], flags

# a DX12 run with the upscaler: no DX11 verdict even with a DX11 device created on the way (vkd3d / launcher probes)
d = tempfile.mkdtemp()
dx12 = os.path.join(d, "OptiScaler.log")
with open(dx12, "w", encoding="utf-8") as f:
    f.write("[I] hkD3D11CreateDevice Device captured\n[I] NVSDK_NGX_D3D12_Init_Ext AppId: 1\n"
            "[I] Initializing SGSR3 1280x720 -> 1920x1080\n")
s12 = sgsr3.log_summary(dx12)
assert s12["api"] is None and presets.hint(p, {"log": s12, "npu": {}}) != {"id": "dx11_run"}, s12
assert sgsr3.run_api("creating Dx11 swapchain", True) is None, "an upscaler was created: not a DX11 verdict"

# FSR 4 refused the output format (ffx error 6): its own hint
with open(dx12, "a", encoding="utf-8") as f:
    f.write("[E] Vela VK: ffxDispatch failed (6)\n")
s6 = sgsr3.log_summary(dx12)
assert s6["out_format"] and presets.hint(p, {"log": s6, "npu": {"layer_found": True, "service": {"installed": True,
                                                                                                  "graphs": ["x"]}}}) \
    in ({"id": "out_format"}, {"id": "npu_service"}), s6
assert presets.hint({"kind": "optiscaler", "model": "sgsr2"}, {"log": s6}) is None, "SGSR 2 picked: no FSR 4 hint"
print("test_dx11 OK")
