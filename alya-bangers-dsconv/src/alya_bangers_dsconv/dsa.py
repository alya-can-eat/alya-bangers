"""Writer and reference reader for .dsa files. FORMAT.md is the specification."""

from __future__ import annotations

import re
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .constants import (
    BLOCK_FRAMES, COVER_SIZE, FORMAT_VERSION, HEADER_SIZE, MAX_TAG_VALUE_BYTES,
    SAMPLE_RATE, SECTOR, SOUND_CLOCK_HZ, THUMB_SIZE, TIMER_DIVIDER,
)

MAGIC = b"DSA1"
FMT_PCM16 = 1

FLAG_COVER = 1 << 0
FLAG_RG_TRACK = 1 << 1
FLAG_RG_ALBUM = 1 << 2

# Little-endian header fields, in order. See FORMAT.md.
HEADER_FIELDS = [
    ("magic", "4s"), ("version", "H"), ("header_size", "H"), ("flags", "I"),
    ("sound_clock_hz", "I"), ("timer_divider", "H"), ("channels", "B"),
    ("sample_format", "B"), ("frame_count", "I"), ("block_frames", "I"),
    ("block_count", "I"), ("duration_ms", "I"),
    ("tags_offset", "I"), ("tags_size", "I"),
    ("cover_offset", "I"), ("cover_size", "I"), ("cover_width", "H"), ("cover_height", "H"),
    ("thumb_offset", "I"), ("thumb_size", "I"), ("thumb_width", "H"), ("thumb_height", "H"),
    ("audio_offset", "I"), ("audio_size", "I"),
    ("rg_track_gain_mdb", "i"), ("rg_track_peak_q16", "I"),
    ("rg_album_gain_mdb", "i"), ("rg_album_peak_q16", "I"),
]
HEADER_STRUCT = struct.Struct("<" + "".join(f for _, f in HEADER_FIELDS))
CRC_OFFSET = HEADER_SIZE - 4
assert HEADER_STRUCT.size <= CRC_OFFSET


def _align(n: int, a: int = SECTOR) -> int:
    return (n + a - 1) // a * a


# ---------------------------------------------------------------- tags ----

def encode_tags(tags: dict[str, list[str]]) -> tuple[bytes, list[str]]:
    """Serialize tags. Returns (bytes, warnings). Keys are sorted, values keep order."""
    warnings = []
    entries = []
    for key in sorted(tags):
        kb = key.encode("utf-8")
        if len(kb) > 255:
            warnings.append(f"tag key too long, dropped: {key[:40]}…")
            continue
        for value in tags[key]:
            vb = value.encode("utf-8")
            if len(vb) > MAX_TAG_VALUE_BYTES:
                vb = vb[:MAX_TAG_VALUE_BYTES].decode("utf-8", errors="ignore").encode("utf-8")
                warnings.append(f"tag '{key}' shortened to {MAX_TAG_VALUE_BYTES // 1024} KiB")
            entries.append(struct.pack("<BH", len(kb), len(vb)) + kb + vb)
    return struct.pack("<I", len(entries)) + b"".join(entries), warnings


def decode_tags(data: bytes) -> dict[str, list[str]]:
    (count,) = struct.unpack_from("<I", data, 0)
    pos, tags = 4, {}
    for _ in range(count):
        klen, vlen = struct.unpack_from("<BH", data, pos)
        pos += 3
        key = data[pos:pos + klen].decode("utf-8")
        pos += klen
        value = data[pos:pos + vlen].decode("utf-8")
        pos += vlen
        tags.setdefault(key, []).append(value)
    return tags


# ---------------------------------------------------------- replaygain ----

_NUM = re.compile(r"[-+]?\d+(?:[.,]\d+)?")


def _num(s: str) -> float | None:
    m = _NUM.search(s.replace("−", "-"))
    return float(m.group(0).replace(",", ".")) if m else None


