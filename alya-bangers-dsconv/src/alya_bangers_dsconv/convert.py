"""The conversion run: plan, collisions, parallel conversion, cleanup, report."""

from __future__ import annotations

import math
import multiprocessing
import os
import shutil
import signal
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import audio
from .constants import DATA_DIR, OUT_EXT, REPORT_NAME, SAMPLE_RATE, __version__
from .cover import convert_cover
from .dsa import DsaWriter, encode_tags, replaygain
from .fat32 import fat_key
from .manifest import Manifest
from .plan import Plan, Source, build_plan
from .playlists import bring_over
from .report import Report
from .tags import best_picture, read_tags

TMP_PREFIX = ".dsconv-"  # the converter's old name; kept so leftovers of older runs are found


# ------------------------------------------------------------- worker ----

@dataclass
class Job:
    src_path: str
    src_rel: str
    dest_path: str
    folder_cover_path: str | None
    seed: int


# Highest sample value allowed before dithering: 2 LSB below full scale, so
# the +-1 LSB dither can never clip.
HEADROOM_LIMIT = 32765 / 32767


def compensate_replaygain(rg: dict, gain: float) -> dict:
    """The audio was scaled by `gain`; adjust ReplayGain so playback loudness
    with ReplayGain stays exactly what it was."""
    if gain >= 1.0:
        return rg
    out = dict(rg)
    db = -20.0 * math.log10(gain)
    for scope in ("track", "album"):
        if out.get(f"{scope}_gain") is not None:
            out[f"{scope}_gain"] += db
        if out.get(f"{scope}_peak") is not None:
            out[f"{scope}_peak"] *= gain
    return out


def _worker_init() -> None:
    signal.signal(signal.SIGINT, signal.SIG_IGN)  # Ctrl-C is handled by the main process


def run_job(job: Job) -> dict:
    res: dict = {"ok": False, "warnings": []}
    tmp = None
    try:
        src = Path(job.src_path)
        tr = read_tags(src)
        tags = tr.tags
        res["warnings"] += tr.warnings

        cover = thumb = None
        cover_from = "none"
        pic = best_picture(tr.pictures)
        if pic is not None:
            try:
                cover, thumb = convert_cover(pic)
                cover_from = "embedded"
            except Exception as e:
                res["warnings"].append(f"embedded cover unreadable ({e}), trying folder cover")
        if cover is None and job.folder_cover_path:
            try:
                cover, thumb = convert_cover(Path(job.folder_cover_path).read_bytes())
                cover_from = "folder:" + Path(job.folder_cover_path).name
            except Exception as e:
                res["warnings"].append(f"folder cover unreadable: {e}")

        # Pass 1: find the peak after resampling. Loud masters often have
        # inter-sample peaks above full scale that would clip in 16 bit.
        _, _, chunks = audio.decode_resampled(src)
        peak = 0.0
        for chunk in chunks:
            if chunk.size:
                peak = max(peak, float(np.abs(chunk).max()))
        gain = 1.0 if peak <= HEADROOM_LIMIT else HEADROOM_LIMIT / peak
        gain_db = 20.0 * math.log10(gain)

        # Pass 2 (the source is in the page cache by now): convert for real.
        info, channels, chunks = audio.decode_resampled(src)
        tags["~source"] = [job.src_rel]
        tags["~codec"] = [info.describe().split(",")[0]]
        if info.bitrate:
            tags["~bitrate"] = [str(info.bitrate)]
        tags["~source_rate"] = [str(info.sample_rate)]
        tags["~source_channels"] = [str(info.channels)]
        if info.bits:
            tags["~source_bits"] = [str(info.bits)]
        tags["~cover"] = [cover_from]
        if gain < 1.0:
            tags["~pregain"] = [f"{gain_db:.2f} dB"]
        tag_bytes, w = encode_tags(tags)
        res["warnings"] += w

        dest = Path(job.dest_path)
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.parent / f"{TMP_PREFIX}{os.getpid()}-{zlib.crc32(job.src_rel.encode()):08x}.tmp"
        with tmp.open("wb", buffering=1 << 20) as f:
            writer = DsaWriter(f, channels, tag_bytes, cover, thumb,
                               compensate_replaygain(replaygain(tags), gain), job.seed)
            for chunk in chunks:
                writer.write(chunk * np.float32(gain) if gain < 1.0 else chunk)
            stats = writer.close()
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, dest)
        tmp = None

        if info.channels > 2:
            res["warnings"].append(f"{info.channels} channels mixed down to stereo")
        total = stats["frames"] * channels
        if total and stats["clipped"] / total > 1e-4:
            res["warnings"].append(f"{stats['clipped']} samples clipped (source peaks above full scale)")
        res.update(ok=True, out_size=dest.stat().st_size, seconds=info.duration_s,
                   has_cover=cover is not None, pregain_db=gain_db)
    except Exception as e:
        res["error"] = f"{type(e).__name__}: {e}"
        if tmp is not None:
            try:
                tmp.unlink()
            except OSError:
                pass
    return res


