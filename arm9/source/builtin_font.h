// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSPLAYER_BUILTIN_FONT_H__
#define DSPLAYER_BUILTIN_FONT_H__

#include <stdint.h>

typedef struct {
    uint32_t cp;
    uint8_t width;          // 8 or 16
    uint8_t bitmap[32];     // 16 rows, 1 or 2 bytes per row, MSB = leftmost pixel
} BuiltinGlyph;

extern const BuiltinGlyph builtin_glyphs[];
extern const int builtin_glyph_count;

#endif
