"""The conversion run: plan, collisions, parallel conversion, cleanup, report."""

from __future__ import annotations

import math
import multiprocessing
import os
import signal
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from . import audio
from .constants import DATA_DIR, OUT_EXT, REPORT_NAME, __version__
from .cover import convert_cover
from .dsa import DsaWriter, encode_tags, replaygain
from .fat32 import fat_key
from .manifest import Manifest
from .plan import Plan, Source, build_plan
from .report import Report
from .tags import best_picture, read_tags

TMP_PREFIX = ".dsconv-"


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
                       yes: bool, dry_run: bool, interactive: bool) -> dict[str, Source]:
    chosen: dict[str, Source] = {}
    accept_all = yes
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
        elif not (accept_all or dry_run or not interactive):
            pick, accept_all = _ask(suggested.dest_rel, ordered, probes)
            how = "your choice" if pick is not suggested else "suggested, confirmed by you"
        elif not interactive and not yes and not dry_run:
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


def run_convert(src_root: Path, dst_root: Path, *, dry_run: bool = False, yes: bool = False,
                delete: bool = False, jobs: int | None = None, force: bool = False,
                build_index: bool = True, quiet: bool = False) -> int:
    from .index import build_library_files  # local import keeps worker start-up light

    src_root = src_root.expanduser().resolve()
    dst_root = dst_root.expanduser().resolve()
    if not src_root.is_dir():
        print(f"error: source folder not found: {src_root}", file=sys.stderr)
        return 2
    if dst_root == src_root:
        print("error: source and output are the same folder", file=sys.stderr)
        return 2
    if dst_root in src_root.parents:
        print("error: the source folder is inside the output folder", file=sys.stderr)
        return 2

    t0 = time.monotonic()
    report = Report(src_root, dst_root, dry_run=dry_run)
    print(f"dsconv {__version__}: scanning {src_root}")
    plan = build_plan(src_root, dst_root)
    manifest = Manifest(dst_root)
    if manifest.stale_settings:
        print("The file format or converter changed since the last run: everything will be reconverted.")
    report.plan(plan)

    chosen = resolve_collisions(plan, manifest, report, yes=yes, dry_run=dry_run,
                                interactive=sys.stdin.isatty() and sys.stdout.isatty())
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

    if dry_run:
        report.to_convert = [s.dest_rel for s, _ in todo]
        report.finish(time.monotonic() - t0, delete=delete)
        report.show()
        return 0

    dst_root.mkdir(parents=True, exist_ok=True)
    for t in temps:
        try:
            t.unlink()
        except OSError:
            pass

    print(f"{len(selected)} songs: {report.unchanged} up to date, {len(todo)} to convert")
    interrupted = False
    if todo:
        workers = jobs or os.cpu_count() or 2
        width = len(str(len(todo)))
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
                    status = "ok    "
                else:
                    report.failed(s, res["error"])
                    status = "FAILED"
                if not quiet or not res["ok"]:
                    print(f"[{done:>{width}}/{len(todo)}] {status} {s.dest_rel}", flush=True)
                since_save += 1
                if since_save >= 25:
                    manifest.save()
                    since_save = 0
        except KeyboardInterrupt:
            interrupted = True
            print("\nInterrupted: finishing the files in progress, then stopping…", flush=True)
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
        for rel in orphans:
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
        print("Progress saved. Run the same command again to continue.")
        return 130

    if build_index:
        print("Building library index and font…", flush=True)
        try:
            stats = build_library_files(dst_root)
            report.index_stats = stats
        except Exception as e:
            report.warn("(index)", f"index/font not built: {type(e).__name__}: {e}")

    report.finish(time.monotonic() - t0, delete=delete)
    (dst_root / DATA_DIR).mkdir(exist_ok=True)
    report.write(dst_root / DATA_DIR / REPORT_NAME)
    report.show()
    return 1 if report.failures else 0
