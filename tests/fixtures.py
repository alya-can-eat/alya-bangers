"""Builds a small synthetic music library with the awkward cases of a real one.

Everything is generated with the converter's own dependencies (PyAV, mutagen,
Pillow), so the tests need nothing else installed.
"""

from __future__ import annotations

import base64
import io
import struct
import unicodedata
from pathlib import Path

import av
import numpy as np
from mutagen.flac import FLAC, Picture
from mutagen.id3 import APIC, ID3, TALB, TIT2, TPE1, TPE2, TRCK, TXXX
from mutagen.mp4 import MP4, MP4Cover
from mutagen.oggopus import OggOpus
from PIL import Image

LAYOUTS = {1: "mono", 2: "stereo", 6: "5.1"}


def tone(rate: int, seconds: float, channels: int, freq: float) -> np.ndarray:
    """Planar float32 (channels, n): channel c plays freq * (1 + c / 2)."""
    n = int(round(rate * seconds))
    t = np.arange(n) / rate
    return np.stack([0.4 * np.sin(2 * np.pi * freq * (1 + c / 2) * t) for c in range(channels)]).astype(np.float32)


def encode(path: Path, codec: str, rate: int, channels: int = 2, seconds: float = 1.0,
           freq: float = 440.0, fmt: str | None = None, bitrate: int | None = None,
           sample_fmt: str | None = None, data: np.ndarray | None = None) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    out = av.open(str(path), "w", format=fmt)
    st = out.add_stream(codec, rate=rate, layout=LAYOUTS[channels])
    if bitrate:
        st.bit_rate = bitrate
    if sample_fmt:
        st.codec_context.format = sample_fmt
    if data is None:
        data = tone(rate, seconds, channels, freq)
    frame = av.AudioFrame.from_ndarray(data, format="fltp", layout=LAYOUTS[channels])
    frame.sample_rate = rate
    cc = st.codec_context
    rs = av.AudioResampler(format=cc.format.name if cc.format else None, layout=LAYOUTS[channels],
                           rate=rate, frame_size=cc.frame_size or 1152)
    pts = 0
    for fr in list(rs.resample(frame)) + list(rs.resample(None)):
        fr.pts = pts
        pts += fr.samples
        for pkt in st.encode(fr):
            out.mux(pkt)
    for pkt in st.encode(None):
        out.mux(pkt)
    out.close()
    return path