# --------------------------------------------------------- collisions ----

def _safe_probe(src: Source):
    try:
        return audio.probe(src.path)
    except Exception:
        return None


def _rank(p) -> tuple:
    return p.quality_rank() if p is not None else (0, 0, 0, 0)


def resolve_collisions(plan: Plan, manifest: Manifest, report: Report, *,
                       yes: bool, dry_run: bool, hooks: "Hooks") -> dict[str, Source]:
    chosen: dict[str, Source] = {}
    accept_all = yes
    can_ask = hooks.can_ask()
    for key, cands in sorted(plan.collisions.items()):
        rels = sorted(c.rel for c in cands)
        probes = {c.rel: _safe_probe(c) for c in cands}
        ordered = sorted(cands, key=lambda c: (_rank(probes[c.rel]), -rels.index(c.rel)), reverse=True)
        suggested = ordered[0]
        saved = manifest.choices.get(key)
        how = "suggested"
        pick = suggested
        if saved and saved.get("candidates") == rels and saved.get("chosen") in rels:
            pick = next(c for c in cands if c.rel == saved["chosen"])
            how = "your earlier choice"
        elif not (accept_all or dry_run or not can_ask):
            pick, accept_all = hooks.ask(suggested.dest_rel, ordered, probes)
            how = "your choice" if pick is not suggested else "suggested, confirmed by you"
        elif not can_ask and not yes and not dry_run:
            how = "suggested; no terminal to ask"
        chosen[key] = pick
        if not dry_run:
            manifest.choices[key] = {"candidates": rels, "chosen": pick.rel}
        report.collision(pick.dest_rel, pick, [c for c in cands if c is not pick], probes, how)
    return chosen


def _ask(dest_rel: str, ordered: list[Source], probes) -> tuple[Source, bool]:
    print(f"\nCollision: these files would all become \"{dest_rel}\":")
    for i, c in enumerate(ordered, 1):
        p = probes[c.rel]
        desc = p.describe() if p else "unreadable"
        mark = "   <- suggested" if i == 1 else ""
        print(f"  {i}) {c.rel}  [{desc}]{mark}")
    while True:
        try:
            ans = input(f"Keep which? [1-{len(ordered)}, Enter = 1, a = suggested for all the rest]: ")
        except EOFError:
            return ordered[0], True
        ans = ans.strip().lower()
        if ans == "":
            return ordered[0], False
        if ans == "a":
            return ordered[0], True
        if ans.isdigit() and 1 <= int(ans) <= len(ordered):
            return ordered[int(ans) - 1], False
        print("  Please type a number from the list, press Enter, or type a.")


# ---------------------------------------------------------------- run ----

def _cover_sig(src_root: Path, src: Source) -> str:
    if not src.folder_cover:
        return ""
    try:
        st = (src_root / src.folder_cover).stat()
        return f"{src.folder_cover}:{st.st_size}:{st.st_mtime_ns}"
    except OSError:
        return ""


