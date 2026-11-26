"""The window, driven offscreen: choose, check, convert, stop, ask."""

import os
import shutil
import time
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
pytest.importorskip("PySide6.QtWidgets")

from PySide6.QtCore import QSettings  # noqa: E402

from alya_bangers_dsconv import cli, gui  # noqa: E402
from alya_bangers_dsconv.dsa import open_dsa  # noqa: E402
from alya_bangers_dsconv.manifest import Manifest  # noqa: E402

SINGLES = "Singles"


@pytest.fixture(scope="module")
def app(tmp_path_factory):
    QSettings.setDefaultFormat(QSettings.IniFormat)
    QSettings.setPath(QSettings.IniFormat, QSettings.UserScope, str(tmp_path_factory.mktemp("settings")))
    return gui.make_app()


def wait(app, cond, seconds=180):
    end = time.monotonic() + seconds
    while not cond():
        assert time.monotonic() < end, "timed out"
        app.processEvents()
        time.sleep(0.01)


def window(app, src, card):
    w = gui.Window()
    w.show()
    w.src, w.card, w.found_card = src, card, False
    w._options_changed()
    return w


def small_library(library, tmp_path):
    """A few songs with one clash (Tide Drive as FLAC and Opus)."""
    src, _ = library
    out = tmp_path / "src" / SINGLES
    out.mkdir(parents=True)
    for n in ("Tide Drive.flac", "Tide Drive.opus", "Kid 🎵 Song.opus", "Dear Wren....flac"):
        shutil.copy2(src / SINGLES / n, out / n)
    return tmp_path / "src"


def test_colours_and_font_are_the_ds_apps(app):
    # palette.c's default theme and its mixes, 5 bits widened like the DS screen shows them
    assert (gui.C["bg"], gui.C["text"], gui.C["acc"]) == ("#210029", "#ffefff", "#f78cff")
    assert gui.C["sel"] == gui.C["sel"].lower() and len(gui.C) == 10
    assert app.font().family() == "Unifont alya-bangers" and app.font().pixelSize() == 16


def test_version_label():
    from alya_bangers_dsconv.constants import version_label
    assert version_label("1.1.dev0") == "pre-1.1" and version_label("1.0") == "1.0"


def test_card_folder(tmp_path):
    assert gui.output_for(tmp_path / "DSPICO") == tmp_path / "DSPICO" / "music"
    assert gui.output_for(tmp_path / "DSPICO" / "Music") == tmp_path / "DSPICO" / "Music"
    assert gui.gb(1.25e9) == "1.2 GB" and gui.gb(150e6) == "150.0 MB" and gui.gb(-3e9) == "3.0 GB"


def test_check_ask_convert_and_nothing_left(app, library, tmp_path, monkeypatch):
    src = small_library(library, tmp_path)
    card = tmp_path / "card" / "music"
    asked = []

    def answer(parent, dest_rel, options):
        asked.append((dest_rel, options))
        return 1, False    # not the suggested FLAC: the Opus file

    monkeypatch.setattr(gui, "ask_clash", answer)
    w = window(app, src, card)
    assert w.go.isEnabled() and w.state == "idle"
    w._go()
    wait(app, lambda: w.state != "checking")
    assert w.state == "ready" and w.go.text() == gui.T["start"]
    assert len(asked) == 1 and asked[0][0] == f"{SINGLES}/Tide Drive.dsa"
    assert gui.T["suggested"] in asked[0][1][0] and ".flac" in asked[0][1][0]
    assert w.status.text().startswith("3 new")    # 4 files, the clash is one song
    assert not card.exists(), "checking writes nothing"

    w._go()
    assert w.state == "running" and w.go.text() == gui.T["stop"]
    assert not w.src_btn.isEnabled()
    wait(app, lambda: w.state == "idle")
    assert w.status.text() == gui.T["done"].format(n=3)
    assert open_dsa(card / SINGLES / "Tide Drive.dsa").tags["~codec"] == ["Opus"]
    assert (card / ".dsmusic" / "library.idx").exists()
    assert "Building library index" in w.details.toPlainText()

    # again: the choice is remembered, nothing to do
    monkeypatch.setattr(gui, "ask_clash", lambda *a: pytest.fail("asked twice"))
    w._go()
    wait(app, lambda: w.state != "checking")
    assert w.state == "idle" and w.status.text() == gui.T["nothing"]
    w.close()