def image(path_or_none, size=(500, 500), color=(200, 30, 30), fmt="PNG") -> bytes:
    img = Image.new("RGB", size, color)
    w, h = size
    s = min(w, h) // 2
    # a white square in the middle, so crops can be checked
    img.paste((255, 255, 255), ((w - s) // 2, (h - s) // 2, (w + s) // 2, (h + s) // 2))
    buf = io.BytesIO()
    img.save(buf, fmt)
    data = buf.getvalue()
    if path_or_none is not None:
        Path(path_or_none).parent.mkdir(parents=True, exist_ok=True)
        Path(path_or_none).write_bytes(data)
    return data


def tag_flac(path: Path, picture: bytes | None = None, **tags) -> None:
    f = FLAC(path)
    for k, v in tags.items():
        f[k] = v if isinstance(v, list) else [v]
    if picture is not None:
        pic = Picture()
        pic.type = 3
        pic.mime = "image/png"
        pic.data = picture
        f.add_picture(pic)
    f.save()


def tag_opus(path: Path, picture: bytes | None = None, **tags) -> None:
    f = OggOpus(path)
    for k, v in tags.items():
        f[k] = v if isinstance(v, list) else [v]
    if picture is not None:
        pic = Picture()
        pic.type = 3
        pic.mime = "image/jpeg"
        pic.data = picture
        f["metadata_block_picture"] = [base64.b64encode(pic.write()).decode("ascii")]
    f.save()


def tag_mp3(path: Path, title, artist, album=None, picture=None, rg=None, albumartist=None, track=None):
    t = ID3()
    t.add(TIT2(encoding=3, text=title))
    t.add(TPE1(encoding=3, text=artist))
    if album:
        t.add(TALB(encoding=3, text=album))
    if albumartist:
        t.add(TPE2(encoding=3, text=albumartist))
    if track:
        t.add(TRCK(encoding=3, text=track))
    if rg:
        t.add(TXXX(encoding=3, desc="REPLAYGAIN_TRACK_GAIN", text=rg))
    if picture:
        t.add(APIC(encoding=3, mime="image/jpeg", type=3, desc="", data=picture))
    t.save(path)


def tag_m4a(path: Path, picture=None, **tags):
    f = MP4(path)
    for k, v in tags.items():
        f[k] = v if isinstance(v, list) else [v]
    if picture:
        f["covr"] = [MP4Cover(picture, imageformat=MP4Cover.FORMAT_PNG)]
    f.save()


def add_riff_info(path: Path, info: dict[str, str]) -> None:
    body = b"INFO"
    for cid, text in info.items():
        raw = text.encode("utf-8") + b"\x00"
        if len(raw) & 1:
            raw += b"\x00"
        body += cid.encode("ascii") + struct.pack("<I", len(raw)) + raw
    data = bytearray(path.read_bytes())
    data += b"LIST" + struct.pack("<I", len(body)) + body
    struct.pack_into("<I", data, 4, len(data) - 8)
    path.write_bytes(bytes(data))


def build_library(root: Path) -> dict:
    """Create the library. Returns facts the tests check against."""
    red = image(None, color=(220, 20, 20))
    jpg_thumb = image(None, size=(1280, 720), color=(20, 20, 220), fmt="JPEG")

    # Bandcamp album with folder cover and a decoy "huge cover"
    alb = root / "Bandcamp" / "300 Lanterns" / "300 Lanterns - HALCYON"
    for n in (1, 2, 3, 10):
        p = encode(alb / f"300 Lanterns - HALCYON - {n:02d} HALCYON {n}.flac", "flac", 44100, seconds=0.6,
                   freq=300 + 40 * n)
        tag_flac(p, title=f"HALCYON {n}", artist="300 Lanterns", album="HALCYON",
                 albumartist="300 Lanterns", tracknumber=f"{n}/10", date="2021-05-01")
    image(alb / "cover.png", color=(10, 200, 10))
    image(alb / "300 Lanterns - HALCYON - huge cover.png", size=(900, 900), color=(250, 0, 250))

    # Japanese titles, embedded cover, folder cover present too (embedded wins)
    k = root / "Bandcamp" / "Kivara" / "Kivara - Paper Archive vol.1"
    p = encode(k / "Kivara - Paper Archive vol.1 - 01 またねまたねまたね.flac", "flac", 48000, seconds=0.5)
    tag_flac(p, picture=red, title="またねまたねまたね", artist="Kivara", album="Paper Archive vol.1",
             tracknumber="1", genre=["Breakcore", "Electronic"])
    image(k / "cover.jpg", color=(0, 0, 0), fmt="JPEG")

    # Untagged CD rip with natural-number file names
    cd = root / "CD" / "Mörli - Bergwind"
    for n in (1, 2, 10):
        encode(cd / f"Track {n}.flac", "flac", 44100, seconds=0.3)

    # Two-disc album in Disc subfolders, album tag but no album artist
    for d in (1, 2):
        p = encode(root / "CD" / "GrEaT NO.2 sOnGs" / f"Disc {d}" / "Track 1.flac", "flac", 44100, seconds=0.3)
        tag_flac(p, album="GrEaT NO.2 sOnGs", title=f"Disc {d} opener", artist=f"Artist {d}",
                 discnumber=str(d), tracknumber="1")

    # 24-bit WAV with RIFF INFO tags
    w = encode(root / "CD" / "Cobalt Engine" / "01-Dream Made Of Glass-mastered v4 24bit.wav",
               "pcm_s24le", 96000, seconds=0.4, fmt="wav")
    add_riff_info(w, {"INAM": "Dream Made Of Glass", "IART": "Cobalt Engine", "IPRD": "PRISM"})

    # Soundcloud MP3 with Chinese, 16:9 embedded thumbnail and ReplayGain
    sc = root / "Soundcloud"
    p = encode(sc / "minto - 星之河 (feat. 林星月).mp3", "libmp3lame", 44100, seconds=0.8, bitrate=192000)
    tag_mp3(p, "星之河 (feat. 林星月)", "minto", picture=jpg_thumb, rg="-6.50 dB")
    p = encode(sc / "Mono Voice.mp3", "libmp3lame", 22050, channels=1, seconds=0.5, bitrate=64000)
    tag_mp3(p, "Mono Voice", "Someone")

    yt = root / "YouTube Music"
    p = encode(yt / "VXLTRA - 00:00.opus", "libopus", 48000, seconds=0.5, bitrate=128000, fmt="ogg")
    tag_opus(p, title="00:00", artist="VXLTRA", r128_track_gain="-512")
    p = encode(yt / "Brassfox - Good F**King Noise.opus", "libopus", 48000, seconds=0.5, fmt="ogg")
    tag_opus(p, picture=jpg_thumb, title="Good F**King Noise", artist="Brassfox")
    p = encode(yt / "Kumoyuki - 春風レター.opus", "libopus", 48000, seconds=0.4, fmt="ogg")
    tag_opus(p, title="春風レター", artist="Kumoyuki")
    p = encode(yt / "Corin Vale - Skyward.m4a", "aac", 44100, seconds=0.6, fmt="ipod", bitrate=128000)
    tag_m4a(p, picture=red, **{"\xa9nam": "Skyward", "\xa9ART": "Corin Vale", "trkn": [(3, 9)]})
    p = encode(yt / "Kid 🎵 Song.opus", "libopus", 48000, seconds=0.3, fmt="ogg")
    tag_opus(p, title="Kid 🎵 Song", artist="Kid")
    # Vietnamese, file name written in decomposed (NFD) form as macOS would
    nfd = unicodedata.normalize("NFD", "Lúa - 5 Phút Mơ.opus")
    p = encode(yt / nfd, "libopus", 48000, seconds=0.3, fmt="ogg")
    tag_opus(p, title=unicodedata.normalize("NFD", "5 Phút Mơ"), artist="Lúa")
    # Collisions: same stem, different format; and case-only difference
    p = encode(yt / "Tide Drive.flac", "flac", 44100, seconds=0.4, freq=500)
    tag_flac(p, title="Tide Drive (FLAC)", artist="Velvet Harbor")
    p = encode(yt / "Tide Drive.opus", "libopus", 48000, seconds=0.4, fmt="ogg", freq=700)
    tag_opus(p, title="Tide Drive (Opus)", artist="Velvet Harbor")
    p = encode(yt / "pebble.mp3", "libmp3lame", 44100, seconds=0.3, bitrate=320000)
    tag_mp3(p, "pebble (mp3)", "Pebble")
    p = encode(yt / "PEBBLE.opus", "libopus", 48000, seconds=0.3, fmt="ogg")
    tag_opus(p, title="PEBBLE (opus)", artist="Pebble")
    # Trailing dots, reserved name, surround
    p = encode(yt / "Dear Wren....flac", "flac", 44100, seconds=0.3)
    encode(yt / "CON.flac", "flac", 44100, seconds=0.3)
    encode(yt / "Surround Test.flac", "flac", 48000, channels=6, seconds=0.3)

    # A brick-walled master: a full-scale square wave overshoots after resampling
    t = np.arange(int(44100 * 0.8)) / 44100
    square = np.where(np.sin(2 * np.pi * 300 * t) >= 0, 1.0, -1.0).astype(np.float32)
    p = encode(root / "Bandcamp" / "Loud" / "hot.flac", "flac", 44100,
               data=np.stack([square, square]))
    tag_flac(p, title="Hot", artist="Loud", replaygain_track_gain="-9.00 dB",
             replaygain_track_peak="1.000000")

    # Disc subfolders take the album folder's cover
    alb2 = root / "CD" / "Two Discs"
    image(alb2 / "cover.jpg", color=(240, 160, 0), fmt="JPEG")
    encode(alb2 / "Day Disc" / "Track 1.flac", "flac", 44100, seconds=0.2)
    encode(alb2 / "Night Disc" / "Track 1.flac", "flac", 44100, seconds=0.2)

    # A folder whose name ends in dots
    encode(root / "Bandcamp" / "Demos..." / "demo.flac", "flac", 44100, seconds=0.2)

    # Folders that differ only in case get merged
    encode(root / "Kivara" / "a.flac", "flac", 44100, seconds=0.2)
    encode(root / "KIVARA" / "b.flac", "flac", 44100, seconds=0.2)

    # Things to ignore
    v = root / "Vinyl"
    v.mkdir(parents=True, exist_ok=True)
    (v / "Noise floor.aup3").write_bytes(b"audacity")
    (v / "notes.txt").write_text("hello")
    (root / "Music_2025-06-08.zip").write_bytes(b"PK\x03\x04")
    encode(v / ".hidden.flac", "flac", 44100, seconds=0.2)
    (root / ".directory").mkdir()
    encode(root / ".directory" / "x.flac", "flac", 44100, seconds=0.2)

    return {"red": red}