def _scan_outputs(dst_root: Path) -> tuple[list[str], list[Path]]:
    """Existing .dsa files (relative) and leftover temp files on the card."""
    outputs, temps = [], []
    if not dst_root.exists():
        return outputs, temps
    for dirpath, dirnames, filenames in os.walk(dst_root):
        here = Path(dirpath)
        if here == dst_root:
            dirnames[:] = [d for d in dirnames if d != DATA_DIR]
        for n in filenames:
            if n.startswith(TMP_PREFIX) and n.endswith(".tmp"):
                temps.append(here / n)
            elif n.lower().endswith(OUT_EXT):
                outputs.append((here / n).relative_to(dst_root).as_posix())
    return outputs, temps


def _remove_empty_dirs(dst_root: Path, start: Path) -> None:
    d = start
    while d != dst_root and dst_root in d.parents:
        try:
            d.rmdir()
        except OSError:
            return
        d = d.parent


class SetupError(Exception):
    """The folders can't be used as given (shown to the user as is)."""


class Hooks:
    """How a run reports and asks. These defaults are the command line's;
    the window has its own."""

    def __init__(self, quiet: bool = False):
        self.quiet = quiet

    def say(self, msg: str) -> None:
        """A line of progress or the summary."""
        print(msg, flush=True)

    def phase(self, name: str) -> None:
        """Where the run is: "scan", "convert", "index" or "done"."""

    def song_done(self, done: int, total: int, src: Source, res: dict) -> None:
        if not self.quiet or not res["ok"]:
            width = len(str(total))
            status = "ok    " if res["ok"] else "FAILED"
            print(f"[{done:>{width}}/{total}] {status} {src.dest_rel}", flush=True)

    def can_ask(self) -> bool:
        return (sys.stdin is not None and sys.stdout is not None
                and sys.stdin.isatty() and sys.stdout.isatty())

    def ask(self, dest_rel: str, ordered: list[Source], probes) -> tuple[Source, bool]:
        """Which of these files becomes dest_rel; True as well: the suggestion for all the rest."""
        return _ask(dest_rel, ordered, probes)

    def stop_requested(self) -> bool:
        return False


@dataclass
class Prepared:
    """What a run will do, worked out before anything is written."""
    src_root: Path
    dst_root: Path
    report: Report
    manifest: Manifest
    selected: list[Source]
    todo: list[tuple[Source, str]]      # (source, its folder cover's signature)
    orphans: list[str]                  # on the card without a source
    temps: list[Path]                   # leftovers of an interrupted run
    t0: float
    playlists: list[Path] = field(default_factory=list)     # .m3u files in the music folder
    dest_by_rel: dict[str, str] = field(default_factory=dict)  # every song: source rel -> on the card

    @property
    def new(self) -> list[Source]:
        return [s for s, _ in self.todo if "out_size" not in self.manifest.files.get(s.dest_rel, {})]

    @property
    def changed(self) -> list[Source]:
        return [s for s, _ in self.todo if "out_size" in self.manifest.files.get(s.dest_rel, {})]