def test_gone_songs_and_space(app, library, tmp_path, monkeypatch):
    monkeypatch.setattr(gui, "ask_clash", lambda *a: (0, True))
    src = small_library(library, tmp_path)
    card = tmp_path / "card" / "music"
    w = window(app, src, card)
    w._go()
    wait(app, lambda: w.state != "checking")
    assert "MB more on the card" in w.status2.text()
    w._go()
    wait(app, lambda: w.state == "idle")
    (src / SINGLES / "Kid 🎵 Song.opus").unlink()
    w._go()
    wait(app, lambda: w.state != "checking")
    assert w.state == "idle" and gui.T["orphans_n"].format(n=1) in w.status2.text()
    w.delete.setChecked(True)      # now removing it is the work to do
    w._go()
    wait(app, lambda: w.state != "checking")
    assert w.state == "ready" and gui.T["remove_n"].format(n=1) in w.status2.text()
    assert "emptier" in w.status2.text()
    w._go()
    wait(app, lambda: w.state == "idle")
    assert not (card / SINGLES / "Kid 🎵 Song.dsa").exists()
    w.close()


def test_doesnt_fit(app, library, tmp_path, monkeypatch):
    monkeypatch.setattr(gui, "ask_clash", lambda *a: (0, True))
    monkeypatch.setattr(gui, "free_space", lambda p: 1000)
    w = window(app, small_library(library, tmp_path), tmp_path / "card" / "music")
    w._go()
    wait(app, lambda: w.state != "checking")
    assert w.state == "idle" and "doesn't fit" in w.status2.text()
    w.close()


def test_stop_keeps_what_is_done(app, library, tmp_path, monkeypatch):
    src, _ = library
    card = tmp_path / "card" / "music"
    w = window(app, src, card)
    monkeypatch.setattr(gui, "ask_clash", lambda *a: (0, True))
    w._go()
    wait(app, lambda: w.state != "checking")
    total = len(w.prep.todo)
    w._go()
    wait(app, lambda: w.bar.value > 0)
    w._go()                                  # Stop
    assert w.state == "stopping" and not w.go.isEnabled()
    wait(app, lambda: w.state == "idle")
    assert w.status.text() == gui.T["stopped"]
    done = len(Manifest(card).files)
    assert 0 < done < total
    w.close()


def test_closing_while_converting_asks(app, library, tmp_path, monkeypatch):
    monkeypatch.setattr(gui, "ask_clash", lambda *a: (0, True))
    src, _ = library
    w = window(app, src, tmp_path / "card" / "music")
    w._go()
    wait(app, lambda: w.state != "checking")
    w._go()
    monkeypatch.setattr(gui, "ask_quit", lambda parent: False)
    w.close()
    assert w.isVisible() and w.state == "running"
    monkeypatch.setattr(gui, "ask_quit", lambda parent: True)
    w.close()
    assert w.state == "stopping"
    wait(app, lambda: not w.isVisible())


def test_bad_folders_are_shown(app, tmp_path):
    w = window(app, tmp_path / "missing", tmp_path / "card" / "music")
    w._go()
    wait(app, lambda: w.state != "checking")
    assert w.status.text().startswith(gui.T["error"].split("{")[0]) and "not found" in w.status.text()
    w.close()


def test_no_arguments_open_the_window(monkeypatch):
    opened = []
    monkeypatch.setattr(gui, "run", lambda: opened.append(1) or 0)
    assert cli.main([]) == 0 and opened == [1]
