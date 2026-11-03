"""Label the textures the recon DLL dumped from the FSR 2 passes and assemble one bench-format frame.

    python -I tools/label_dump.py --dump <captures>/fsr2dump --out <captures>/fsr2frame
        [--render 960x540] [--display 1920x1080] [--pick color=p0001_t4,depth=...]

The recon writes, per FSR 2 pass, p<pso>.json + p<pso>_<t|u><reg>.bin (each texture once). Roles are chosen by
what the game hands FSR 2 (no names needed): colour = render-size 3/4-channel colour format that FSR only reads;
depth = render-size 32/24-bit depth-like format; motion = 2-channel float (render or display size) that FSR only
reads; output = display-size colour UAV of the accumulate pass that no FSR pass reads back (internal history is
read the next frame). Prints the table so the choice can be checked; --pick overrides any role.
Writes <out>/f000000.json + f000000_{color,depth,mv,output}.bin for tools/show_inputs.py.
"""
import argparse
import glob
import json
import os
import shutil

COLOR = {2, 10, 11, 24, 26, 28, 29, 87, 88, 91}
DEPTH = {39, 40, 41, 44, 45, 46, 55}
MV2 = {16, 34, 35}
UAV_FLAG = 0x4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--render", default="")
    ap.add_argument("--display", default="")
    ap.add_argument("--pick", default="")
    a = ap.parse_args()

    tex, acc_pso = [], None
    for js in sorted(glob.glob(os.path.join(a.dump, "*p[0-9]*.json"))):
        m = json.load(open(js))
        if m.get("accumulate"):
            acc_pso = m["pso"]
        for reg, info in m["images"].items():
            tex.append({"pso": m["pso"], "acc": bool(m.get("accumulate")), "reg": reg,
                        "path": js[:-5] + f"_{reg}.bin", "w": info["size"][0], "h": info["size"][1],
                        "fmt": info["dxgi"], "flags": info.get("flags", 0), "info": info,
                        "key": f"p{m['pso']:04d}_{reg}"})
    if not tex:
        raise SystemExit("no textures in dump")

    sizes = sorted({(t["w"], t["h"]) for t in tex}, key=lambda s: s[0] * s[1])
    disp = tuple(map(int, a.display.split("x"))) if a.display else sizes[-1]
    rend = tuple(map(int, a.render.split("x"))) if a.render else next(
        (s for s in sizes if s[0] * 2 <= disp[0] * 1.01 or s[0] < disp[0]), sizes[0])

    def readonly(t):  # FSR only reads it: an SRV register and the resource has no UAV flag
        return t["reg"].startswith("t") or not (t["flags"] & UAV_FLAG)

    roles = {}
    for t in tex:
        wh = (t["w"], t["h"])
        if "color" not in roles and wh == rend and t["fmt"] in COLOR and readonly(t):
            roles["color"] = t
        if "depth" not in roles and wh == rend and (t["fmt"] in DEPTH or t["flags"] & 0x2) and readonly(t):
            roles["depth"] = t
        if "mv" not in roles and wh in (rend, disp) and t["fmt"] in MV2 and readonly(t):
            roles["mv"] = t
    outs = [t for t in tex if t["acc"] and (t["w"], t["h"]) == disp and t["fmt"] in COLOR and t["reg"].startswith("u")]
    if outs:
        roles["output"] = outs[0]
    for kv in filter(None, a.pick.split(",")):
        k, v = kv.split("=")
        roles[k] = next(t for t in tex if t["key"] == v)

    print(f"render {rend[0]}x{rend[1]}  display {disp[0]}x{disp[1]}  accumulate pso#{acc_pso}")
    print(f"{'key':14} {'size':>10} {'fmt':>4} {'flags':>6}  role")
    for t in tex:
        role = ",".join(k for k, v in roles.items() if v is t)
        print(f"{t['key']:14} {t['w']:>4}x{t['h']:<5} {t['fmt']:>4} 0x{t['flags']:<4x}  {role}")

    os.makedirs(a.out, exist_ok=True)
    imgs = {}
    for role, t in roles.items():
        shutil.copyfile(t["path"], os.path.join(a.out, f"f000000_{role}.bin"))
        info = dict(t["info"])
        info["texel_bytes"] = info.get("row_bytes", t["w"] * info["texel_bytes"]) // t["w"]
        imgs[role] = info
    meta = {"frame": 0, "render": list(rend), "display": list(disp), "images": imgs,
            "sources": {k: v["key"] for k, v in roles.items()}}
    json.dump(meta, open(os.path.join(a.out, "f000000.json"), "w"), indent=2)
    print(f"wrote {a.out}/f000000.json with {sorted(imgs)}")


if __name__ == "__main__":
    main()
