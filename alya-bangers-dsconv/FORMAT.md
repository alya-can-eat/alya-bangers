# alya-bangers-dsconv file formats (version 1)

This is the contract between the converter (`alya-bangers-dsconv`) and the DS player
(alya-bangers).
There is **no backwards compatibility**: when a format changes, its version
number goes up, the converter reconverts everything on the next run, and the
player only accepts its own version.

All integers are **little-endian** (the DS's native byte order). Offsets are
in bytes from the start of the file. "u32" = unsigned 32-bit, "i32" = signed,
`NONE` = `0xFFFFFFFF`.

On the card:

```
<music root>/                 mirror of the source folder tree
    Some Artist/Album/01 Song.dsa
    ...
<music root>/.dsmusic/
    library.idx               index of every .dsa below the music root
    font.bin                  every glyph the player needs to draw
    manifest.json             converter bookkeeping (the player ignores it)
    last-run.txt              human-readable report of the last run
```

Folder and file names are FAT32-safe (see "Names" at the end).

---

## 1. `.dsa` — one song

```
+-------------------+  0
| header (512 B)    |
+-------------------+  tags_offset (= 512)
| tags              |
+-------------------+  cover_offset (4-byte aligned)   only if FLAG_COVER
| cover 128x128     |
| thumb 32x32       |
+-------------------+  audio_offset (multiple of 512)
| audio blocks      |
+-------------------+  audio_offset + audio_size = end of file
```

### 1.1 Header

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | char[4] | magic | `DSA1` |
| 4 | u16 | version | 1 |
| 6 | u16 | header_size | 512 |
| 8 | u32 | flags | bit 0 `FLAG_COVER`, bit 1 `FLAG_RG_TRACK`, bit 2 `FLAG_RG_ALBUM` |
| 12 | u32 | sound_clock_hz | 16756991 (ARM7 bus clock / 2) |
| 16 | u16 | timer_divider | 512 |
| 18 | u8 | channels | 1 or 2 |
| 19 | u8 | sample_format | 1 = signed 16-bit PCM |
| 20 | u32 | frame_count | real frames (a frame = one sample per channel) |
| 24 | u32 | block_frames | 4096 |
| 28 | u32 | block_count | `ceil(frame_count / block_frames)` |
| 32 | u32 | duration_ms | `round(frame_count * 1000 / rate)` |
| 36 | u32 | tags_offset | |
| 40 | u32 | tags_size | |
| 44 | u32 | cover_offset | 0 without cover |
| 48 | u32 | cover_size | 32768 |
| 52 | u16 | cover_width | 128 |
| 54 | u16 | cover_height | 128 |
| 56 | u32 | thumb_offset | |
| 60 | u32 | thumb_size | 2048 |
| 64 | u16 | thumb_width | 32 |
| 66 | u16 | thumb_height | 32 |
| 68 | u32 | audio_offset | multiple of 512 |
| 72 | u32 | audio_size | `block_count * block_frames * 2 * channels` |
| 76 | i32 | rg_track_gain_mdb | ReplayGain track gain in milli-dB (valid if bit 1) |
| 80 | u32 | rg_track_peak_q16 | track peak × 65536, 0 = unknown |
| 84 | i32 | rg_album_gain_mdb | (valid if bit 2) |
| 88 | u32 | rg_album_peak_q16 | |
| 92–507 | | reserved | zero |
| 508 | u32 | header_crc32 | CRC-32 (zlib/IEEE) of bytes 0–507 |

**Sample rate.** The rate is `sound_clock_hz / timer_divider`
= 32728.498046875 Hz. Program each sound channel with timer register
`0x10000 - timer_divider` (= `0xFE00`). The DS mixer produces one output
sample every 512 sound-clock ticks, so at this rate the hardware plays every
sample exactly once and never resamples. (Source: melonDS `SPU.cpp`: internal
rate 16756991 Hz, one mixer sample per 512 cycles.)

**ReplayGain** uses the ReplayGain 2 reference (−18 LUFS). Opus `R128_*_GAIN`
tags (relative to −23 LUFS) are converted (+5 dB). Gains are *not* applied to
the audio; the player applies the one it wants (track or album mode),
e.g. through the channel volume registers.

**Headroom.** Loud masters often have inter-sample peaks above full scale
after resampling. Such songs are turned down just enough that the highest
sample stays 2 LSB below full scale (no clipping, not even from the dither).
The amount is in the tag `~pregain` (e.g. `-1.77 dB`), and the header's gains
are already raised by it and its peaks scaled, so playback *with* ReplayGain
sounds exactly as it would from the source. The text tags `replaygain_*`
still describe the source.

### 1.2 Tags

```
u32 entry_count
entry × entry_count:
    u8  key_length
    u16 value_length
    u8  key[key_length]      UTF-8, lowercase
    u8  value[value_length]  UTF-8, NFC-normalized, no NUL terminator
```

Entries are sorted by key. A multi-value tag is the same key repeated, in
order (`artist = A`, `artist = B`). Values are at most 16 KiB.

Key names follow Quod Libet: `title`, `artist`, `album`, `albumartist`,
`tracknumber` (e.g. `3/12`), `discnumber`, `date`, `genre`, `composer`,
`performer`, `grouping`, `version`, `discsubtitle`, the sort keys
`titlesort`, `artistsort`, `albumsort`, `albumartistsort`, `composersort`,
`replaygain_track_gain`, `musicbrainz_albumid`, `~#rating` (0..1),
`~#playcount`, and anything else the file contains (lowercased).

Keys starting with `~` are added by the converter:

| Key | Meaning |
|---|---|
| `~source` | source path relative to the source root (original spelling) |
| `~codec` | FLAC, MP3, Opus, AAC, Vorbis, PCM, ALAC, … |
| `~bitrate` | source bit rate in bit/s |
| `~source_rate`, `~source_channels`, `~source_bits` | source format |
| `~cover` | `embedded`, `folder:<file name>` or `none` |
| `~pregain` | only if the song was turned down to avoid clipping, e.g. `-1.77 dB` |

A track without a `title` tag has none here either; the player shows the
file name instead (as Quod Libet does).

### 1.3 Cover and thumbnail

Raw pixels, row by row, top-left first. Each pixel is a u16
`r | g << 5 | b << 10 | 0x8000` (5 bits per channel, bit 15 set = opaque):
exactly what a DS 16-bit bitmap background expects, so the data can be copied
straight into VRAM. Images are cropped to a centred square, scaled
(Lanczos) and Floyd–Steinberg dithered. Transparent areas become black.

Cover choice: embedded front cover → any embedded picture → in the song's
folder `cover.*`, `folder.*`, `front.*`, `album.*`, `albumart.*` (in this
order, any case) → the only image in the folder → none. Songs in a disc
subfolder (`Disc 1`, `CD2`, `Disc 01 [Side:Dawn]`, `Day Disc`, `1`, …)
without their own image use the parent (album) folder's cover the same way.

### 1.4 Audio

Signed 16-bit PCM, TPDF-dithered, in blocks of `block_frames` frames. Inside a
block the channels are **not interleaved**:

```
block 0: L[0..4095] R[0..4095]      (stereo: 16384 bytes = 32 sectors)
block 1: L[4096..8191] R[...]
...
```

Mono files have one channel per block (8192 bytes). Every block starts on a
512-byte sector boundary. The last block is padded with exact zeros after
`frame_count`.

Reading frame `n`: block `b = n / 4096`, index `i = n % 4096`; channel `c`
is at `audio_offset + b * block_bytes + c * 8192 + i * 2`.

Streaming idea for the player: two hardware channels (left panned left,
right panned right), each looping over its own ring buffer of whole blocks;
read one block from the card, copy its left half into the left ring and its
right half into the right ring.

---

## 2. `library.idx` — the index

Built by reading back all `.dsa` files, so it always matches the card. The
player can load it whole (roughly 100–200 bytes per song).

### 2.1 Header (64 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | magic `DSIX` |
| 4 | u16 | version (1) |
| 6 | u16 | header_size (64) |
| 8 | u32 | track_count |
| 12 | u32 | key_count |
| 16 | u32 | value_count |
| 20 | u32 | album_count |
| 24 | u32 | tagref_count |
| 28 | u32 | strings_offset |
| 32 | u32 | strings_size |
| 36 | u32 | keys_offset |
| 40 | u32 | values_offset |
| 44 | u32 | tracks_offset |
| 48 | u32 | tagrefs_offset |
| 52 | u32 | albums_offset |
| 56 | u32 | album_tracks_offset |
| 60 | u32 | readings_offset (0 = none, see 2.9) |

All tables start on a 4-byte boundary.

### 2.2 Strings

UTF-8, each terminated by a NUL byte. Other tables refer to strings by their
byte offset inside this block.

### 2.3 Keys

`key_count × u32` string offsets, sorted by name. A key id is the position in
this table.

### 2.4 Values

`value_count × u32` string offsets. **Values are stored in sort order**, so
comparing two value ids compares the strings: ignore case and accents first
("Équinoxe" next to "Equinoxe"), numbers by value ("Track 2" before
"Track 10"), then exact string as tie-break. Every distinct string appears
once. Sorting anything by any tag on the DS is an integer sort.

### 2.5 Tracks (48 bytes each, sorted by path like a file manager)

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | path (string offset, relative to the music root, `/`-separated) |
| 4 | u32 | duration_ms |
| 8 | u32 | first tagref |
| 12 | u16 | tagref count |
| 14 | u16 | flags: bit 0 has cover, bit 1 has title |
| 16 | u32 | album id or NONE |
| 20 | u16 | disc number (0 = none) |
| 22 | u16 | track number (0 = none) |
| 24 | u32 | title value id or NONE |
| 28 | u32 | title sort value id (`titlesort`, else = title) |
| 32 | u32 | first artist value id or NONE |
| 36 | u32 | artist sort value id (`artistsort`, else = artist) |
| 40 | u16 | year (first 4-digit number of date/originaldate, 0 = none) |
| 42 | u16 | reserved |
| 44 | u32 | reserved |

### 2.6 Tagrefs (8 bytes each)

`u16 key id, u16 reserved, u32 value id`. A track's tagrefs are contiguous,
sorted by key; multi-value tags appear several times. They hold every tag
except long free text and technical keys (comment, description, lyrics,
encoder, replaygain, musicbrainz ids, `~source` …) and values longer than
200 bytes. Those stay available in the `.dsa` itself. `~codec`, `~#rating`
and `~#playcount` are included.

This is what Quod Libet-style browsing runs on: list the distinct values of
any key (genre, mood, composer, …), filter tracks by them, and sort with the
value ids.

### 2.7 Albums (32 bytes each, sorted by album sort name, then album artist, then year)

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | album name value id |
| 4 | u32 | album sort value id (`albumsort`, else = name) |
| 8 | u32 | album artist value id or NONE (`albumartist`; else the artist if all tracks share it; else NONE = "Various") |
| 12 | u32 | album artist sort value id or NONE |
| 16 | u32 | first entry in album_tracks |
| 20 | u16 | track count |
| 22 | u16 | year (earliest) |
| 24 | u32 | track whose cover represents the album, or NONE |
| 28 | u32 | reserved |

Grouping (like Quod Libet): tracks belong to the same album when they share
the `album` tag, the `albumartist` tag and `musicbrainz_albumid` (if any).
Tracks without `albumartist` are grouped per folder instead, where a folder
named like `Disc 1`, `CD2`, `Day Disc` or `1` counts as its parent, so
multi-disc rips stay one album. **Tracks without an `album` tag belong to no
album.**

### 2.8 Album tracks

`u32` track indices, per album in disc number, then track number, then path
order.

### 2.9 Readings (for search)

Names with kanji or hanzi (values of `title`, `artist`, `album`,
`albumartist`) get two readings, so the DS can find them without knowing
Chinese or Japanese: 林星月 by "lin", 春風レター by "はる".

```
u32 count
count × { u32 value id, u32 kana, u32 pinyin }    sorted by value id
```

`kana` and `pinyin` are string offsets (or `NONE`). Kana is the hiragana
reading from pykakasi (`はるかぜれたー`), pinyin the reading with tone marks
from pypinyin, other characters kept (`xīng zhī hé (feat. lín xīng yuè)`).
Every name gets both: whether a name is Chinese or Japanese isn't known, and
a wrong reading only means a match nobody types. Players that don't know
this table ignore the offset; without pykakasi and pypinyin installed the
converter writes no table.

---

## 3. `font.bin` — glyphs

Contains every character that appears in the index (values, keys, paths)
plus printable ASCII, a few UI symbols (… • · ♪ ° – — × ← ↑ → ↓ �) and the
umlauts of the German interface (ä ö ü Ä Ö Ü). Glyphs
come from GNU Unifont 15.1.01: 16 pixels high, 8 or 16 pixels wide.

### 3.1 Header (32 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | magic `DSFT` |
| 4 | u16 | version (1) |
| 6 | u16 | header_size (32) |
| 8 | u8 | glyph height (16) |
| 9 | u8[3] | reserved |
| 12 | u32 | glyph_count |
| 16 | u32 | table_offset |
| 20 | u32 | bitmap_offset |
| 24 | u32 | bitmap_size |
| 28 | u32 | reserved |

### 3.2 Glyph table (12 bytes per glyph, sorted by code point, then variant)

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | Unicode code point |
| 4 | u32 | bitmap offset (relative to bitmap_offset) |
| 8 | u8 | width in pixels (8 or 16) |
| 9 | u8 | flags: bit 0 combining mark, bit 1 Japanese variant |
| 10 | i8 | x offset (combining marks: draw at pen x + offset, don't advance) |
| 11 | u8 | reserved |

### 3.3 Bitmaps

1 bit per pixel, row by row, most significant bit = leftmost pixel. A row is
1 byte (8 px wide) or 2 bytes (16 px wide), so a glyph is 16 or 32 bytes.

### 3.4 Lookup rules for the player

1. Decode UTF-8 to code points; binary-search the table.
2. **Japanese variants.** Chinese and Japanese draw some Han characters
   differently. If the string being drawn contains kana (U+3040–30FF,
   U+31F0–31FF, U+FF66–FF9F) and the table has a second entry for the same
   code point with flag bit 1, use that one. Otherwise use the first.
3. Missing code point: draw U+FFFD (always present).

---

## Names

Converted names mirror the source names with these changes, so they are
valid on FAT32 (and on Windows):

- `" * / : < > ? \ |` become `＂ ＊ ／ ： ＜ ＞ ？ ＼ ｜` (full-width lookalikes)
- control characters are removed
- leading spaces are removed; a name can't *end* in a dot or space, so a
  folder name loses trailing spaces and its trailing dots become `．`
  (file names end in `.dsa`, so dots before it stay: `S.T.A.R.S..dsa`)
- names are NFC-normalized
- `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9` get a `_`
- names are cut to 255 UTF-16 code units (extension kept)
- the extension becomes `.dsa`

Names that then differ only in upper/lower case (FAT32 treats them as the
same) are collisions; for files, the converter asks which one to keep, and
folders are merged.
