"""The window: pick the music folder and the DS card, check, convert.

Looks like the DS app: its default theme's colours (mixed the same way as
arm9/source/palette.c) and its font (GNU Unifont, as squares).
"""

from __future__ import annotations

import gzip
import html
import os
import sys
import threading
import time
from importlib import resources
from pathlib import Path

from PySide6.QtCore import QByteArray, QLocale, QObject, QRectF, QSettings, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QFontDatabase, QGuiApplication, QIcon, QPainter, QPixmap
from PySide6.QtWidgets import (
    QAbstractButton, QApplication, QButtonGroup, QDialog, QFileDialog, QFrame, QHBoxLayout, QLabel,
    QPlainTextEdit, QPushButton, QSizePolicy, QVBoxLayout, QWidget,
)

from .constants import DATA_DIR, version_label
from .convert import Hooks, Prepared, SetupError, execute, free_space, prepare, space_needed

APP_NAME = "alya-bangers-dsconv"
FONT_PX = 16
REPO_URL = "https://github.com/alya-can-eat/alya-bangers"

# ------------------------------------------------------------ colours ----

# Ultraviolet, the DS app's default theme: background, text, accent (5 bits each)
BG5, TEXT5, ACC5 = (4, 0, 5), (31, 29, 31), (30, 17, 31)


def _toward(a, b, k):
    """k/256 of the way from a to b, rounded, like palette.c."""
    return tuple((x * (256 - k) + y * k + 128) >> 8 for x, y in zip(a, b))


def _hex(c5) -> str:
    return "#" + "".join(f"{(v << 3) | (v >> 2):02x}" for v in c5)


C = {
    "bg": _hex(BG5), "text": _hex(TEXT5), "acc": _hex(ACC5),
    "zebra": _hex(_toward(BG5, TEXT5, 5)), "panel": _hex(_toward(BG5, TEXT5, 15)),
    "line": _hex(_toward(BG5, TEXT5, 45)), "faint": _hex(_toward(BG5, TEXT5, 65)),
    "dim": _hex(_toward(BG5, TEXT5, 133)), "sel": _hex(_toward(BG5, ACC5, 76)),
    "acc_dim": _hex(_toward(BG5, ACC5, 137)),
}

# --------------------------------------------------------------- text ----