def prepare(src_root: Path, dst_root: Path, *, dry_run: bool = False, yes: bool = False,
            force: bool = False, hooks: Hooks | None = None) -> Prepared:
    """Scan, settle collisions (asking if allowed) and compare with the card.
    Writes nothing. Raises SetupError if the folders can't be used."""
    hooks = hooks or Hooks()
    src_root = src_root.expanduser().resolve()
    dst_root = dst_root.expanduser().resolve()
    if not src_root.is_dir():
        raise SetupError(f"source folder not found: {src_root}")
    if dst_root == src_root:
        raise SetupError("source and output are the same folder")
    if dst_root in src_root.parents:
        raise SetupError("the source folder is inside the output folder")

    t0 = time.monotonic()
    hooks.phase("scan")
    report = Report(src_root, dst_root, dry_run=dry_run)
    hooks.say(f"alya-bangers-dsconv {__version__}: scanning {src_root}")
    plan = build_plan(src_root, dst_root)
    manifest = Manifest(dst_root)
    if manifest.stale_settings:
        hooks.say("The file format or converter changed since the last run: everything will be reconverted.")
    report.plan(plan)

    chosen = resolve_collisions(plan, manifest, report, yes=yes, dry_run=dry_run, hooks=hooks)
    selected: list[Source] = []
    for key, cands in plan.groups.items():
        selected.append(chosen[key] if len(cands) > 1 else cands[0])
    selected.sort(key=lambda s: s.dest_rel.casefold())
    wanted = {fat_key(s.dest_rel) for s in selected}

    todo: list[tuple[Source, str]] = []
    for s in selected:
        sig = _cover_sig(src_root, s)
        if not force and manifest.up_to_date(s.dest_rel, s.rel, s.size, s.mtime_ns, sig, dst_root):
            report.unchanged += 1
        else:
            todo.append((s, sig))

    outputs, temps = _scan_outputs(dst_root)
    orphans = sorted(o for o in outputs if fat_key(o) not in wanted)
    orphans += sorted(k for k in manifest.files
                      if fat_key(k) not in wanted and k not in orphans and (dst_root / k).exists())
    report.orphans = orphans
    report.playlists_found = len(plan.playlists)
    return Prepared(src_root, dst_root, report, manifest, selected, todo, orphans, temps, t0,
                    plan.playlists, {s.rel: s.dest_rel for s in plan.sources})


# Bytes of output per second of stereo audio, plus the header and covers of one file.
OUT_BYTES_PER_SECOND = SAMPLE_RATE * 2 * 2
OUT_BYTES_PER_FILE = 48 * 1024


def _seconds(path: Path) -> float | None:
    try:
        import mutagen
        f = mutagen.File(path)
        if f is not None and f.info is not None and f.info.length > 0:
            return float(f.info.length)
    except Exception:
        pass
    return None


def space_needed(prep: Prepared, delete: bool) -> int:
    """About how many bytes the card needs on top of what's there now (negative:
    it gets emptier). From the songs' lengths, without decoding anything."""
    need = 0
    for s, _ in prep.todo:
        sec = _seconds(s.path)
        # unknown length: assume an MP3 of 256 kbit/s, a fair middle
        need += int((sec if sec is not None else s.size / 32000) * OUT_BYTES_PER_SECOND) + OUT_BYTES_PER_FILE
        old = prep.manifest.files.get(s.dest_rel, {}).get("out_size")
        if old:
            need -= old
    if delete:
        for rel in prep.orphans:
            try:
                need -= (prep.dst_root / rel).stat().st_size
            except OSError:
                pass
    return need


def free_space(dst_root: Path) -> int | None:
    """Free bytes where dst_root is (or would be)."""
    p = dst_root
    while not p.exists() and p != p.parent:
        p = p.parent
    try:
        return shutil.disk_usage(p).free
    except OSError:
        return None


