#!/usr/bin/env python3
"""A made-up music library for the README's pictures: invented artists,
albums and titles, synthetic audio, covers drawn here (most songs have none
and show the player's built-in picture).

    python3 tests/demo/make_demo_card.py /tmp/demo        # -> /tmp/demo/card/music
    cd tests/host && ./screens /tmp/demo/card/music shots demo

Needs ffmpeg and dsconv's Python dependencies (run it from `nix develop` in
dsconv/, or with them installed).
"""
import math
import os
import random
import subprocess
import sys

from PIL import Image, ImageDraw

OUT = sys.argv[1]
SRC = os.path.join(OUT, "src")
CARD = os.path.join(OUT, "card", "music")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def song(path, secs, freq, **tags):
    """A few seconds of a chord with a beat, so the visualizer moves."""
    path = os.path.join(SRC, path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        return
    meta = []
    for k, v in tags.items():
        meta += ["-metadata", f"{k}={v}"]
    expr = (f"0.22*sin({freq}*2*PI*t)+0.14*sin({freq * 1.5}*2*PI*t)+0.09*sin({freq * 4}*2*PI*t)"
            f"+0.05*(random(0)-0.5)+0.2*sin(55*2*PI*t)*gt(mod(t,0.5),0.4)")
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                    f"aevalsrc=exprs='{expr}|{expr}':s=44100:d={secs}", *meta, path], check=True)


def cover_frost(path):
    """Frostbyte: snow crystals on deep blue."""
    rnd = random.Random(7)
    im = Image.new("RGB", (512, 512))
    d = ImageDraw.Draw(im)
    for y in range(512):
        t = y / 511
        d.line([(0, y), (511, y)], fill=(int(10 + 20 * t), int(20 + 50 * t), int(60 + 90 * t)))
    for _ in range(140):
        x, y, r = rnd.randrange(512), rnd.randrange(512), rnd.choice((1, 1, 2))
        d.ellipse([x - r, y - r, x + r, y + r], fill=(200, 225, 255))
    for cx, cy, s in ((256, 240, 150), (110, 400, 50), (420, 120, 40)):
        for k in range(6):
            a = k * math.pi / 3
            ex, ey = cx + s * math.cos(a), cy + s * math.sin(a)
            d.line([(cx, cy), (ex, ey)], fill=(225, 240, 255), width=max(2, s // 25))
            for f in (0.45, 0.7):
                bx, by = cx + s * f * math.cos(a), cy + s * f * math.sin(a)
                for side in (-1, 1):
                    b = a + side * math.pi / 4
                    d.line([(bx, by), (bx + s * 0.22 * math.cos(b), by + s * 0.22 * math.sin(b))],
                           fill=(225, 240, 255), width=max(2, s // 30))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)


def cover_orbit(path):
    """Orbit: rings round a warm sun."""
    im = Image.new("RGB", (512, 512), (24, 12, 30))
    d = ImageDraw.Draw(im)
    for r in range(260, 0, -4):
        t = r / 260
        d.ellipse([256 - r, 256 - r, 256 + r, 256 + r], fill=(int(24 + 60 * (1 - t)), int(12 + 20 * (1 - t)), int(30 + 40 * (1 - t))))
    for r, w in ((210, 3), (165, 2), (120, 2)):
        d.ellipse([256 - r, 256 - r * 0.45, 256 + r, 256 + r * 0.45], outline=(250, 190, 140), width=w)
    d.ellipse([206, 206, 306, 306], fill=(255, 170, 110))
    d.ellipse([226, 222, 266, 262], fill=(255, 215, 170))
    for a, r in ((0.6, 210), (2.4, 165), (4.4, 120)):
        x, y = 256 + r * math.cos(a), 256 + r * 0.45 * math.sin(a)
        d.ellipse([x - 9, y - 9, x + 9, y + 9], fill=(255, 235, 220))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)


def album(artist, name, year, genre, titles, freq, secs=(28, 44), cover=None, folder="Bandcamp"):
    base = f"{folder}/{artist}/{name}"
    for i, t in enumerate(titles):
        song(f"{base}/{i + 1:02d} {t}.flac", secs[0] + (i * 7) % (secs[1] - secs[0]), freq + 25 * i,
             title=t, artist=artist, album=name, albumartist=artist, date=str(year), tracknumber=str(i + 1),
             genre=genre)
    if cover:
        cover(os.path.join(SRC, base, "cover.png"))


# --- the library: every name is made up
album("Nightglass", "Afterglow Arcade", 2023, "Synthwave",
      ["Neon Rain", "Glass Hearts", "Midnight Tram", "Static Bloom", "Velvet Signal", "Lantern Code",
       "Paper Satellites", "Slow Comet"], 220)
album("Nightglass", "Night Shift EP", 2021, "Synthwave", ["Afterhours", "Window Seat", "Last Light Home"], 260)
album("Mira Kestrel", "Coastline Tapes", 2021, "Dream Pop",
      ["Saltwater Radio", "Harbor Lights", "Low Tide Waltz", "Seafoam Letters", "Lighthouse Keeper"], 300)
album("Polar Moth", "Frostbyte", 2019, "Ambient", ["Snowfield", "Thermal", "Aurora Loop", "Quiet Hours"], 180,
      cover=cover_frost)
album("Sölve", "Orbit", 2024, "Indie", ["Solstice", "Gravity Well", "Satellite Hearts"], 330, cover=cover_orbit)
album("Ribbon Atlas", "Small Hours", 2022, "Lo-fi", ["Kettle Steam", "Rainy Platform", "Soft Static"], 240)
for path, title, artist, genre, freq in [
        ("Singles/月影ノ鈴 - 星屑レター.flac", "星屑レター", "月影ノ鈴", "J-Pop", 350),
        ("Singles/月影ノ鈴 - ハナビラ.flac", "ハナビラ", "月影ノ鈴", "J-Pop", 370),
        ("Singles/林星月 - 夏夜.flac", "夏夜", "林星月", "C-Pop", 390),
        ("Singles/林星月 - Starlight Café.flac", "Starlight Café", "林星月", "C-Pop", 410),
        ("Singles/Витражи - Вишнёвый свет.flac", "Вишнёвый свет", "Витражи", "Indie", 430),
        ("Singles/Витражи - Зимний вокзал.flac", "Зимний вокзал", "Витражи", "Indie", 450)]:
    song(path, 32, freq, title=title, artist=artist, genre=genre, date="2022")

# two artist values (as Quod Libet writes them): ffmpeg can't, so mutagen adds the second
two = os.path.join(SRC, "Singles", "Nightglass, Mira Kestrel - Two Lights.flac")
if not os.path.exists(two):
    song("Singles/Nightglass, Mira Kestrel - Two Lights.flac", 36, 280, title="Two Lights", artist="Nightglass",
         genre="Synthwave", date="2024")
    from mutagen.flac import FLAC
    f = FLAC(two)
    f["artist"] = ["Nightglass", "Mira Kestrel"]
    f.save()

env = dict(os.environ, PYTHONPATH=os.path.join(REPO, "dsconv", "src"))
subprocess.run([sys.executable, "-m", "dsconv", SRC, CARD, "--yes"], check=True, env=env)
print("demo card:", CARD)
