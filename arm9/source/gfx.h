// SPDX-License-Identifier: MIT
//
// Both screens as 256x192 16-bit bitmaps. Drawing goes to a back buffer in
// RAM; gfx_present() copies changed screens to VRAM during vblank. Every
// primitive is clipped to the canvas' clip rectangle.

#ifndef DSPLAYER_GFX_H__
#define DSPLAYER_GFX_H__

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 256
#define SCREEN_H 192

// Opaque BGR555 color (bit 15 must be set or the pixel is transparent).
#define COLOR(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10) | 0x8000))

typedef enum { SCREEN_TOP = 0, SCREEN_BOTTOM = 1 } Screen;

typedef struct {
    uint16_t *px;   // SCREEN_W * SCREEN_H pixels
    bool dirty;
    int cx0, cy0, cx1, cy1;     // clip rectangle, x1/y1 exclusive
} Canvas;

void gfx_init(void);
Canvas *gfx_canvas(Screen s);
void gfx_present(void);

void gfx_clip(Canvas *c, int x0, int y0, int x1, int y1);
void gfx_unclip(Canvas *c);

void gfx_fill(Canvas *c, int x, int y, int w, int h, uint16_t color);
void gfx_rect(Canvas *c, int x, int y, int w, int h, uint16_t color);   // 1 px outline
void gfx_round_fill(Canvas *c, int x, int y, int w, int h, int r, uint16_t color);
void gfx_disc(Canvas *c, int cx, int cy, int r, uint16_t color);
void gfx_triangle(Canvas *c, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
void gfx_blit(Canvas *c, int x, int y, int w, int h, const uint16_t *src);

// Mix two colors: t = 0..256 parts of a.
uint16_t gfx_mix(uint16_t a, uint16_t b, int t);

#endif
