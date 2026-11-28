"""Playlists from the music folder onto the card, for the DS app."""

import shutil
from pathlib import Path

from alya_bangers_dsconv.convert import run_convert
from alya_bangers_dsconv.playlists import PL_NAME, ds_name, file_name

SINGLES = "Singles"


def small_library(library, tmp_path):
    src, _ = library
    out = tmp_path / "src" / SINGLES
    out.mkdir(parents=True)
    for n in ("Tide Drive.flac", "Tide Drive.opus", "Kid 🎵 Song.opus", "Dear Wren....flac"):
        shutil.copy2(src / SINGLES / n, out / n)
    return tmp_path / "src"


def card_playlists(card: Path) -> Path:
    return card / ".dsmusic" / "playlists"


def test_new_playlists_go_on_the_card(library, tmp_path):
    src = small_library(library, tmp_path)
    # made on a Windows computer, with a BOM, a stream and a song that isn't there
    (src / "Night drive.m3u8").write_bytes(
        "\ufeff#EXTM3U\n#EXTINF:12,Kid - Song\nSingles/Kid 🎵 Song.opus\nSingles\\Dear Wren....flac\n"
        "C:\\Users\\alya\\Music\\Singles\\Tide Drive.opus\n../elsewhere/gone.mp3\nhttp://radio.example/stream\n"
        "Singles/Kid 🎵 Song.opus\n".encode("utf-8"))
    # in a subfolder: relative to it, and a file:// URL
    gym = (src / SINGLES / "Tide Drive.flac").resolve().as_uri()
    (src / SINGLES / "gym.m3u").write_text(f"#PLAYLIST:Gym\nKid 🎵 Song.opus\n{gym}\n", encoding="utf-8")
    card = tmp_path / "card" / "music"
    pl = card_playlists(card)
    pl.mkdir(parents=True)
    (pl / "mine.m3u").write_text("#EXTM3U\n#PLAYLIST:GYM\nSingles/Dear Wren....dsa\n", encoding="utf-8")

    assert run_convert(src, card, yes=True, jobs=2, quiet=True) == 0
    night = (pl / "Night drive.m3u").read_text(encoding="utf-8").splitlines()
    assert night == ["#EXTM3U", "#PLAYLIST:Night drive", f"{SINGLES}/Kid 🎵 Song.dsa",
                     f"{SINGLES}/Dear Wren....dsa", f"{SINGLES}/Tide Drive.dsa"]
    for line in night[2:]:
        assert (card / line).is_file()
    # the card has a "GYM" already: its copy stays, nothing else is added
    assert (pl / "mine.m3u").read_text(encoding="utf-8") == "#EXTM3U\n#PLAYLIST:GYM\nSingles/Dear Wren....dsa\n"
    assert sorted(p.name for p in pl.iterdir()) == ["Night drive.m3u", "mine.m3u"]
    report = (card / ".dsmusic" / "last-run.txt").read_text(encoding="utf-8")
    assert "Playlists: 1 new on the card, 1 already there" in report
    assert "Night drive: 3 songs (2 not found in the music folder)" in report
    assert ".m3u" not in report.split("Ignored:")[-1].split("\n")[0]

    # changed on the PC afterwards: the card's copy (maybe changed on the DS) stays
    (src / "Night drive.m3u8").write_text("Singles/Dear Wren....flac\n", encoding="utf-8")
    assert run_convert(src, card, yes=True, jobs=2, quiet=True) == 0
    assert (pl / "Night drive.m3u").read_text(encoding="utf-8").splitlines() == night


def test_any_music_folder_has_its_own(library, tmp_path):
    # not called "music", not at the card's top: its playlists go with it
    src = small_library(library, tmp_path)
    (src / "a.m3u").write_text("Singles/Kid 🎵 Song.opus\n", encoding="utf-8")
    out = tmp_path / "card" / "Audio" / "Musik"
    assert run_convert(src, out, yes=True, jobs=2, quiet=True) == 0
    assert (card_playlists(out) / "a.m3u").read_text(encoding="utf-8").splitlines()[2] == f"{SINGLES}/Kid 🎵 Song.dsa"
    assert not (tmp_path / "card" / "_nds").exists() and not (out.parent / "_nds").exists()


def test_names_like_the_ds():
    assert len(ds_name("a" * 40).encode()) == PL_NAME - 1
    assert ds_name("ö" * 20) == "ö" * 15             # not cut inside a character
    assert file_name('AC/DC: "best"?', set()) == "AC_DC_ _best__.m3u"
    assert file_name("Mix...", set()) == "Mix.m3u"
    assert file_name("Mix", {"mix.m3u"}) == "Mix 2.m3u"
