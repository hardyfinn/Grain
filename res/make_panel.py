#!/usr/bin/env python3
# Draws res/Pollen.svg. VCV Rack can't draw SVG <text>, so every label is
# turned into outline paths with fontTools. Positions are in mm and must match
# src/Pollen.cpp.
#   pip install fonttools
#   python3 res/make_panel.py
import math
import os
import random
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_BOLD = os.environ.get("PANEL_FONT_BOLD", "/usr/share/fonts/opentype/inter/Inter-Bold.otf")
FONT_MED = os.environ.get("PANEL_FONT_MED", "/usr/share/fonts/opentype/inter/Inter-Medium.otf")

W, H = 182.88, 128.5  # 36 HP

BG = "#2B3524"
BG_DARK = "#232C1D"
INK = "#F2ECD8"
SOFT = "#B3B79C"
HONEY = "#F5C52A"
SCENE_A = "#7FC8F0"
SCENE_B = "#F08E6A"
OUTBOX = "#171C13"

fonts = {}


def text(s, x, y, size, font=FONT_MED, fill=INK, anchor="middle", spacing=0.04):
    """Return an SVG path for string s; (x, y) is the baseline point in mm."""
    if font not in fonts:
        f = TTFont(font)
        fonts[font] = (f, f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm)
    f, gs, cmap, upm = fonts[font]
    scale = size / upm
    track = spacing * upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width + track for n in names) - track
    if anchor == "middle":
        x0 = x - width * scale / 2
    elif anchor == "end":
        x0 = x - width * scale
    else:
        x0 = x
    pen = SVGPathPen(gs)
    cx = 0.0
    for n in names:
        tp = TransformPen(pen, (scale, 0, 0, -scale, x0 + cx * scale, y))
        gs[n].draw(tp)
        cx += gs[n].width + track
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'


parts = []
add = parts.append


def dotted(x1, y1, x2, y2, color=HONEY, w=0.35, opacity=0.55):
    add(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{w}" '
        f'stroke-dasharray="0.01 1.4" stroke-linecap="round" opacity="{opacity}"/>')


def tick(cx, cy, value, label, r0=6.6, r1=7.7, fill=INK, size=1.6):
    """A mark at a knob position (value 0..1 over Rack's -150..+150 degree sweep)."""
    a = math.radians(-150 + 300 * value)
    sx, sy = math.sin(a), -math.cos(a)
    add(f'<line x1="{cx + r0 * sx:.3f}" y1="{cy + r0 * sy:.3f}" x2="{cx + r1 * sx:.3f}" '
        f'y2="{cy + r1 * sy:.3f}" stroke="{fill}" stroke-width="0.35"/>')
    if label:
        rl = r1 + 1.5
        add(text(label, cx + rl * sx, cy + rl * sy + size * 0.36, size, FONT_BOLD, fill))


