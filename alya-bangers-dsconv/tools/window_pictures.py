"""The README pictures of the window, taken offscreen at twice the size (like
the DS pictures): after the check, and while converting.

    python3 tests/demo/make_demo_card.py /tmp/demo        (in the repo root: the made-up library)
    python3 tools/window_pictures.py /tmp/demo/src ../docs/img

The folders are shown as they would be on a PC (~/Music, the card under
/run/media); the songs are the made-up ones.
"""

import os
import sys
import tempfile
import time
from pathlib import Path

os.environ["QT_QPA_PLATFORM"] = "offscreen"
os.environ["QT_SCALE_FACTOR"] = "2"
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))


def main() -> None:
    from PySide6.QtCore import QSettings

    from alya_bangers_dsconv import gui

    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    tmp = Path(tempfile.mkdtemp())
    QSettings.setDefaultFormat(QSettings.IniFormat)
    QSettings.setPath(QSettings.IniFormat, QSettings.UserScope, str(tmp / "settings"))
    app = gui.make_app()
    w = gui.Window()
    w.make_music.setChecked(False)      # found by itself: the card's music folder
    w.src, w.card_dir, w.found_card = src, tmp / "DSPICO" / "music", True
    w._set_output()
    w._options_changed()
    w.show()

    def wait(cond, seconds=600):
        end = time.monotonic() + seconds
        while not cond() and time.monotonic() < end:
            app.processEvents()
            time.sleep(0.01)

    def shoot(name):
        w.src_path.setText("~/Music")
        w.card_path.setText("/run/media/alya/DSPICO/music")
        app.processEvents()
        w.resize(640, w.minimumSizeHint().height())
        app.processEvents()
        w.grab().save(str(out / name))
        print(out / name)

    w._go()
    wait(lambda: w.state != "checking")
    shoot("converter.png")
    w._go()
    wait(lambda: w.state != "running" or w.bar.value >= 0.4)
    shoot("converter_run.png")
    wait(lambda: w.state == "idle")


if __name__ == "__main__":
    main()
