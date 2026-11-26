# Converter

[‹ alya-bangers](../README.md)

<table>
<tr>
<td><img src="img/converter.png" width="400" alt="alya-bangers-dsconv after the check: 33 new songs, about 150 MB more on the card, Start"></td>
<td><img src="img/converter_run.png" width="400" alt="alya-bangers-dsconv converting: 14 of 33, the progress bar, the song it just did"></td>
</tr>
</table>

alya-bangers-dsconv puts your music on the card. Same colours and font as on the DS :-)

- Get it from the [releases](../../../releases) (or a pre-release build from [GitHub Actions](../../../actions/workflows/release.yml)): the `.exe` for Windows, the `.AppImage` for Linux. Then double-click it.
  - Windows: the first time "Windows protected your PC" shows up, because the file isn't signed. *More info*, then *Run anyway*.
  - Linux: allow executing it first (in the file's properties, or `chmod +x`).
  - NixOS: AppImages don't run as they are there. The flake is easier: `nix run github:alya-can-eat/alya-bangers?dir=alya-bangers-dsconv`, or `nix profile install` it for an entry in the app launcher.
- Choose your music folder and the DS card. A card that already has music from it is found by itself.
- Convert first checks what's new and whether it fits on the card, then Start.
  - Next time only new and changed songs get converted.
- Two files that would become the same song (like a FLAC and an Opus of it): it asks which one goes on the card and suggests the better one. The answer is remembered.
- *Remove songs that are gone from the music folder*: also deletes the songs on the card you don't have anymore.
- Stop finishes the songs in progress. Next time it goes on from there.
- English or German, like the computer.
- It also works on the command line, more in [alya-bangers-dsconv](../alya-bangers-dsconv/).

## SD card:

```
/music/                      <- what the converter writes
/music/.dsmusic/font.bin
/music/.dsmusic/library.idx
/_nds/alya-bangers/state.bin <- written by the player
/_nds/alya-bangers/stats.bin <- ratings and plays from the DS
/_nds/alya-bangers/eq.bin    <- your equalizer profiles
/alya-bangers.nds            <- anywhere, start it from Pico Launcher
```

Put your music on it with the converter (above), or on the command line:

```sh
alya-bangers-dsconv ~/Music /run/media/alya/DSPICO/music
```
