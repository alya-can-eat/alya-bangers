// SPDX-License-Identifier: MIT
//
// Text drawing with font.bin (written by dsconv), falling back to a few
// built-in glyphs (ASCII) when the file is missing or lacks a character.

#ifndef DSPLAYER_FONT_H__
#define DSPLAYER_FONT_H__

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

#define FONT_HEIGHT 16

// Load font.bin. Returns false (and keeps the ASCII fallback) on failure.
bool font_load(const char *path);
bool font_loaded(void);

// Width in pixels of a UTF-8 string.
int font_text_width(const char *s);

// Draw a UTF-8 string with its top-left corner at (x, y). Text wider than
// max_w is cut and ends with "…". Returns the width drawn.
int font_draw(Canvas *c, int x, int y, int max_w, const char *s, uint16_t color);

#endif
