"""Decoding (PyAV/FFmpeg) and resampling (libsoxr) to the DS rate."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import av
import numpy as np
import soxr

from .constants import SAMPLE_RATE

LOSSLESS = {"flac", "alac", "wavpack", "ape", "tta", "mlp", "truehd"}


@dataclass
class Probe:
    codec: str
    sample_rate: int
    channels: int
    bits: int          # lossless bit depth, 0 if unknown/lossy
    bitrate: int       # bits per second, 0 if unknown
    duration_s: float

    @property
    def lossless(self) -> bool:
        return self.codec in LOSSLESS or self.codec.startswith("pcm_")

    def describe(self) -> str:
        name = {"aac": "AAC", "mp3": "MP3", "mp3float": "MP3", "opus": "Opus",
                "vorbis": "Vorbis", "flac": "FLAC", "alac": "ALAC"}.get(
            self.codec, "PCM" if self.codec.startswith("pcm_") else self.codec)
        parts = [name]
        if self.lossless:
            if self.bits:
                parts.append(f"{self.bits}-bit")
            parts.append(f"{self.sample_rate / 1000:g} kHz")
        elif self.bitrate:
            parts.append(f"{round(self.bitrate / 1000)} kbit/s")
        m, s = divmod(int(round(self.duration_s)), 60)
        parts.append(f"{m}:{s:02d}")
        return ", ".join(parts)

    def quality_rank(self) -> tuple:
        """Higher is better: lossless > Opus > AAC = Vorbis > MP3 > other."""
        if self.lossless:
            return (5, self.bits, self.sample_rate, 0)
        cls = {"opus": 4, "aac": 3, "vorbis": 3, "mp3": 2, "mp3float": 2}.get(self.codec, 1)
        return (cls, 0, 0, self.bitrate)


def _audio_stream(container):
    for s in container.streams:
        if s.type == "audio":
            return s
    raise ValueError("no audio stream")


def probe(path: Path) -> Probe:
    with av.open(str(path)) as c:
        s = _audio_stream(c)
        cc = s.codec_context
        bits = 0
        if s.codec_context.name in LOSSLESS or cc.name.startswith("pcm_"):
            bits = getattr(cc, "bits_per_raw_sample", 0) or getattr(cc, "bits_per_coded_sample", 0) or 0
        rate = cc.sample_rate or 0
        channels = cc.channels or 0
        bitrate = s.bit_rate or c.bit_rate or 0
        if s.duration is not None and s.time_base is not None:
            dur = float(s.duration * s.time_base)
        elif c.duration is not None:
            dur = c.duration / 1_000_000
        else:
            dur = 0.0
        return Probe(cc.name, rate, channels, int(bits or 0), int(bitrate or 0), dur)


def decode_resampled(path: Path) -> tuple[Probe, int, Iterator[np.ndarray]]:
    """Open a file; return (probe, output channels, iterator of float32 (n, ch) chunks).

    Mono stays mono, everything with more than one channel becomes stereo
    (FFmpeg's standard downmix). Encoder delay and padding (MP3, AAC, Opus
    pre-skip) and Opus output gain are applied by FFmpeg while decoding.
    """
    info = probe(path)
    out_ch = 1 if info.channels == 1 else 2

    def gen():
        with av.open(str(path)) as c:
            stream = _audio_stream(c)
            stream.thread_type = "AUTO"
            in_rate = stream.codec_context.sample_rate
            fmt = av.AudioResampler(format="fltp", layout="mono" if out_ch == 1 else "stereo",
                                    rate=in_rate)
            rs = soxr.ResampleStream(in_rate, SAMPLE_RATE, out_ch, dtype="float32", quality="VHQ")

            def push(frames, last=False):
                for fr in frames:
                    a = fr.to_ndarray()  # (channels, samples), planar float
                    if a.size == 0:
                        continue
                    y = rs.resample_chunk(np.ascontiguousarray(a.T, dtype=np.float32))
                    if len(y):
                        yield y
                if last:
                    y = rs.resample_chunk(np.zeros((0, out_ch), dtype=np.float32), last=True)
                    if len(y):
                        yield y

            for frame in c.decode(stream):
                yield from push(fmt.resample(frame))
            yield from push(fmt.resample(None), last=True)

    return info, out_ch, gen()
