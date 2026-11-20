"""Scan the source tree and decide what goes where on the card."""

from __future__ import annotations

import os
import re
import unicodedata
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

from .constants import AUDIO_EXTS, DATA_DIR, DISC_DIR_PATTERN, IMAGE_EXTS, OUT_EXT
from .cover import find_folder_cover
from .fat32 import fat_key, safe_component


@dataclass
class Source:
    rel: str            # source path relative to the source root, '/' separated
    path: Path          # absolute source path
    size: int
    mtime_ns: int
    dest_rel: str       # output path relative to the output root
    folder_cover: str | None  # source-relative path of the folder's cover image


@dataclass
class Plan:
    sources: list[Source] = field(default_factory=list)
    groups: dict[str, list[Source]] = field(default_factory=dict)  # fat_key(dest) -> candidates
    ignored: Counter = field(default_factory=Counter)  # extension -> count
    images: int = 0
    hidden: int = 0
    renamed: list[tuple[str, str]] = field(default_factory=list)
    merged_dirs: list[tuple[str, str]] = field(default_factory=list)  # (source dir, merged into)

    @property
    def collisions(self) -> dict[str, list[Source]]:
        return {k: v for k, v in self.groups.items() if len(v) > 1}


def _natural_names(names):
    return sorted(names, key=lambda n: (unicodedata.normalize("NFC", n).casefold(), n))


DISC_DIR = re.compile(DISC_DIR_PATTERN, re.IGNORECASE)


def _parent_cover(src_root: Path, parent: Path) -> str | None:
    """Cover of the album folder, for songs in a disc subfolder."""
    try:
        names = [n for n in os.listdir(parent) if (parent / n).is_file()]
    except OSError:
        return None
    cover = find_folder_cover(parent, sorted(names))
    if cover is None:
        return None
    rel = parent.relative_to(src_root).as_posix()
    return cover if rel == "." else f"{rel}/{cover}"


def build_plan(src_root: Path, dst_root: Path | None) -> Plan:
    plan = Plan()
    src_root = src_root.resolve()
    skip = dst_root.resolve() if dst_root is not None else None

    dest_dirs: dict[str, str] = {"": ""}        # source dir rel -> dest dir rel
    dir_spelling: dict[str, str] = {}           # fat_key(dest dir) -> first spelling
    dir_owner: dict[str, str] = {}              # fat_key(dest dir) -> first source dir

    for dirpath, dirnames, filenames in os.walk(src_root, followlinks=False):
        here = Path(dirpath)
        if skip is not None and (here == skip or skip in here.parents):
            dirnames[:] = []
            continue
        rel_dir = here.relative_to(src_root).as_posix()
        rel_dir = "" if rel_dir == "." else rel_dir

        keep = []
        for d in _natural_names(dirnames):
            if d.startswith("."):
                plan.hidden += 1
                continue
            child_src = f"{rel_dir}/{d}" if rel_dir else d
            parent_dest = dest_dirs[rel_dir]
            comp = safe_component(d)
            want = f"{parent_dest}/{comp}" if parent_dest else comp
            key = fat_key(want)
            if key in dir_spelling:
                if dir_owner[key] != child_src:
                    plan.merged_dirs.append((child_src, dir_owner[key]))
                want = dir_spelling[key]
            else:
                dir_spelling[key] = want
                dir_owner[key] = child_src
            dest_dirs[child_src] = want
            keep.append(d)
        dirnames[:] = keep

        files = _natural_names(filenames)
        visible = [n for n in files if not n.startswith(".")]
        plan.hidden += len(files) - len(visible)
        cover = find_folder_cover(here, visible)
        cover_rel = (f"{rel_dir}/{cover}" if rel_dir else cover) if cover else None
        if cover_rel is None and rel_dir and DISC_DIR.search(here.name):
            cover_rel = _parent_cover(src_root, here.parent)

        for name in visible:
            p = here / name
            ext = p.suffix.lower()
            if ext in IMAGE_EXTS:
                plan.images += 1
                continue
            if ext not in AUDIO_EXTS:
                plan.ignored[ext or "(no extension)"] += 1
                continue
            try:
                st = p.stat()
            except OSError:
                plan.ignored["(unreadable)"] += 1
                continue
            if not p.is_file():
                continue
            rel = f"{rel_dir}/{name}" if rel_dir else name
            dest_dir = dest_dirs[rel_dir]
            out_name = safe_component(p.stem, OUT_EXT)
            dest_rel = f"{dest_dir}/{out_name}" if dest_dir else out_name
            if DATA_DIR in PurePosixPath(dest_rel).parts:
                plan.ignored["(inside .dsmusic)"] += 1
                continue
            src = Source(rel, p, st.st_size, st.st_mtime_ns, dest_rel, cover_rel)
            plan.sources.append(src)
            plan.groups.setdefault(fat_key(dest_rel), []).append(src)

            nfc_rel = unicodedata.normalize("NFC", rel)
            expected = str(PurePosixPath(nfc_rel).with_suffix(OUT_EXT))
            if dest_rel != expected:
                plan.renamed.append((rel, dest_rel))
    return plan
