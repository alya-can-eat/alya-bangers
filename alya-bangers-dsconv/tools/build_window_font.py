"""Build src/alya_bangers_dsconv/data/window_font.ttf.gz, the window's font:
the DS app's Unifont glyphs as an outline font, every pixel a square, so it
draws as crisp as on the DS at 16 px (and 32, 48 …).

Only the blocks the window needs (Latin, Greek, Cyrillic, kana, symbols, a few
emoji); anything else in a file name falls back to a system font.

    nix-shell -p python3Packages.fonttools --run "python3 tools/build_window_font.py"
    (or: pip install fonttools)

GNU Unifont: GPL-2.0-or-later with the GNU font embedding exception (see LICENSES/).
"""

from __future__ import annotations

import gzip
import io
import sys
from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

HERE = Path(__file__).resolve().parent.parent
DATA = HERE / "src" / "alya_bangers_dsconv" / "data"

PX = 64                 # font units per pixel: 16 px = 1024 units per em
ASCENT, DESCENT = 14, 2  # Unifont's baseline is 14 pixels below the top

RANGES = [
    (0x0020, 0x007E), (0x00A0, 0x02FF),          # Latin, IPA, spacing modifiers
    (0x0370, 0x03FF), (0x0400, 0x052F),          # Greek, Cyrillic
    (0x1E00, 0x1EFF),                            # more Latin (Vietnamese …)
    (0x2000, 0x2BFF),                            # punctuation, arrows, symbols, boxes, dingbats
    (0x3000, 0x30FF),                            # CJK punctuation, kana
    (0xFF00, 0xFFEF),                            # full-width forms (FAT32-safe names use them)
]
EXTRA = [0x1F3A7, 0x1F3B5, 0x1F3B6, 0x1F4BE, 0x1F4BF, 0x1F4C1, 0x1F4C2, 0x1F50D, 0x1F310]


def load() -> dict[int, str]:
    glyphs = {}
    with gzip.open(DATA / "unifont.txt.gz", "rt", encoding="ascii") as f:
        for line in f:
            if line.startswith("G "):
                cp, val = line[2:].strip().split(":", 1)
                glyphs[int(cp, 16)] = val
    return glyphs


def rects(hexdata: str) -> tuple[int, list[tuple[int, int, int, int]]]:
    """Width in pixels and the glyph as rectangles (x0, y0, x1, y1), rows top down."""
    w = len(hexdata) // 4           # 32 hex digits: 8 wide, 64: 16 wide
    digits = w // 4
    rows = [int(hexdata[r * digits:(r + 1) * digits], 16) for r in range(16)]
    out: list[list[int]] = []
    open_runs: dict[tuple[int, int], list[int]] = {}
    for y, bits in enumerate(rows + [0]):
        runs = []
        x = 0
        while x < w:
            if bits >> (w - 1 - x) & 1:
                x0 = x
                while x < w and bits >> (w - 1 - x) & 1:
                    x += 1
                runs.append((x0, x))
            else:
                x += 1
        still = {}
        for r in runs:
            if r in open_runs:
                open_runs[r][3] = y + 1
                still[r] = open_runs.pop(r)
            else:
                still[r] = [r[0], y, r[1], y + 1]
        out += open_runs.values()
        open_runs = still
    return w, [tuple(r) for r in out]


def main() -> None:
    src = load()
    cps = [cp for a, b in RANGES for cp in range(a, b + 1) if cp in src] + [cp for cp in EXTRA if cp in src]
    names = [".notdef"] + [f"uni{cp:04X}" if cp < 0x10000 else f"u{cp:05X}" for cp in cps]
    fb = FontBuilder(16 * PX, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({cp: n for cp, n in zip(cps, names[1:])})
    glyf, hmtx = {}, {}
    empty = TTGlyphPen(None).glyph()
    glyf[".notdef"] = empty
    hmtx[".notdef"] = (8 * PX, 0)
    for cp, name in zip(cps, names[1:]):
        w, rs = rects(src[cp])
        pen = TTGlyphPen(None)
        for x0, y0, x1, y1 in rs:
            top, bottom = (ASCENT - y0) * PX, (ASCENT - y1) * PX
            pen.moveTo((x0 * PX, bottom))   # clockwise, as TrueType wants outer contours
            pen.lineTo((x0 * PX, top))
            pen.lineTo((x1 * PX, top))
            pen.lineTo((x1 * PX, bottom))
            pen.closePath()
        glyf[name] = pen.glyph()
        hmtx[name] = (w * PX, min((r[0] for r in rs), default=0) * PX)
    fb.setupGlyf(glyf)
    fb.setupHorizontalMetrics(hmtx)
    fb.setupHorizontalHeader(ascent=ASCENT * PX, descent=-DESCENT * PX)
    fb.setupNameTable({
        "familyName": "Unifont alya-bangers",
        "styleName": "Regular",
        "copyright": "GNU Unifont glyphs, GPL-2.0-or-later with the GNU font embedding exception",
        "description": "GNU Unifont as squares, for the alya-bangers-dsconv window",
    })
    fb.setupOS2(sTypoAscender=ASCENT * PX, sTypoDescender=-DESCENT * PX, sTypoLineGap=0,
                usWinAscent=ASCENT * PX, usWinDescent=DESCENT * PX, sxHeight=8 * PX, sCapHeight=10 * PX,
                achVendID="ALYA", fsType=0)
    fb.setupPost(isFixedPitch=0)
    fb.setupHead(unitsPerEm=16 * PX)
    buf = io.BytesIO()
    fb.save(buf)
    out = DATA / "window_font.ttf.gz"
    with open(out, "wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as gz:
        gz.write(buf.getvalue())
    print(f"{len(cps)} glyphs, {len(buf.getvalue())} bytes, {out.stat().st_size} gzipped -> {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
