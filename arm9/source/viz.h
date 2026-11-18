// SPDX-License-Identifier: MIT

#ifndef DSPLAYER_VIZ_H__
#define DSPLAYER_VIZ_H__

#define VIZ_BANDS 20

// Tables (a few ms of float math): call at start, not while music plays.
void viz_init(void);
// Measure the bands once per drawn frame; levels fall slowly when quiet.
void viz_update(void);
// 0..256
int viz_level(int band);

#endif
