// SPDX-License-Identifier: GPL-3.0-or-later
//
// A colour triangle: a ring of hues around a triangle that points at the
// chosen hue. Its corners are the pure hue, white and black, so a point
// inside picks how strong and how bright the colour is.

#ifndef DSPLAYER_COLORPICK_H__
#define DSPLAYER_COLORPICK_H__

#include <stdint.h>

#include "gfx.h"

#define CP_CX       174     // centre on the bottom screen
#define CP_CY       106
#define CP_R_OUT    80      // the ring
#define CP_R_IN     66
#define CP_R_TRI    62      // the triangle's corners

#define CP_HUES     1536    // hue steps around the ring (6 x 256)

typedef struct {
    int h;      // 0 .. CP_HUES-1, red at the right, counterclockwise
    int s, v;   // 0..256
} Hsv;

uint16_t cp_hsv_to_rgb(Hsv c);
// keep_h: the hue to keep for grays (which have none)
Hsv cp_rgb_to_hsv(uint16_t rgb, int keep_h);

enum { CP_NONE, CP_RING, CP_TRIANGLE };
// Which part of the picker a point is on.
int cp_part_at(int x, int y);
// The hue under a point (for the ring).
int cp_hue_at(int x, int y);
// Strength and brightness under a point, kept inside the triangle of hue h.
void cp_sv_at(int h, int x, int y, int *s, int *v);

// The picker with its left edge moved by dx (the settings pages slide).
void cp_draw(Canvas *cv, int dx, Hsv c);

#endif
