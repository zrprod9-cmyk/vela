"""0.8.16: which Snapdragon the console has, and what Vela can run on it.

Read from the kernel, never from a name the user can change (values as the Retroid Pocket 6 / SM8550 console reports them):
- /proc/device-tree/compatible: NUL-separated, e.g. "retroidpocket,rp6\0qcom,qcs8550\0qcom,sm8550\0";
- /sys/devices/soc0/soc_id: the socinfo number (Linux include/dt-bindings/arm/qcom,ids.h: SM8550 519, QCS8550 603,
  SM8650 557, SM8750 618, SM8850 660; SM8650P 577, from Qualcomm's RFC patch, not in mainline qcom,ids.h: the Lenovo Legion Tab Gen 3 /
  TB321FU reports it; SM8850P 661, CANOEP, observed on the Lenovo Legion Y700 Gen 5), machine: "QCS8550" / "SM8750" / "CANOEP";
- /proc/device-tree/soc@0/gpu@*/compatible: "qcom,adreno-43050a01" (Adreno 740), "qcom,adreno-43051401" (Adreno 750,
  Linux arch/arm64/boot/dts/qcom/sm8650.dtsi), "qcom,adreno-44050001" (Adreno 830, Linux arch/arm64/boot/dts/qcom/sm8750.dtsi),
  "qcom,adreno-44050a01" (Adreno 840, Linux arch/arm64/boot/dts/qcom/kaanapali.dtsi).

Policy:
- SM8550 (Adreno 740, Hexagon v73): everything as in the 0.8.16 release -- the NPU graphs are built for v73, FSR 4 Fast is tuned here.
- SM8650 (Adreno 750, Hexagon v75): as SM8550. The v73 graphs run unchanged on the QNN V75 runtime of the NPU part, with
  the same 17 fsr4npu-check output hashes as SM8550 (measured on a Lenovo Legion Tab Gen 3 / TB321FU).
- SM8750 (Adreno 830, Hexagon v79): the NPU only when a v79 graph set is on the system or in the plugin
  (V79_DIRS: a "v79" folder with at least one *.bin); without it FSR 4 runs on the GPU, FSR 3, DittoSR and SGSR 2 stay, FSR 4 Fast
  (tuned on the A740 + v73 NPU) is not offered, and the panel says so.
- SM8850 (Adreno 840, Hexagon v81): runs the v73 graphs unchanged on the QNN V81 runtime (same fsr4npu-check output hashes as SM8550, measured on a Lenovo Legion Y700 Gen 5 / SM8850P), and automatically switches to a native v81 graph set (V81_DIRS: a "v81" folder with at least one *.bin) when present for lower NPU execute latency.
- any other chip (e.g. the SM8250 / Adreno 650 of the Retroid Pocket 5 has no HTP, and FSR 4 on its
  GPU alone is an estimated 50-80 ms per frame = unplayable): no NPU, no FSR 4, no FSR 4 Fast; FSR 3, DittoSR and SGSR 2 stay.
"""
import glob
import os

SM8550, SM8650, SM8750, SM8850 = "SM8550", "SM8650", "SM8750", "SM8850"
NO_FSR4 = "FSR 4 needs the Snapdragon 8 Gen 2 or newer NPU; use FSR 3, DittoSR or SGSR 2"  # = en.json model.why.chip_no_fsr4
SOC_IDS = {519: SM8550, 603: SM8550, 557: SM8650, 577: SM8650, 618: SM8750, 660: SM8850, 661: SM8850}  # QCS8550 = SM8550 die; 577 = SM8650P; 660 = QCOM_ID_SM8850, 661 = SM8850P (CANOEP)
COMPATIBLE = {"qcom,sm8550": SM8550, "qcom,qcs8550": SM8550, "qcom,sm8650": SM8650, "qcom,sm8750": SM8750,
              "qcom,kaanapali": SM8850, "qcom,sm8850": SM8850, "qcom,canoe": SM8850, "qcom,canoep": SM8850}
GPUS = {"43050a01": "Adreno 740", "43051401": "Adreno 750", "44050000": "Adreno 830", "44050001": "Adreno 830",
        "44050a01": "Adreno 840", "650.2": "Adreno 650"}  # SM8250 (sm8250.dtsi "qcom,adreno-650.2"), SM8850 (kaanapali.dtsi "qcom,adreno-44050a01")
NPU_ARCH = {SM8550: "v73", SM8650: "v75", SM8750: "v79", SM8850: "v81"}
GPU_OF = {SM8550: "Adreno 740", SM8650: "Adreno 750", SM8750: "Adreno 830", SM8850: "Adreno 840"}
# where a Hexagon v79 / v81 graph set may live: the system component's graph dir, or the plugin's assets
V79_DIRS = ("/usr/lib64/vela-npu/v79",)
V81_DIRS = ("/usr/lib64/vela-npu/v81",)


def _read(path, binary=False):
    try:
        with open(path, "rb") as f:
            data = f.read(4096)
    except OSError:
        return None
    return data if binary else data.decode("utf-8", "replace").strip()


