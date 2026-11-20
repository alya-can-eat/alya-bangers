"""End-to-end tests on a generated library (see fixtures.py)."""

import os
import shutil
import struct
import time
from pathlib import Path

import numpy as np
import pytest

import fixtures
from dsconv.constants import BLOCK_FRAMES, HEADER_SIZE, SAMPLE_RATE, SECTOR
from dsconv.convert import run_convert
from dsconv.dsa import FLAG_COVER, open_dsa
from dsconv.index import read_font, read_index
from dsconv.manifest import Manifest

YT = "YouTube Music"


@pytest.fixture(scope="session")
def library(tmp_path_factory):
    root = tmp_path_factory.mktemp("lib")
    facts = fixtures.build_library(root)
    return root, facts


@pytest.fixture(scope="session")
def converted(library, tmp_path_factory):
    src, facts = library
    dst = tmp_path_factory.mktemp("card") / "music"
    rc = run_convert(src, dst, yes=True, jobs=2, quiet=True)
    assert rc == 0
    return src, dst, facts


def dsa(dst, rel):
    return open_dsa(dst / rel)


def peak_hz(x: np.ndarray) -> float:
    n = len(x)
    spec = np.abs(np.fft.rfft(x * np.hanning(n)))
    return np.argmax(spec) * SAMPLE_RATE / n


def test_tree_is_mirrored_with_fat32_names(converted):
    _, dst, _ = converted
    expect = [
        "Bandcamp/300 Lanterns/300 Lanterns - HALCYON/300 Lanterns - HALCYON - 01 HALCYON 1.dsa",
        "Bandcamp/Kivara/Kivara - Paper Archive vol.1/Kivara - Paper Archive vol.1 - 01 またねまたねまたね.dsa",
        "CD/Mörli - Bergwind/Track 10.dsa",
        "CD/GrEaT NO.2 sOnGs/Disc 2/Track 1.dsa",
        f"{YT}/VXLTRA - 00：00.dsa",
        f"{YT}/Brassfox - Good F＊＊King Noise.dsa",
        f"{YT}/Dear Wren....dsa",                      # dots before .dsa are fine
        "Bandcamp/Demos\uFF0E\uFF0E\uFF0E/demo.dsa",          # folder: trailing dots -> ．
        f"{YT}/CON_.dsa",
        f"{YT}/Kid 🎵 Song.dsa",
        f"{YT}/Lúa - 5 Phút Mơ.dsa",   # NFC, although the source name was NFD
        "KIVARA/a.dsa", "KIVARA/b.dsa",      # Kivara/ and KIVARA/ merged
    ]
    for rel in expect:
        assert (dst / rel).is_file(), rel
    names = {p.relative_to(dst).as_posix() for p in dst.rglob("*.dsa")}
    assert not any("hidden" in n or n.startswith(".directory") for n in names)
    assert len(names) == 30


def test_ignored_files_are_not_converted(converted):
    _, dst, _ = converted
    report = (dst / ".dsmusic" / "last-run.txt").read_text(encoding="utf-8")
    assert "Ignored:" in report and ".aup3" in report and ".txt" in report and ".zip" in report


def test_header_layout_and_alignment(converted):
    _, dst, _ = converted
    for p in dst.rglob("*.dsa"):
        f = open_dsa(p)
        h = f.header
        assert h["sound_clock_hz"] / h["timer_divider"] == SAMPLE_RATE
        assert h["audio_offset"] % SECTOR == 0
        assert h["audio_offset"] >= HEADER_SIZE + h["tags_size"]
        assert h["block_frames"] == BLOCK_FRAMES
        assert h["audio_size"] == h["block_count"] * BLOCK_FRAMES * 2 * h["channels"]
        assert p.stat().st_size == h["audio_offset"] + h["audio_size"]
        assert (h["block_count"] - 1) * BLOCK_FRAMES < h["frame_count"] <= h["block_count"] * BLOCK_FRAMES


def test_audio_rate_length_and_channels(converted):
    _, dst, _ = converted
    f = dsa(dst, "Bandcamp/300 Lanterns/300 Lanterns - HALCYON/300 Lanterns - HALCYON - 02 HALCYON 2.dsa")
    assert f.header["frame_count"] == round(0.6 * SAMPLE_RATE)   # lossless: sample exact
    pcm = f.read_audio().astype(np.float64) / 32768
    left, right = pcm[2000:18000, 0], pcm[2000:18000, 1]
    assert abs(peak_hz(left) - 380) < 3       # fixture: left = 300 + 40 * 2 Hz
    assert abs(peak_hz(right) - 570) < 3      # right = 1.5 x left
    assert 0.35 < np.abs(left).max() < 0.45   # amplitude 0.4 survives


