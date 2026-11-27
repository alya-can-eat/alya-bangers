#!/usr/bin/env python3
"""Make the README's pictures from the screen test's output: both DS screens,
twice the size (pixels kept sharp), rounded, on a transparent background.

    python3 tests/demo/make_demo_card.py /tmp/demo
    cd tests/host && ./screens /tmp/demo/card/music /tmp/demo/shots demo
    python3 tools/gen_readme_images.py /tmp/demo/shots docs/img

The demo card is a made-up library (no real artists or titles). Stills and
films (rd_*: both screens, 30 frames per second, like the DS draws each
screen) come from the demo mode; the films become animated WebP (needs
ffmpeg with libwebp).
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

SRC, OUT = sys.argv[1], sys.argv[2]
SCALE = 2
GAP = 16 * SCALE        # between the screens (the hinge)
RADIUS = 6 * SCALE

# the demo mode's pictures (tests/host/screens.c, demo())
STILLS = ["browse", "player", "info", "queue", "eq", "two_artists", "search", "search_accents", "search_kana",
          "search_pinyin", "search_cyrillic", "search_filter", "search_list", "search_tags", "search_or_pick",
          "search_or", "settings", "sound", "presets",
          "german_search"]
FILMS = ["rd_browse", "rd_player", "rd_search", "rd_theme"]


def screens(path):
    """The test's 256 x 400 picture (top, hinge, bottom) as two screens."""
    im = Image.open(path).convert("RGB")
    return im.crop((0, 0, 256, 192)), im.crop((0, 208, 256, 400))


def mask():
    w, h = 256 * SCALE, 192 * SCALE
    m = Image.new("L", (w, h * 2 + GAP), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, w - 1, h - 1], RADIUS, fill=255)
    d.rounded_rectangle([0, h + GAP, w - 1, 2 * h + GAP - 1], RADIUS, fill=255)
    return m


MASK = mask()


def compose(top, bottom):
    w, h = 256 * SCALE, 192 * SCALE
    im = Image.new("RGB", (w, 2 * h + GAP), (0, 0, 0))
    im.paste(top.resize((w, h), Image.NEAREST), (0, 0))
    im.paste(bottom.resize((w, h), Image.NEAREST), (0, h + GAP))
    return im


os.makedirs(OUT, exist_ok=True)
for name in STILLS:
    im = compose(*screens(os.path.join(SRC, name + ".ppm"))).convert("RGBA")
    im.putalpha(MASK)
    im.save(os.path.join(OUT, name + ".png"), optimize=True)

for film in FILMS:
    # lossless animated WebP (ffmpeg): a fraction of a GIF's size, every colour kept
    files = sorted(f for f in os.listdir(SRC) if f.startswith(film + "_") and f.endswith(".ppm"))
    with tempfile.TemporaryDirectory() as tmp:
        for i, f in enumerate(files):
            im = compose(*screens(os.path.join(SRC, f))).convert("RGBA")
            im.putalpha(MASK)
            im.save(os.path.join(tmp, f"{i:04d}.png"))
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-framerate", "30", "-i", os.path.join(tmp, "%04d.png"),
                        "-c:v", "libwebp_anim", "-lossless", "1", "-loop", "0",
                        os.path.join(OUT, film[3:] + ".webp")], check=True)
print("made", len(STILLS), "pictures and", len(FILMS), "animations in", OUT)
