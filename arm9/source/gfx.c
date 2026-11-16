// SPDX-License-Identifier: MIT

#include <nds.h>

#include "gfx.h"

static uint16_t back_top[SCREEN_W * SCREEN_H] __attribute__((aligned(32)));
static uint16_t back_bottom[SCREEN_W * SCREEN_H] __attribute__((aligned(32)));
static Canvas canvases[2] = { { back_top, true }, { back_bottom, true } };
static uint16_t *vram[2];

void gfx_init(void)
{
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    lcdMainOnTop();

    int top = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    int bottom = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    vram[SCREEN_TOP] = bgGetGfxPtr(top);
    vram[SCREEN_BOTTOM] = bgGetGfxPtr(bottom);
}

Canvas *gfx_canvas(Screen s)
{
    return &canvases[s];
}

void gfx_present(void)
{
    for (int i = 0; i < 2; i++)
    {
        Canvas *c = &canvases[i];
        if (!c->dirty)
            continue;
        // A bitmap row is 256 pixels both in RAM and in VRAM, so the 192
        // visible rows are one contiguous copy.
        DC_FlushRange(c->px, sizeof(back_top));
        dmaCopy(c->px, vram[i], sizeof(back_top));
        c->dirty = false;
    }
}

void gfx_fill(Canvas *c, int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0)
        return;
    for (int j = 0; j < h; j++)
    {
        uint16_t *row = c->px + (y + j) * SCREEN_W + x;
        for (int i = 0; i < w; i++)
            row[i] = color;
    }
    c->dirty = true;
}

void gfx_blit(Canvas *c, int x, int y, int w, int h, const uint16_t *src)
{
    for (int j = 0; j < h; j++)
    {
        int yy = y + j;
        if (yy < 0 || yy >= SCREEN_H)
            continue;
        for (int i = 0; i < w; i++)
        {
            int xx = x + i;
            if (xx >= 0 && xx < SCREEN_W)
                c->px[yy * SCREEN_W + xx] = src[j * w + i] | 0x8000;
        }
    }
    c->dirty = true;
}
