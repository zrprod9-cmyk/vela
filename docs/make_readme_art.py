"""README pictures for Vela, drawn with code from our own icon geometry and our own Bright Memory: Infinite
screenshots (no image generation). Writes banner.png (1280x400), social-preview.png (1280x640), compare.png and
how-it-works-light.svg / how-it-works-dark.svg.

    python make_readme_art.py <outdir> <cmp_dir>

<cmp_dir> holds the five 1920x1080 in-game screenshots of one spot 1_fsr4fast_up15.png, 2_fsr4fast.png,
3_fsr4.png, 4_fsr3.png, 5_sgsr2.png). The FPS labels are the ones of that capture *_en.png:
640x360 -> 1920x1080, Adreno 740 at 680 MHz, mean over 8 s from MangoHud, same spot, camera still).
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

OUT, CMP = sys.argv[1], sys.argv[2]
FONTS = "C:/Windows/Fonts"
def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)
REG, SEMI, BOLD = "segoeui.ttf", "seguisb.ttf", "segoeuib.ttf"

NAVY, NAVY_DEEP, SAIL1, SAIL2 = "#16324a", "#0c1c2b", "#f4f1ea", "#6fb7e6"
MUTED = "#9fb6c9"

# the five modes of the CMP capture, in the README's order (FSR 4 first); FPS as labelled by the capture
MODES = [
    ("3_fsr4.png", "FSR 4", "FSR 4.1.1 network", "39.6"),
    ("2_fsr4fast.png", "FSR 4 Fast", "same network, lighter GPU passes", "41.8"),
    ("1_fsr4fast_up15.png", "FSR 4 Fast 1.5\u00d7", "network to 720p + 1.5\u00d7 pass", "44.6"),
    ("4_fsr3.png", "FSR 3.1", "AMD FSR 3.1.4, no network", "40.1"),
    ("5_sgsr2.png", "SGSR 2", "Snapdragon GSR 2", "44.8"),
]
# 1:1 crops of the 1920x1080 frames (x0, y0, x1, y1): the tree's foliage and the lantern
TREE = (880, 130, 1240, 490)
LANTERN = (430, 660, 790, 1020)


def frame(name):
    return Image.open(os.path.join(CMP, name)).convert("RGB")


def icon(size):
    """The Vela icon (same geometry as make_icon.py), supersampled."""
    S = size * 8
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, S - 1, S - 1), radius=int(S * 56 / 256), fill=NAVY)
    for pts, col in (([(0.24, 0.22), (0.47, 0.30), (0.50, 0.74)], SAIL1), ([(0.80, 0.20), (0.53, 0.30), (0.50, 0.74)], SAIL2)):
        d.polygon([(x * S, y * S) for x, y in pts], fill=col)
    w = int(S * 10 / 256)
    d.line([(0.30 * S, 0.82 * S), (0.70 * S, 0.82 * S)], fill=SAIL2, width=w)
    for x in (0.30, 0.70):
        d.ellipse((x * S - w / 2, 0.82 * S - w / 2, x * S + w / 2, 0.82 * S + w / 2), fill=SAIL2)
    return im.resize((size, size), Image.LANCZOS)


def gradient(w, h):
    """Navy background, darker to the right, with a faint light band at the bottom (the 'sea' of the icon)."""
    top = Image.new("RGB", (w, h), NAVY)
    dark = Image.new("RGB", (w, h), NAVY_DEEP)
    mask = Image.linear_gradient("L").rotate(90, expand=True).resize((w, h))   # 255 left -> 0 right
    mask = Image.eval(mask, lambda v: 255 - v)
    return Image.composite(dark, top, mask)


def rounded(im, r):
    m = Image.new("L", im.size, 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, im.size[0] - 1, im.size[1] - 1), radius=r, fill=255)
    out = im.convert("RGBA"); out.putalpha(m)
    return out


def shadow(base, box, r, blur=14, alpha=110):
    sh = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle(box, radius=r, fill=(0, 0, 0, alpha))
    base.alpha_composite(sh.filter(ImageFilter.GaussianBlur(blur)))


def strip(base, x0, y0, tile_w, tile_h, gap, crops, labels, label_font):
    """Tiles of our real screenshots with a small caption under each."""
    d = ImageDraw.Draw(base)
    for i, (im, lab) in enumerate(zip(crops, labels)):
        x = x0 + i * (tile_w + gap)
        shadow(base, (x, y0 + 6, x + tile_w, y0 + tile_h + 6), 14)
        base.alpha_composite(rounded(im.resize((tile_w, tile_h), Image.LANCZOS), 14), (x, y0))
        d.multiline_text((x + 2, y0 + tile_h + 10), lab, font=label_font, fill=MUTED, spacing=4)


def title_block(base, x, y, ic, title_size, tag_size, sub_size, center=False):
    d = ImageDraw.Draw(base)
    tf, gf, sf = font(SEMI, title_size), font(SEMI, tag_size), font(REG, sub_size)
    title, tag = "Vela", "FSR 4 on your handheld\u2019s NPU"
    sub = "Decky plugin for armadaOS \u00b7 Snapdragon handhelds \u00b7 Hexagon NPU + Adreno GPU"
    icon_im = icon(ic)
    tw = d.textlength(title, font=tf)
    if center:
        W = base.size[0]
        row_w = ic + 28 + tw
        ix = int((W - row_w) / 2)
        base.alpha_composite(icon_im, (ix, y))
        d.text((ix + ic + 28, y + ic / 2), title, font=tf, fill=SAIL1, anchor="lm")
        y2 = y + ic + 26
        d.text((W / 2, y2), tag, font=gf, fill=SAIL2, anchor="mt")
        d.text((W / 2, y2 + tag_size + 22), sub, font=sf, fill=MUTED, anchor="mt")
    else:
        base.alpha_composite(icon_im, (x, y))
        d.text((x + ic + 26, y + ic / 2 + 2), title, font=tf, fill=SAIL1, anchor="lm")
        y2 = y + ic + 30
        d.text((x, y2), tag, font=gf, fill=SAIL2)
        d.text((x, y2 + tag_size + 18), sub, font=sf, fill=MUTED)


def banner():
    W, H = 1280, 400
    base = gradient(W, H).convert("RGBA")
    # no screenshots in the banner: the comparison has its own block on the page
    title_block(base, 0, 96, 112, 104, 34, 18, center=True)
    base.convert("RGB").save(os.path.join(OUT, "banner.png"), optimize=True)


def social():
    W, H = 1280, 640
    base = gradient(W, H).convert("RGBA")
    title_block(base, 0, 70, 120, 112, 40, 22, center=True)
    crops, labels = [], []
    for name, lab, _, fps in MODES:
        crops.append(frame(name).crop(TREE)); labels.append(f"{lab}\n{fps} FPS")
    tw, gap = 200, 22
    x0 = int((W - (5 * tw + 4 * gap)) / 2)
    strip(base, x0, 344, tw, 200, gap, crops, labels, font(REG, 17))
    base.convert("RGB").save(os.path.join(OUT, "social-preview.png"), optimize=True)


def compare():
    T, G, M = 360, 12, 32          # tile (1:1 crop), gap, margin
    W = M * 2 + 5 * T + 4 * G
    head, lab_h, foot = 112, 92, 78
    H = head + lab_h + 2 * T + G + foot
    BG, FG, SUB, GOOD = (17, 22, 28), (238, 240, 242), (150, 162, 175), (125, 214, 145)
    base = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(base)
    d.text((M, 26), "Bright Memory: Infinite on Retroid Pocket 6 \u2014 five upscalers, same spot", font=font(SEMI, 34), fill=FG)
    d.text((M, 72), "Render 640\u00d7360 \u2192 output 1920\u00d71080 (3\u00d7) \u00b7 Adreno 740 at 680 MHz \u00b7 FSR 4 network on "
           "the Hexagon NPU \u00b7 FPS = mean over 8 s (MangoHud), camera still", font=font(REG, 20), fill=SUB)
    best = max(float(m[3]) for m in MODES)
    for i, (name, lab, note, fps) in enumerate(MODES):
        x = M + i * (T + G); y = head
        d.text((x, y + 4), lab, font=font(SEMI, 24), fill=FG)
        d.text((x, y + 38), note, font=font(REG, 17), fill=SUB)
        fw = d.textlength(fps, font=font(BOLD, 30))
        d.text((x + T - fw - 44, y + 2), fps, font=font(BOLD, 30), fill=GOOD if float(fps) >= best - 0.3 else FG)
        d.text((x + T - 38, y + 14), "FPS", font=font(REG, 16), fill=SUB)
        im = frame(name)
        base.paste(im.crop(TREE), (x, head + lab_h))
        base.paste(im.crop(LANTERN), (x, head + lab_h + T + G))
    y = head + lab_h + 2 * T + G + 18
    d.text((M, y), "Crops at 1:1 from in-game screenshots (click for full size). The lantern flame is animated: its brightness "
           "differs between screenshots, not between upscalers.", font=font(REG, 18), fill=SUB)
    d.text((M, y + 28), "FSR 4 Fast = the same network with lighter GPU passes; 1.5\u00d7 mode = network to 1280\u00d7720, then "
           "one 1.5\u00d7 pass. Captured 2026-11-02.", font=font(REG, 18), fill=SUB)
    base.save(os.path.join(OUT, "compare.png"), optimize=True)


def diagram(theme):
    """How a frame goes through Vela (FSR 4 model, NPU part installed). Exact mode: the GPU waits for the NPU in the same
    frame (plugin presets.py: npu=2, npuexact=1)."""
    if theme == "light":
        bg, fg, sub, line, box, gpu, npu, acc = "#ffffff", "#1f2328", "#59636e", "#8c959f", "#f6f8fa", "#e8f1fb", "#eaf6ee", "#0969da"
        gpu_s, npu_s = "#54aeff", "#4ac26b"
    else:
        bg, fg, sub, line, box, gpu, npu, acc = "#0d1117", "#e6edf3", "#9198a1", "#6e7681", "#161b22", "#0f2540", "#0f2d1c", "#4493f8"
        gpu_s, npu_s = "#1f6feb", "#2ea043"
    W, H = 960, 470
    ff = "font-family=\"-apple-system,BlinkMacSystemFont,'Segoe UI','Noto Sans',Helvetica,Arial,sans-serif\""
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" role="img" '
         f'aria-label="How Vela upscales one frame">',
         '<defs><marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
         f'<path d="M0,0 L10,5 L0,10 z" fill="{line}"/></marker></defs>',
         f'<rect width="{W}" height="{H}" rx="12" fill="{bg}"/>']

    def rect(x, y, w, h, fill, stroke, title, lines, dash=False):
        da = ' stroke-dasharray="6 5"' if dash else ""
        s.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="10" fill="{fill}" stroke="{stroke}" stroke-width="1.5"{da}/>')
        s.append(f'<text x="{x + 16}" y="{y + 28}" {ff} font-size="16" font-weight="600" fill="{fg}">{title}</text>')
        for i, t in enumerate(lines):
            s.append(f'<text x="{x + 16}" y="{y + 52 + i * 20}" {ff} font-size="13.5" fill="{sub}">{t}</text>')

    def arrow(x1, y1, x2, y2, label=None, lx=None, ly=None, anchor="middle", both=False):
        st = ' marker-start="url(#a)"' if both else ""
        s.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{line}" stroke-width="1.6" marker-end="url(#a)"{st}/>')
        if label:
            s.append(f'<text x="{lx}" y="{ly}" {ff} font-size="12.5" fill="{sub}" text-anchor="{anchor}">{label}</text>')

    # row 1: game -> fork -> provider
    rect(20, 24, 250, 112, box, line, "Game (DX12, Proton)", ["asks for DLSS, FSR 2/3 or XeSS", "gives colour, depth,", "motion vectors, jitter"])
    rect(340, 24, 270, 112, box, line, "Vela\u2019s OptiScaler fork", ["installed as dxgi.dll (ARM64EC)", "takes the upscaler call,", "picks the model from the panel"])
    rect(680, 24, 260, 112, box, line, "FSR 4 provider (Vulkan)", ["FSR 4.1.1 network, INT8", "FSR 4 Fast: lighter GPU passes", "no NPU part \u2192 all on the GPU"])
    arrow(270, 80, 338, 80)
    arrow(610, 80, 678, 80)
    # row 2: GPU part and NPU part of one frame
    s.append(f'<text x="20" y="182" {ff} font-size="13" font-weight="600" fill="{acc}">ONE FRAME, FSR 4 WITH THE VELA NPU PART</text>')
    rect(20, 196, 280, 128, gpu, gpu_s, "Adreno 740 GPU", ["pre-pass: inputs \u2192 network tensors", "output passes: network result", "\u2192 the upscaled frame"])
    rect(375, 196, 245, 128, box, line, "Vela NPU layer + daemon", ["shares a dma-buf with the", "daemon; the GPU waits for it", "in the same frame; NPU fails → GPU"])
    rect(690, 196, 250, 128, npu, npu_s, "Hexagon NPU (HTP v73)", ["the FSR 4 network as a compiled", "QNN graph, INT8 (via FastRPC)", "Control 1080p: 8.3 ms per frame"])
    arrow(300, 248, 373, 248, "tensors", 337, 240)
    arrow(620, 248, 688, 248)
    arrow(688, 286, 622, 286)
    arrow(373, 286, 300, 286, "result", 337, 306)
    arrow(810, 136, 810, 160)
    s.append(f'<line x1="810" y1="160" x2="160" y2="160" stroke="{line}" stroke-width="1.6"/>')
    arrow(160, 160, 160, 194)
    # row 3: output
    rect(20, 360, 920, 86, box, line, "Output", ["the game\u2019s 1920\u00d71080 (or 1280\u00d7720) frame, upscaled; the game\u2019s UI is drawn on top as usual.",
                                                 "FSR 3 and SGSR 2 run on the GPU only. Resident Evil 4: a small interceptor of the game\u2019s built-in FSR 2 replaces the fork."])
    arrow(160, 324, 160, 358)
    s.append("</svg>")
    open(os.path.join(OUT, f"how-it-works-{theme}.svg"), "w", encoding="utf-8", newline="\n").write("\n".join(s) + "\n")


if __name__ == "__main__":
    banner(); social(); compare(); diagram("light"); diagram("dark")
    print("ok")
