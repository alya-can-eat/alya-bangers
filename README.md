# dsplayer

A music player for the Nintendo DS (made for the DS Lite with a DSpico). It
plays the `.dsa` files written by [dsconv](../dsconv) from the `/music` folder
of the SD card.

Version 0.2: the interface from the mockup. Browse by artist, album, title
or folder, a player screen with a turning record, a queue you can edit.

## What it does

- **Bottom screen, browsing:** tabs Artist · Album · Title · Folder · Search.
  Picking an artist opens their albums, picking an album its songs; the tabs
  then show your choice (tap the name to go back to that list, tap the small
  × to clear it). Lists slide sideways when you switch tabs
  or open an artist, album or folder. Tapping a song plays the list it is in. Artists that differ
  only in case ("KIVARA", "Kivara") are one artist unless you change that in
  the settings.
- **Bottom bar:** play/pause, next, add to queue, ⮟ (the player), sort,
  brightness (4 levels), ⋮ settings.
- **Player:** tap ⮟ and the browser slides down until only the tabs are left.
  Seek bar (drag it), shuffle, previous, play/pause, next, repeat (off, all,
  one), ⓘ song info, ☰ queue, ⮝ back to browsing (tapping a tab does too).
- **Top screen:** time and date, cover, title, artist, album, progress (the
  song's length on the right, or the time left: see settings). In
  player mode: the record turning at 33⅓ rpm, a visualizer, the next song.
- **Add to queue:** hold a row for *Play now / Play next / Add to queue*, or
  turn on the add-to-queue button: then tapping rows adds them instead of
  playing them (an artist or album adds all its songs).
- **Queue (☰):** tap a song to jump to it, × removes it, hold and drag to
  move it.
- **Lid closed:** the screens go dark and the music keeps playing; L/R change
  songs (both can be changed in the settings).
- **Switching on** loads the last song at the same position, paused. Settings,
  queue and sort orders are kept in `/_nds/dsplayer/state.bin`.
- Songs without a cover show a built-in picture (a moonlit beach).
- Names are drawn with the `font.bin` dsconv writes, so Japanese, Chinese,
  Cyrillic, Vietnamese and emoji show correctly.

| Button | Browsing | Player |
|---|---|---|
| Up / Down | move (faster when held) | |
| Left / Right | page up / down | −10 s / +10 s |
| A | open / play | play / pause |
| B | back one tab or folder | back to browsing |
| X | add-to-queue mode | song info |
| Y | row menu | queue |
| SELECT | player | back to browsing |
| START | play / pause | play / pause |
| L / R | previous / next song (L restarts the song after 3 s) | same |

In menus: Up/Down, A chooses, B closes. If the card is ever too slow, a red
"drops: N" appears at the top. Tell me if you see it on real hardware.

## The SD card

```
/music/                  <- dsconv output folder
/music/.dsmusic/font.bin
/music/.dsmusic/library.idx
/_nds/dsplayer/state.bin <- written by the player
/dsplayer.nds            <- anywhere, start it from Pico Launcher
```

```sh
dsconv ~/Music /run/media/alya/DSPICO/music
```

## Build

With the official BlocksDS container (needs Podman or Docker; on NixOS add
`virtualisation.podman.enable = true;` to your configuration):

```sh
./build.sh
```

This produces `dsplayer.nds`. The first run downloads the image (about
430 MB).

## Test in melonDS before using the card

melonDS is in nixpkgs (`nix-shell -p melonDS`).

1. Convert some music into a test folder: `dsconv ~/Music/some-album /tmp/dscard/music`
2. In melonDS's emulator settings (DLDI section): enable DLDI, set the folder
   to sync the SD image with to `/tmp/dscard`, and turn that sync on.
3. *File → Open ROM*: `dsplayer.nds`.

An emulator is not the real DS: timing and SD speed differ, so the final test
is always the DS Lite.

## How playback works

- The ARM9 reads the card, the ARM7 drives the sound hardware (messages in
  `common/include/ipc.h`).
- Two hardware channels (left panned left, right panned right) loop over a
  ring of 4 blocks each (0.5 s). The channel timer is `0x10000 - 512`, so they
  play at 16756991 / 512 = 32728.5 Hz, exactly the DS mixer's rate.
- An ARM7 timer with the /1024 prescaler ticks once per sample (same crystal,
  33513982 / 1024 Hz) and interrupts after every 4096 samples; the ARM9 then
  loads the next block into the slot that just finished. A `.dsa` block holds
  all left samples, then all right samples, so each half is read straight
  from the card into its ring.
- Pause and seek restart the stream at a block boundary (0.125 s steps).

## Tests on the PC

`tests/host` compiles the ARM9 code for the PC against a mock libnds and a
simulated ARM7 that "plays" the ring buffers:

```sh
cd tests/host
make test FILES="/tmp/dscard/music/some/song.dsa"   # checks the audio sample by sample
make screens-run CARD=/tmp/dscard/music             # clicks through the UI, checks it, screenshots in shots/
```

They need a gcc that can build 32-bit programs (`-m32`), e.g.
`nix-shell -p gcc_multi`.

`tools/gen_builtin_font.py` regenerates the built-in glyphs (ASCII, UI
symbols and icons) from dsconv's copy of Unifont, `tools/gen_default_cover.py`
the picture shown for songs without a cover, `tools/gen_icon.py` the
icon of the `.nds` file.

## Not yet

Search, playlists, genre and other tags as tabs, colours from the cover,
gapless playback.

## Licence

MIT. Built-in fallback glyphs: GNU Unifont (GPL-2.0-or-later with the font
embedding exception). Makefiles: BlocksDS templates (CC0).