TEXTS = {
    "en": {
        "music": "Music folder", "card": "DS card", "choose": "Choose…",
        "not_chosen": "not chosen yet", "free": "{0} free", "found": "found by itself",
        "remove": "Remove songs that are gone from the music folder",
        "pick_both": "Choose your music folder and the DS card.",
        "ready": "Convert first checks what's new.",
        "convert": "Convert", "start": "Start", "stop": "Stop", "stopping": "Stopping after the songs in progress…",
        "checking": "Checking your music…",
        "counts": "{new} new · {changed} changed · {same} up to date",
        "remove_n": "{n} songs will be removed.", "orphans_n": "{n} songs on the card are gone from the music folder.",
        "fits": "About {need} more on the card, {free} free.", "emptier": "The card gets about {need} emptier.",
        "no_fit": "That doesn't fit: about {need} more needed, but only {free} free.",
        "nothing": "Everything on the card is up to date.",
        "converting": "Converting: {done} of {total}", "left": "about {t} left", "less_min": "less than a minute left",
        "scanning": "Looking at your music…", "index": "Building the library index and font…",
        "done": "Done: {n} songs converted.", "done_failed": "Done, but {f} songs failed. See Details.",
        "stopped": "Stopped. Press Convert to go on where it stopped.",
        "eject": "Eject the card before you take it out.",
        "details": "Details",
        "clash_title": "Two files, one song",
        "clash_text": "These would all become<br>{name}<br>Which one goes on the card?",
        "suggested": "suggested", "same_for_rest": "The suggested one for all the others too",
        "keep": "Keep", "error": "Can't start: {msg}",
        "quit_title": "Stop converting?",
        "quit_text": "The songs in progress get finished first. Next time it goes on from there.",
        "quit_yes": "Stop and close", "quit_no": "Keep going",
        "music_dialog": "Your music folder", "card_dialog": "The DS card (or its music folder)",
        "about": "About", "edition": "PC converter", "tagline": "Puts your music on the DS card",
        "licence": "Licence: GNU GPL v3", "free_sw": "Free software, no warranty",
        "parts": "Uses FFmpeg, Qt, libsoxr, GNU Unifont and more, each under its own licence.",
        "close": "Close",
    },
    "de": {
        "music": "Musikordner", "card": "DS-Karte", "choose": "Wählen…",
        "not_chosen": "noch nicht gewählt", "free": "{0} frei", "found": "von selbst gefunden",
        "remove": "Songs entfernen, die nicht mehr im Musikordner sind",
        "pick_both": "Wähle deinen Musikordner und die DS-Karte.",
        "ready": "Konvertieren prüft zuerst, was neu ist.",
        "convert": "Konvertieren", "start": "Start", "stop": "Stopp",
        "stopping": "Stoppt nach den Songs, die gerade laufen…",
        "checking": "Deine Musik wird geprüft…",
        "counts": "{new} neu · {changed} geändert · {same} aktuell",
        "remove_n": "{n} Songs werden entfernt.", "orphans_n": "{n} Songs auf der Karte sind nicht mehr im Musikordner.",
        "fits": "Etwa {need} mehr auf der Karte, {free} frei.", "emptier": "Die Karte wird etwa {need} leerer.",
        "no_fit": "Das passt nicht: etwa {need} mehr nötig, aber nur {free} frei.",
        "nothing": "Auf der Karte ist alles aktuell.",
        "converting": "Konvertiert: {done} von {total}", "left": "noch etwa {t}",
        "less_min": "noch weniger als eine Minute",
        "scanning": "Deine Musik wird angeschaut…", "index": "Bibliothek und Schrift werden erstellt…",
        "done": "Fertig: {n} Songs konvertiert.", "done_failed": "Fertig, aber {f} Songs gingen nicht. Siehe Details.",
        "stopped": "Gestoppt. Mit Konvertieren geht es dort weiter.",
        "eject": "Karte auswerfen, bevor du sie herausnimmst.",
        "details": "Details",
        "clash_title": "Zwei Dateien, ein Song",
        "clash_text": "Diese würden alle zu<br>{name}<br>Welche kommt auf die Karte?",
        "suggested": "Vorschlag", "same_for_rest": "Bei allen anderen auch den Vorschlag nehmen",
        "keep": "Behalten", "error": "Geht nicht: {msg}",
        "quit_title": "Konvertieren stoppen?",
        "quit_text": "Die Songs, die gerade laufen, werden fertig. Nächstes Mal geht es dort weiter.",
        "quit_yes": "Stoppen und schliessen", "quit_no": "Weitermachen",
        "music_dialog": "Dein Musikordner", "card_dialog": "Die DS-Karte (oder ihr Musikordner)",
        "about": "Über", "edition": "PC-Konverter", "tagline": "Bringt deine Musik auf die DS-Karte",
        "licence": "Lizenz: GNU GPL v3", "free_sw": "Freie Software, ohne Garantie",
        "parts": "Braucht FFmpeg, Qt, libsoxr, GNU Unifont und mehr, jedes unter seiner eigenen Lizenz.",
        "close": "Schliessen",
    },
}


def _lang() -> str:
    return "de" if QLocale.system().language() == QLocale.Language.German else "en"


T = TEXTS["en"]


def gb(n: float) -> str:
    """Bytes as GB or MB, like the DS app writes numbers."""
    n = abs(n)
    return f"{n / 1e9:.1f} GB" if n >= 1e9 else f"{max(n / 1e6, 0.1):.1f} MB" if n >= 1e5 else "0.1 MB"


