"""Build the release file for the system this runs on, then try it.

    Windows: dist/release/alya-bangers-dsconv-<version>-windows.exe   (one file)
    Linux:   dist/release/alya-bangers-dsconv-<version>-x86_64.AppImage

    python -m pip install ".[build]"
    python packaging/build.py                 (Linux: appimagetool on PATH or in $APPIMAGETOOL)
    python packaging/build.py --no-appimage   (Linux: only the folder in dist/, tried as it is)

Trying it: the packed app converts a short generated song from the command
line, and opens its window offscreen for a moment (it checks its own font and
icon and closes again).
"""

from __future__ import annotations

import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
NAME = "alya-bangers-dsconv"
sys.path.insert(0, str(ROOT / "src"))
from alya_bangers_dsconv.constants import version_label  # noqa: E402

LABEL = version_label()
DIST = ROOT / "dist"
RELEASE = DIST / "release"


def pyinstaller(onefile: bool) -> None:
    args = [
        sys.executable, "-m", "PyInstaller", str(HERE / "entry.py"),
        "--name", NAME, "--windowed", "--noconfirm", "--clean",
        "--distpath", str(DIST), "--workpath", str(ROOT / "build" / "pyinstaller"),
        "--specpath", str(ROOT / "build"),
        "--paths", str(ROOT / "src"),
        "--collect-data", "alya_bangers_dsconv",
        "--collect-data", "pykakasi", "--collect-data", "pypinyin",
        "--icon", str(HERE / "icon.ico"),
        "--onefile" if onefile else "--onedir",
    ]
    subprocess.run(args, check=True)


DESKTOP = f"""[Desktop Entry]
Type=Application
Name=alya-bangers-dsconv
GenericName=Music converter for the DS
Comment=Puts your music on the card for alya-bangers, the Nintendo DS music player
Exec={NAME} %F
Icon={NAME}
Categories=AudioVideo;Audio;
Terminal=false
StartupWMClass={NAME}
"""

APPRUN = f"""#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/lib/{NAME}/{NAME}" "$@"
"""


def appimage() -> Path:
    tool = os.environ.get("APPIMAGETOOL") or shutil.which("appimagetool")
    if not tool:
        sys.exit("appimagetool not found (put it on PATH or in $APPIMAGETOOL)")
    appdir = ROOT / "build" / "AppDir"
    shutil.rmtree(appdir, ignore_errors=True)
    shutil.copytree(DIST / NAME, appdir / "usr" / "lib" / NAME, symlinks=True)
    (appdir / f"{NAME}.desktop").write_text(DESKTOP, encoding="utf-8")
    shutil.copy2(ROOT / "src" / "alya_bangers_dsconv" / "data" / "icon.png", appdir / f"{NAME}.png")
    run = appdir / "AppRun"
    run.write_text(APPRUN, encoding="utf-8")
    run.chmod(0o755)
    out = RELEASE / f"{NAME}-{LABEL}-x86_64.AppImage"
    env = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")
    subprocess.run([tool, "--no-appstream", str(appdir), str(out)], check=True, env=env)
    return out


def song(path: Path, seconds: float = 2.0, rate: int = 44100) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(int(seconds * rate)):
            v = int(8000 * math.sin(2 * math.pi * 440 * i / rate))
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))


def try_it(app: Path) -> None:
    env = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1")
    with tempfile.TemporaryDirectory() as tmp:
        src, out = Path(tmp) / "music", Path(tmp) / "card" / "music"
        song(src / "Test Artist" / "A Song.wav")
        r = subprocess.run([str(app), str(src), str(out), "--yes", "--quiet"], env=env, timeout=600)
        made = out / "Test Artist" / "A Song.dsa"
        if r.returncode != 0 or not made.exists() or not (out / ".dsmusic" / "library.idx").exists():
            sys.exit(f"the packed app didn't convert (exit {r.returncode})")
        print(f"converted with the packed app: {made.stat().st_size} bytes")
    env.update(QT_QPA_PLATFORM="offscreen", ALYA_BANGERS_DSCONV_SELFTEST="1")
    r = subprocess.run([str(app)], env=env, timeout=120)
    if r.returncode != 0:
        sys.exit(f"the packed window didn't open properly (exit {r.returncode})")
    print("the packed window opened with its font and icon")


def main() -> None:
    RELEASE.mkdir(parents=True, exist_ok=True)
    if sys.platform == "win32":
        pyinstaller(onefile=True)
        out = RELEASE / f"{NAME}-{LABEL}-windows.exe"
        shutil.copy2(DIST / f"{NAME}.exe", out)
    elif sys.platform.startswith("linux"):
        pyinstaller(onefile=False)
        if "--no-appimage" in sys.argv:
            out = DIST / NAME / NAME
        else:
            out = appimage()
    else:
        sys.exit("only Windows and Linux builds for now")
    try_it(out)
    print(f"{out} ({out.stat().st_size / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
