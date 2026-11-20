"""Read tags and embedded pictures from any supported file into one shape.

Tags come out as {key: [values]} with Quod Libet style lowercase key names
(title, artist, albumartist, tracknumber, discnumber, date, genre, artistsort,
replaygain_track_gain, musicbrainz_albumid, ...). Values are NFC-normalized
strings; multi-value tags stay multi-value.
"""

from __future__ import annotations

import base64
import struct
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

import mutagen
from mutagen.apev2 import APEv2, APENoHeaderError
from mutagen.flac import FLAC, Picture
from mutagen.id3 import ID3
from mutagen.mp4 import MP4

ID3_TEXT = {
    "TIT1": "grouping", "TIT2": "title", "TIT3": "version",
    "TPE1": "artist", "TPE2": "albumartist", "TPE3": "conductor", "TPE4": "arranger",
    "TEXT": "lyricist", "TCOM": "composer", "TENC": "encodedby", "TALB": "album",
    "TRCK": "tracknumber", "TPOS": "discnumber", "TSRC": "isrc", "TCOP": "copyright",
    "TPUB": "organization", "TSST": "discsubtitle", "TOLY": "author", "TMOO": "mood",
    "TBPM": "bpm", "TDRC": "date", "TYER": "date", "TDOR": "originaldate",
    "TORY": "originaldate", "TOAL": "originalalbum", "TOPE": "originalartist",
    "TSOP": "artistsort", "TSOA": "albumsort", "TSOT": "titlesort",
    "TSO2": "albumartistsort", "TSOC": "composersort", "TMED": "media",
    "TCMP": "compilation", "TLAN": "language", "TCON": "genre", "TSSE": "encoder",
}

MP4_TEXT = {
    "\xa9nam": "title", "\xa9ART": "artist", "aART": "albumartist", "\xa9alb": "album",
    "\xa9day": "date", "\xa9gen": "genre", "\xa9wrt": "composer", "\xa9grp": "grouping",
    "\xa9cmt": "comment", "\xa9lyr": "lyrics", "desc": "description", "cprt": "copyright",
    "sonm": "titlesort", "soar": "artistsort", "soal": "albumsort",
    "soaa": "albumartistsort", "soco": "composersort", "\xa9too": "encoder",
}

# Free-form names used by MusicBrainz Picard and others, mapped to Quod Libet keys.
FREEFORM = {
    "musicbrainz album id": "musicbrainz_albumid",
    "musicbrainz artist id": "musicbrainz_artistid",
    "musicbrainz album artist id": "musicbrainz_albumartistid",
    "musicbrainz release group id": "musicbrainz_releasegroupid",
    "musicbrainz release track id": "musicbrainz_releasetrackid",
    "musicbrainz track id": "musicbrainz_trackid",
    "musicbrainz album type": "musicbrainz_albumtype",
    "musicbrainz album status": "musicbrainz_albumstatus",
    "musicbrainz album release country": "releasecountry",
    "album artist": "albumartist",
}

APE_KEYS = {
    "album artist": "albumartist", "track": "tracknumber", "disc": "discnumber",
    "year": "date",
}

RIFF_INFO = {
    "INAM": "title", "IART": "artist", "IPRD": "album", "ICRD": "date",
    "IGNR": "genre", "ITRK": "tracknumber", "IPRT": "tracknumber",
    "ICMT": "comment", "ICOP": "copyright", "ISFT": "encoder",
}

# Keys that hold binary data or that every container fills with noise.
SKIP_KEYS = {
    "metadata_block_picture", "coverart", "coverartmime", "cover art (front)",
    "cover art (back)", "itunnorm", "itunsmpb", "itunes_cddb_1",
}

FRONT_COVER = 3


@dataclass
class TagResult:
    tags: dict[str, list[str]] = field(default_factory=dict)
    pictures: list[tuple[int, bytes]] = field(default_factory=list)  # (type, data)
    warnings: list[str] = field(default_factory=list)

    def add(self, key: str, value) -> None:
        key = unicodedata.normalize("NFC", str(key)).strip().lower()
        if not key or key in SKIP_KEYS:
            return
        if isinstance(value, bytes):
            try:
                value = value.decode("utf-8")
            except UnicodeDecodeError:
                return
        value = unicodedata.normalize("NFC", str(value)).replace("\x00", "").strip()
        if not value:
            return
        vals = self.tags.setdefault(key, [])
        if value not in vals:
            vals.append(value)


def _from_vorbis(res: TagResult, vc) -> None:
    for key, values in vc.as_dict().items():
        k = key.lower()
        if k == "metadata_block_picture":
            for v in values:
                try:
                    pic = Picture(base64.b64decode(v))
                    res.pictures.append((pic.type, pic.data))
                except Exception:
                    res.warnings.append("unreadable embedded picture")
            continue
        if k.startswith("rating:") or k == "fmps_rating":
            for v in values:
                res.add("~#rating", v)
            continue
        if k.startswith("playcount:"):
            for v in values:
                res.add("~#playcount", v)
            continue
        for v in values:
            res.add(FREEFORM.get(k, k), v)


