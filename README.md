# alya-bangers
Music player for the Nintendo DS, requires a flashcart. :-)

Version 1.0, DS Edition.

> [!NOTE]
> The `.nds` and the converter are in the [releases](../../releases).
>
> There are pre-release builds in [GitHub Actions](../../actions/workflows/release.yml): open the newest run, the files are at the bottom under *Artifacts* (`nds`, `converter-Windows`, `converter-Linux`). Downloading them needs a GitHub account, and they're kept for 90 days. They are less tested than a release.
> If you want to build it yourself (see [Building](docs/development.md#building)).

<table>
<tr>
<td><img src="docs/img/player.png" width="256" alt="The player: a moonlit cover over its record on the top screen, the controls below"></td>
<td><img src="docs/img/browse.png" width="256" alt="Browsing: the artists, Nightglass selected, a song playing on the top screen"></td>
<td><img src="docs/img/search.png" width="256" alt="Search: typing night on the keyboard, the results on the top screen"></td>
</tr>
</table>

Your music gets put on the card from the PC with [alya-bangers-dsconv](docs/converter.md), which is in this repo too (`alya-bangers-dsconv/`). Everything else is the DS program.

---

> [!WARNING]
> I just wanted to listen to my music on my DS on a player I like (and looks really cool lol), so I vibecoded it... Expect a huge part of this repo to be AI generated/asssited. Like I said, I just want to enjoy my music. :-)

---

> [!NOTE]
> All pictures, here and in the docs, are the app itself, taken by its screen test with a made-up library. The artists and songs don't exist.

## Features:
- Browse by artist, album, title or folder
- Search in any script (ABC, かな, 拼音, АБВ)
- Player with a turning record and a visualizer
- Queue you can edit
- 3-band equalizer with your own profiles
- High cut and dither for cleaner highs
- Ratings and plays
- Themes with your own colours
- English and German
- A converter with a window for Windows and Linux

## Getting started:

1. Download `alya-bangers-….nds` and the converter for your PC from the [releases](../../releases).
2. Copy the `.nds` onto your flashcart's SD card, anywhere.
3. Open the converter, choose your music folder and the card, then Convert. ([More about the converter](docs/converter.md))
4. Start alya-bangers from the flashcart's menu (Pico Launcher on the DSpico).

## More:

- [Browsing](docs/browsing.md): the tabs, the bottom bar, adding to the queue, several artists
- [Player](docs/player.md): the turning record, ratings and plays, the queue, the equalizer, the lid
- [Search](docs/search.md): the keyboards, kanji and hanzi by their readings, filters
- [Settings and themes](docs/settings.md): sound, your own colours, language
- [Buttons](docs/buttons.md): what every button does
- [Converter](docs/converter.md): putting your music on the card, what's on the card
- [FAQ](docs/faq.md): why the music gets converted first, how much fits
- [How playback works](docs/how-it-works.md): technical stuff, in case you're curious
- [Development](docs/development.md): building, testing in melonDS, tests on the PC, releasing
- [Licences](docs/licences.md): the third-party code in the `.nds`

## Not there yet:

- Playlists
- Genre and other tags as tabs
- Search: genre and other tags (composer, label, mood …) found while typing, the other tags in + Filter, two of the same tag meaning "or"
- Colours from the cover
- Gapless playback
- A date and time format of its own (right now it follows the language)
- Arabic script (right to left, joined letters; Persian and Urdu with it)
- A battery warning you can hear

## Licence:

© 2026 Alya (alya-can-eat). GNU GPL v3 or later (`LICENSE`).

- Built-in fallback glyphs: GNU Unifont (GPL-2.0-or-later with the font embedding exception, see `LICENSES/`)
- Makefiles: BlocksDS templates (CC0)

Code: <https://github.com/alya-can-eat>

Third-party code in the `.nds`: [Licences](docs/licences.md)

### Trademarks:

This is an unofficial homebrew project. It's not affiliated with or endorsed by Nintendo. Nintendo DS and Nintendo DS Lite are trademarks of Nintendo.
