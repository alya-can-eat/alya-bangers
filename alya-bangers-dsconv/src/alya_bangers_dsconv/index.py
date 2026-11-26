"""library.idx and font.bin, built by reading back the .dsa files on the card.

Layouts are specified in FORMAT.md.
"""

from __future__ import annotations

import gzip
import os
import re
import struct
import unicodedata
from importlib import resources
from pathlib import Path, PurePosixPath

from .collate import sort_key
from .constants import (
    DATA_DIR, DISC_DIR_PATTERN, FONT_NAME, FONT_VERSION, INDEX_NAME, INDEX_VERSION, NONE_ID, OUT_EXT,
)
from .dsa import FLAG_COVER, DsaError, open_dsa

MAX_INDEX_VALUE_BYTES = 200

# Long free text and technical tags stay in the .dsa files but not in the index.
INDEX_SKIP = {
    "comment", "description", "synopsis", "lyrics", "unsyncedlyrics", "purl",
    "encoder", "encodedby", "encoding", "encodersettings", "encoder settings",
    "~source", "~source_rate", "~source_channels", "~source_bits", "~bitrate", "~cover",
    "waveformatextensible_channel_mask",
}
INDEX_SKIP_PREFIX = ("replaygain_", "r128_", "musicbrainz_", "acoustid_")

DISC_DIR = re.compile(DISC_DIR_PATTERN, re.IGNORECASE)
KANA = re.compile(r"[぀-ヿㇰ-ㇿｦ-ﾟ]")
HAN = re.compile(r"[㐀-䶿一-鿿豈-﫿]")

UI_CHARS = "".join(chr(c) for c in range(0x20, 0x7F)) + "…•·♪°–—×←↑→↓�äöüÄÖÜ"

TRACK_HAS_COVER = 1 << 0
TRACK_HAS_TITLE = 1 << 1


def _first_int(s: str | None) -> int:
    if not s:
        return 0
    m = re.match(r"\s*(\d+)", s)
    return min(int(m.group(1)), 0xFFFF) if m else 0


def _year(tags) -> int:
    for k in ("date", "originaldate", "year"):
        for v in tags.get(k, []):
            m = re.search(r"(\d{4})", v)
            if m:
                return int(m.group(1))
    return 0


def _indexable(key: str) -> bool:
    return key not in INDEX_SKIP and not key.startswith(INDEX_SKIP_PREFIX)


# Search on the DS: names with kanji / hanzi get a kana and a pinyin reading,
# so 林星月 is found by "lin" and 春 by "はる". Only the names the search looks at.
READING_KEYS = ("title", "artist", "album", "albumartist")


def _reading_tools():
    """pykakasi and pypinyin, or None when they aren't installed (no readings then)."""
    try:
        import pykakasi
        from pypinyin import Style, pinyin
    except ImportError:
        return None
    kks = pykakasi.kakasi()

    def kana(s: str) -> str:
        return "".join(x["hira"] for x in kks.convert(s))

    def py(s: str) -> str:
        # runs of hanzi become syllables, everything else stays as it is: 星之河 (feat. 林星月)
        # -> xīng zhī hé (feat. lín xīng yuè)
        out = []
        for run in re.split(f"({HAN.pattern}+)", s):
            if run and HAN.match(run):
                out.append(" ".join(p[0] for p in pinyin(run, style=Style.TONE)))
            else:
                out.append(run)
        return "".join(out)

    return kana, py


def readings(values) -> dict[str, tuple[str | None, str | None]]:
    """value -> (kana reading, pinyin reading) for every value with Han characters."""
    tools = _reading_tools()
    if tools is None:
        return {}
    kana, py = tools
    out = {}
    for v in values:
        if HAN.search(v):
            k, p = kana(v), py(v)
            out[v] = (unicodedata.normalize("NFC", k) or None, unicodedata.normalize("NFC", p) or None)
    return out


class Strings:
    def __init__(self):
        self.data = bytearray()
        self.off: dict[str, int] = {}

    def add(self, s: str) -> int:
        o = self.off.get(s)
        if o is None:
            o = len(self.data)
            self.off[s] = o
            self.data += s.encode("utf-8") + b"\x00"
        return o


def _album_dir(path: str) -> str:
    parent = PurePosixPath(path).parent
    if DISC_DIR.search(parent.name) and parent.parent != parent:
        return str(parent.parent)
    return str(parent)


