"""The Vela icon, drawn from geometry (no image generation): two sails forming a V on a rounded navy square.
Writes icon.svg, icon.png (256x256) and icon_64.png. python make_icon.py <outdir>"""
import sys
from PIL import Image, ImageDraw
OUT = sys.argv[1]
BG, SAIL1, SAIL2, SEA = "#16324a", "#f4f1ea", "#6fb7e6", "#6fb7e6"
LEFT = [(0.24, 0.22), (0.47, 0.30), (0.50, 0.74)]    # left sail: a tall triangle leaning to the mast foot
RIGHT = [(0.80, 0.20), (0.53, 0.30), (0.50, 0.74)]   # right sail: the other arm of the V
WAVE_Y, WAVE_X = 0.82, (0.30, 0.70)
svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">',
       f'<rect x="0" y="0" width="256" height="256" rx="56" fill="{BG}"/>']
for pts, col in ((LEFT, SAIL1), (RIGHT, SAIL2)):
    svg.append(f'<polygon points="{" ".join(f"{x*256:.1f},{y*256:.1f}" for x, y in pts)}" fill="{col}"/>')
svg.append(f'<line x1="{WAVE_X[0]*256:.1f}" y1="{WAVE_Y*256:.1f}" x2="{WAVE_X[1]*256:.1f}" y2="{WAVE_Y*256:.1f}" '
           f'stroke="{SEA}" stroke-width="10" stroke-linecap="round"/></svg>')
open(f"{OUT}/icon.svg", "w", newline="\n").write("\n".join(svg) + "\n")
S = 2048
im = Image.new("RGBA", (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
d.rounded_rectangle((0, 0, S - 1, S - 1), radius=int(S * 56 / 256), fill=BG)
for pts, col in ((LEFT, SAIL1), (RIGHT, SAIL2)):
    d.polygon([(x * S, y * S) for x, y in pts], fill=col)
w = int(S * 10 / 256)
d.line([(WAVE_X[0] * S, WAVE_Y * S), (WAVE_X[1] * S, WAVE_Y * S)], fill=SEA, width=w)
for x in WAVE_X:
    d.ellipse((x * S - w / 2, WAVE_Y * S - w / 2, x * S + w / 2, WAVE_Y * S + w / 2), fill=SEA)
im.resize((256, 256), Image.LANCZOS).save(f"{OUT}/icon.png", optimize=True)
im.resize((64, 64), Image.LANCZOS).save(f"{OUT}/icon_64.png", optimize=True)
print("ok")
