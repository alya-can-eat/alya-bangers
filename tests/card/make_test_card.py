#!/usr/bin/env python3
"""The screen test's music library: made-up artists, albums and titles with
the awkward cases of a real library (one artist in two spellings, two artists
in one tag, very long and untagged titles, Japanese, Chinese, Cyrillic,
accents, genres), synthetic audio, two covers drawn here.

    python3 tests/card/make_test_card.py /tmp/testcard     # -> /tmp/testcard/card/music
    cd tests/host && make screens-run CARD=/tmp/testcard/card/music

Needs ffmpeg and the converter's Python dependencies (mutagen, pykakasi, pypinyin, ...).
"""
import os
import random
import subprocess
import sys

from PIL import Image, ImageDraw

OUT = sys.argv[1]
SRC = os.path.join(OUT, "src")
CARD = os.path.join(OUT, "card", "music")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Paper Archive vol.1: 13 songs, from a very long Japanese title to two letters
ALBUM = ["またね" * 10, "Dawnline", "River Knows", "Violet Static", "hmmmmmmmmmmmmmmmmm", "or", "Persist",
         "Disarray [feat. Unit 9]", "Umbra [feat. Halo]", "Daylight [feat. Halo]",
         "Some quiet words [feat. Pale Fern]", "Contour (Pale Fern Cover)", "Unwritten (Moss The Moth Cover)"]


def song(path, secs, freq, **tags):
    """A chord with some noise and a beat, so the visualizer has something to show."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        return
    meta = []
    for k, v in tags.items():
        meta += ["-metadata", f"{k}={v}"]
    expr = (f"0.25*sin({freq}*2*PI*t)+0.15*sin({freq * 1.5}*2*PI*t)+0.1*sin({freq * 4}*2*PI*t)"
            f"+0.05*(random(0)-0.5)+0.2*sin(55*2*PI*t)*gt(mod(t,0.5),0.4)")
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                    f"aevalsrc=exprs='{expr}|{expr}':s=44100:d={secs}", *meta, path], check=True)


def cover_moon():
    """A moonlit sea at dusk."""
    s = 512
    img = Image.new("RGB", (s, s))
    d = ImageDraw.Draw(img)
    for y in range(s):
        t = y / s
        top, bot = (34, 18, 74), (236, 122, 150)
        d.line([(0, y), (s, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t ** 1.4) for i in range(3)))
    d.ellipse([300, 90, 460, 250], fill=(250, 226, 214))
    rnd = random.Random(3)
    for _ in range(90):
        d.point((rnd.randrange(s), rnd.randrange(0, 260)), fill=(255, 240, 250))
    for i in range(14):
        y = 340 + i * 12
        d.line([(0, y), (s, y)], fill=(250, 200, 200) if i % 2 else (120, 60, 120), width=3)
    d.polygon([(0, 330), (90, 250), (170, 300), (260, 220), (380, 330)], fill=(24, 12, 40))
    d.rectangle([0, 430, s, s], fill=(30, 14, 46))
    return img


def cover_rings():
    """Teal rings round a triangle."""
    s = 512
    img = Image.new("RGB", (s, s), (10, 22, 38))
    d = ImageDraw.Draw(img)
    for r in range(240, 0, -24):
        d.ellipse([256 - r, 256 - r, 256 + r, 256 + r], outline=(16 + r // 6, 70 + r // 3, 96 + r // 3), width=6)
    d.polygon([(256, 96), (410, 380), (102, 380)], outline=(120, 250, 230), width=10)
    d.ellipse([226, 250, 286, 310], fill=(240, 250, 250))
    return img


base = f"{SRC}/Bandcamp/Kivara/Kivara - Paper Archive vol.1"
for i, t in enumerate(ALBUM):
    song(f"{base}/Kivara - Paper Archive vol.1 - {i + 1:02d} {t.replace('[', '-').replace(']', '-')}.flac", 6,
         220 + 20 * i, title=t, artist="Kivara", album="Paper Archive vol.1", albumartist="Kivara", date="2020",
         tracknumber=str(i + 1))
cover_moon().save(f"{base}/cover.png")
base2 = f"{SRC}/Bandcamp/Kivara/Kivara - Echoes Of Quiet Worlds"
for i, t in enumerate(["In Stillness", "Eternal", "Resurgence"]):
    song(f"{base2}/Kivara - Echoes Of Quiet Worlds - {i + 1:02d} {t}.flac", 5, 330 + 30 * i, title=t, artist="Kivara",
         album="Echoes Of Quiet Worlds", albumartist="Kivara", date="2019", tracknumber=str(i + 1), genre="Breakcore")
cover_rings().save(f"{base2}/cover.png")
# the same artist in capitals: one artist unless names are case-sensitive
for t, a in [("Fondness", "Kivara"), ("Gone", "KIVARA"), ("Mental Static", "KIVARA"),
             ("Everything Will Be Fine", "Kivara")]:
    song(f"{SRC}/Bandcamp/Kivara/Kivara - {t}.flac", 4, 300, title=t, artist=a, date="2021")
for a, t in [("Keito Hayama", "終ワリノ鐘"), ("Keiji Onoda", "取リ残サレタ夢"), ("Ken Aoba", "Lightfall"),
             ("Ken Aoba", "Secondhand"), ("kensei umino", "Rocketry"), ("Kenna Gray", "Strangeness"),
             ("Kivara feat. Ombo", "Altogether"), ("Kivara x Lumen The Moth", "Hushed"), ("King Marrow", "Pigeonhole"),
             ("Aethel", "Sunward"), ("D512", "Norrland")]:
    song(f"{SRC}/Singles/{a} - {t}.opus", 4, 260, title=t, artist=a)
song(f"{SRC}/CD/Track 01.flac", 4, 200)   # no tags at all


def two_artists():
    """Two values of ARTIST in one song, as taggers like Quod Libet write them."""
    from mutagen.flac import FLAC
    p = f"{SRC}/Bandcamp/Ken Aoba, Aethel - Duet.flac"
    if os.path.exists(p):
        return
    song(p, 4, 280, title="Duet", date="2022")
    f = FLAC(p)
    f["artist"] = ["Ken Aoba", "Aethel"]
    f.save()


two_artists()
# search: Chinese (pinyin), Japanese (kana, kanji readings), Cyrillic, accents, genres
for a, t, g in [("洛星涵", "星之河", "Vocal Synth"), ("洛星涵", "Glint", "Vocal Synth"),
                ("NEKO*09", "ラビットロード", "Vocal Synth"), ("Витор", "Ария №3", "Pop"), ("Винил", "Ива", "Pop"),
                ("Gölmo", "Schwalbe", "Rock"), ("Velvet Harbor", "Golden Hours", "Electronic"),
                ("Velvet Harbor", "Goodbye Again", "Electronic")]:
    song(f"{SRC}/Singles/{a} - {t}.opus", 3, 240, title=t, artist=a, genre=g)

env = dict(os.environ, PYTHONPATH=os.path.join(REPO, "alya-bangers-dsconv", "src"))
subprocess.run([sys.executable, "-m", "alya_bangers_dsconv", SRC, CARD, "--yes"], check=True, env=env)
print("test card:", CARD)