def _scan(dst_root: Path):
    tracks, bad = [], []
    for dirpath, dirnames, filenames in os.walk(dst_root):
        here = Path(dirpath)
        if here == dst_root:
            dirnames[:] = [d for d in dirnames if d != DATA_DIR]
        for n in filenames:
            if not n.lower().endswith(OUT_EXT) or n.startswith("."):
                continue
            p = here / n
            rel = p.relative_to(dst_root).as_posix()
            try:
                f = open_dsa(p)
            except (DsaError, OSError, ValueError, UnicodeDecodeError) as e:
                bad.append((rel, str(e)))
                continue
            tracks.append((rel, f))
    tracks.sort(key=lambda t: tuple(sort_key(c) for c in t[0].split("/")))
    return tracks, bad


def build_index(dst_root: Path) -> tuple[bytes, dict, set[str]]:
    tracks, bad = _scan(dst_root)

    # Pass 1: collect values and keys.
    values: set[str] = set()
    keys: set[str] = set()
    searched: set[str] = set()      # the values search looks at (READING_KEYS)
    per_track = []
    for rel, f in tracks:
        t = {k: v for k, v in f.tags.items()}
        refs = []
        for k in sorted(t):
            if not _indexable(k):
                continue
            for v in t[k]:
                if len(v.encode("utf-8")) <= MAX_INDEX_VALUE_BYTES:
                    refs.append((k, v))
                    keys.add(k)
                    values.add(v)
                    if k in READING_KEYS:
                        searched.add(v)
        per_track.append((rel, f, t, refs))

    # Albums
    groups: dict[tuple, list[int]] = {}
    for i, (rel, f, t, _) in enumerate(per_track):
        album = t.get("album", [None])[0]
        if not album:
            continue  # like Quod Libet: no album tag, no album
        mbid = t.get("musicbrainz_albumid", [""])[0]
        aa = t.get("albumartist")
        key = (album, "\x1f".join(aa) if aa else "dir:" + _album_dir(rel), mbid)
        groups.setdefault(key, []).append(i)

    album_info = []
    for (name, _aa, _mb), members in groups.items():
        ts = [per_track[i][2] for i in members]
        values.add(name)
        aa = ts[0].get("albumartist", [None])[0]
        aa_sort = ts[0].get("albumartistsort", [None])[0]
        if aa is None:
            firsts = {x.get("artist", [None])[0] for x in ts}
            if len(firsts) == 1 and None not in firsts:
                aa = firsts.pop()
                aa_sort = ts[0].get("artistsort", [None])[0]
        album_sort = ts[0].get("albumsort", [None])[0]
        for v in (aa, aa_sort, album_sort):
            if v:
                values.add(v)
        years = [y for y in (_year(x) for x in ts) if y]
        members.sort(key=lambda i: (_first_int(per_track[i][2].get("discnumber", [None])[0]),
                                    _first_int(per_track[i][2].get("tracknumber", [None])[0]), i))
        cover = next((i for i in members if per_track[i][1].header["flags"] & FLAG_COVER), NONE_ID)
        album_info.append(dict(name=name, sort=album_sort or name, aa=aa, aa_sort=aa_sort or aa,
                               year=min(years) if years else 0,
                               members=members, cover=cover))

    value_list = sorted(values, key=sort_key)
    vid = {v: i for i, v in enumerate(value_list)}
    key_list = sorted(keys)
    kid = {k: i for i, k in enumerate(key_list)}

    album_info.sort(key=lambda a: (vid[a["sort"]], vid[a["aa_sort"]] if a["aa_sort"] else NONE_ID, a["year"]))
    track_album = {}
    for aid, a in enumerate(album_info):
        for i in a["members"]:
            track_album[i] = aid

    strings = Strings()
    value_offs = [strings.add(v) for v in value_list]
    key_offs = [strings.add(k) for k in key_list]

    track_recs, tagrefs = [], []
    for i, (rel, f, t, refs) in enumerate(per_track):
        first = len(tagrefs)
        for k, v in refs:
            tagrefs.append(struct.pack("<HHI", kid[k], 0, vid[v]))

        def v_of(key, fallback=NONE_ID):
            vals = t.get(key)
            if vals and vals[0] in vid:
                return vid[vals[0]]
            return fallback

        title = v_of("title")
        artist = v_of("artist")
        flags = (TRACK_HAS_COVER if f.header["flags"] & FLAG_COVER else 0) | (TRACK_HAS_TITLE if title != NONE_ID else 0)
        track_recs.append(struct.pack(
            "<IIIHHIHHIIIIHHI",
            strings.add(rel), f.header["duration_ms"], first, len(refs), flags,
            track_album.get(i, NONE_ID),
            _first_int(t.get("discnumber", [None])[0]), _first_int(t.get("tracknumber", [None])[0]),
            title, v_of("titlesort", title), artist, v_of("artistsort", artist),
            min(_year(t), 0xFFFF), 0, 0))

    # Readings: (value id, kana, pinyin), sorted by value id
    reading_recs = []
    for v, (k, p) in sorted(readings(searched).items(), key=lambda x: vid[x[0]]):
        reading_recs.append(struct.pack("<III", vid[v], strings.add(k) if k else NONE_ID,
                                        strings.add(p) if p else NONE_ID))

    album_recs, album_tracks = [], []
    for a in album_info:
        album_recs.append(struct.pack(
            "<IIIIIHHII",
            vid[a["name"]], vid[a["sort"]],
            vid[a["aa"]] if a["aa"] else NONE_ID, vid[a["aa_sort"]] if a["aa_sort"] else NONE_ID,
            len(album_tracks), len(a["members"]), min(a["year"], 0xFFFF),
            a["cover"], 0))
        album_tracks += [struct.pack("<I", i) for i in a["members"]]

    HDR = 64
    body = []
    pos = HDR

    def place(chunk: bytes) -> int:
        nonlocal pos
        pad = (-pos) % 4
        body.append(b"\x00" * pad)
        pos += pad
        off = pos
        body.append(chunk)
        pos += len(chunk)
        return off

    strings_off = place(bytes(strings.data))
    keys_off = place(b"".join(struct.pack("<I", o) for o in key_offs))
    values_off = place(b"".join(struct.pack("<I", o) for o in value_offs))
    tracks_off = place(b"".join(track_recs))
    tagrefs_off = place(b"".join(tagrefs))
    albums_off = place(b"".join(album_recs))
    albumtracks_off = place(b"".join(album_tracks))
    readings_off = place(struct.pack("<I", len(reading_recs)) + b"".join(reading_recs)) if reading_recs else 0

    header = struct.pack(
        "<4sHHIIIIIIIIIIIIII",
        b"DSIX", INDEX_VERSION, HDR, len(track_recs), len(key_list), len(value_list),
        len(album_recs), len(tagrefs), strings_off, len(strings.data), keys_off, values_off,
        tracks_off, tagrefs_off, albums_off, albumtracks_off, readings_off)
    assert len(header) == HDR
    data = header + b"".join(body)

    text = set(strings.off)  # every string the DS may draw
    stats = {"tracks": len(track_recs), "albums": len(album_recs), "values": len(value_list),
             "keys": len(key_list), "readings": len(reading_recs), "unreadable": bad, "index_bytes": len(data)}
    return data, stats, text


