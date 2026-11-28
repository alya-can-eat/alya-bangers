// SPDX-License-Identifier: GPL-3.0-or-later
//
// Starting another .nds (a launcher) through Pico Loader, which has to be on
// the card in /_pico (the DSpico has it; other cards can get it). On the PC
// tests this is a mock (tests/host/hostsim.c).

#ifndef DSPLAYER_BOOT_H__
#define DSPLAYER_BOOT_H__

#include <stdbool.h>

#define PICO_LOADER_9   "/_pico/picoLoader9.bin"
#define PICO_LOADER_7   "/_pico/picoLoader7.bin"

// Pico Loader is on the card (both parts, not too big for where they go).
bool boot_ready(void);

// Starts the .nds at path ("/…", or with its drive: "fat:/…", "sd:/…").
// Doesn't come back when it works; false when it couldn't (the screens and
// sound are as they were, apart from the brightness).
bool boot_nds(const char *path);

#endif
