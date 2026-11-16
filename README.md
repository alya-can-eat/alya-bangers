# dsplayer

A music player for the Nintendo DS (made for the DS Lite with a DSpico). It
plays the `.dsa` files written by [dsconv](../dsconv) from the `/music` folder
of the SD card.

Version 0.1: a folder browser that plays music. Plain on purpose; it gets
pretty later.

## What it does

- **Bottom screen:** the folders and songs of `/music`, folders first, in
  natural order (Track 2 before Track 10), no limit on entries per folder.
  Names are drawn with the `font.bin` dsconv writes, so Japanese, Chinese,
  Cyrillic, Vietnamese and emoji show correctly.
- **Top screen:** cover, title, artist · album, elapsed / total time.
- Plays the selected song, then the next one in the same folder.
- **Lid closed:** both screens go dark, the music keeps playing.

| Button | |
|---|---|
| Up / Down | move |
| Left / Right | page up / down |
| A | open folder / play song |
| B | back to the parent folder |
| START | pause / resume |
| L / R | previous / next song (L restarts the song after 3 s) |
| X / Y | +10 s / −10 s |
| SELECT | jump to the folder of the song that's playing |

If the card is ever too slow, a red "dropouts: N" appears in the top-left
corner. Tell me if you see it on real hardware.

## The SD card

```
/music/                  <- dsconv output folder
/music/.dsmusic/font.bin
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
make screens-run CARD=/tmp/dscard/music             # screenshots of the UI in shots/
```

They need a gcc that can build 32-bit programs (`-m32`), e.g.
`nix-shell -p gcc_multi`.

## Not in 0.1

Artist/album/tag views (from `library.idx`), playlists, shuffle,
ReplayGain, gapless playback, touch, a nicer look.

## Licence

MIT. Built-in fallback glyphs: GNU Unifont (GPL-2.0-or-later with the font
embedding exception). Makefiles: BlocksDS templates (CC0).