def replaygain(tags: dict[str, list[str]]) -> dict[str, float | None]:
    """Track/album gain in dB (ReplayGain 2 reference, -18 LUFS) and peaks.

    Opus R128 gains are relative to -23 LUFS, so they get +5 dB.
    """
    out: dict[str, float | None] = {}
    for scope in ("track", "album"):
        gain = None
        r128 = tags.get(f"r128_{scope}_gain")
        if r128:
            q = _num(r128[0])
            if q is not None:
                gain = q / 256.0 + 5.0
        if gain is None and tags.get(f"replaygain_{scope}_gain"):
            gain = _num(tags[f"replaygain_{scope}_gain"][0])
        peak = None
        if tags.get(f"replaygain_{scope}_peak"):
            peak = _num(tags[f"replaygain_{scope}_peak"][0])
        out[f"{scope}_gain"] = gain
        out[f"{scope}_peak"] = peak
    return out


# -------------------------------------------------------------- writer ----

class DsaWriter:
    """Streams float32 audio into a .dsa file.

    Usage: w = DsaWriter(file, channels, tags_bytes, cover, thumb, rg, seed);
    w.write(frames) as often as needed; w.close().
    """

    def __init__(self, fileobj, channels: int, tags_bytes: bytes,
                 cover: bytes | None, thumb: bytes | None,
                 rg: dict[str, float | None], seed: int):
        self.f = fileobj
        self.channels = channels
        self.rng = np.random.default_rng(seed)
        self.pending = np.zeros((0, channels), dtype=np.float32)
        self.frame_count = 0
        self.block_count = 0
        self.clipped = 0
        self.rg = rg

        self.h = {name: 0 for name, _ in HEADER_FIELDS}
        h = self.h
        h.update(magic=MAGIC, version=FORMAT_VERSION, header_size=HEADER_SIZE,
                 sound_clock_hz=SOUND_CLOCK_HZ, timer_divider=TIMER_DIVIDER,
                 channels=channels, sample_format=FMT_PCM16, block_frames=BLOCK_FRAMES)

        pos = HEADER_SIZE
        h["tags_offset"], h["tags_size"] = pos, len(tags_bytes)
        pos += len(tags_bytes)
        if cover is not None and thumb is not None:
            pos = _align(pos, 4)
            h["cover_offset"], h["cover_size"] = pos, len(cover)
            h["cover_width"] = h["cover_height"] = COVER_SIZE
            pos += len(cover)
            h["thumb_offset"], h["thumb_size"] = pos, len(thumb)
            h["thumb_width"] = h["thumb_height"] = THUMB_SIZE
            pos += len(thumb)
            h["flags"] |= FLAG_COVER
        h["audio_offset"] = _align(pos)

        self.f.write(b"\x00" * HEADER_SIZE)
        self.f.write(tags_bytes)
        if h["flags"] & FLAG_COVER:
            self.f.write(b"\x00" * (h["cover_offset"] - (HEADER_SIZE + len(tags_bytes))))
            self.f.write(cover)
            self.f.write(thumb)
        self.f.write(b"\x00" * (h["audio_offset"] - self.f.tell()))

    def _emit_blocks(self, final: bool) -> None:
        n = len(self.pending)
        full = n // BLOCK_FRAMES
        if final and n % BLOCK_FRAMES:
            pad = np.zeros((BLOCK_FRAMES - n % BLOCK_FRAMES, self.channels), dtype=np.float32)
            self.pending = np.concatenate([self.pending, pad])
            full += 1
        if not full:
            return
        take = full * BLOCK_FRAMES
        chunk = self.pending[:take]
        self.pending = self.pending[take:]

        # TPDF dither of +-1 LSB, then 16-bit. Zero padding stays exact zero.
        x = chunk.astype(np.float64) * 32767.0
        x += self.rng.random(x.shape) - self.rng.random(x.shape)
        np.rint(x, out=x)
        over = (x > 32767) | (x < -32768)
        self.clipped += int(over.sum())
        np.clip(x, -32768, 32767, out=x)
        pcm = x.astype("<i2")
        if final:
            pcm[n:] = 0  # padding after the last real frame

        # Block layout: for each block, all frames of channel 0, then channel 1.
        blocks = pcm.reshape(full, BLOCK_FRAMES, self.channels).transpose(0, 2, 1)
        self.f.write(np.ascontiguousarray(blocks).tobytes())
        self.block_count += full

    def write(self, frames: np.ndarray) -> None:
        """frames: float32 array of shape (n, channels), nominal range [-1, 1]."""
        if frames.size == 0:
            return
        self.frame_count += len(frames)
        self.pending = np.concatenate([self.pending, frames.astype(np.float32, copy=False)])
        if len(self.pending) >= 8 * BLOCK_FRAMES:
            self._emit_blocks(final=False)

    def close(self) -> dict:
        self._emit_blocks(final=True)
        h = self.h
        h["frame_count"] = self.frame_count
        h["block_count"] = self.block_count
        h["audio_size"] = self.block_count * BLOCK_FRAMES * 2 * self.channels
        h["duration_ms"] = int(round(self.frame_count * 1000 / SAMPLE_RATE))
        for scope in ("track", "album"):
            gain = self.rg.get(f"{scope}_gain")
            if gain is not None:
                h["flags"] |= FLAG_RG_TRACK if scope == "track" else FLAG_RG_ALBUM
                h[f"rg_{scope}_gain_mdb"] = int(round(gain * 1000))
                peak = self.rg.get(f"{scope}_peak")
                if peak is not None and peak > 0:
                    h[f"rg_{scope}_peak_q16"] = min(int(round(peak * 65536)), 0xFFFFFFFF)
        header = bytearray(HEADER_SIZE)
        HEADER_STRUCT.pack_into(header, 0, *(h[name] for name, _ in HEADER_FIELDS))
        struct.pack_into("<I", header, CRC_OFFSET, zlib.crc32(header[:CRC_OFFSET]))
        end = self.f.tell()
        self.f.seek(0)
        self.f.write(header)
        self.f.seek(end)
        return {"frames": self.frame_count, "clipped": self.clipped, "bytes": end}


