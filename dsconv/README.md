# dsconv

Converts a music folder (FLAC, Opus, MP3, AAC/M4A, Vorbis, WAV, AIFF, WavPack)
into `.dsa` files that [alya-bangers](../README.md), the Nintendo DS player,
can stream straight into its sound hardware, keeping the folder structure. It also writes a library index (for
browsing by artist, album, genre or any other tag) and a font with exactly the
characters your library needs.

The formats are specified in [FORMAT.md](FORMAT.md).

## Run it

Nothing to install by hand: every dependency comes as a prebuilt package with
its native libraries included (FFmpeg via PyAV, libsoxr, numpy, mutagen,
Pillow, and the GNU Unifont glyphs are bundled).

**NixOS / Nix** (flakes enabled), from this folder:

```sh
nix run . -- ~/Music /run/media/alya/DSPICO/music
```

To have `dsconv` as a normal command: `nix profile install .`

**Any other Linux, macOS or Windows** with [uv](https://docs.astral.sh/uv/):

```sh
uvx --from . dsconv ~/Music /path/to/card/music
```

(On NixOS use the Nix route; uv's prebuilt wheels need a normal Linux loader.)

## First run

```sh
dsconv ~/Music /run/media/alya/DSPICO/music --dry-run
```

shows every rename and collision without writing anything. Then run it for
real without `--dry-run`. Later runs only convert new or changed files, so
just run the same command again after changing your library.

## What it does

- **Mirrors your folders.** `~/Music/Bandcamp/X/01 Song.flac` becomes
  `music/Bandcamp/X/01 Song.dsa`.
- **Converts the audio** to 16-bit PCM at 32728.5 Hz, the DS mixer's own rate,
  so the DS never resamples (high-quality soxr resampling, dithered).
  About 470 MB per hour of music. Loud masters whose peaks would clip after
  resampling are turned down just enough (their ReplayGain values are
  adjusted to match).
- **Keeps all tags** (multi-value too) under Quod Libet's key names, and
  ReplayGain values for the player to apply.
- **Covers:** embedded front cover first, then `cover.*` / `folder.*` /
  `front.*` / `album.*` in the folder, then the folder's only image. Disc
  subfolders (`Disc 1`, `Day Disc`, `1`, …) use the album folder's cover, so one
  `cover.jpg` next to them is enough.
  Stored ready for the DS screen (128×128 and 32×32).
- **Ignores** anything that isn't audio (Audacity projects, text files, zips…)
  and hidden files/folders, and tells you what it skipped.
- **FAT32-safe names:** `:` `*` `?` … become full-width lookalikes
  (`00：00`), a folder name can't end in a dot (`Demos...` becomes
  `Demos．．．`), reserved names get a `_`. Nothing to do by hand.
- **Collisions** (e.g. `Song.flac` and `Song.opus` in the same folder, or names
  that only differ in upper/lower case): it asks which one to keep, suggesting
  the better format (lossless > Opus > AAC/Vorbis > MP3, then bitrate). Your
  answer is remembered. `-y` takes the suggestion without asking.
- **Safe to interrupt:** Ctrl-C finishes the files in progress, saves, and the
  next run continues. Files are written to a temporary name first, so the card
  never holds half a song.

## Options

| Option | |
|---|---|
| `-n`, `--dry-run` | show what would happen, write nothing |
| `-y`, `--yes` | never ask; keep the suggested file in collisions |
| `--delete` | remove `.dsa` files whose source is gone (and folders left empty) |
| `-j N`, `--jobs N` | parallel conversions (default: all cores) |
| `--force` | reconvert everything |
| `--no-index` | skip the index and font |
| `-q`, `--quiet` | only problems and the summary |

Other commands:

```sh
dsconv inspect song.dsa [--cover cover.png]   # header, tags, cover
dsconv export song.dsa song.wav               # listen to the converted audio on the PC
dsconv index /path/to/card/music              # rebuild index and font only
```

After each run the full report is in `<output>/.dsmusic/last-run.txt`.

## Changing the format

There is no backwards compatibility on purpose. When `FORMAT_VERSION` or
`CONVERTER_REVISION` in `src/dsconv/constants.py` changes, the next run
reconverts everything automatically.

## Development

```sh
nix develop -c pytest          # or: uv run --with pytest pytest
```

The tests build a small library with the awkward cases of a real one
(colons and asterisks in names, NFD file names, case collisions, disc
subfolders, 24-bit WAV with RIFF tags, embedded vs. folder covers, CJK and
emoji titles, 5.1 audio, damaged files) and check the output down to the audio
frequencies, block layout, index and font.

## Licence

© 2026 Alya (alya-can-eat). Code: GNU GPL v3 or later (`LICENSE`). Bundled font data: GNU Unifont,
GPL-2.0-or-later with the font embedding exception (see `LICENSES/`).

Code: <https://github.com/alya-can-eat>
