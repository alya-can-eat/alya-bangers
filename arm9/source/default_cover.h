// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSPLAYER_DEFAULT_COVER_H__
#define DSPLAYER_DEFAULT_COVER_H__

#include <stdint.h>

// Shown for songs without a cover: 128x128 and 32x32, BGR555.
extern const uint16_t default_cover[128 * 128];
extern const uint16_t default_thumb[32 * 32];

#endif
