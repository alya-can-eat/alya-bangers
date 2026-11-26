"""Build the converter's icon from the DS app's (tools/gen_icon.py in the repo
root): the same 32x32 pixel art, scaled up without smoothing.

Writes src/alya_bangers_dsconv/data/icon.png (256 px, the window and the
Linux app icon) and packaging/icon.ico (16 to 256 px, Windows).

    python3 tools/build_app_icon.py
"""

import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent.parent
REPO = HERE.parent

with tempfile.TemporaryDirectory() as tmp:
    gif = Path(tmp) / "icon.gif"
    subprocess.run([sys.executable, str(REPO / "tools" / "gen_icon.py"), str(gif)], check=True)
    small = Image.open(gif).convert("RGBA")

big = small.resize((256, 256), Image.NEAREST)
big.save(HERE / "src" / "alya_bangers_dsconv" / "data" / "icon.png", optimize=True)
(HERE / "packaging").mkdir(exist_ok=True)
big.save(HERE / "packaging" / "icon.ico", sizes=[(s, s) for s in (16, 32, 48, 64, 128, 256)])
print("icon.png and icon.ico written", file=sys.stderr)
