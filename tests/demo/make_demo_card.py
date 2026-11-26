#!/usr/bin/env python3
"""A made-up music library for the README's pictures: invented artists,
albums and titles, synthetic audio, covers drawn here (most songs have none
and show the player's built-in picture).

    python3 tests/demo/make_demo_card.py /tmp/demo        # -> /tmp/demo/card/music
    cd tests/host && ./screens /tmp/demo/card/music shots demo

Needs ffmpeg and the converter's Python dependencies (run it from `nix develop` in
alya-bangers-dsconv/, or with them installed).
"""
import math
import os
import random
import subprocess
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw

OUT = sys.argv[1]
SRC = os.path.join(OUT, "src")
CARD = os.path.join(OUT, "card", "music")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# --- audio: short songs that look like music to the visualizer (drums, bass,
# chords, a melody; calmer for Ambient and Lo-fi), the same every run

# minor key: i - VI - III - VII (semitones above the root, triad intervals)
PROG = [(0, (0, 3, 7)), (8, (0, 4, 7)), (3, (0, 4, 7)), (10, (0, 4, 7))]
PENTA = [0, 3, 5, 7, 10, 12, 15]


def _env(m, rate, attack, decay=None, release=0.02):
    t = np.arange(m) / rate
    e = np.minimum(1.0, t / max(attack, 1e-4))
    if decay:
        e *= np.exp(-t / decay)
    r = min(m, int(release * rate))
    if r > 1:
        e[-r:] *= np.linspace(1.0, 0.0, r)
    return e


def _tone(f, m, rate, harmonics, vibrato=0.0):
    t = np.arange(m) / rate
    ph = 2 * np.pi * f * t
    if vibrato:
        ph += vibrato * np.sin(2 * np.pi * 5.5 * t)
    w = np.zeros(m)
    for k, a in harmonics:
        if f * k < rate * 0.45:
            w += a * np.sin(k * ph)
    return w


def _hp(x):
    """Crude high-pass (first difference), keeps noise bright."""
    return np.diff(x, prepend=0.0)


def _mtof(n):
    return 440.0 * 2 ** ((n - 69) / 12)