def test_padding_after_last_frame_is_silent(converted):
    _, dst, _ = converted
    f = dsa(dst, f"{YT}/Tide Drive.dsa")
    h = f.header
    with (dst / f"{YT}/Tide Drive.dsa").open("rb") as fh:
        fh.seek(h["audio_offset"] + (h["block_count"] - 1) * BLOCK_FRAMES * 2 * h["channels"])
        last = np.frombuffer(fh.read(BLOCK_FRAMES * 2 * h["channels"]), "<i2").reshape(h["channels"], BLOCK_FRAMES)
    used = h["frame_count"] - (h["block_count"] - 1) * BLOCK_FRAMES
    assert not last[:, used:].any()


def test_mono_stays_mono_and_surround_becomes_stereo(converted):
    _, dst, _ = converted
    assert dsa(dst, "Soundcloud/Mono Voice.dsa").channels == 1
    assert dsa(dst, f"{YT}/Surround Test.dsa").channels == 2


def test_lossy_lengths_respect_encoder_delay(converted):
    _, dst, _ = converted
    for rel, seconds in [(f"{YT}/VXLTRA - 00：00.dsa", 0.5),
                         (f"{YT}/Corin Vale - Skyward.dsa", 0.6),
                         ("Soundcloud/minto - 星之河 (feat. 林星月).dsa", 0.8)]:
        frames = dsa(dst, rel).header["frame_count"]
        assert abs(frames / SAMPLE_RATE - seconds) < 0.03, rel


def test_tags_are_read_from_every_container(converted):
    _, dst, _ = converted
    t = dsa(dst, "Bandcamp/Kivara/Kivara - Paper Archive vol.1/Kivara - Paper Archive vol.1 - 01 またねまたねまたね.dsa").tags
    assert t["title"] == ["またねまたねまたね"] and t["genre"] == ["Breakcore", "Electronic"]
    t = dsa(dst, "Soundcloud/minto - 星之河 (feat. 林星月).dsa").tags
    assert t["title"] == ["星之河 (feat. 林星月)"] and t["artist"] == ["minto"] and t["~codec"] == ["MP3"]
    t = dsa(dst, f"{YT}/Corin Vale - Skyward.dsa").tags
    assert t["title"] == ["Skyward"] and t["tracknumber"] == ["3/9"]
    t = dsa(dst, "CD/Cobalt Engine/01-Dream Made Of Glass-mastered v4 24bit.dsa").tags
    assert t["title"] == ["Dream Made Of Glass"] and t["album"] == ["PRISM"] and t["~source_bits"] == ["24"]
    t = dsa(dst, f"{YT}/Lúa - 5 Phút Mơ.dsa").tags
    assert t["title"] == ["5 Phút Mơ"]    # NFC
    t = dsa(dst, f"{YT}/VXLTRA - 00：00.dsa").tags
    assert t["~source"] == [f"{YT}/VXLTRA - 00:00.opus"]


def test_replaygain(converted):
    _, dst, _ = converted
    h = dsa(dst, "Soundcloud/minto - 星之河 (feat. 林星月).dsa").header
    assert h["flags"] & 2 and h["rg_track_gain_mdb"] == -6500
    h = dsa(dst, f"{YT}/VXLTRA - 00：00.dsa").header
    assert h["flags"] & 2 and h["rg_track_gain_mdb"] == 3000   # R128 -512/256 = -2 dB, +5 dB


def _center_and_corner(f):
    from dsconv.cover import bgr555_to_image
    img = bgr555_to_image(f.read_cover(), 128)
    return img.getpixel((64, 64)), img.getpixel((4, 4))


