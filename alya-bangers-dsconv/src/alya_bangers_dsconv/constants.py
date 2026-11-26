"""Numbers shared by the converter, the index and the DS player."""

__version__ = "1.1.dev0"


def version_label(v: str = __version__) -> str:
    """As people see it: "1.1.dev0" (between releases) is "pre-1.1"."""
    return "pre-" + v.split(".dev")[0] if ".dev" in v else v


# Bump FORMAT_VERSION whenever the .dsa layout changes, CONVERTER_REVISION when the
# converter produces different output with the same layout. Either one makes the
# next run reconvert every file.
FORMAT_VERSION = 1
INDEX_VERSION = 1
FONT_VERSION = 1
CONVERTER_REVISION = 2  # 2: headroom instead of clipping

# DS sound hardware: channel timers count at the ARM7 bus clock / 2, and the
# mixer emits one output sample every 512 of those ticks. A channel timer
# divider of 512 therefore plays at exactly the mixer rate, so the hardware
# never resamples (it has no interpolation).
SOUND_CLOCK_HZ = 16_756_991
TIMER_DIVIDER = 512
SAMPLE_RATE = SOUND_CLOCK_HZ / TIMER_DIVIDER  # 32728.498046875 Hz

SECTOR = 512
HEADER_SIZE = 512
BLOCK_FRAMES = 4096  # frames per audio block, per channel

COVER_SIZE = 128
THUMB_SIZE = 32

MAX_TAG_VALUE_BYTES = 16 * 1024

AUDIO_EXTS = {
    ".flac", ".wav", ".wave", ".mp3", ".opus", ".ogg", ".oga",
    ".m4a", ".aac", ".aif", ".aiff", ".wv",
}
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif"}
COVER_STEMS = ("cover", "folder", "front", "album", "albumart")

# Folder names that are one disc of a bigger release ("Disc 1", "CD2",
# "Disc 01 [Side:Dawn]", "Day Disc", "1"). Used for album grouping and
# for looking up the cover in the parent folder.
DISC_DIR_PATTERN = r"^(?:disc|disk|cd)\b|\bdisc$|^\d{1,2}$"

OUT_EXT = ".dsa"
DATA_DIR = ".dsmusic"
MANIFEST_NAME = "manifest.json"
INDEX_NAME = "library.idx"
FONT_NAME = "font.bin"
REPORT_NAME = "last-run.txt"
NONE_ID = 0xFFFFFFFF
