"""The manifest on the card remembers what was converted from what, and the
collision choices, so later runs only redo what changed and never ask twice."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

from .constants import (
    BLOCK_FRAMES, CONVERTER_REVISION, COVER_SIZE, DATA_DIR, FORMAT_VERSION,
    MANIFEST_NAME, SAMPLE_RATE, THUMB_SIZE, __version__,
)


def settings_hash() -> str:
    s = json.dumps({
        "format": FORMAT_VERSION, "revision": CONVERTER_REVISION, "rate": SAMPLE_RATE,
        "block": BLOCK_FRAMES, "cover": COVER_SIZE, "thumb": THUMB_SIZE,
    }, sort_keys=True)
    return hashlib.sha1(s.encode()).hexdigest()[:16]


class Manifest:
    def __init__(self, root: Path):
        self.path = root / DATA_DIR / MANIFEST_NAME
        self.files: dict[str, dict] = {}
        self.choices: dict[str, dict] = {}
        self.settings = settings_hash()
        self.stale_settings = False
        if self.path.exists():
            try:
                data = json.loads(self.path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                data = {}
            self.choices = data.get("choices", {})
            if data.get("settings") == self.settings:
                self.files = data.get("files", {})
            else:
                # Format or converter output changed: keep the record of which
                # outputs exist (for --delete) but treat every file as outdated.
                self.files = {k: {"src": v.get("src")} for k, v in data.get("files", {}).items()}
                self.stale_settings = bool(data)

    def up_to_date(self, dest_rel: str, src_rel: str, size: int, mtime_ns: int,
                   cover_sig: str, dst_root: Path) -> bool:
        e = self.files.get(dest_rel)
        if not e or e.get("src") != src_rel or e.get("size") != size:
            return False
        if e.get("mtime_ns") != mtime_ns or e.get("cover") != cover_sig:
            return False
        try:
            return (dst_root / dest_rel).stat().st_size == e.get("out_size")
        except OSError:
            return False

    def record(self, dest_rel: str, src_rel: str, size: int, mtime_ns: int,
               cover_sig: str, out_size: int) -> None:
        self.files[dest_rel] = {"src": src_rel, "size": size, "mtime_ns": mtime_ns,
                                "cover": cover_sig, "out_size": out_size}

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        data = {"tool": "dsconv", "version": __version__, "format_version": FORMAT_VERSION,
                "settings": self.settings, "files": self.files, "choices": self.choices}
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, ensure_ascii=False, indent=1, sort_keys=True),
                       encoding="utf-8")
        os.replace(tmp, self.path)
