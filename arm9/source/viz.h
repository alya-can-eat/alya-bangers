// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSPLAYER_VIZ_H__
#define DSPLAYER_VIZ_H__

#include <stdbool.h>

#define VIZ_BANDS 20

// Tables (a few ms of float math): call at start, not while music plays.
void viz_init(void);
// Measure the bands once per drawn frame; levels fall slowly when quiet.
// Playing, the levels of what plays now; else they fall to 0.
// True while any bar is still up.
bool viz_update(bool playing);
// 0..256
int viz_level(int band);

#endif