def test_cover_priority(converted):
    _, dst, _ = converted
    # folder cover.png (green) beats "huge cover.png" (magenta)
    c, edge = _center_and_corner(dsa(dst, "Bandcamp/300 Lanterns/300 Lanterns - HALCYON/300 Lanterns - HALCYON - 01 HALCYON 1.dsa"))
    assert c == (255, 255, 255) and edge[1] > 150 and edge[0] < 60
    # embedded red cover beats the folder's black cover.jpg
    c, edge = _center_and_corner(dsa(dst, "Bandcamp/Kivara/Kivara - Paper Archive vol.1/Kivara - Paper Archive vol.1 - 01 またねまたねまたね.dsa"))
    assert edge[0] > 150 and edge[1] < 60
    # 16:9 thumbnail cropped to the centre: white square spans half the width
    f = dsa(dst, f"{YT}/Brassfox - Good F＊＊King Noise.dsa")
    from dsconv.cover import bgr555_to_image
    img = bgr555_to_image(f.read_cover(), 128)
    row = [img.getpixel((x, 64)) for x in range(128)]
    white = [x for x, p in enumerate(row) if p[0] > 200 and p[2] > 200]
    assert 28 <= white[0] <= 36 and 92 <= white[-1] <= 99
    assert not dsa(dst, "CD/Mörli - Bergwind/Track 1.dsa").header["flags"] & FLAG_COVER


def test_collisions_prefer_better_format(converted):
    _, dst, _ = converted
    assert dsa(dst, f"{YT}/Tide Drive.dsa").tags["~codec"] == ["FLAC"]
    pebble = list((dst / YT).glob("[Pp][Ee][Bb][Bb][Ll][Ee].dsa"))
    assert len(pebble) == 1 and dsa(dst, f"{YT}/{pebble[0].name}").tags["~codec"] == ["Opus"]
    m = Manifest(dst)
    assert len(m.choices) == 2


def test_index(converted):
    _, dst, _ = converted
    idx = read_index((dst / ".dsmusic" / "library.idx").read_bytes())
    V = idx["values"]
    paths = [t["path"] for t in idx["tracks"]]
    bergwind = [p for p in paths if "Bergwind" in p]
    assert bergwind == ["CD/Mörli - Bergwind/Track 1.dsa", "CD/Mörli - Bergwind/Track 2.dsa", "CD/Mörli - Bergwind/Track 10.dsa"]
    albums = {V[a["name"]]: a for a in idx["albums"]}
    halcyon = albums["HALCYON"]
    assert [idx["tracks"][i]["track"] for i in halcyon["tracks"]] == [1, 2, 3, 10]
    assert V[halcyon["albumartist"]] == "300 Lanterns" and halcyon["year"] == 2021
    best = albums["GrEaT NO.2 sOnGs"]           # Disc 1/ and Disc 2/ form one album
    assert [idx["tracks"][i]["disc"] for i in best["tracks"]] == [1, 2]
    assert "YouTube Music" not in albums       # no album tag, no album
    assert V == sorted(V, key=__import__("dsconv.collate", fromlist=["sort_key"]).sort_key)
    t = next(t for t in idx["tracks"] if t["path"].endswith("春風レター.dsa"))
    assert V[t["title"]] == "春風レター" and t["tags"]["artist"] == ["Kumoyuki"]


def test_readings_for_search(converted):
    _, dst, _ = converted
    idx = read_index((dst / ".dsmusic" / "library.idx").read_bytes())
    r = idx["readings"]
    assert r["春風レター"][0] == "はるかぜれたー"                  # kana: 春 is found by はる
    assert r["星之河 (feat. 林星月)"][1] == "xīng zhī hé (feat. lín xīng yuè)"   # pinyin: found by "lin"
    assert "またねまたねまたね" not in r and "Kumoyuki" not in r  # no kanji, no reading needed
    font = read_font((dst / ".dsmusic" / "font.bin").read_bytes())
    assert all((ord(ch), 0) in font for ch in "línxīngyuèは")  # readings can be drawn


def test_font_covers_library_text(converted):
    _, dst, _ = converted
    font = read_font((dst / ".dsmusic" / "font.bin").read_bytes())
    for ch in "またね星之河林星月🎵ơú：＊Aa0":
        assert (ord(ch), 0) in font, ch
    assert font[(ord("三"), 0)]["width"] == 16 and font[(ord("A"), 0)]["width"] == 8
    assert (ord("春"), 1) in font            # Japanese variant: 春 appears next to kana


