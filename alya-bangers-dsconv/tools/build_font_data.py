"""Build src/alya_bangers_dsconv/data/unifont.txt.gz from a GNU Unifont installation.

Only needed when updating the bundled font. Usage:

    python tools/build_font_data.py /usr/share/unifont

Reads unifont.hex (plane 0), unifont_all.hex (planes 1-3), unifont_jp.hex
(Japanese glyph variants) and plane00-combining.txt, and writes one compact
gzip file:

    # comment lines
    G <cp>:<hex>    base glyph
    J <cp>:<hex>    Japanese variant, only where it differs from the base glyph
    C <cp>:<offset> horizontal offset of a combining mark (usually -8 or -16)

Planes 0x0E (tags, variation selectors) and 0x0F (private use, CSUR) are left out.
"""

import gzip
import sys
from pathlib import Path


def read_hex(path: Path) -> dict[int, str]:
    glyphs = {}
    with path.open(encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            cp, data = line.split(":", 1)
            glyphs[int(cp, 16)] = data.upper()
    return glyphs


def main() -> None:
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "/usr/share/unifont")
    out = Path(__file__).resolve().parent.parent / "src" / "alya_bangers_dsconv" / "data" / "unifont.txt.gz"

    base = read_hex(src / "unifont.hex")
    allg = read_hex(src / "unifont_all.hex")
    jp = read_hex(src / "unifont_jp.hex")

    glyphs = dict(base)
    for cp, data in allg.items():
        plane = cp >> 16
        if 1 <= plane <= 3 and cp not in glyphs:
            glyphs[cp] = data

    combining = {}
    comb_file = src / "plane00-combining.txt"
    if comb_file.exists():
        for line in comb_file.read_text(encoding="ascii").splitlines():
            if ":" in line:
                cp, off = line.split(":", 1)
                combining[int(cp, 16)] = int(off)

    lines = [
        "# alya-bangers-dsconv font data generated from GNU Unifont by tools/build_font_data.py",
        "# GNU Unifont: GPL-2.0-or-later with the GNU font embedding exception",
    ]
    for cp in sorted(glyphs):
        lines.append(f"G {cp:X}:{glyphs[cp]}")
    n_jp = 0
    for cp in sorted(jp):
        if cp in glyphs and jp[cp] != glyphs[cp]:
            lines.append(f"J {cp:X}:{jp[cp]}")
            n_jp += 1
    for cp in sorted(combining):
        lines.append(f"C {cp:X}:{combining[cp]}")

    data = ("\n".join(lines) + "\n").encode("ascii")
    with gzip.open(out, "wb", compresslevel=9) as f:
        f.write(data)
    print(f"{len(glyphs)} glyphs, {n_jp} Japanese variants, {len(combining)} combining offsets")
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