# -------------------------------------------------------------- reader ----

@dataclass
class DsaFile:
    path: Path
    header: dict
    tags: dict[str, list[str]] = field(default_factory=dict)

    @property
    def channels(self) -> int:
        return self.header["channels"]

    @property
    def sample_rate(self) -> float:
        return self.header["sound_clock_hz"] / self.header["timer_divider"]

    def read_cover(self, thumb: bool = False) -> bytes | None:
        h = self.header
        if not h["flags"] & FLAG_COVER:
            return None
        off, size = (h["thumb_offset"], h["thumb_size"]) if thumb else (h["cover_offset"], h["cover_size"])
        with self.path.open("rb") as f:
            f.seek(off)
            return f.read(size)

    def read_audio(self) -> np.ndarray:
        """All valid frames as int16, shape (frames, channels)."""
        h = self.header
        with self.path.open("rb") as f:
            f.seek(h["audio_offset"])
            raw = f.read(h["audio_size"])
        a = np.frombuffer(raw, dtype="<i2").reshape(h["block_count"], h["channels"], h["block_frames"])
        return a.transpose(0, 2, 1).reshape(-1, h["channels"])[: h["frame_count"]]


class DsaError(Exception):
    pass


def read_header(raw: bytes) -> dict:
    if len(raw) < HEADER_SIZE or raw[:4] != MAGIC:
        raise DsaError("not a .dsa file")
    values = HEADER_STRUCT.unpack_from(raw, 0)
    h = dict(zip((n for n, _ in HEADER_FIELDS), values))
    (crc,) = struct.unpack_from("<I", raw, CRC_OFFSET)
    if crc != zlib.crc32(raw[:CRC_OFFSET]):
        raise DsaError("header checksum mismatch (file damaged or still being written)")
    if h["version"] != FORMAT_VERSION:
        raise DsaError(f"format version {h['version']}, this converter writes {FORMAT_VERSION}")
    return h


def open_dsa(path: Path, with_tags: bool = True) -> DsaFile:
    with Path(path).open("rb") as f:
        raw = f.read(HEADER_SIZE)
        h = read_header(raw)
        tags = {}
        if with_tags and h["tags_size"]:
            f.seek(h["tags_offset"])
            tags = decode_tags(f.read(h["tags_size"]))
    return DsaFile(Path(path), h, tags)