def execute(prep: Prepared, *, delete: bool = False, jobs: int | None = None,
            build_index: bool = True, hooks: Hooks | None = None) -> int:
    """Convert what prepare() found, delete what's gone (if asked), build the
    index and font. 0: done, 1: some files failed, 130: stopped (progress kept)."""
    from .index import build_library_files  # local import keeps worker start-up light

    hooks = hooks or Hooks()
    src_root, dst_root, report, manifest = prep.src_root, prep.dst_root, prep.report, prep.manifest
    todo = prep.todo
    dst_root.mkdir(parents=True, exist_ok=True)
    for t in prep.temps:
        try:
            t.unlink()
        except OSError:
            pass

    hooks.phase("convert")
    hooks.say(f"{len(prep.selected)} songs: {report.unchanged} up to date, {len(todo)} to convert")
    interrupted = False
    if todo:
        workers = jobs or os.cpu_count() or 2
        done = 0
        since_save = 0
        # "spawn", not "fork": the main process may already have run FFmpeg
        # threads (probing collisions), and forking those deadlocks.
        ex = ProcessPoolExecutor(max_workers=workers, initializer=_worker_init,
                                 mp_context=multiprocessing.get_context("spawn"))
        futs = {}
        try:
            for s, sig in todo:
                job = Job(str(s.path), s.rel, str(dst_root / s.dest_rel),
                          str(src_root / s.folder_cover) if s.folder_cover else None,
                          zlib.crc32(s.rel.encode("utf-8")))
                futs[ex.submit(run_job, job)] = (s, sig)
            for fut in as_completed(futs):
                s, sig = futs[fut]
                res = fut.result()
                done += 1
                if res["ok"]:
                    manifest.record(s.dest_rel, s.rel, s.size, s.mtime_ns, sig, res["out_size"])
                    report.converted(s, res)
                else:
                    report.failed(s, res["error"])
                hooks.song_done(done, len(todo), s, res)
                since_save += 1
                if since_save >= 25:
                    manifest.save()
                    since_save = 0
                if hooks.stop_requested():
                    raise KeyboardInterrupt
        except KeyboardInterrupt:
            interrupted = True
            hooks.say("\nInterrupted: finishing the files in progress, then stopping…")
            ex.shutdown(wait=True, cancel_futures=True)
            # Files that were already handed to a worker still complete; record
            # them so the next run does not redo them.
            for fut, (s, sig) in futs.items():
                if fut.done() and not fut.cancelled() and fut.exception() is None:
                    res = fut.result()
                    if res["ok"]:
                        manifest.record(s.dest_rel, s.rel, s.size, s.mtime_ns, sig, res["out_size"])
        finally:
            ex.shutdown(wait=True, cancel_futures=True)

    if delete and not interrupted:
        for rel in prep.orphans:
            p = dst_root / rel
            try:
                p.unlink()
                report.deleted.append(rel)
            except FileNotFoundError:
                pass
            except OSError as e:
                report.warn(rel, f"could not delete: {e}")
            manifest.files.pop(rel, None)
            _remove_empty_dirs(dst_root, p.parent)
    manifest.save()

    if interrupted:
        hooks.say("Progress saved. Run the same command again to continue.")
        hooks.phase("done")
        return 130

    if build_index:
        hooks.phase("index")
        hooks.say("Building library index and font…")
        try:
            stats = build_library_files(dst_root)
            report.index_stats = stats
        except Exception as e:
            report.warn("(index)", f"index/font not built: {type(e).__name__}: {e}")

    if prep.playlists:
        hooks.say("Playlists…")
        try:
            report.playlists = bring_over(prep.playlists, src_root, dst_root, prep.dest_by_rel)
        except OSError as e:
            report.warn("(playlists)", f"not put on the card: {e}")

    report.finish(time.monotonic() - prep.t0, delete=delete)
    (dst_root / DATA_DIR).mkdir(exist_ok=True)
    report.write(dst_root / DATA_DIR / REPORT_NAME)
    report.show(hooks.say)
    hooks.phase("done")
    return 1 if report.failures else 0


def run_convert(src_root: Path, dst_root: Path, *, dry_run: bool = False, yes: bool = False,
                delete: bool = False, jobs: int | None = None, force: bool = False,
                build_index: bool = True, quiet: bool = False, hooks: Hooks | None = None) -> int:
    hooks = hooks or Hooks(quiet=quiet)
    try:
        prep = prepare(src_root, dst_root, dry_run=dry_run, yes=yes, force=force, hooks=hooks)
    except SetupError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if dry_run:
        prep.report.to_convert = [s.dest_rel for s, _ in prep.todo]
        prep.report.finish(time.monotonic() - prep.t0, delete=delete)
        prep.report.show(hooks.say)
        return 0
    return execute(prep, delete=delete, jobs=jobs, build_index=build_index, hooks=hooks)
