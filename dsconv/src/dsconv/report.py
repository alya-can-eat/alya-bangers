"""Collects what happened during a run and prints / saves the summary."""

from __future__ import annotations

import time
from pathlib import Path

LIST_LIMIT = 15  # entries per section on screen; the report file has everything


def _fmt_duration(seconds: float) -> str:
    seconds = int(round(seconds))
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


class Report:
    def __init__(self, src_root: Path, dst_root: Path, dry_run: bool):
        self.src_root, self.dst_root, self.dry_run = src_root, dst_root, dry_run
        self.unchanged = 0
        self.n_converted = 0
        self.audio_seconds = 0.0
        self.bytes_written = 0
        self.no_cover: list[str] = []
        self.turned_down: list[tuple[str, float]] = []
        self.failures: list[tuple[str, str]] = []
        self.warnings: list[tuple[str, str]] = []
        self.collisions: list[tuple[str, str, list[str], str]] = []
        self.orphans: list[str] = []
        self.deleted: list[str] = []
        self.to_convert: list[str] = []
        self.index_stats: dict | None = None
        self.ignored = {}
        self.images = 0
        self.hidden = 0
        self.renamed: list[tuple[str, str]] = []
        self.merged: list[tuple[str, str]] = []
        self.elapsed = 0.0
        self.lines: list[str] = []
        self.screen: list[str] = []

    # -- collection
    def plan(self, plan) -> None:
        self.ignored = dict(plan.ignored)
        self.images = plan.images
        self.hidden = plan.hidden
        self.renamed = plan.renamed
        self.merged = plan.merged_dirs

    def collision(self, dest_rel, pick, losers, probes, how) -> None:
        def d(s):
            p = probes.get(s.rel)
            return f"{s.rel} [{p.describe() if p else 'unreadable'}]"
        self.collisions.append((dest_rel, d(pick), [d(x) for x in losers], how))

    def converted(self, src, res) -> None:
        self.n_converted += 1
        self.audio_seconds += res.get("seconds", 0.0)
        self.bytes_written += res.get("out_size", 0)
        if not res.get("has_cover"):
            self.no_cover.append(src.rel)
        db = res.get("pregain_db", 0.0)
        if db < 0:
            self.turned_down.append((src.rel, db))
        for w in res.get("warnings", []):
            self.warnings.append((src.rel, w))

    def failed(self, src, error: str) -> None:
        self.failures.append((src.rel, error))

    def warn(self, where: str, msg: str) -> None:
        self.warnings.append((where, msg))

    # -- output
    def _section(self, title: str, items: list[str], screen: list[str]) -> None:
        if not items:
            return
        self.lines.append(f"\n{title} ({len(items)}):")
        self.lines += [f"  {i}" for i in items]
        screen.append(f"\n{title} ({len(items)}):")
        limit = None if self.dry_run else LIST_LIMIT  # a dry run writes no report file
        screen += [f"  {i}" for i in items[:limit]]
        if limit is not None and len(items) > limit:
            screen.append(f"  … and {len(items) - limit} more (see the report file)")

    def finish(self, elapsed: float, delete: bool) -> None:
        self.elapsed = elapsed
        screen: list[str] = []
        head = []
        if self.dry_run:
            head.append("Dry run: nothing was written.")
            head.append(f"Would convert {len(self.to_convert)} files, {self.unchanged} are up to date.")
        else:
            rate = f", {self.audio_seconds / elapsed:.0f}x real time" if elapsed > 0 and self.audio_seconds else ""
            head.append(f"Converted {self.n_converted} files ({_fmt_duration(self.audio_seconds)} of audio, "
                        f"{self.bytes_written / 1e9:.2f} GB) in {_fmt_duration(elapsed)}{rate}.")
            head.append(f"Up to date: {self.unchanged}. Failed: {len(self.failures)}.")
        if self.ignored:
            parts = ", ".join(f"{n} × {ext}" for ext, n in sorted(self.ignored.items(), key=lambda x: -x[1]))
            head.append(f"Ignored: {parts}.")
        if self.images:
            head.append(f"Image files (used as covers where they match): {self.images}.")
        if self.turned_down:
            worst = min(db for _, db in self.turned_down)
            head.append(f"Turned down to avoid clipping: {len(self.turned_down)} songs, by up to "
                        f"{-worst:.1f} dB (ReplayGain values adjusted to match).")
        if self.index_stats:
            s = self.index_stats
            head.append(f"Index: {s['tracks']} tracks, {s['albums']} albums, {s['values']} distinct tag values, "
                        f"{s.get('readings', 0)} names with readings for search; "
                        f"font: {s['glyphs']} glyphs ({s['missing']} characters without a glyph).")
        self.lines = list(head)
        screen += head

        self._section("FAILED", [f"{r}: {e}" for r, e in self.failures], screen)
        self._section("Collisions", [
            f"{dest}\n      kept:    {kept} ({how})" + "".join(f"\n      skipped: {l}" for l in losers)
            for dest, kept, losers, how in self.collisions], screen)
        self._section("Renamed for FAT32", [f"{a}\n      -> {b}" for a, b in self.renamed], screen)
        self._section("Folders merged (same name apart from upper/lower case)",
                      [f"{a} -> {b}" for a, b in self.merged], screen)
        if self.orphans and not self.deleted:
            if self.dry_run and delete:
                hint = "would be deleted"
            else:
                hint = "run with --delete to remove them"
            self._section(f"Files on the card without a source ({hint})", self.orphans, screen)
        self._section("Deleted (source gone)", sorted(self.deleted), screen)
        self._section("Warnings", [f"{r}: {w}" for r, w in sorted(self.warnings)], screen)
        self._section("No cover found", sorted(self.no_cover), screen)
        if self.turned_down:  # details only in the report file
            self.lines.append(f"\nTurned down to avoid clipping ({len(self.turned_down)}):")
            self.lines += [f"  {r}: {db:.2f} dB" for r, db in sorted(self.turned_down)]
        self.screen = screen

    def show(self) -> None:
        print("\n".join(self.screen), flush=True)

    def write(self, path: Path) -> None:
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        text = f"dsconv run {stamp}\nsource: {self.src_root}\noutput: {self.dst_root}\n\n" + "\n".join(self.lines) + "\n"
        path.write_text(text, encoding="utf-8")
        self.screen.append(f"\nFull report: {path}")
