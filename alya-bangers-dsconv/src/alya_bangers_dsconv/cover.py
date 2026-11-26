"""Turn cover images into DS-ready pixel data.

Output pixels are 16-bit little-endian BGR555 with bit 15 set (opaque), the
format DS bitmap backgrounds use: value = r | g << 5 | b << 10 | 0x8000.
Images are cropped to a centred square, scaled, and Floyd-Steinberg dithered
from 8 to 5 bits per channel.
"""

from __future__ import annotations

import hashlib
import io
import sys
from array import array
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

from .constants import COVER_SIZE, COVER_STEMS, IMAGE_EXTS, THUMB_SIZE


def find_folder_cover(directory: Path, files: list[str]) -> str | None:
    """Pick the cover file for a folder from its file names.

    An exact cover/folder/front/album/albumart name wins (in that order);
    otherwise a folder with exactly one image uses that image.
    """
    images = [n for n in files if Path(n).suffix.lower() in IMAGE_EXTS and not n.startswith(".")]
    by_stem = {}
    for n in sorted(images):
        by_stem.setdefault(Path(n).stem.lower(), n)
    for stem in COVER_STEMS:
        if stem in by_stem:
            return by_stem[stem]
    if len(images) == 1:
        return images[0]
    return None


def _dither_bgr555(rgb: np.ndarray) -> bytes:
    """Floyd-Steinberg dither an (h, w, 3) uint8 image to BGR555 bytes.

    Plain Python floats are several times faster than numpy scalars here.
    """
    h, w, _ = rgb.shape
    rows = [[float(v) for v in r] for r in rgb.reshape(h, w * 3).tolist()]
    out = array("H", bytes(2 * h * w))
    scale, inv = 31.0 / 255.0, 255.0 / 31.0
    for y in range(h):
        row = rows[y]
        nxt = rows[y + 1] if y + 1 < h else None
        for x in range(w):
            i = x * 3
            q3 = [0, 0, 0]
            for c in range(3):
                old = row[i + c]
                q = int(old * scale + 0.5)
                q = 0 if q < 0 else (31 if q > 31 else q)
                e = old - q * inv
                if x + 1 < w:
                    row[i + 3 + c] += e * 0.4375
                if nxt is not None:
                    if x > 0:
                        nxt[i - 3 + c] += e * 0.1875
                    nxt[i + c] += e * 0.3125
                    if x + 1 < w:
                        nxt[i + 3 + c] += e * 0.0625
                q3[c] = q
            out[y * w + x] = q3[0] | (q3[1] << 5) | (q3[2] << 10) | 0x8000
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


def _square(img: Image.Image) -> Image.Image:
    w, h = img.size
    side = min(w, h)
    left, top = (w - side) // 2, (h - side) // 2
    return img.crop((left, top, left + side, top + side))


_CACHE: dict[str, tuple[bytes, bytes]] = {}
_CACHE_MAX = 64


def _convert(data: bytes) -> tuple[bytes, bytes]:
    img = Image.open(io.BytesIO(data))
    img = ImageOps.exif_transpose(img)
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
        img = Image.alpha_composite(bg, img)
    img = _square(img.convert("RGB"))
    big = img.resize((COVER_SIZE, COVER_SIZE), Image.Resampling.LANCZOS)
    small = img.resize((THUMB_SIZE, THUMB_SIZE), Image.Resampling.LANCZOS, reducing_gap=3.0)
    return (
        _dither_bgr555(np.asarray(big, dtype=np.uint8)),
        _dither_bgr555(np.asarray(small, dtype=np.uint8)),
    )


def convert_cover(data: bytes) -> tuple[bytes, bytes]:
    """Return (cover 128x128, thumbnail 32x32) as BGR555 bytes. Raises on bad images.

    Results are cached by content hash, since every track of an album usually
    carries the same picture.
    """
    digest = hashlib.sha1(data).hexdigest()
    hit = _CACHE.get(digest)
    if hit is None:
        hit = _convert(data)
        if len(_CACHE) >= _CACHE_MAX:
            _CACHE.pop(next(iter(_CACHE)))
        _CACHE[digest] = hit
    return hit


def bgr555_to_image(data: bytes, size: int) -> Image.Image:
    """Inverse of the conversion, for `alya-bangers-dsconv inspect --cover`."""
    px = np.frombuffer(data, dtype="<u2").reshape(size, size)
    r = (px & 31).astype(np.uint8)
    g = ((px >> 5) & 31).astype(np.uint8)
    b = ((px >> 10) & 31).astype(np.uint8)
    rgb = np.stack([r, g, b], axis=-1)
    rgb = (rgb.astype(np.uint16) * 255 // 31).astype(np.uint8)
    return Image.fromarray(rgb, "RGB")
