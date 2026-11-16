// SPDX-License-Identifier: MIT
//
// Both screens as 256x192 16-bit bitmaps. Drawing goes to a back buffer in
// RAM; gfx_present() copies changed screens to VRAM during vblank.

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
} Canvas;

void gfx_init(void);
Canvas *gfx_canvas(Screen s);
void gfx_present(void);

void gfx_fill(Canvas *c, int x, int y, int w, int h, uint16_t color);
void gfx_blit(Canvas *c, int x, int y, int w, int h, const uint16_t *src);

#endif
