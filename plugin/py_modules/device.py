"""0.8.16: which Snapdragon the console has, and what Vela can run on it.

Read from the kernel, never from a name the user can change (values as the Retroid Pocket 6 / SM8550 console reports them):
- /proc/device-tree/compatible: NUL-separated, e.g. "retroidpocket,rp6\0qcom,qcs8550\0qcom,sm8550\0";
- /sys/devices/soc0/soc_id: the socinfo number (Linux include/dt-bindings/arm/qcom,ids.h: SM8550 519, QCS8550 603,
  SM8750 618), machine: "QCS8550" / "SM8750";
- /proc/device-tree/soc@0/gpu@*/compatible: "qcom,adreno-43050a01" (Adreno 740), "qcom,adreno-44050001" (Adreno 830,
  Linux arch/arm64/boot/dts/qcom/sm8750.dtsi).

Policy:
- SM8550 (Adreno 740, Hexagon v73): everything as in the 0.8.16 release -- the NPU graphs are built for v73, FSR 4 Fast is tuned here.
- SM8750 (Adreno 830, Hexagon v79): the NPU only when a v79 graph set is on the system or in the plugin
  (V79_DIRS: a "v79" folder with at least one *.bin); without it FSR 4 runs on the GPU, FSR 3 and SGSR 2 stay, FSR 4 Fast
  (tuned on the A740 + v73 NPU) is not offered, and the panel says so.
- any other chip (e.g. the SM8250 / Adreno 650 of the Retroid Pocket 5 has no HTP, and FSR 4 on its
  GPU alone is an estimated 50-80 ms per frame = unplayable): no NPU, no FSR 4, no FSR 4 Fast; FSR 3 and SGSR 2 stay.
"""
import glob
import os

SM8550, SM8750 = "SM8550", "SM8750"
NO_FSR4 = "FSR 4 needs the Snapdragon 8 Gen 2 or newer NPU; use FSR 3 or SGSR 2"  # = en.json model.why.chip_no_fsr4
SOC_IDS = {519: SM8550, 603: SM8550, 618: SM8750}  # QCS8550 = the SM8550 die (IoT part number), same GPU and cDSP
COMPATIBLE = {"qcom,sm8550": SM8550, "qcom,qcs8550": SM8550, "qcom,sm8750": SM8750}
GPUS = {"43050a01": "Adreno 740", "44050000": "Adreno 830", "44050001": "Adreno 830",
        "650.2": "Adreno 650"}  # SM8250 (Linux arch/arm64/boot/dts/qcom/sm8250.dtsi "qcom,adreno-650.2")
NPU_ARCH = {SM8550: "v73", SM8750: "v79"}
GPU_OF = {SM8550: "Adreno 740", SM8750: "Adreno 830"}
# where a Hexagon v79 graph set may live: the system component's graph dir, or the plugin's assets
V79_DIRS = ("/usr/lib64/vela-npu/v79",)


def _read(path, binary=False):
    try:
        with open(path, "rb") as f:
            data = f.read(4096)
    except OSError:
        return None
    return data if binary else data.decode("utf-8", "replace").strip()


def detect(root="/"):
    """{"soc": "SM8550" | "SM8750" | None, "soc_id", "machine", "gpu", "gpu_chip", "npu_arch", "compatible"}"""
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


def policy(dev, v79_dirs=V79_DIRS):
    """What this chip may run. npu: the NPU path may be offered at all (the service checks still apply on top);
    fsr4: FSR 4 is offered at all (not on chips without a supported NPU); fsr4fast: FSR 4 Fast is offered; unavailable: English lines (logged, in game_state["device"]); note: one line, or None."""
    soc = dev.get("soc")
    if soc == SM8550:
        return {"npu": True, "npu_graphs": "v73", "fsr4": True, "fsr4fast": True, "unavailable": [], "note": None}
    if soc == SM8750:
        d = v79_graphs(v79_dirs)
        if d:
            return {"npu": True, "npu_graphs": "v79", "graph_dir": d, "fsr4": True, "fsr4fast": True, "unavailable": [], "note": None}
        un = ["NPU: no FSR 4 graphs for the Snapdragon 8 Elite (Hexagon v79)",
              "FSR 4 Fast: tuned for the Adreno 740 and the v73 NPU"]
        return {"npu": False, "npu_graphs": None, "fsr4": True, "fsr4fast": False, "unavailable": un,
                "note": "Snapdragon 8 Elite: FSR 4 on the GPU only (no NPU graphs for v79), FSR 4 Fast is not available"}
    name = dev.get("machine") or dev.get("gpu") or "unknown chip"
    return {"npu": False, "npu_graphs": None, "fsr4": False, "fsr4fast": False,
            "unavailable": [f"NPU: chip {name} is not supported", f"FSR 4 and FSR 4 Fast: {NO_FSR4}"],
            "note": f"Chip {name}: {NO_FSR4}"}


def state(dev, pol):
    """game_state["device"]"""
    return {"soc": dev.get("soc") or "unknown", "gpu": dev.get("gpu"), "npu_arch": dev.get("npu_arch"),
            "npu": pol["npu"], "npu_graphs": pol["npu_graphs"], "fsr4": pol.get("fsr4", True), "fsr4fast": pol["fsr4fast"],
            "unavailable": list(pol["unavailable"]), "note": pol["note"]}