def minutes(sec: float) -> str:
    m = int(sec // 60 + 0.5)
    if m < 1:
        return ""
    return f"{m} min" if m < 60 else f"{m // 60} h {m % 60:02d} min"


# --------------------------------------------------------------- card ----

def output_for(chosen: Path) -> Path:
    """Where the songs go when this folder was chosen: its music folder,
    unless it is one already."""
    return chosen if chosen.name.lower() == "music" else chosen / "music"


def _mount_points() -> list[Path]:
    if sys.platform == "win32":
        import ctypes
        import string
        k32 = ctypes.windll.kernel32
        k32.SetErrorMode(1)  # no "insert a disk" boxes for empty card readers
        mask = k32.GetLogicalDrives()
        return [Path(f"{d}:\\") for i, d in enumerate(string.ascii_uppercase)
                if mask >> i & 1 and i >= 3 and k32.GetDriveTypeW(f"{d}:\\") == 2]   # 2: removable
    user = os.environ.get("USER") or os.environ.get("USERNAME") or ""
    bases = [Path("/Volumes")] if sys.platform == "darwin" else \
        [Path("/run/media") / user, Path("/media") / user, Path("/media"), Path("/mnt")]
    out = []
    for b in bases:
        try:
            out += [p for p in sorted(b.iterdir()) if p.is_dir()]
        except OSError:
            pass
    return out


def find_cards() -> list[Path]:
    """Music folders on removable drives that this converter wrote before."""
    found = []
    for m in _mount_points():
        try:
            for name in ("music", "Music", "MUSIC"):
                if (m / name / DATA_DIR).is_dir():
                    found.append(m / name)
                    break
        except OSError:
            pass
    return found


# ------------------------------------------------------------ widgets ----

class Toggle(QAbstractButton):
    """The DS app's switch: a pill, the knob on the right and the pill lit when on."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setCheckable(True)
        self.setCursor(Qt.PointingHandCursor)
        self.setFixedSize(QSize(40, 22))

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        on = self.isChecked()
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(C["acc"] if on else C["line"]))
        p.drawRoundedRect(QRectF(1, 2, 38, 18), 9, 9)
        p.setBrush(QColor(C["text"] if on else C["dim"]))
        p.drawEllipse(QRectF(23 if on else 5, 5, 12, 12))


class Bar(QWidget):
    """Progress like the player's: a thin line, lit up to where it is."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.value = 0.0
        self.setFixedHeight(10)

    def set(self, v: float) -> None:
        self.value = max(0.0, min(1.0, v))
        self.update()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(Qt.NoPen)
        r = QRectF(0, 2, self.width(), 6)
        p.setBrush(QColor(C["faint"]))
        p.drawRoundedRect(r, 3, 3)
        if self.value > 0:
            p.setBrush(QColor(C["acc"]))
            p.drawRoundedRect(QRectF(0, 2, max(6.0, self.width() * self.value), 6), 3, 3)


def label(text: str = "", kind: str = "", wrap: bool = False) -> QLabel:
    lb = QLabel(text)
    if kind:
        lb.setObjectName(kind)
    lb.setWordWrap(wrap)
    lb.setTextInteractionFlags(Qt.NoTextInteraction)
    return lb


def line() -> QFrame:
    f = QFrame()
    f.setObjectName("line")
    f.setFixedHeight(1)
    return f


def header(title: str, right: str = "", icon: str = "") -> QFrame:
    h = QFrame()
    h.setObjectName("header")
    lay = QHBoxLayout(h)
    lay.setContentsMargins(12, 6, 12, 6)
    if icon:
        lay.addWidget(label(icon, "acc"))
    lay.addWidget(label(title))
    lay.addStretch(1)
    if right:
        lay.addWidget(label(right, "dim"))
    return h


def stylesheet(family: str) -> str:
    return f"""
    * {{ font-family: "{family}"; font-size: {FONT_PX}px; }}
    QWidget {{ background: {C['bg']}; color: {C['text']}; }}
    QLabel#dim {{ color: {C['dim']}; }}
    QLabel#acc {{ color: {C['acc']}; }}
    QFrame#header, QFrame#header QLabel {{ background: {C['panel']}; }}
    QFrame#line {{ background: {C['line']}; }}
    QPushButton {{ background: transparent; color: {C['text']}; border: 1px solid {C['line']};
                  border-radius: 6px; padding: 3px 12px; }}
    QPushButton:hover {{ border-color: {C['acc_dim']}; }}
    QPushButton:pressed {{ background: {C['sel']}; }}
    QPushButton:disabled {{ color: {C['faint']}; border-color: {C['faint']}; }}
    QPushButton#primary {{ background: {C['acc']}; color: {C['bg']}; border: 1px solid {C['acc']}; }}
    QPushButton#primary:pressed {{ background: {C['acc_dim']}; }}
    QPushButton#primary:disabled {{ background: {C['faint']}; color: {C['dim']}; border-color: {C['faint']}; }}
    QPushButton#link {{ border: none; color: {C['dim']}; padding: 3px 0; text-align: left; }}
    QPushButton#link:hover {{ color: {C['text']}; }}
    QPushButton#choice {{ border: none; border-radius: 0; text-align: left; padding: 4px 10px; }}
    QPushButton#choice:checked {{ background: {C['sel']}; color: {C['acc']}; }}
    QPlainTextEdit {{ background: {C['zebra']}; color: {C['dim']}; border: 1px solid {C['line']};
                     border-radius: 6px; padding: 4px; selection-background-color: {C['sel']};
                     selection-color: {C['acc']}; }}
    QScrollBar:vertical {{ background: transparent; width: 10px; margin: 2px; }}
    QScrollBar::handle:vertical {{ background: {C['line']}; border-radius: 3px; min-height: 24px; }}
    QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
    QScrollBar::add-page, QScrollBar::sub-page {{ background: transparent; }}
    QToolTip {{ background: {C['panel']}; color: {C['text']}; border: 1px solid {C['line']}; }}
    """


# ------------------------------------------------------------ dialogs ----

class Ask(QDialog):
    """A small question in the app's style."""

    def __init__(self, parent, title: str, text: str, choices: list[str] | None = None,
                 extra: str | None = None, buttons: tuple[str, ...] = ("OK",)):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.setModal(True)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 12)
        lay.setSpacing(8)
        lay.addWidget(header(title))
        body = QVBoxLayout()
        body.setContentsMargins(12, 4, 12, 0)
        body.setSpacing(6)
        q = label(text, "", wrap=True)
        q.setTextFormat(Qt.RichText if "<" in text else Qt.PlainText)
        body.addWidget(q)
        self.group = QButtonGroup(self)
        for i, c in enumerate(choices or []):
            b = QPushButton(c)
            b.setObjectName("choice")
            b.setCheckable(True)
            b.setChecked(i == 0)
            self.group.addButton(b, i)
            body.addWidget(b)
        self.extra = None
        if extra:
            row = QHBoxLayout()
            self.extra = Toggle()
            row.addWidget(label(extra), 1)
            row.addWidget(self.extra)
            body.addLayout(row)
        row = QHBoxLayout()
        row.addStretch(1)
        self.answer = None
        for i, t in enumerate(buttons):
            b = QPushButton(t)
            if i == 0:
                b.setObjectName("primary")
                b.setDefault(True)
            b.clicked.connect(lambda _=False, i=i: self._done(i))
            row.addWidget(b)
        body.addLayout(row)
        lay.addLayout(body)
        self.setMinimumWidth(460)

    def _done(self, i: int) -> None:
        self.answer = i
        self.accept()


