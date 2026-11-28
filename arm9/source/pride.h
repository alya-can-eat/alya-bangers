// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pride flags for the player's play button (Settings › Theme › Pride).

#ifndef DSPLAYER_PRIDE_H__
#define DSPLAYER_PRIDE_H__

#include <stdint.h>

enum {
    PRIDE_NONE,                 // the accent colour
    PRIDE_RAINBOW, PRIDE_PROGRESS, PRIDE_LESBIAN, PRIDE_GAY, PRIDE_BI, PRIDE_PAN, PRIDE_ASEXUAL,
    PRIDE_TRANS, PRIDE_NONBINARY, PRIDE_AGENDER, PRIDE_AROMANTIC, PRIDE_INTERSEX, PRIDE_GENDERFLUID,
    PRIDE_GENDERQUEER, PRIDE_AROACE, PRIDE_DEMISEXUAL, PRIDE_DEMIGENDER,
    PRIDE_COUNT
};

// The flag's colour (BGR555, bit 15 set) at pixel x, y of a w x h picture of it.
uint16_t pride_color(int flag, int x, int y, int w, int h);

#endif
