#!/usr/bin/env python3
"""Generate icon.gif, the 32x32 16-colour icon of the .nds file: the record
and the moonlit cover of the player screen, in the theme's colours.

    python3 tools/gen_icon.py icon.gif [preview.png]
"""
import math
import sys
from PIL import Image

OUT = sys.argv[1]

def c5(r, g, b):
    f = lambda v: (v << 3) | (v >> 2)
    return (f(r), f(g), f(b))

# the default theme (theme_default in arm9/source/palette.c): background, text, accent
BG, TEXT, ACC = (4, 0, 5), (31, 29, 31), (30, 17, 31)


def toward(a, b, k):
    """k/256 of the way from a to b, like palette.c."""
    return tuple((x * (256 - k) + y * k + 128) >> 8 for x, y in zip(a, b))


P = {
    # the record follows the theme (mixes fitted to the hand-picked icon of the first theme)
    "bg": c5(*BG), "edge": c5(*ACC), "vinyl": c5(*toward(BG, TEXT, 19)), "groove": c5(*toward(BG, TEXT, 52)),
    "rim": c5(*toward(BG, TEXT, 92)), "label": c5(*toward(BG, ACC, 138)),
    # the moonlit cover is the default cover art, the same in every theme
    "sky0": c5(5, 3, 11), "sky1": c5(10, 5, 15),
    "sky2": c5(19, 10, 18), "moon": c5(31, 28, 27), "mount": c5(3, 2, 6), "wl": c5(31, 24, 25),
    "wd": c5(16, 8, 16), "beach": c5(13, 7, 13), "shore": c5(21, 13, 19),
}
names = list(P)
img = Image.new("RGB", (32, 32))
alpha = [[1] * 32 for _ in range(32)]

def inside_round(x, y, r=4.5):
    if not (0 <= x < 32 and 0 <= y < 32):
        return False
    cx = r if x < r else (31 - r if x > 31 - r else x)
    cy = r if y < r else (31 - r if y > 31 - r else y)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r + 0.5

def edge_of_round(x, y):
    if not inside_round(x, y):
        return False
    return any(not inside_round(x + dx, y + dy) or not (0 <= x + dx < 32 and 0 <= y + dy < 32)
               for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))

def mountain(x, y):
    # two peaks over the water line (y 17)
    return y <= 17 and (y >= 14 + abs(x - 17) * 0.75 or y >= 13 + abs(x - 21) * 0.8) and x <= 26

for y in range(32):
    for x in range(32):
        if not inside_round(x, y):
            alpha[y][x] = 0
            img.putpixel((x, y), (0, 0, 0))
            continue
        col = P["bg"]
        d = math.hypot(x - 12.5, y - 15.5)
        if d <= 10.6:
            col = P["rim"] if d > 9.6 else (P["label"] if d <= 3.2 else (P["groove"] if abs(d - 7.4) < 0.5 or abs(d - 5.2) < 0.45 else P["vinyl"]))
        if 13 <= x <= 29 and 8 <= y <= 24:
            if y <= 10: col = P["sky0"]
            elif y <= 13: col = P["sky1"]
            else: col = P["sky2"]
            if math.hypot(x - 25, y - 11.5) <= 2.4: col = P["moon"]
            if mountain(x, y): col = P["mount"]
            if 18 <= y <= 21: col = P["wl"] if y % 2 == 0 else P["wd"]
            if y == 22: col = P["shore"]
            if y >= 23: col = P["beach"]
        if edge_of_round(x, y):
            col = P["edge"]
        img.putpixel((x, y), col)

pal = [P[n] for n in names]
out = Image.new("P", (32, 32))
flat = [255, 0, 255]
for c in pal: flat += list(c)
out.putpalette(flat + [0] * (768 - len(flat)))
for y in range(32):
    for x in range(32):
        out.putpixel((x, y), 0 if not alpha[y][x] else 1 + pal.index(img.getpixel((x, y))))
out.save(OUT, transparency=0)
prev = Image.new("RGB", (32 * 2 + 4, 32), (200, 200, 200))
out.info["transparency"] = 0
rgba = out.convert("RGBA")
dark = Image.new("RGB", (32, 32), (40, 40, 40)); dark.paste(rgba, mask=rgba)
light = Image.new("RGB", (32, 32), (230, 230, 230)); light.paste(rgba, mask=rgba)
prev.paste(dark, (0, 0)); prev.paste(light, (36, 0))
if len(sys.argv) > 2:
    prev.resize((prev.width * 8, 256), Image.NEAREST).save(sys.argv[2])