def _from_id3(res: TagResult, id3: ID3) -> None:
    for frame in id3.values():
        fid = frame.FrameID
        if fid in ID3_TEXT:
            key = ID3_TEXT[fid]
            if fid == "TCON":
                values = frame.genres
            else:
                values = [str(t) for t in frame.text]
            for v in values:
                res.add(key, v)
        elif fid == "TXXX":
            desc = frame.desc.strip().lower()
            key = FREEFORM.get(desc, desc)
            if key.startswith("quodlibet::"):
                key = key[len("quodlibet::"):]
            for v in frame.text:
                res.add(key, str(v))
        elif fid == "COMM":
            if frame.desc.lower().startswith("itun"):
                continue
            for v in frame.text:
                res.add("comment", str(v))
        elif fid == "USLT":
            res.add("lyrics", frame.text)
        elif fid == "UFID" and frame.owner == "http://musicbrainz.org":
            res.add("musicbrainz_trackid", frame.data)
        elif fid == "WOAR":
            res.add("website", frame.url)
        elif fid == "POPM":
            if frame.rating:
                res.add("~#rating", f"{frame.rating / 255:.2f}")
            if getattr(frame, "count", 0):
                res.add("~#playcount", str(frame.count))
        elif fid == "APIC":
            res.pictures.append((int(frame.type), frame.data))


def _from_mp4(res: TagResult, tags) -> None:
    for key, values in tags.items():
        if key in MP4_TEXT:
            for v in values:
                res.add(MP4_TEXT[key], v)
        elif key in ("trkn", "disk"):
            name = "tracknumber" if key == "trkn" else "discnumber"
            for n, total in values:
                if n:
                    res.add(name, f"{n}/{total}" if total else str(n))
        elif key == "tmpo":
            for v in values:
                res.add("bpm", str(v))
        elif key == "cpil":
            res.add("compilation", "1" if values else "0")
        elif key == "gnre":
            continue  # legacy numeric genre; ©gen is what taggers write today
        elif key == "covr":
            for cover in values:
                res.pictures.append((FRONT_COVER, bytes(cover)))
        elif key.startswith("----:"):
            name = key.split(":", 2)[2].strip().lower()
            name = FREEFORM.get(name, name)
            for v in values:
                res.add(name, bytes(v))


def _from_ape(res: TagResult, ape) -> None:
    for key, value in ape.items():
        k = key.strip().lower()
        if k.startswith("cover art"):
            data = bytes(value.value)
            nul = data.find(b"\x00")
            if nul >= 0:
                res.pictures.append((FRONT_COVER if "front" in k else 0, data[nul + 1:]))
            continue
        if value.kind != 0:  # not text
            continue
        k = APE_KEYS.get(k, FREEFORM.get(k, k))
        for v in str(value).split("\x00"):
            res.add(k, v)


def _riff_info(res: TagResult, path: Path) -> None:
    """Minimal RIFF LIST/INFO reader for WAV files without ID3 tags."""
    try:
        with path.open("rb") as f:
            head = f.read(12)
            if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WAVE":
                return
            while True:
                ch = f.read(8)
                if len(ch) < 8:
                    return
                cid, size = ch[:4], struct.unpack("<I", ch[4:])[0]
                if cid == b"LIST":
                    body = f.read(size)
                    if body[:4] == b"INFO":
                        _parse_info(res, body[4:])
                    if size & 1:
                        f.read(1)
                elif cid == b"data":
                    f.seek(size + (size & 1), 1)
                else:
                    f.seek(size + (size & 1), 1)
    except OSError:
        return


def _parse_info(res: TagResult, body: bytes) -> None:
    pos = 0
    while pos + 8 <= len(body):
        cid = body[pos:pos + 4].decode("latin-1")
        size = struct.unpack("<I", body[pos + 4:pos + 8])[0]
        raw = body[pos + 8:pos + 8 + size].split(b"\x00")[0]
        pos += 8 + size + (size & 1)
        if cid not in RIFF_INFO:
            continue
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("cp1252", errors="replace")
        res.add(RIFF_INFO[cid], text)


def read_tags(path: Path) -> TagResult:
    res = TagResult()
    try:
        f = mutagen.File(path)
    except Exception as e:  # damaged tags must not stop the audio conversion
        res.warnings.append(f"tags unreadable: {e}")
        f = None

    if f is not None and f.tags is not None:
        tags = f.tags
        if isinstance(tags, ID3):
            _from_id3(res, tags)
        elif isinstance(f, MP4):
            _from_mp4(res, tags)
        elif isinstance(tags, APEv2):
            _from_ape(res, tags)
        elif hasattr(tags, "as_dict"):
            _from_vorbis(res, tags)
    if isinstance(f, FLAC):
        for pic in f.pictures:
            res.pictures.append((pic.type, pic.data))

    # MP3 files sometimes carry APEv2 tags next to (or instead of) ID3.
    if path.suffix.lower() == ".mp3":
        try:
            _from_ape(res, APEv2(path))
        except (APENoHeaderError, Exception):
            pass

    if path.suffix.lower() in (".wav", ".wave") and not res.tags:
        _riff_info(res, path)
    return res


def best_picture(pictures: list[tuple[int, bytes]]) -> bytes | None:
    if not pictures:
        return None
    for ptype, data in pictures:
        if ptype == FRONT_COVER and data:
            return data
    for _, data in pictures:
        if data:
            return data
    return None