def test_second_run_is_incremental(converted, tmp_path):
    src, dst, _ = converted
    work_src = tmp_path / "src"
    shutil.copytree(src, work_src)
    work_dst = tmp_path / "dst"
    shutil.copytree(dst, work_dst)
    # sources moved, so their paths in the manifest still match (relative)
    for p in work_src.rglob("*"):
        st = (src / p.relative_to(work_src)).stat()
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))
    assert run_convert(work_src, work_dst, yes=True, jobs=2, quiet=True) == 0
    m = Manifest(work_dst)
    before = {k: v["out_size"] for k, v in m.files.items()}
    assert len(before) == 30

    target = work_dst / "Bandcamp/300 Lanterns/300 Lanterns - HALCYON/300 Lanterns - HALCYON - 03 HALCYON 3.dsa"
    mtime = target.stat().st_mtime_ns
    time.sleep(0.01)
    other = work_src / "CD/Mörli - Bergwind/Track 2.flac"
    os.utime(other, ns=(other.stat().st_atime_ns, other.stat().st_mtime_ns + 10**9))
    assert run_convert(work_src, work_dst, yes=True, jobs=2, quiet=True) == 0
    assert target.stat().st_mtime_ns == mtime         # untouched
    report = (work_dst / ".dsmusic" / "last-run.txt").read_text(encoding="utf-8")
    assert "Converted 1 files" in report

    shutil.rmtree(work_src / "KIVARA")
    (work_src / "Kivara" / "a.flac").unlink()
    assert run_convert(work_src, work_dst, yes=True, jobs=2, quiet=True, delete=True) == 0
    assert not (work_dst / "KIVARA").exists()          # emptied folder removed too
    assert "KIVARA/a.dsa" not in Manifest(work_dst).files


def test_dry_run_writes_nothing(library, tmp_path):
    src, _ = library
    dst = tmp_path / "out"
    assert run_convert(src, dst, dry_run=True, yes=True) == 0
    assert not dst.exists()


def test_interactive_collision_choice_is_remembered(library, tmp_path, monkeypatch):
    src, _ = library
    small = tmp_path / "src" / YT
    small.mkdir(parents=True)
    for n in ("Tide Drive.flac", "Tide Drive.opus"):
        shutil.copy2(src / YT / n, small / n)
    dst = tmp_path / "out"
    answers = iter(["9", "2"])  # an invalid answer first, then the Opus file
    monkeypatch.setattr("builtins.input", lambda prompt="": next(answers))
    monkeypatch.setattr("sys.stdin.isatty", lambda: True, raising=False)
    monkeypatch.setattr("sys.stdout.isatty", lambda: True, raising=False)
    assert run_convert(tmp_path / "src", dst, jobs=1, quiet=True) == 0
    assert open_dsa(dst / YT / "Tide Drive.dsa").tags["~codec"] == ["Opus"]
    # second run: no question, same choice
    monkeypatch.setattr("builtins.input", lambda prompt="": pytest.fail("asked twice"))
    assert run_convert(tmp_path / "src", dst, jobs=1, quiet=True) == 0
    assert open_dsa(dst / YT / "Tide Drive.dsa").tags["~codec"] == ["Opus"]


def test_damaged_file_is_reported_not_fatal(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    (src / "broken.flac").write_bytes(b"fLaC" + b"\x00" * 100)
    fixtures.encode(src / "fine.flac", "flac", 44100, seconds=0.2)
    dst = tmp_path / "out"
    assert run_convert(src, dst, yes=True, jobs=1, quiet=True) == 1
    assert (dst / "fine.dsa").exists() and not (dst / "broken.dsa").exists()
    assert not list(dst.rglob("*.tmp"))
    assert "FAILED" in (dst / ".dsmusic" / "last-run.txt").read_text(encoding="utf-8")


def test_loud_master_is_turned_down_not_clipped(converted):
    _, dst, _ = converted
    f = dsa(dst, "Bandcamp/Loud/hot.dsa")
    pcm = f.read_audio()
    assert np.abs(pcm.astype(np.int32)).max() <= 32766       # nothing pinned to the rails
    pregain = float(f.tags["~pregain"][0].split()[0])
    assert -3.0 < pregain < -0.1
    # ReplayGain compensates, so playback with ReplayGain is unchanged
    assert abs(f.header["rg_track_gain_mdb"] - (-9.0 - pregain) * 1000) <= 5
    assert abs(f.header["rg_track_peak_q16"] - 10 ** (pregain / 20) * 65536) < 40  # tag is rounded to 0.01 dB
    report = (dst / ".dsmusic" / "last-run.txt").read_text(encoding="utf-8")
    assert "Turned down to avoid clipping: 1 songs" in report
    assert "samples clipped" not in report


def test_disc_folders_use_the_album_cover(converted):
    _, dst, _ = converted
    for disc in ("Day Disc", "Night Disc"):
        f = dsa(dst, f"CD/Two Discs/{disc}/Track 1.dsa")
        assert f.tags["~cover"] == ["folder:cover.jpg"]
        assert f.header["flags"] & FLAG_COVER
