"""FAT32-safe file and folder names.

FAT32 long file names cannot contain  " * / : < > ? \\ |  or control characters,
cannot end in a dot or space (only the very end of the whole name counts), hold at most 255 UTF-16 code units, and are
compared case-insensitively. Forbidden characters are replaced by their
full-width lookalikes, so names still read the same on the DS.
"""

import unicodedata

REPLACEMENTS = {
    '"': "＂",  # ＂
    "*": "＊",  # ＊
    "/": "／",  # ／
    ":": "：",  # ：
    "<": "＜",  # ＜
    ">": "＞",  # ＞
    "?": "？",  # ？
    "\\": "＼",  # ＼
    "|": "｜",  # ｜
}

# Not forbidden by FAT32 itself, but Windows refuses them, which matters if the
# card is ever opened there.
RESERVED = {"CON", "PRN", "AUX", "NUL"} | {f"COM{i}" for i in range(1, 10)} | {
    f"LPT{i}" for i in range(1, 10)
}

MAX_UTF16_UNITS = 255


def utf16_len(s: str) -> int:
    return sum(2 if ord(c) > 0xFFFF else 1 for c in s)


def _clean(s: str) -> str:
    s = unicodedata.normalize("NFC", s)
    out = []
    for c in s:
        if c in REPLACEMENTS:
            out.append(REPLACEMENTS[c])
        elif ord(c) < 0x20 or ord(c) == 0x7F:
            continue
        else:
            out.append(c)
    return "".join(out)


def _truncate(stem: str, budget: int) -> str:
    out, used = [], 0
    for c in stem:
        n = 2 if ord(c) > 0xFFFF else 1
        if used + n > budget:
            break
        out.append(c)
        used += n
    return "".join(out)


FULLWIDTH_DOT = "\uFF0E"  # ．


def _fix_end(name: str) -> str:
    """A FAT32 name can't end in a space or a dot. Trailing spaces are dropped
    (invisible anyway); trailing dots become full-width dots, so a folder named
    "Demos..." still reads "Demos．．．"."""
    name = name.rstrip(" ")
    dots = len(name) - len(name.rstrip("."))
    if dots:
        name = name[:-dots] + FULLWIDTH_DOT * dots
    return name


def safe_component(name: str, ext: str = "") -> str:
    """Return a FAT32-safe version of one path component.

    `ext` (e.g. ".dsa") is appended after cleaning and never truncated. The
    end-of-name rule only concerns the whole name, so with an extension the
    stem keeps its trailing dots ("S.T.A.R.S." -> "S.T.A.R.S..dsa").
    """
    stem = _clean(name).lstrip(" ")
    if not ext:
        stem = _fix_end(stem)
    if not stem:
        stem = "_"
    if stem.split(".")[0].upper() in RESERVED:
        stem = stem + "_"
    budget = MAX_UTF16_UNITS - utf16_len(ext)
    if utf16_len(stem) > budget:
        stem = _truncate(stem, budget)
        if not ext:
            stem = _fix_end(stem)
        stem = stem or "_"
    return stem + ext


def fat_key(path: str) -> str:
    """Key under which FAT32 considers two names the same.

    FAT compares long names with a per-character uppercase table, so characters
    whose uppercase form is longer (ß -> SS) stay as they are.
    """
    out = []
    for c in unicodedata.normalize("NFC", path):
        u = c.upper()
        out.append(u if len(u) == 1 else c)
    return "".join(out)
