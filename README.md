# alya-bangers

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
  song's length on the right, or the time left: see settings). When the song
  changes in player mode, the cover slides off to the left over its record,
  the next cover grows out of the Next spot and its record rolls out. Going
  back plays it backwards; a song picked from a list or the queue first
  slides over the Next spot. Paused, the record spins down. In browse mode
  the covers behave like a stack in list order: a song further up is put on
  from the left, for a song further down the old cover is lifted off to the
  left (next and previous do the same). A song from another list is put on. The
  song's texts on the top screen fade out and the new ones fade in. In
  player mode: the record turning at 33⅓ rpm, a visualizer, the next song.
- **Add to queue:** hold a row for *Play now / Play next / Add to queue*, or
  turn on the add-to-queue button: then tapping rows adds them instead of
  playing them (an artist or album adds all its songs).
- **Queue (☰):** tap a song to jump to it, × removes it, hold and drag to
  move it.
- **Lid closed:** the screens go dark and the music keeps playing; L/R change
  songs (both can be changed in the settings).
- **Switching on** loads the last song at the same position, paused. Settings,
  queue and sort orders are kept in `/_nds/alya-bangers/state.bin` (one saved
  under the old name, `/_nds/dsplayer/state.bin`, is read once).
- **Settings (⋮):** pages that slide in from the right (‹, B or Left go
  back; A or Right open one):
  - *Playback:* ReplayGain, pause when the lid closes, L/R skip with the lid closed
  - *Browsing:* case-sensitive names, scroll all long rows
  - *Player:* time on the right (length or time left), cover background,
    shuffle by rating
  - *Theme:* the app's three colours (accent, background, text), picked on
    a colour triangle: the ring chooses the hue, the triangle (pure hue,
    white, black at its corners) how strong and bright. Every other colour
    is mixed from these three, and the whole app changes as you drag.
    Up / Down choose the colour, Left / Right turn its hue. *Reset* (always
    in fixed colours, findable with any theme) brings back the default.
  - *Regional* (language, date and time format): marked "later"
  - *About:* name, version, licence, where the code lives
- **The artist in the player** (start of the top line): tap it for the
  artist's albums in the browser, with the playing song's album selected.
  The line scrolls, so the tap goes by where the name is right then.
- **Rating and plays (ⓘ song info):** tap a star to give the song that many
  (the last lit one again: none), or use Left / Right. A song counts as
  played once you've listened to half of it or 4 minutes, whichever comes
  first (listening time: seeking ahead doesn't count), once per listen.
  Shown: your rating from the DS, else the one from the file's tags; plays
  from the tags plus the ones on the DS. They're kept in
  `/_nds/alya-bangers/stats.bin` (dsconv's library index is only read).
- **Shuffle by rating** (Settings → Player): shuffled, a song's chance to come next
  doubles with every star. Unrated songs count as 3 stars, so 5 stars come
  up 4× as often as them and 1 star a quarter as often. Over a long
  shuffle (a whole tab) your favourites play much more; a short queue still
  plays every song once per round, just the good ones earlier.
- Songs without a cover show a built-in picture (a moonlit beach).
- Names are drawn with the `font.bin` dsconv writes, so Japanese, Chinese,
  Cyrillic, Vietnamese and emoji show correctly.

| Button | Browsing | Player |
|---|---|---|
| Up / Down | move (faster when held) | |
| Left / Right | tab to the left / right | −10 s / +10 s |
| A | open / play | play / pause |
| B | the player | back to browsing |
| X | add-to-queue mode | song info |
| Y | row menu | queue |
| SELECT | player | back to browsing |
| START | play / pause | play / pause |
| L / R | previous / next song (L restarts the song after 3 s) | same |

Menus (settings, sort, the row menu, song info, queue) slide up when they
open and down when they close. Buttons light up when tapped, play and pause morph
into each other, the highlight glides from row to row (lists, menus, queue,
settings), the progress bars move smoothly and the seek knob grows while you
drag it. Settings pages slide in from the right. The record has a
soft purple glare at the top left. In player mode the top screen's
background is the cover, blurred and darkened (Settings → Player → Cover background).
A new song's background is made a slice per frame while the covers move and
faded in once they have settled. A very dark cover would melt into its dark
background (on the DS screen, everything below about 24 of 255 looks
black), so then the background gets lighter; covers that stand out keep it
as it is.
Closing the lid fades both screens to black before the light goes off;
opening it fades them back in. In menus: Up/Down, A chooses, B closes. If the card is ever too slow, a red
"drops: N" appears at the top. Tell me if you see it on real hardware.

## The SD card

```
/music/                      <- dsconv output folder
/music/.dsmusic/font.bin
/music/.dsmusic/library.idx
/_nds/alya-bangers/state.bin <- written by the player
/_nds/alya-bangers/stats.bin <- ratings and plays from the DS
/alya-bangers.nds            <- anywhere, start it from Pico Launcher
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

This produces `alya-bangers.nds`. The first run downloads the image (about
430 MB).

## Test in melonDS before using the card

melonDS is in nixpkgs (`nix-shell -p melonDS`).

1. Convert some music into a test folder: `dsconv ~/Music/some-album /tmp/dscard/music`
2. In melonDS's emulator settings (DLDI section): enable DLDI, set the folder
   to sync the SD image with to `/tmp/dscard`, and turn that sync on.
3. *File → Open ROM*: `alya-bangers.nds`.

An emulator is not the real DS: timing and SD speed differ, so the final test
is always the DS Lite.

`stats.bin`: a 16-byte header (`ABST`, version 1, header size, record
count, record size), then 24-byte records sorted by key (FNV-1a 64 of the
song's path below `/music`, plays, Unix time of the last play, rating:
0 = none, 1–5 stars, 6 = rated with no stars), then a check value:
CRC-32 of the header XOR CRC-32 of the records. It's written to `stats.new` and renamed, so a save cut
short never loses the old one.

## How playback works

- The ARM9 reads the card, the ARM7 drives the sound hardware (messages in
  `common/include/ipc.h`).
- Two hardware channels (left panned left, right panned right) loop over a
  ring of 8 blocks each (1 s). The channel timer is `0x10000 - 512`, so they
  play at 16756991 / 512 = 32728.5 Hz, exactly the DS mixer's rate.
- An ARM7 timer with the /1024 prescaler ticks once per sample (same crystal,
  33513982 / 1024 Hz) and interrupts after every 4096 samples; the ARM9 then
  loads the next block into the slot that just finished. A `.dsa` block holds
  all left samples, then all right samples, so each half is read straight
  from the card into its ring.
- Playback comes before drawing: while 3 blocks or more are ready ahead, the
  ARM9 reads one block per call (twice per frame), so the reading is spread
  out; when fewer are ready, it reads all that's missing at once. The ARM7
  (and with it the sound hardware) has priority on main RAM, so copying
  whole screens can't make the sound wait.
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
gapless playback, the Regional settings page (language, date and time
format).

## Licence

© 2026 Alya (alya-can-eat). GNU GPL v3 or later (`LICENSE`). Built-in fallback glyphs: GNU Unifont
(GPL-2.0-or-later with the font embedding exception, see `LICENSES/`).
Makefiles: BlocksDS templates (CC0).

Code: <https://github.com/alya-can-eat>