def detect(root="/"):
    """{"soc": "SM8550" | "SM8650" | "SM8750" | "SM8850" | None, "soc_id", "machine", "gpu", "gpu_chip", "npu_arch", "compatible"}"""
    j = lambda *p: os.path.join(root, *p)
    raw = _read(j("proc", "device-tree", "compatible"), binary=True) or b""
    compat = [c for c in raw.decode("utf-8", "replace").split("\0") if c]
    sid = _read(j("sys", "devices", "soc0", "soc_id"))
    try:
        sid = int(sid) if sid else None
    except ValueError:
        sid = None
    soc = next((COMPATIBLE[c] for c in compat if c in COMPATIBLE), None) or SOC_IDS.get(sid)
    chip = None
    for p in sorted(glob.glob(j("proc", "device-tree", "soc@0", "gpu@*", "compatible"))):
        for c in (_read(p, binary=True) or b"").decode("utf-8", "replace").split("\0"):
            if c.startswith("qcom,adreno-") and c != "qcom,adreno-smmu":
                chip = c[len("qcom,adreno-"):]
                break
        if chip:
            break
    gpu = GPUS.get(chip) if chip else GPU_OF.get(soc)
    if chip and not gpu:
        gpu = f"Adreno {chip}"
    return {"soc": soc, "soc_id": sid, "machine": _read(j("sys", "devices", "soc0", "machine")), "gpu": gpu,
            "gpu_chip": chip, "npu_arch": NPU_ARCH.get(soc), "compatible": compat}


def v79_graphs(dirs):
    """The first folder of dirs holding a v79 graph set (any *.bin), or None."""
    for d in dirs:
        if glob.glob(os.path.join(d, "*.bin")):
            return d
    return None


def policy(dev, v79_dirs=V79_DIRS, v81_dirs=None):
    """What this chip may run. npu: the NPU path may be offered at all (the service checks still apply on top);
    fsr4: FSR 4 is offered at all (not on chips without a supported NPU); fsr4fast: FSR 4 Fast is offered; unavailable: English lines (logged, in game_state["device"]); note: one line, or None."""
    if v81_dirs is None:
        v81_dirs = V81_DIRS if v79_dirs is V79_DIRS else ()
    soc = dev.get("soc")
    if soc in (SM8550, SM8650):  # SM8650: the v73 graphs on the V75 runtime
        # anf: the ANF families are HTP v73 graphs, measured on the SM8550; on the SM8650 they run on the V75 runtime like FSR 4's
        # (0.9.1: same rule), but ANF is offered there only after the NPU installer's check of the five families' output hashes
        # passed on this unit (anf_check: npu.read_anf_check, the file the installer writes)
        return {"npu": True, "npu_graphs": "v73", "fsr4": True, "fsr4fast": True, "anf": True, "anf_check": soc == SM8650,
                "unavailable": [], "note": None}
    if soc == SM8750:
        d = v79_graphs(v79_dirs)
        if d:
            return {"npu": True, "npu_graphs": "v79", "graph_dir": d, "fsr4": True, "fsr4fast": True, "unavailable": [], "note": None}
        un = ["NPU: no FSR 4 graphs for the Snapdragon 8 Elite (Hexagon v79)",
              "FSR 4 Fast: tuned for the Adreno 740 and the v73 NPU"]
        return {"npu": False, "npu_graphs": None, "fsr4": True, "fsr4fast": False, "unavailable": un,
                "note": "Snapdragon 8 Elite: FSR 4 on the GPU only (no NPU graphs for v79), FSR 4 Fast is not available"}
    if soc == SM8850:
        d = v79_graphs(v81_dirs)
        if d:
            return {"npu": True, "npu_graphs": "v81", "graph_dir": d, "fsr4": True, "fsr4fast": True, "unavailable": [], "note": None}
        return {"npu": True, "npu_graphs": "v73", "fsr4": True, "fsr4fast": True, "unavailable": [], "note": None}
    name = dev.get("machine") or dev.get("gpu") or "unknown chip"
    return {"npu": False, "npu_graphs": None, "fsr4": False, "fsr4fast": False,
            "unavailable": [f"NPU: chip {name} is not supported", f"FSR 4 and FSR 4 Fast: {NO_FSR4}"],
            "note": f"Chip {name}: {NO_FSR4}"}


def refresh(owner, v79_dirs=V79_DIRS, v81_dirs=None, log=None):
    """0.8.21: the NPU part can be installed or removed while the plugin runs. Without this the policy of the plugin's
    start stayed: on SM8750 installing/removing v79/ toggles the NPU and FSR 4 Fast until a reboot, and on SM8850
    installing/removing v81/ switches between v73 and v81 graphs live. owner has .dev, .chip and .npu.
    True when the policy changed."""
    pol = policy(owner.dev, v79_dirs, v81_dirs)
    if pol == owner.chip:
        return False
    owner.chip = pol
    owner.npu.chip = pol
    if log:
        log(f"Vela device: NPU part changed: {state(owner.dev, pol)}")
    return True


def state(dev, pol):
    """game_state["device"]"""
    return {"soc": dev.get("soc") or "unknown", "gpu": dev.get("gpu"), "npu_arch": dev.get("npu_arch"),
            "npu": pol["npu"], "npu_graphs": pol["npu_graphs"], "fsr4": pol.get("fsr4", True), "fsr4fast": pol["fsr4fast"], "anf": pol.get("anf", False),
            "unavailable": list(pol["unavailable"]), "note": pol["note"]}