# ---------------------------------------------------------------- font ----

def _load_font_data():
    base, jp, comb = {}, {}, {}
    with resources.files("alya_bangers_dsconv").joinpath("data/unifont.txt.gz").open("rb") as raw:
        with gzip.open(raw, "rt", encoding="ascii") as f:
            for line in f:
                if line[0] == "#":
                    continue
                kind, rest = line[0], line[2:].strip()
                cp, val = rest.split(":", 1)
                cp = int(cp, 16)
                if kind == "G":
                    base[cp] = val
                elif kind == "J":
                    jp[cp] = val
                elif kind == "C":
                    comb[cp] = int(val)
    return base, jp, comb


def build_font(strings: set[str]) -> tuple[bytes, dict]:
    base, jp, comb = _load_font_data()
    chars = set(UI_CHARS)
    jp_chars = set()
    for s in strings:
        chars.update(s)
        if KANA.search(s):
            jp_chars.update(HAN.findall(s))

    entries = []   # (cp, variant, hexdata, flags, xoff)
    missing = []
    for c in sorted(chars):
        cp = ord(c)
        if cp < 0x20 or 0x7F <= cp < 0xA0:
            continue
        g = base.get(cp)
        if g is None or len(g) not in (32, 64):
            missing.append(c)
            continue
        is_comb = cp in comb or unicodedata.combining(c) > 0
        xoff = comb.get(cp, -(len(g) // 4)) if is_comb else 0
        entries.append((cp, 0, g, 1 if is_comb else 0, xoff))
        if c in jp_chars and cp in jp and len(jp[cp]) == len(g):
            entries.append((cp, 1, jp[cp], (1 if is_comb else 0) | 2, xoff))
    entries.sort(key=lambda e: (e[0], e[1]))

    table, bitmaps = [], bytearray()
    for cp, _variant, g, flags, xoff in entries:
        table.append(struct.pack("<IIBBbB", cp, len(bitmaps), len(g) // 4, flags, max(-128, min(127, xoff)), 0))
        bitmaps += bytes.fromhex(g)

    HDR = 32
    table_bytes = b"".join(table)
    table_off = HDR
    bitmap_off = table_off + len(table_bytes)
    header = struct.pack("<4sHHBBBBIIIII", b"DSFT", FONT_VERSION, HDR, 16, 0, 0, 0,
                         len(entries), table_off, bitmap_off, len(bitmaps), 0)
    assert len(header) == HDR
    data = header + table_bytes + bytes(bitmaps)
    return data, {"glyphs": len(entries), "missing": len(missing),
                  "missing_chars": "".join(missing), "font_bytes": len(data)}


def _atomic_write(path: Path, data: bytes) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def build_library_files(dst_root: Path) -> dict:
    out = dst_root / DATA_DIR
    out.mkdir(parents=True, exist_ok=True)
    idx, stats, text = build_index(dst_root)
    font, fstats = build_font(text)
    _atomic_write(out / INDEX_NAME, idx)
    _atomic_write(out / FONT_NAME, font)
    stats.update(fstats)
    return stats


# ------------------------------------------------------------- readers ----
# Reference readers: what the DS player has to do, in Python. Used by the
# tests and by `alya-bangers-dsconv index --show`.

def read_index(data: bytes) -> dict:
    (magic, version, hsize, n_tracks, n_keys, n_values, n_albums, n_refs, s_off, s_size,
     k_off, v_off, t_off, r_off, a_off, at_off, rd_off) = struct.unpack_from("<4sHHIIIIIIIIIIIIII", data, 0)
    if magic != b"DSIX" or version != INDEX_VERSION:
        raise ValueError("not a library.idx of this version")

    def s(off):
        end = data.index(b"\x00", s_off + off)
        return data[s_off + off:end].decode("utf-8")

    keys = [s(o) for o in struct.unpack_from(f"<{n_keys}I", data, k_off)]
    values = [s(o) for o in struct.unpack_from(f"<{n_values}I", data, v_off)]
    refs = [struct.unpack_from("<HHI", data, r_off + 8 * i) for i in range(n_refs)]
    tracks = []
    for i in range(n_tracks):
        (path, dur, rfirst, rcount, flags, album, disc, track, title, title_sort, artist,
         artist_sort, year, _a, _b) = struct.unpack_from("<IIIHHIHHIIIIHHI", data, t_off + 48 * i)
        tags = {}
        for k, _p, v in refs[rfirst:rfirst + rcount]:
            tags.setdefault(keys[k], []).append(values[v])
        tracks.append(dict(path=s(path), duration_ms=dur, flags=flags, album=album, disc=disc,
                           track=track, title=title, title_sort=title_sort, artist=artist,
                           artist_sort=artist_sort, year=year, tags=tags))
    album_tracks = struct.unpack_from(f"<{0}I", data, at_off)
    albums = []
    for i in range(n_albums):
        name, sort, aa, aa_sort, first, count, year, cover, flags = struct.unpack_from(
            "<IIIIIHHII", data, a_off + 32 * i)
        members = list(struct.unpack_from(f"<{count}I", data, at_off + 4 * first))
        albums.append(dict(name=name, sort=sort, albumartist=aa, albumartist_sort=aa_sort,
                           tracks=members, year=year, cover=cover, flags=flags))
    reads = {}
    if rd_off:
        (n,) = struct.unpack_from("<I", data, rd_off)
        for i in range(n):
            v, k, p = struct.unpack_from("<III", data, rd_off + 4 + 12 * i)
            reads[values[v]] = (s(k) if k != NONE_ID else None, s(p) if p != NONE_ID else None)
    return dict(keys=keys, values=values, tracks=tracks, albums=albums, readings=reads)


def read_font(data: bytes) -> dict[tuple[int, int], dict]:
    magic, version, hsize, height, _a, _b, _c, count, t_off, b_off, b_size, _r = struct.unpack_from(
        "<4sHHBBBBIIIII", data, 0)
    if magic != b"DSFT" or version != FONT_VERSION:
        raise ValueError("not a font.bin of this version")
    glyphs = {}
    for i in range(count):
        cp, off, width, flags, xoff, _p = struct.unpack_from("<IIBBbB", data, t_off + 12 * i)
        size = height * width // 8
        glyphs[(cp, 1 if flags & 2 else 0)] = dict(
            width=width, flags=flags, xoff=xoff, bitmap=data[b_off + off:b_off + off + size])
    return glyphs
