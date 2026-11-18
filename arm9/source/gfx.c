// SPDX-License-Identifier: MIT

#include <nds.h>

#include "gfx.h"

static uint16_t back_top[SCREEN_W * SCREEN_H] __attribute__((aligned(32)));
static uint16_t back_bottom[SCREEN_W * SCREEN_H] __attribute__((aligned(32)));
static Canvas canvases[2] = {
    { back_top, true, 0, 0, SCREEN_W, SCREEN_H },
    { back_bottom, true, 0, 0, SCREEN_W, SCREEN_H },
};
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

void gfx_clip(Canvas *c, int x0, int y0, int x1, int y1)
{
    c->cx0 = x0 < 0 ? 0 : x0;
    c->cy0 = y0 < 0 ? 0 : y0;
    c->cx1 = x1 > SCREEN_W ? SCREEN_W : x1;
    c->cy1 = y1 > SCREEN_H ? SCREEN_H : y1;
}

void gfx_unclip(Canvas *c)
{
    gfx_clip(c, 0, 0, SCREEN_W, SCREEN_H);
}

static inline void span(Canvas *c, int y, int x0, int x1, uint16_t color)
{
    if (y < c->cy0 || y >= c->cy1)
        return;
    if (x0 < c->cx0) x0 = c->cx0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (x0 >= x1)
        return;
    uint16_t *p = c->px + y * SCREEN_W + x0;
    uint16_t *end = c->px + y * SCREEN_W + x1;
    if (((uintptr_t)p & 2) && p < end)
        *p++ = color;
    uint32_t two = color | ((uint32_t)color << 16);
    while (p + 2 <= end)
    {
        *(uint32_t *)(void *)p = two;
        p += 2;
    }
    if (p < end)
        *p = color;
}

void gfx_fill(Canvas *c, int x, int y, int w, int h, uint16_t color)
{
    for (int j = 0; j < h; j++)
        span(c, y + j, x, x + w, color);
    c->dirty = true;
}

void gfx_rect(Canvas *c, int x, int y, int w, int h, uint16_t color)
{
    span(c, y, x, x + w, color);
    span(c, y + h - 1, x, x + w, color);
    for (int j = 1; j < h - 1; j++)
    {
        span(c, y + j, x, x + 1, color);
        span(c, y + j, x + w - 1, x + w, color);
    }
    c->dirty = true;
}

// Inset of a rounded corner on row j (0 = top edge) for radius r.
static int corner_inset(int j, int r)
{
    int d = r - j;  // distance from the corner circle's centre row
    if (d <= 0)
        return 0;
    int k = 0;
    while (k < r && (r - k) * (r - k) + d * d > r * r + r)
        k++;
    return k;
}

void gfx_round_fill(Canvas *c, int x, int y, int w, int h, int r, uint16_t color)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++)
    {
        int k = 0;
        if (j < r)
            k = corner_inset(j, r);
        else if (j >= h - r)
            k = corner_inset(h - 1 - j, r);
        span(c, y + j, x + k, x + w - k, color);
    }
    c->dirty = true;
}

void gfx_disc(Canvas *c, int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++)
    {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r)
            dx++;
        span(c, cy + dy, cx - dx, cx + dx + 1, color);
    }
    c->dirty = true;
}

void gfx_triangle(Canvas *c, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    int ymin = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int ymax = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    const int xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
    for (int y = ymin; y <= ymax; y++)
    {
        // intersections of the scanline (at the pixel centre) with the edges
        int lo = 1 << 30, hi = -(1 << 30);
        for (int e = 0; e < 3; e++)
        {
            int ax = xs[e], ay = ys[e], bx = xs[(e + 1) % 3], by = ys[(e + 1) % 3];
            if (ay == by)
            {
                if (y == ay)
                {
                    if (ax < lo) lo = ax;
                    if (bx < lo) lo = bx;
                    if (ax > hi) hi = ax;
                    if (bx > hi) hi = bx;
                }
                continue;
            }
            if ((y < ay && y < by) || (y > ay && y > by))
                continue;
            // x = ax + (y - ay) * (bx - ax) / (by - ay), rounded
            int num = (y - ay) * (bx - ax) * 2 + (by - ay);
            int den = (by - ay) * 2;
            int x = ax + (num >= 0 ? num / den : -((-num + den - 1) / den));
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        if (lo <= hi)
            span(c, y, lo, hi + 1, color);
    }
    c->dirty = true;
}

void gfx_blit(Canvas *c, int x, int y, int w, int h, const uint16_t *src)
{
    int x0 = x < c->cx0 ? c->cx0 : x;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w;
    if (x0 >= x1)
        return;
    for (int j = 0; j < h; j++)
    {
        int yy = y + j;
        if (yy < c->cy0 || yy >= c->cy1)
            continue;
        const uint16_t *s = src + j * w + (x0 - x);
        uint16_t *d = c->px + yy * SCREEN_W + x0;
        for (int i = 0; i < x1 - x0; i++)
            d[i] = s[i] | 0x8000;
    }
    c->dirty = true;
}

uint16_t gfx_mix(uint16_t a, uint16_t b, int t)
{
    int r = ((a & 31) * t + (b & 31) * (256 - t) + 128) >> 8;
    int g = (((a >> 5) & 31) * t + ((b >> 5) & 31) * (256 - t) + 128) >> 8;
    int bl = (((a >> 10) & 31) * t + ((b >> 10) & 31) * (256 - t) + 128) >> 8;
    return COLOR(r, g, bl);
}