def synth(secs, seed, genre="", rate=44100):
    """Stereo float array (n, 2), peak 0.85."""
    rnd = np.random.default_rng(seed)
    n = int(secs * rate)
    out = np.zeros((n, 2))
    style = {"Ambient": "ambient", "Lo-fi": "lofi", "Dream Pop": "dream"}.get(genre, "beat")
    bpm = {"ambient": 66, "lofi": 82, "dream": 92}.get(style, 104 + seed % 24)
    beat = 60.0 / bpm
    root = 40 + seed % 9                      # E2..C3
    eighth = beat / 2

    def add(start, sig, pan=0.0):
        a = int(start * rate)
        if a >= n:
            return
        sig = sig[: n - a]
        out[a:a + len(sig), 0] += sig * (1 - max(pan, 0.0))
        out[a:a + len(sig), 1] += sig * (1 + min(pan, 0.0))

    saw = [(k, 1.0 / k) for k in range(1, 9)]
    square = [(k, 1.0 / k) for k in (1, 3, 5, 7, 9)]
    bars = int(secs / (4 * beat)) + 1
    for bar in range(bars):
        t0 = bar * 4 * beat
        off, tri = PROG[bar % 4]
        chord_root = root + off
        # chords (pad): held for the bar
        m = int(4 * beat * rate)
        for i, iv in enumerate(tri + (12,)):
            f = _mtof(chord_root + 12 + iv)
            amp = 0.05 if style != "ambient" else 0.07
            add(t0, amp * _tone(f * (1.003 if i % 2 else 0.997), m, rate, saw) *
                _env(m, rate, 0.25 if style != "ambient" else 1.2, release=0.3), pan=(i - 1.5) * 0.3)
        if style == "ambient":
            # slow bells high up, a breath of wind
            for k in range(3):
                nt = chord_root + 36 + PENTA[rnd.integers(len(PENTA))]
                mm = int(2.5 * rate)
                add(t0 + k * 1.3 * beat + rnd.uniform(0, 0.3),
                    0.06 * _tone(_mtof(nt), mm, rate, [(1, 1), (2.76, 0.4), (5.4, 0.2)]) *
                    _env(mm, rate, 0.005, 0.7), pan=rnd.uniform(-0.6, 0.6))
            mm = int(4 * beat * rate)
            wind = np.convolve(rnd.standard_normal(mm), np.ones(24) / 24, "same")
            add(t0, 0.05 * wind * (0.6 + 0.4 * np.sin(np.linspace(0, np.pi, mm))))
            continue
        # bass: eighths on the chord root, a fifth now and then
        for e in range(8):
            if style == "lofi" and e % 2:
                continue
            nt = chord_root - 12 + (7 if e == 6 and rnd.random() < 0.5 else 0)
            mm = int(eighth * rate * (2 if style == "lofi" else 1))
            add(t0 + e * eighth, 0.22 * _tone(_mtof(nt), mm, rate, saw[:5]) * _env(mm, rate, 0.004, 0.22))
        # drums
        for b in range(4):
            tb = t0 + b * beat
            mm = int(0.35 * rate)
            tt = np.arange(mm) / rate
            f = 48 + 90 * np.exp(-tt / 0.035)
            kick = np.sin(2 * np.pi * np.cumsum(f) / rate) * np.exp(-tt / 0.16)
            if style != "dream" or b in (0, 2):
                add(tb, 0.55 * kick)
            if b in (1, 3):
                mm = int(0.25 * rate)
                tt = np.arange(mm) / rate
                sn = 0.22 * _hp(rnd.standard_normal(mm)) * np.exp(-tt / 0.08) + \
                    0.18 * np.sin(2 * np.pi * 185 * tt) * np.exp(-tt / 0.05)
                add(tb, sn * (0.6 if style == "lofi" else 1.0))
            for h in range(2):
                swing = 0.06 * beat if (style == "lofi" and h) else 0.0
                mm = int(0.08 * rate)
                tt = np.arange(mm) / rate
                open_hat = h == 1 and b == 3 and rnd.random() < 0.5
                hat = _hp(_hp(rnd.standard_normal(mm if not open_hat else mm * 3)))
                tt = np.arange(len(hat)) / rate
                hat *= np.exp(-tt / (0.025 if not open_hat else 0.09))
                add(tb + h * eighth + swing, (0.035 if style == "lofi" else 0.05) * hat, pan=0.3)
        # melody: pentatonic eighths with rests, square-ish, a little vibrato
        if style in ("beat", "dream") and bar % 8 >= 2:
            for e in range(8):
                if rnd.random() < 0.35:
                    continue
                nt = root + 24 + PENTA[rnd.integers(len(PENTA))]
                ln = eighth * (2 if rnd.random() < 0.3 else 1)
                mm = int(ln * rate)
                add(t0 + e * eighth, 0.09 * _tone(_mtof(nt), mm, rate, square, vibrato=0.4) *
                    _env(mm, rate, 0.01, 0.35), pan=-0.2)
        if style == "lofi" and bar % 2:
            for e in (1, 4, 6):
                nt = chord_root + 24 + tri[e % 3]
                mm = int(beat * rate)
                add(t0 + e * eighth, 0.07 * _tone(_mtof(nt), mm, rate, [(1, 1), (2, 0.3), (3, 0.1)]) *
                    _env(mm, rate, 0.003, 0.4), pan=0.25)
    # vinyl-ish hiss for lo-fi
    if style == "lofi":
        out += 0.004 * rnd.standard_normal((n, 2))
    fade = int(0.5 * rate)
    out[:fade] *= np.linspace(0, 1, fade)[:, None]
    out[-fade:] *= np.linspace(1, 0, fade)[:, None]
    # mastered loud like real music: a soft limiter, then the peak at 0.85
    out = np.tanh(2.2 * out / max(1e-9, np.abs(out).max())) / np.tanh(2.2)
    return 0.85 * out


def song(path, secs, freq, **tags):
    """A synthetic song, style from its genre, seeded by freq and length."""
    path = os.path.join(SRC, path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        return
    x = synth(secs, int(freq) * 1000 + int(secs), tags.get("genre", ""))
    tmp = path + ".wav"
    with wave.open(tmp, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    meta = []
    for k, v in tags.items():
        meta += ["-metadata", f"{k}={v}"]
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", tmp, *meta, path], check=True)
    os.remove(tmp)


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

env = dict(os.environ, PYTHONPATH=os.path.join(REPO, "alya-bangers-dsconv", "src"))
subprocess.run([sys.executable, "-m", "alya_bangers_dsconv", SRC, CARD, "--yes"], check=True, env=env)
print("demo card:", CARD)
