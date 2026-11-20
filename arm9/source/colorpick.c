// SPDX-License-Identifier: GPL-3.0-or-later
//
// The picker is drawn into a cached image: the ring once, the triangle
// again when the hue changes (fixed-point barycentric coordinates, a few
// adds per pixel).

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "colorpick.h"

#define D (2 * CP_R_OUT + 1)            // the image is D x D around the centre
#define X0 (CP_CX - CP_R_OUT)
#define Y0 (CP_CY - CP_R_OUT)
#define PI2 6.2831853f

static uint16_t *img;
static int img_h = -1;
static uint8_t to5[256];                // 8 bit -> 5 bit, rounded

// Pure hue in 8 bit: red, yellow, green, cyan, blue, magenta around the ring.
static void hue_rgb8(int h, int *r, int *g, int *b)
{
    h %= CP_HUES;
    if (h < 0)
        h += CP_HUES;
    int f = h & 255;
    switch (h >> 8)
    {
    case 0: *r = 255; *g = f; *b = 0; break;
    case 1: *r = 255 - f; *g = 255; *b = 0; break;
    case 2: *r = 0; *g = 255; *b = f; break;
    case 3: *r = 0; *g = 255 - f; *b = 255; break;
    case 4: *r = f; *g = 0; *b = 255; break;
    default: *r = 255; *g = 0; *b = 255 - f; break;
    }
}

uint16_t cp_hsv_to_rgb(Hsv c)
{
    int ch[3];
    hue_rgb8(c.h, &ch[0], &ch[1], &ch[2]);
    for (int k = 0; k < 3; k++)
    {
        int x = (ch[k] * c.s + 255 * (256 - c.s)) >> 8;     // towards white
        x = x * c.v >> 8;                                    // towards black
        ch[k] = (x * 31 + 127) / 255;
    }
    return COLOR(ch[0], ch[1], ch[2]);
}

Hsv cp_rgb_to_hsv(uint16_t rgb, int keep_h)
{
    int r = (rgb & 31) * 255 / 31, g = ((rgb >> 5) & 31) * 255 / 31, b = ((rgb >> 10) & 31) * 255 / 31;
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    Hsv c = { keep_h, 0, mx * 256 / 255 };
    if (mx == 0 || mx == mn)
        return c;           // gray: no hue of its own
    c.s = (mx - mn) * 256 / mx;
    int span = mx - mn;
    if (mx == r)
        c.h = (g - b) * 256 / span;
    else if (mx == g)
        c.h = 512 + (b - r) * 256 / span;
    else
        c.h = 1024 + (r - g) * 256 / span;
    if (c.h < 0)
        c.h += CP_HUES;
    return c;
}

// Angle of (x, y) with y up, in CP_HUES steps (atan approximated, < 0.3°).
static int angle_of(int x, int y)
{
    if (x == 0 && y == 0)
        return 0;
    int ax = abs(x), ay = abs(y);
    int mn = ax < ay ? ax : ay, mx = ax < ay ? ay : ax;
    int z = mn * 1024 / mx;                                     // tan, 0..1024
    int a = (192 * z + 67 * z * (1024 - z) / 1024) / 1024;      // 0..192 = 0..45°
    if (ay > ax)
        a = 384 - a;
    if (x < 0)
        a = 768 - a;
    if (y < 0)
        a = CP_HUES - a;
    return a % CP_HUES;
}

int cp_hue_at(int x, int y)
{
    return angle_of(x - CP_CX, CP_CY - y);
}

int cp_part_at(int x, int y)
{
    int dx = x - CP_CX, dy = y - CP_CY, d2 = dx * dx + dy * dy;
    if (d2 > (CP_R_OUT + 6) * (CP_R_OUT + 6))
        return CP_NONE;
    if (d2 >= (CP_R_IN - 4) * (CP_R_IN - 4))
        return CP_RING;
    return CP_TRIANGLE;
}

// Corners for hue h: the hue, white, black (screen coordinates).
static void corners(int h, float p[6])
{
    for (int k = 0; k < 3; k++)
    {
        float a = (float)(h + k * 512) * (PI2 / CP_HUES);
        p[2 * k] = CP_CX + CP_R_TRI * cosf(a);
        p[2 * k + 1] = CP_CY - CP_R_TRI * sinf(a);
    }
}

// Barycentric coordinates as a = ax*x + ay*y + a0 (and the same for b).
static void plane(const float p[6], float ka[3], float kb[3])
{
    float det = (p[3] - p[5]) * (p[0] - p[4]) + (p[4] - p[2]) * (p[1] - p[5]);
    ka[0] = (p[3] - p[5]) / det;
    ka[1] = (p[4] - p[2]) / det;
    ka[2] = -(ka[0] * p[4] + ka[1] * p[5]);
    kb[0] = (p[5] - p[1]) / det;
    kb[1] = (p[0] - p[4]) / det;
    kb[2] = -(kb[0] * p[4] + kb[1] * p[5]);
}