class About(QDialog):
    """Like the DS app's About page: the name, the version, the licence, where
    the code is."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"{T['about']} {APP_NAME}")
        self.setModal(True)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 12)
        lay.setSpacing(0)
        lay.addWidget(header(T["about"], icon="ⓘ"))
        body = QVBoxLayout()
        body.setContentsMargins(24, 18, 24, 0)
        body.setSpacing(6)
        name = label(APP_NAME, "acc")
        name.setStyleSheet(f"font-size: {2 * FONT_PX}px;")  # twice the size, like on the DS: 2×2 pixels
        rows = [
            (name, 10),
            (label(f"Version {version_label()} · {T['edition']}", "dim"), 12),
            (label(T["tagline"]), 0),
            (label("© 2026 Alya (alya-can-eat)"), 12),
            (label(T["licence"]), 0),
            (label(T["free_sw"], "dim"), 12),
        ]
        for w, gap in rows:
            w.setAlignment(Qt.AlignHCenter)
            body.addWidget(w)
            body.addSpacing(gap)
        shown = REPO_URL.split("://", 1)[1]
        self.link = QLabel(f'<a href="{REPO_URL}" style="color:{C["acc"]}; text-decoration:none">{shown}</a>')
        self.link.setTextFormat(Qt.RichText)
        self.link.setOpenExternalLinks(True)
        self.link.setTextInteractionFlags(Qt.TextBrowserInteraction)
        self.link.setCursor(Qt.PointingHandCursor)
        self.link.setAlignment(Qt.AlignHCenter)
        body.addWidget(self.link)
        body.addSpacing(14)
        parts = label(T["parts"], "dim", wrap=True)
        parts.setAlignment(Qt.AlignHCenter)
        body.addWidget(parts)
        body.addSpacing(10)
        row = QHBoxLayout()
        row.addStretch(1)
        ok = QPushButton(T["close"])
        ok.setObjectName("primary")
        ok.setDefault(True)
        ok.clicked.connect(self.accept)
        row.addWidget(ok)
        body.addLayout(row)
        lay.addLayout(body)
        self.setMinimumWidth(520)


def ask_clash(parent, dest_rel: str, options: list[str]) -> tuple[int, bool]:
    """Which file goes on the card (index into options), and whether to take
    the suggestion for all the others too. Tests replace this."""
    name = f'<span style="color:{C["acc"]}">{html.escape(dest_rel)}</span>'
    d = Ask(parent, T["clash_title"], T["clash_text"].format(name=name),
            choices=options, extra=T["same_for_rest"], buttons=(T["keep"],))
    d.exec()
    return max(d.group.checkedId(), 0), bool(d.extra and d.extra.isChecked())


def ask_quit(parent) -> bool:
    d = Ask(parent, T["quit_title"], T["quit_text"], buttons=(T["quit_yes"], T["quit_no"]))
    d.exec()
    return d.answer == 0


# ------------------------------------------------------- the run's side ----

class Bridge(QObject):
    """Signals from the thread doing the work to the window."""
    log = Signal(str)
    phase = Signal(str)
    song = Signal(int, int, str, bool)
    clash = Signal(str, list)
    checked = Signal(object, object, object)    # Prepared or error text, bytes needed, bytes free
    finished = Signal(int)


class WindowHooks(Hooks):
    def __init__(self, bridge: Bridge):
        super().__init__(quiet=True)
        self.bridge = bridge
        self.stop = threading.Event()
        self.answered = threading.Event()
        self.answer: tuple[int, bool] = (0, False)

    def say(self, msg: str) -> None:
        self.bridge.log.emit(msg.strip("\n"))

    def phase(self, name: str) -> None:
        self.bridge.phase.emit(name)

    def song_done(self, done, total, src, res) -> None:
        if not res["ok"]:
            self.bridge.log.emit(f"FAILED {src.dest_rel}: {res.get('error')}")
        self.bridge.song.emit(done, total, src.dest_rel, bool(res["ok"]))

    def can_ask(self) -> bool:
        return True

    def ask(self, dest_rel, ordered, probes):
        opts = []
        for i, c in enumerate(ordered):
            p = probes.get(c.rel)
            desc = p.describe() if p else "?"
            opts.append(f"{c.rel}\n   {desc}" + (f" · {T['suggested']}" if i == 0 else ""))
        self.answered.clear()
        self.bridge.clash.emit(dest_rel, opts)
        self.answered.wait()
        i, rest = self.answer
        return ordered[i], rest

    def stop_requested(self) -> bool:
        return self.stop.is_set()


# ------------------------------------------------------------- window ----

class Window(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(APP_NAME)
        self.settings = QSettings()     # the app's organization and name, set in make_app
        self.bridge = Bridge()
        self.hooks = WindowHooks(self.bridge)
        self.prep: Prepared | None = None
        self.worker: threading.Thread | None = None
        self.state = "idle"      # idle, checking, ready, running, stopping
        self.close_after = False
        self.t_start = 0.0
        self.found_card = False

        b = self.bridge
        b.log.connect(self._log)
        b.phase.connect(self._phase)
        b.song.connect(self._song)
        b.clash.connect(self._clash)
        b.checked.connect(self._checked)
        b.finished.connect(self._finished)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        top = header(APP_NAME, version_label(), "♪")
        # ⓘ: About, like on the DS
        self.about_btn = QPushButton("ⓘ")
        self.about_btn.setObjectName("link")
        self.about_btn.setToolTip(T["about"])
        self.about_btn.setCursor(Qt.PointingHandCursor)
        self.about_btn.clicked.connect(self._about)
        top.layout().addSpacing(10)
        top.layout().addWidget(self.about_btn)
        root.addWidget(top)

        body = QVBoxLayout()
        body.setContentsMargins(16, 10, 16, 14)
        body.setSpacing(8)
        root.addLayout(body)

        # music folder
        self.src_path = label("", "acc")
        self.src_btn = QPushButton(T["choose"])
        self.src_btn.clicked.connect(self._choose_src)
        body.addLayout(self._row(label(T["music"]), self.src_btn))
        body.addWidget(self.src_path)
        body.addWidget(line())

        # card
        self.card_path = label("", "acc")
        self.card_free = label("", "dim")
        self.card_btn = QPushButton(T["choose"])
        self.card_btn.clicked.connect(self._choose_card)
        body.addLayout(self._row(label(T["card"]), self.card_btn))
        body.addWidget(self.card_path)
        body.addWidget(self.card_free)
        body.addWidget(line())

        # remove songs that are gone
        self.delete = Toggle()
        self.delete.toggled.connect(self._options_changed)
        body.addLayout(self._row(label(T["remove"]), self.delete))
        body.addWidget(line())

        # status
        self.status = label("", "", wrap=True)
        self.status2 = label("", "dim", wrap=True)
        self.bar = Bar()
        self.bar.hide()
        body.addSpacing(4)
        body.addWidget(self.status)
        body.addWidget(self.bar)
        body.addWidget(self.status2)
        body.addStretch(1)

        # bottom row
        self.details_btn = QPushButton(f"▸ {T['details']}")
        self.details_btn.setObjectName("link")
        self.details_btn.clicked.connect(self._toggle_details)
        self.go = QPushButton(T["convert"])
        self.go.setObjectName("primary")
        self.go.clicked.connect(self._go)
        body.addLayout(self._row(self.details_btn, self.go))

        self.details = QPlainTextEdit()
        self.details.setReadOnly(True)
        self.details.setMaximumBlockCount(20000)
        self.details.setMinimumHeight(220)
        self.details.hide()
        body.addWidget(self.details, 3)

        self.setMinimumWidth(600)
        self._load()
        self._refresh()

    def _about(self) -> None:
        About(self).exec()

    # -- layout helpers
    @staticmethod
    def _row(left: QWidget, right: QWidget) -> QHBoxLayout:
        r = QHBoxLayout()
        r.setSpacing(10)
        left.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        r.addWidget(left, 1)
        r.addWidget(right, 0, Qt.AlignRight | Qt.AlignVCenter)
        return r

    # -- settings
    def _load(self) -> None:
        s = self.settings
        src = s.value("music", "")
        card = s.value("card", "")
        self.src = Path(src) if src and Path(src).is_dir() else None
        self.card = Path(card) if card and Path(card).parent.exists() else None
        if self.card is None:
            cards = find_cards()
            if cards:
                self.card = cards[0]
                self.found_card = True
        self.delete.setChecked(s.value("delete", "false") in (True, "true"))

    def _save(self) -> None:
        s = self.settings
        s.setValue("music", str(self.src) if self.src else "")
        s.setValue("card", str(self.card) if self.card else "")
        s.setValue("delete", self.delete.isChecked())

    # -- choosing
    def _choose_src(self) -> None:
        start = str(self.src) if self.src else str(Path.home())
        d = QFileDialog.getExistingDirectory(self, T["music_dialog"], start)
        if d:
            self.src = Path(d)
            self._options_changed()

    def _choose_card(self) -> None:
        start = str(self.card.parent if self.card else next(iter(_mount_points()), Path.home()))
        d = QFileDialog.getExistingDirectory(self, T["card_dialog"], start)
        if d:
            self.card = output_for(Path(d))
            self.found_card = False
            self._options_changed()

    def _options_changed(self, *_) -> None:
        if self.state in ("checking", "running", "stopping"):
            return
        self.prep = None
        self.state = "idle"
        self.status.setText("")
        self._save()
        self._refresh()

    # -- what's shown
    def _refresh(self) -> None:
        self.src_path.setText(str(self.src) if self.src else T["not_chosen"])
        self.src_path.setObjectName("acc" if self.src else "dim")
        self.card_path.setText(str(self.card) if self.card else T["not_chosen"])
        self.card_path.setObjectName("acc" if self.card else "dim")
        for w in (self.src_path, self.card_path):
            w.style().unpolish(w)
            w.style().polish(w)
        free = free_space(self.card) if self.card else None
        bits = []
        if free is not None:
            bits.append(T["free"].format(gb(free)))
        if self.found_card:
            bits.append(T["found"])
        self.card_free.setText(" · ".join(bits))
        self.card_free.setVisible(bool(bits))
        busy = self.state in ("checking", "running", "stopping")
        for w in (self.src_btn, self.card_btn, self.delete):
            w.setEnabled(not busy)
        if self.state == "idle":
            self.go.setText(T["convert"])
            self.go.setEnabled(bool(self.src and self.card))
            if not (self.src and self.card):
                self._status(T["pick_both"])
            elif not self.status.text():
                self._status(T["ready"])
        elif self.state == "checking":
            self.go.setText(T["convert"])
            self.go.setEnabled(False)
        elif self.state == "running":
            self.go.setText(T["stop"])
            self.go.setEnabled(True)
        elif self.state == "stopping":
            self.go.setText(T["stop"])
            self.go.setEnabled(False)

    def _status(self, a: str, b: str = "", bar: float | None = None) -> None:
        self.status.setText(a)
        self.status2.setText(b)
        self.status2.setVisible(bool(b))
        self.bar.setVisible(bar is not None)
        if bar is not None:
            self.bar.set(bar)

    def _toggle_details(self) -> None:
        show = not self.details.isVisible()
        self.details.setVisible(show)
        self.details_btn.setText(f"{'▾' if show else '▸'} {T['details']}")
        if not show:
            QTimer.singleShot(0, lambda: self.resize(self.width(), self.minimumSizeHint().height()))

    def _log(self, msg: str) -> None:
        self.details.appendPlainText(msg)

    # -- the button
    def _go(self) -> None:
        if self.state == "idle":
            self._check()
        elif self.state == "ready":
            self._start()
        elif self.state == "running":
            self.hooks.stop.set()
            self.state = "stopping"
            self._status(T["stopping"], self.status2.text(), self.bar.value)
            self._refresh()

    def _check(self) -> None:
        self.state = "checking"
        self.details.clear()
        self._status(T["checking"])
        self._refresh()
        src, card, delete = self.src, self.card, self.delete.isChecked()

        def work():
            try:
                prep = prepare(src, card, hooks=self.hooks)
                need = space_needed(prep, delete)
                self.bridge.checked.emit(prep, need, free_space(card))
            except SetupError as e:
                self.bridge.checked.emit(str(e), 0, None)
            except Exception as e:  # shown, not swallowed
                self.bridge.checked.emit(f"{type(e).__name__}: {e}", 0, None)

        self.worker = threading.Thread(target=work, daemon=True)
        self.worker.start()

    def _checked(self, prep, need, free) -> None:
        if isinstance(prep, str):
            self.state = "idle"
            self._refresh()
            self._status(T["error"].format(msg=prep))
            return
        self.prep = prep
        delete = self.delete.isChecked()
        counts = T["counts"].format(new=len(prep.new), changed=len(prep.changed), same=prep.report.unchanged)
        lines = []
        if prep.orphans:
            lines.append((T["remove_n"] if delete else T["orphans_n"]).format(n=len(prep.orphans)))
        work = bool(prep.todo) or (delete and prep.orphans)
        fits = free is None or need <= 0 or need * 1.01 <= free
        if not work:
            self.state = "idle"
            self._refresh()
            self._status(T["nothing"], "\n".join(lines))
            return
        if need > 0:
            lines.append((T["fits"] if fits else T["no_fit"]).format(need=gb(need), free=gb(free or 0)))
        elif need < 0:
            lines.append(T["emptier"].format(need=gb(-need)))
        if fits:
            self.state = "ready"
            self.go.setText(T["start"])
            self.go.setEnabled(True)
            for w in (self.src_btn, self.card_btn, self.delete):
                w.setEnabled(True)
        else:
            self.state = "idle"
            self._refresh()
        self._status(counts, "\n".join(lines))

    def _start(self) -> None:
        prep, delete = self.prep, self.delete.isChecked()
        self.state = "running"
        self.hooks.stop.clear()
        self.t_start = time.monotonic()
        self._status(T["converting"].format(done=0, total=len(prep.todo)), "", 0.0)
        self._refresh()

        def work():
            try:
                code = execute(prep, delete=delete, hooks=self.hooks)
            except Exception as e:
                self.bridge.log.emit(f"{type(e).__name__}: {e}")
                code = 3
            self.bridge.finished.emit(code)

        self.worker = threading.Thread(target=work, daemon=True)
        self.worker.start()

    # -- from the run
    def _phase(self, name: str) -> None:
        if name == "index" and self.state in ("running", "stopping"):
            self._status(T["index"], "", 1.0)

    def _song(self, done: int, total: int, rel: str, ok: bool) -> None:
        if self.state != "running":
            return
        left = ""
        el = time.monotonic() - self.t_start
        if done >= 3 and done < total:
            m = minutes(el / done * (total - done))
            left = T["left"].format(t=m) if m else T["less_min"]
        head = T["converting"].format(done=done, total=total) + (f" · {left}" if left else "")
        self._status(head, Path(rel).with_suffix("").as_posix(), done / total if total else 1.0)

    def _clash(self, dest_rel: str, options: list) -> None:
        self.hooks.answer = ask_clash(self, dest_rel, options)
        self.hooks.answered.set()

    def _finished(self, code: int) -> None:
        r = self.prep.report if self.prep else None
        self.prep = None
        self.state = "idle"
        self._refresh()
        if code == 130:
            self._status(T["stopped"])
        elif r is not None and r.failures:
            self._status(T["done_failed"].format(f=len(r.failures)), T["eject"])
            if not self.details.isVisible():
                self._toggle_details()
        elif code == 0 and r is not None:
            self._status(T["done"].format(n=r.n_converted), T["eject"])
        else:
            self._status(T["error"].format(msg=f"code {code}"))
        if self.close_after:
            self.close()

    def closeEvent(self, ev) -> None:
        if self.state in ("running", "stopping"):
            if self.state == "running" and not ask_quit(self):
                ev.ignore()
                return
            self.hooks.stop.set()
            self.state = "stopping"
            self.close_after = True
            self._status(T["stopping"])
            self._refresh()
            ev.ignore()
            return
        if self.state == "checking":
            self.hooks.answer = (0, True)
            self.hooks.answered.set()
        self._save()
        ev.accept()


# -------------------------------------------------------------- start ----

def load_font() -> str:
    raw = resources.files("alya_bangers_dsconv").joinpath("data/window_font.ttf.gz").read_bytes()
    fid = QFontDatabase.addApplicationFontFromData(QByteArray(gzip.decompress(raw)))
    fams = QFontDatabase.applicationFontFamilies(fid)
    return fams[0] if fams else "monospace"


def app_icon() -> QIcon:
    data = resources.files("alya_bangers_dsconv").joinpath("data/icon.png").read_bytes()
    pm = QPixmap()
    pm.loadFromData(data)
    return QIcon(pm)


def make_app(argv: list[str] | None = None) -> QApplication:
    global T
    app = QApplication.instance()
    if app is None:
        # whole-number scaling only, so the pixel font stays as sharp as on the DS
        QGuiApplication.setHighDpiScaleFactorRoundingPolicy(Qt.HighDpiScaleFactorRoundingPolicy.Round)
        app = QApplication(argv or [APP_NAME])
    T = TEXTS[_lang()]
    app.setApplicationName(APP_NAME)
    app.setOrganizationName("alya-bangers")
    app.setDesktopFileName(APP_NAME)
    family = load_font()
    f = QFont(family)
    f.setPixelSize(FONT_PX)
    f.setStyleStrategy(QFont.NoAntialias)
    f.setHintingPreference(QFont.PreferNoHinting)
    app.setFont(f)
    app.setStyleSheet(stylesheet(family))
    try:
        app.setWindowIcon(app_icon())
    except (FileNotFoundError, OSError):
        pass
    return app


SELFTEST = "ALYA_BANGERS_DSCONV_SELFTEST"


def run() -> int:
    app = make_app(sys.argv[:1])
    w = Window()
    w.resize(640, w.sizeHint().height())
    w.show()
    if os.environ.get(SELFTEST):
        # for the release builds: the packed window opens with its font and icon, then closes
        ok = app.font().family() == "Unifont alya-bangers" and not app.windowIcon().isNull()
        QTimer.singleShot(300, lambda: app.exit(0 if ok else 3))
    return app.exec()
