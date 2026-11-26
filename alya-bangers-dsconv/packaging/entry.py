"""Start of the packed app (the .exe and the AppImage): the window when
double-clicked, the command line with arguments."""

import multiprocessing
import sys

if __name__ == "__main__":
    # the conversions run in worker processes started from this same file
    multiprocessing.freeze_support()
    if sys.platform == "win32" and len(sys.argv) > 1:
        # The .exe is a window app, so it has no console of its own. Started
        # from one with arguments, write there.
        import ctypes
        if ctypes.windll.kernel32.AttachConsole(-1):
            sys.stdout = open("CONOUT$", "w", encoding="utf-8", errors="replace")
            sys.stderr = sys.stdout
            sys.stdin = open("CONIN$", "r", encoding="utf-8", errors="replace")
    from alya_bangers_dsconv.cli import main
    sys.exit(main())
