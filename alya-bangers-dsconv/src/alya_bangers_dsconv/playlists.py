"""Playlists from the music folder (.m3u, .m3u8) onto the card, for the DS app.

Only the ones the card doesn't have yet (by name, upper/lower case doesn't
matter): once a playlist is on the card, the DS's copy is the one that
counts, so what was changed there stays.

They are written the way the DS app writes its own (arm9/source/playlist.c):
/_nds/alya-bangers/playlists/<name>.m3u next to the music folder, with
#PLAYLIST:<name> and then the songs as paths below the music folder.
"""

from __future__ import annotations

import os
import unicodedata
from pathlib import Path, PurePosixPath, PureWindowsPath
from urllib.parse import unquote, urlparse

PLAYLIST_EXTS = {".m3u", ".m3u8"}
CARD_DIR = ("_nds", "alya-bangers", "playlists")
# arm9/source/playlist.h
PL_MAX = 64
PL_NAME = 32        # bytes of UTF-8, with the 0
PL_SONGS = 4000
FILE_MAX = 47       # bytes of a file name before " 2" and ".m3u" (the DS reads names under 64)


def card_dir(dst_root: Path) -> Path | None:
    """Where the DS app looks for playlists, or None if the output isn't a
    card's music folder (then there's no card to put them on)."""
    if dst_root.name.casefold() != "music":
        return None
    return dst_root.parent.joinpath(*CARD_DIR)


def _cut(s: str, limit: int) -> str:
    """At most limit bytes of UTF-8, not in the middle of a character."""
    b = s.encode("utf-8")[:limit]
    return b.decode("utf-8", "ignore")


def ds_name(name: str) -> str:
    """The name as the DS keeps it (it has room for PL_NAME - 1 bytes)."""
    return _cut(unicodedata.normalize("NFC", name).strip(), PL_NAME - 1)


def file_name(name: str, taken: set[str]) -> str:
    """Like the DS app: the characters FAT can't have become _, a name that's
    taken gets a number."""
    base = "".join("_" if c in '/\\:*?"<>|' or ord(c) < 0x20 else c for c in name)
    base = _cut(base, FILE_MAX).rstrip(". ") or "Playlist"
    f = f"{base}.m3u"
    k = 2
    while f.casefold() in taken and k < 1000:
        f = f"{base} {k}.m3u"
        k += 1
    return f


def read_playlist(path: Path) -> tuple[str | None, list[str]]:
    """Its #PLAYLIST name (if it has one) and its entries as written."""
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = raw.decode("cp1252", "replace")      # old Windows players
    name = None
    entries = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("#PLAYLIST:"):
            name = line[10:].strip() or None
        elif not line.startswith("#"):
            entries.append(line)
    return name, entries


def card_names(pl_dir: Path) -> tuple[set[str], set[str]]:
    """The names of the playlists on the card (casefolded) and their file names."""
    names, files = set(), set()
    try:
        children = list(pl_dir.iterdir())
    except OSError:
        return names, files
    for p in children:
        if p.suffix.lower() != ".m3u" or not p.is_file():
            continue
        files.add(p.name.casefold())
        try:
            name, _ = read_playlist(p)
        except OSError:
            continue
        names.add(ds_name(name or p.stem).casefold())
    return names, files


def _key(rel: str) -> str:
    return unicodedata.normalize("NFC", rel).casefold()


class SongFinder:
    """An entry of a playlist -> the song's path on the card."""

    def __init__(self, src_root: Path, dest_by_rel: dict[str, str]):
        self.src_root = src_root
        self.by_key = {_key(r): d for r, d in dest_by_rel.items()}

    def find(self, entry: str, playlist: Path) -> str | None:
        if entry.lower().startswith("file:"):
            u = urlparse(entry)
            entry = unquote(u.path)
            if len(entry) > 2 and entry[0] == "/" and entry[2] == ":":
                entry = entry[1:]               # file:///C:/Music/...
        if "://" in entry:
            return None                         # a stream, not a file
        win = "\\" in entry or (len(entry) > 1 and entry[1] == ":")
        parts = PureWindowsPath(entry).parts if win else PurePosixPath(entry).parts
        # where it points, if that's below the music folder
        if not (win and os.sep == "/"):
            p = Path(entry) if Path(entry).is_absolute() else playlist.parent / entry
            try:
                rel = Path(os.path.normpath(p)).relative_to(self.src_root).as_posix()
                d = self.by_key.get(_key(rel))
                if d:
                    return d
            except ValueError:
                pass
        # made on another computer: the end of the path that matches a song here
        clean = [x for x in parts if x not in ("\\", "/") and not x.endswith(":\\") and x not in (".", "..")]
        for i in range(len(clean)):
            d = self.by_key.get(_key("/".join(clean[i:])))
            if d:
                return d
        return None


def bring_over(playlists: list[Path], src_root: Path, dst_root: Path, dest_by_rel: dict[str, str]) -> dict:
    """Write the playlists the card doesn't have yet. Returns what happened:
    added [(name, songs, not found)], kept [name], skipped [(name, why)]."""
    out = {"added": [], "kept": [], "skipped": []}
    pl_dir = card_dir(dst_root)
    if pl_dir is None or not playlists:
        return out
    names, files = card_names(pl_dir)
    finder = SongFinder(src_root, dest_by_rel)
    for path in sorted(playlists, key=lambda p: p.as_posix().casefold()):
        try:
            name, entries = read_playlist(path)
        except OSError as e:
            out["skipped"].append((path.name, f"can't be read: {e}"))
            continue
        name = ds_name(name or path.stem)
        if name.casefold() in names:
            out["kept"].append(name)
            continue
        if len(names) >= PL_MAX:
            out["skipped"].append((name, f"the card has {PL_MAX} playlists already"))
            continue
        songs, seen, missing = [], set(), 0
        for e in entries:
            d = finder.find(e, path)
            if d is None:
                missing += 1
            elif d.casefold() not in seen:
                seen.add(d.casefold())
                songs.append(d)
        if not songs:
            out["skipped"].append((name, "none of its songs are in the music folder"))
            continue
        cut = max(0, len(songs) - PL_SONGS)
        songs = songs[:PL_SONGS]
        f = file_name(name, files)
        pl_dir.mkdir(parents=True, exist_ok=True)
        tmp = pl_dir / (f + ".new")
        tmp.write_text("#EXTM3U\n#PLAYLIST:" + name + "\n" + "".join(s + "\n" for s in songs),
                       encoding="utf-8", newline="\n")
        os.replace(tmp, pl_dir / f)
        names.add(name.casefold())
        files.add(f.casefold())
        out["added"].append((name, len(songs), missing + cut))
    return out