void cp_sv_at(int h, int x, int y, int *s, int *v)
{
    float p[6], ka[3], kb[3];
    corners(h, p);
    plane(p, ka, kb);
    float a = ka[0] * x + ka[1] * y + ka[2], b = kb[0] * x + kb[1] * y + kb[2], c = 1.0f - a - b;
    // outside: the nearest edge, near enough
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (c < 0) c = 0;
    float sum = a + b + c;
    a /= sum;
    b /= sum;
    float vv = a + b;
    *v = (int)(vv * 256 + 0.5f);
    *s = vv > 0.001f ? (int)(a / vv * 256 + 0.5f) : 0;
}

static void make_ring(void)
{
    for (int i = 0; i < 256; i++)
        to5[i] = (uint8_t)((i * 31 + 127) / 255);
    for (int y = 0; y < D; y++)
    {
        for (int x = 0; x < D; x++)
        {
            int dx = x - CP_R_OUT, dy = y - CP_R_OUT, d2 = dx * dx + dy * dy;
            uint16_t c = 0;
            if (d2 >= CP_R_IN * CP_R_IN && d2 <= CP_R_OUT * CP_R_OUT)
            {
                int r, g, b;
                hue_rgb8(angle_of(dx, -dy), &r, &g, &b);
                c = COLOR(to5[r], to5[g], to5[b]);
            }
            img[y * D + x] = c;
        }
    }
}

static void make_triangle(int h)
{
    for (int y = CP_R_OUT - CP_R_IN; y <= CP_R_OUT + CP_R_IN; y++)
        for (int x = CP_R_OUT - CP_R_IN; x <= CP_R_OUT + CP_R_IN; x++)
        {
            int dx = x - CP_R_OUT, dy = y - CP_R_OUT;
            if (dx * dx + dy * dy < CP_R_IN * CP_R_IN)
                img[y * D + x] = 0;
        }
    float p[6], ka[3], kb[3];
    corners(h, p);
    plane(p, ka, kb);
    int hr, hg, hb;
    hue_rgb8(h, &hr, &hg, &hb);
    // 16.16 fixed point, so each pixel is a few additions
    int32_t ax = (int32_t)(ka[0] * 65536), bx = (int32_t)(kb[0] * 65536);
    for (int y = CP_CY - CP_R_TRI - 1; y <= CP_CY + CP_R_TRI + 1; y++)
    {
        int x = CP_CX - CP_R_TRI - 1;
        int32_t a = (int32_t)((ka[0] * x + ka[1] * y + ka[2]) * 65536);
        int32_t b = (int32_t)((kb[0] * x + kb[1] * y + kb[2]) * 65536);
        uint16_t *row = img + (y - Y0) * D;
        for (; x <= CP_CX + CP_R_TRI + 1; x++, a += ax, b += bx)
        {
            if (a < 0 || b < 0 || a + b > 65536)
                continue;
            int r = (a * hr + b * 255) >> 16, g = (a * hg + b * 255) >> 16, bl = (a * hb + b * 255) >> 16;
            row[x - X0] = COLOR(to5[r], to5[g], to5[bl]);
        }
    }
    img_h = h;
}

// A marker that shows on any colour: white circle around a black one.
static void marker(Canvas *cv, int cx, int cy)
{
    for (int dy = -6; dy <= 6; dy++)
    {
        int y = cy + dy;
        if (y < cv->cy0 || y >= cv->cy1)
            continue;
        for (int dx = -6; dx <= 6; dx++)
        {
            int x = cx + dx, d2 = dx * dx + dy * dy;
            if (x < cv->cx0 || x >= cv->cx1)
                continue;
            if (d2 >= 16 && d2 <= 25)
                cv->px[y * SCREEN_W + x] = COLOR(0, 0, 0);
            else if (d2 > 25 && d2 <= 36)
                cv->px[y * SCREEN_W + x] = COLOR(31, 31, 31);
        }
    }
}

void cp_draw(Canvas *cv, int dx, Hsv c)
{
    if (!img)
    {
        img = malloc(D * D * sizeof(uint16_t));
        if (!img)
            return;
        make_ring();
    }
    if (img_h != c.h)
        make_triangle(c.h);
    for (int y = 0; y < D; y++)
    {
        int sy = Y0 + y;
        if (sy < cv->cy0 || sy >= cv->cy1)
            continue;
        const uint16_t *src = img + y * D;
        uint16_t *dst = cv->px + sy * SCREEN_W;
        for (int x = 0; x < D; x++)
        {
            int sx = X0 + dx + x;
            if (src[x] && sx >= cv->cx0 && sx < cv->cx1)
                dst[sx] = src[x];
        }
    }
    // the hue on the ring, the colour in the triangle
    float a = (float)c.h * (PI2 / CP_HUES), mid = (CP_R_IN + CP_R_OUT) / 2.0f;
    marker(cv, dx + CP_CX + (int)lroundf(mid * cosf(a)), CP_CY - (int)lroundf(mid * sinf(a)));
    float p[6];
    corners(c.h, p);
    float wa = c.s * c.v / 65536.0f, wb = (256 - c.s) * c.v / 65536.0f, wc = 1.0f - c.v / 256.0f;
    marker(cv, dx + (int)lroundf(wa * p[0] + wb * p[2] + wc * p[4]), (int)lroundf(wa * p[1] + wb * p[3] + wc * p[5]));
    cv->dirty = true;
}