add(f'<rect width="{W}" height="{H}" fill="{BG}"/>')
# pollen specks (deterministic)
rnd = random.Random(7)
for _ in range(260):
    x, y = rnd.uniform(1, W - 1), rnd.uniform(1, H - 1)
    if 4 < x < 179 and 12 < y < 59:
        continue  # not behind the display
    r = rnd.choice([0.15, 0.2, 0.25, 0.35])
    add(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{r}" fill="{HONEY}" opacity="{rnd.uniform(0.08, 0.22):.2f}"/>')

# title
add(text("POLLEN", 6.0, 10.4, 4.2, FONT_BOLD, HONEY, anchor="start", spacing=0.14))
add(text("granular sampler  ·  4 sounds  ·  2 scenes", W - 22.0, 10.1, 2.1, FONT_MED, SOFT, anchor="end"))

# display window
add(f'<rect x="4.1" y="12.6" width="174.7" height="45.8" rx="1.6" fill="#0C0E0A"/>')

# section dividers
for x in (43.0, 93.5, 161.5):
    dotted(x, 60.5, x, 108.5)
dotted(3.0, 109.8, W - 3.0, 109.8)

# section headers
hy = 61.6
add(text("SOUND", 22.0, hy, 2.2, FONT_BOLD, HONEY, spacing=0.12))
add(text("WHERE IN THE AUDIO", 68.0, hy, 2.2, FONT_BOLD, HONEY, spacing=0.12))
add(text("GRAIN", 127.0, hy, 2.2, FONT_BOLD, HONEY, spacing=0.12))
add(text("MOD DEPTH", 172.2, hy, 2.0, FONT_BOLD, HONEY, spacing=0.08))

# SOUND section
for i, x in enumerate([8.0, 17.3, 26.6, 35.9]):
    add(text(str(i + 1), x, 76.4, 2.4, FONT_BOLD, INK))
for x, lab, hint in [(8.0, "LATCH", "stay on"), (17.3, "SAMPLE", "record"), (26.6, "THRU", "hear in")]:
    add(text(lab, x, 89.0, 2.0, FONT_BOLD, "#F2A0A0" if lab == "SAMPLE" else INK, spacing=0.05))
    add(text(hint, x, 91.2, 1.5, FONT_MED, SOFT))
add(text("SCENE A", 8.0, 104.0, 1.9, FONT_BOLD, SCENE_A, spacing=0.04))
add(text("SCENE B", 19.0, 104.0, 1.9, FONT_BOLD, SCENE_B, spacing=0.04))
add(text("knobs edit", 13.5, 106.3, 1.5, FONT_MED, SOFT))
add(text("MORPH", 37.0, 84.6, 2.0, FONT_BOLD, INK, spacing=0.05))
add(text("B", 40.6, 89.6, 1.9, FONT_BOLD, SCENE_B, anchor="start"))
add(text("A", 40.6, 108.4, 1.9, FONT_BOLD, SCENE_A, anchor="start"))

# knobs
row1, row2 = 73.0, 99.0
knobs = [
    (52.0, row1, "START", "window start"),
    (68.0, row1, "WRAP", "window width"),
    (84.0, row1, "SCAN", "move through"),
    (103.0, row1, "RATE", "how often"),
    (127.0, row1, "SIZE", "how long"),
    (151.0, row1, "PITCH", "how fast"),
    (52.0, row2, "SHAPE", "envelope"),
    (84.0, row2, "LEVEL", "grain volume"),
    (103.0, row2, "TONE", "LP  ·  HP"),
    (127.0, row2, "BLUR", "reverb"),
]
for x, y, lab, hint in knobs:
    add(text(lab, x, y - (7.9 if y == row1 else 10.2), 2.3, FONT_BOLD, INK, spacing=0.06))
    add(text(hint, x, y + 9.6, 1.55, FONT_MED, SOFT))
# small knob: morph CV amount
add(text("MORPH CV", 151.0, row2 - 5.6, 1.9, FONT_BOLD, INK, spacing=0.05))
add(text("amount", 151.0, row2 + 6.6, 1.55, FONT_MED, SOFT))
# buttons in the knob area
add(text("REVERSE", 68.0, row2 - 6.2, 2.0, FONT_BOLD, "#F2A0A0", spacing=0.05))
add(text("this scene", 68.0, row2 + 7.4, 1.5, FONT_MED, SOFT))
for x, lab in [(115.0, "RATE–SIZE"), (139.0, "SIZE–PITCH")]:
    add(text("LINK", x, row1 - 6.2, 1.9, FONT_BOLD, HONEY, spacing=0.08))
    add(text(lab, x, row1 + 7.4, 1.4, FONT_MED, SOFT))
add(text("grain", 137.6, 61.5, 1.4, FONT_MED, SOFT, anchor="start"))

# knob landmarks
tick(84.0, row1, 0.5, "", fill=HONEY)            # SCAN still at noon
tick(84.0, row1, 0.0, "←", size=1.8)
tick(84.0, row1, 1.0, "→", size=1.8)
tick(103.0, row1, 0.0, "1/trig", size=1.25)      # RATE fully down
tick(151.0, row1, 0.5, "", fill=HONEY)          # PITCH original
tick(151.0, row1, 0.25, "-1", size=1.4)
tick(151.0, row1, 0.75, "+1", size=1.4)
tick(151.0, row1, 0.0, "-2", size=1.4)
tick(151.0, row1, 1.0, "+2", size=1.4)
tick(103.0, row2, 0.5, "", fill=HONEY)
for i, sh in enumerate(["bell", "sinc", "tri", "decay", "ramp", "tukey", "sqr"]):
    tick(52.0, row2, i / 6.0, sh, size=1.15)

# MOD section
for i, lab in enumerate(["X", "Y", "Z", "RAND"]):
    y = 69.0 + 9.5 * i
    add(text(lab, 166.4, y + 0.9, 2.3 if lab != "RAND" else 1.8, FONT_BOLD, INK, anchor="end"))
add(text("press, then", 172.2, 105.4, 1.45, FONT_MED, SOFT))
add(text("turn knobs", 172.2, 107.4, 1.45, FONT_MED, SOFT))

# jacks
jy = 118.0
add(f'<rect x="122.6" y="110.9" width="47.4" height="15.6" rx="1.2" fill="{OUTBOX}"/>')
ins = ["IN L", "IN R", "GATE", "NEXT", "SELECT", "SYNC", "MORPH", "X", "Y", "Z"]
for i, lab in enumerate(ins):
    add(text(lab, 9.0 + 11.0 * i, 112.9, 1.9, FONT_BOLD, INK, spacing=0.03))
add(text("audio in", 14.5, 125.3, 1.45, FONT_MED, SOFT))
add(text("play  ·  pick  ·  clock", 42.0, 125.3, 1.45, FONT_MED, SOFT))
add(text("CV  ± 5 V", 86.0, 125.3, 1.45, FONT_MED, SOFT))
for i, lab in enumerate(["OUT L", "OUT R", "A", "B"]):
    add(text(lab, 129.0 + 11.5 * i, 112.9, 1.9, FONT_BOLD, INK, spacing=0.03))
add(text("audio out", 134.75, 125.3, 1.45, FONT_MED, SOFT))
add(text("CV out (menu)", 157.75, 125.3, 1.45, FONT_MED, SOFT))

svg = (
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" '
    f'viewBox="0 0 {W} {H}">\n' + "\n".join(parts) + "\n</svg>\n"
)
with open(os.path.join(HERE, "Pollen.svg"), "w") as fh:
    fh.write(svg)
print("wrote Pollen.svg")
