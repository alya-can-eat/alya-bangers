"""Sort order for tag values, computed on the PC so the DS only compares integers.

Case and accents are ignored at first ("Équinoxe" sorts with "Equinoxe"),
numbers compare by value ("Track 2" before "Track 10"), and exact ties are
broken by the original string so the order is total and stable.
"""

import re
import unicodedata

_SPLIT = re.compile(r"(\d+)")


def sort_key(s: str) -> tuple:
    folded = unicodedata.normalize("NFKD", s)
    folded = "".join(c for c in folded if not unicodedata.combining(c)).casefold().strip()
    parts = []
    for i, piece in enumerate(_SPLIT.split(folded)):
        if not piece:
            continue
        if i % 2:
            parts.append((0, int(piece), ""))
        else:
            parts.append((1, 0, piece))
    return (tuple(parts), s)
