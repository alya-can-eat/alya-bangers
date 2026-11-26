"""Command line entry point."""

from __future__ import annotations

import argparse
import sys
import wave
from pathlib import Path

from .constants import __version__

USAGE = """\
alya-bangers-dsconv                              open the window
alya-bangers-dsconv SOURCE OUTPUT [options]      convert a music folder for the DS
alya-bangers-dsconv inspect FILE.dsa [--cover X.png]
alya-bangers-dsconv export FILE.dsa OUT.wav
alya-bangers-dsconv index OUTPUT                 rebuild library index and font only
"""


def _convert(argv: list[str]) -> int:
    p = argparse.ArgumentParser(
        prog="alya-bangers-dsconv", usage=USAGE,
        description="Convert a music folder into .dsa files for the DS player, keeping the "
                    "folder structure. Run it again any time: only new or changed files are converted.")
    p.add_argument("source", type=Path, help="your music folder")
    p.add_argument("output", type=Path, help="where the converted tree goes, e.g. the music folder on the SD card")
    p.add_argument("-n", "--dry-run", action="store_true",
                   help="show what would happen (renames, collisions, work) without writing anything")
    p.add_argument("-y", "--yes", action="store_true",
                   help="never ask: in a collision keep the suggested (best quality) file")
    p.add_argument("--delete", action="store_true",
                   help="remove converted files whose source no longer exists")
    p.add_argument("-j", "--jobs", type=int, default=None, help="parallel conversions (default: all CPU cores)")
    p.add_argument("--force", action="store_true", help="reconvert everything")
    p.add_argument("--no-index", action="store_true", help="skip building the library index and font")
    p.add_argument("-q", "--quiet", action="store_true", help="only print problems and the summary")
    p.add_argument("--version", action="version", version=f"alya-bangers-dsconv {__version__}")
    a = p.parse_args(argv)
    from .convert import run_convert
    return run_convert(a.source, a.output, dry_run=a.dry_run, yes=a.yes, delete=a.delete,
                       jobs=a.jobs, force=a.force, build_index=not a.no_index, quiet=a.quiet)


def _inspect(argv: list[str]) -> int:
    p = argparse.ArgumentParser(prog="alya-bangers-dsconv inspect", description="Show the header and tags of a .dsa file.")
    p.add_argument("file", type=Path)
    p.add_argument("--cover", type=Path, help="also save the cover as a PNG")
    a = p.parse_args(argv)
    from .cover import bgr555_to_image
    from .dsa import open_dsa
    f = open_dsa(a.file)
    h = f.header
    print(f"{a.file}")
    print(f"  format version {h['version']}, {h['channels']} channel(s), 16-bit PCM")
    print(f"  rate {f.sample_rate:.3f} Hz (clock {h['sound_clock_hz']} / {h['timer_divider']})")
    print(f"  {h['frame_count']} frames = {h['duration_ms'] / 1000:.3f} s, {h['block_count']} blocks of {h['block_frames']}")
    print(f"  audio at byte {h['audio_offset']}, {h['audio_size']} bytes")
    print(f"  cover: {'yes' if h['flags'] & 1 else 'no'}")
    if h["flags"] & 2:
        print(f"  replaygain track: {h['rg_track_gain_mdb'] / 1000:+.2f} dB, peak {h['rg_track_peak_q16'] / 65536:.4f}")
    if h["flags"] & 4:
        print(f"  replaygain album: {h['rg_album_gain_mdb'] / 1000:+.2f} dB, peak {h['rg_album_peak_q16'] / 65536:.4f}")
    print("  tags:")
    for k in sorted(f.tags):
        for v in f.tags[k]:
            v1 = v.replace("\n", " ")
            print(f"    {k} = {v1[:200]}{'…' if len(v1) > 200 else ''}")
    if a.cover:
        data = f.read_cover()
        if data is None:
            print("no cover in this file", file=sys.stderr)
            return 1
        bgr555_to_image(data, h["cover_width"]).save(a.cover)
        print(f"cover saved to {a.cover}")
    return 0


def _export(argv: list[str]) -> int:
    p = argparse.ArgumentParser(prog="alya-bangers-dsconv export",
                                description="Write the audio of a .dsa file as WAV, to listen on the PC.")
    p.add_argument("file", type=Path)
    p.add_argument("out", type=Path)
    a = p.parse_args(argv)
    from .dsa import open_dsa
    f = open_dsa(a.file, with_tags=False)
    pcm = f.read_audio()
    with wave.open(str(a.out), "wb") as w:
        w.setnchannels(f.channels)
        w.setsampwidth(2)
        w.setframerate(round(f.sample_rate))
        w.writeframes(pcm.astype("<i2").tobytes())
    print(f"wrote {a.out} ({len(pcm)} frames at {round(f.sample_rate)} Hz)")
    return 0


def _index(argv: list[str]) -> int:
    p = argparse.ArgumentParser(prog="alya-bangers-dsconv index", description="Rebuild library.idx and font.bin.")
    p.add_argument("output", type=Path)
    a = p.parse_args(argv)
    from .index import build_library_files
    s = build_library_files(a.output.expanduser().resolve())
    print(f"{s['tracks']} tracks, {s['albums']} albums, {s['values']} values, "
          f"{s.get('readings', 0)} readings, {s['glyphs']} glyphs")
    for rel, err in s["unreadable"]:
        print(f"  unreadable: {rel}: {err}")
    return 0


def _window() -> int:
    try:
        from .gui import run
    except ImportError as e:
        print(USAGE, end="")
        print(f"\n(Without arguments the window opens, but it needs PySide6: {e})", file=sys.stderr)
        return 2
    return run()


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        return _window()    # double-clicked: the window
    cmds = {"inspect": _inspect, "export": _export, "index": _index}
    try:
        if argv and argv[0] in cmds:
            return cmds[argv[0]](argv[1:])
        return _convert(argv)
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 130
    except BrokenPipeError:  # output piped into e.g. `head`
        import os
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        return 0


if __name__ == "__main__":
    sys.exit(main())
