# Development

[‹ alya-bangers](../README.md)

For building it yourself, testing and making releases.

## Building:

With the official BlocksDS container. You need Podman or Docker for it.

> [!TIP]
> On NixOS add `virtualisation.podman.enable = true;` to your configuration.

```sh
./build.sh
```

This makes `alya-bangers.nds`. The first run downloads the image, which is about 430 MB.

GitHub Actions builds it the same way for the releases (see [Releasing](#releasing)).

## Testing in melonDS:

You can try it in melonDS before using the real card. melonDS is in nixpkgs (`nix-shell -p melonDS`).

1. Convert some music into a test folder: choose `/tmp/dscard` as the card in the converter, or `alya-bangers-dsconv ~/Music/some-album /tmp/dscard/music`
2. In melonDS's emulator settings (DLDI section): enable DLDI, set the folder to sync the SD image with to `/tmp/dscard` and turn that sync on.
3. *File → Open ROM*: `alya-bangers.nds`

> [!WARNING]
> An emulator is not a real DS! Timing and SD speed are different, so the final test always has to be on the DS Lite.

## Tests on the PC:

`tests/host` compiles the ARM9 code for the PC against a mock libnds and a simulated ARM7 that "plays" the ring buffers.

```sh
python3 tests/card/make_test_card.py /tmp/testcard   # the test library, made up
cd tests/host
make test_player && ./test_player "/tmp/testcard/card/music/CD/Track 01.dsa"   # checks the audio sample by sample
make test_eq && ./test_eq   # the equalizer, high cut and dither against exact references, curves, no clipping
make screens-run CARD=/tmp/testcard/card/music   # clicks through the UI, checks it, screenshots in shots/
```

- The screen test expects exactly this made-up library (its artists, albums and counts).
- `make_test_card.py` needs ffmpeg and the converter's Python dependencies.
- The converter's own tests: `cd alya-bangers-dsconv && nix develop -c pytest` (the window's tests run offscreen).
- The tests need a gcc that can build 32-bit programs (`-m32`), e.g. `nix-shell -p gcc_multi`.
- The screen test also draws every screen in German and checks that every text exists in both languages and fits its place. All texts of the interface are in `arm9/source/i18n.c` (English and German side by side).

### Generated stuff:

- README pictures: `tests/demo/make_demo_card.py` builds the made-up library, the screen test's demo mode (`./screens CARD out demo`) shoots the screens and films, and `tools/gen_readme_images.py` turns them into `docs/img/`. (The three commands are in the script.)
- `tools/gen_builtin_font.py`: the built-in glyphs (ASCII, UI symbols, icons, the umlauts and the keyboards' letters) from the converter's copy of Unifont
- `tools/gen_fold_tables.py`: the tables search uses to ignore accents
- `tools/gen_eq_tables.py`: the equalizer's filters for every gain, the high cut's filters and the noise shaping (needs scipy, e.g. `nix-shell -p python3Packages.scipy`)
- `tools/gen_default_cover.py`: the picture for songs without a cover
- `tools/gen_icon.py`: the icon of the `.nds` file
- `alya-bangers-dsconv/tools/build_app_icon.py`: the converter's icon, the same picture
- `alya-bangers-dsconv/tools/build_window_font.py`: the converter's font, Unifont as squares (needs fontTools)
- `alya-bangers-dsconv/tools/window_pictures.py`: the converter's pictures in the docs

## Releasing:

Between releases the version is the next one with "pre" in front: `pre-1.1` on the DS, `1.1.dev0` for the converter (its window shows `pre-1.1` too).

1. Change the version (e.g. `pre-1.1` and `1.1.dev0` both become `1.1`) in:
   - `arm9/source/app.h` (`APP_VERSION`, shown on the About page)
   - `alya-bangers-dsconv/src/alya_bangers_dsconv/constants.py`, `alya-bangers-dsconv/pyproject.toml` and `alya-bangers-dsconv/flake.nix` (the converter's window and file names)
   - the top of `README.md`
2. Commit that and push a tag with the same version: `git tag 1.1 && git push origin 1.1`
3. `.github/workflows/release.yml` then builds:
   - `alya-bangers.nds`, with `./build.sh` in the BlocksDS container (in the release as `alya-bangers-1.1.nds`)
   - the converter's `.exe` and AppImage with `alya-bangers-dsconv/packaging/build.py`, which also tries both (converts a song, opens the window)
   - and puts all three into a draft release.
4. Try that `.nds` on the DS, give the release a title, then publish it.
5. Set the next pre-version (`pre-1.2`, `1.2.dev0`), so builds in between don't look like the release.

> [!NOTE]
> The tag alone doesn't change the version inside the files: without step 1 the About page still says the old one.

Started by hand ([Actions › release](../../../actions/workflows/release.yml) › Run workflow) it only makes the three files, without a release. Those are the pre-release builds.

## Not there yet, in detail:

- A battery warning you can hear
- Start another launcher from alya-bangers (Pico Launcher, TWiLight Menu++, AKMenu-Next, nds-hb-menu, YSMenu …):
  - With hbmenu's loader: it reads the `.nds` from the card, gives it alya-bangers' DLDI driver and its path, and restarts both CPUs into it. So it works on most flashcarts, not only the DSpico, and can be tested in melonDS.
  - Only launchers that are a `.nds` file, no flashcart menus like `_DS_MENU.DAT`, no games directly (that stays the launcher's job).
  - Settings › Launcher picks the `.nds`, a button fades the music out, saves the position and starts it.
  - Together with alya-bangers as `_picoboot.nds` on the DSpico: the music player first, the games one tap away.
