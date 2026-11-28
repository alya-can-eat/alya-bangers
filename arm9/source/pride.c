// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pride flags: horizontal stripes, and for some a shape (the Progress
// chevron with the intersex circle, the intersex circle, the demisexual
// triangle, the ally flag's rainbow Λ). Drawn at any size from their colours; small, so the
// 5-stripe lesbian flag and integer maths.

#include <stdlib.h>

#include "pride.h"

#define RGB(h) ((uint16_t)((((h) >> 19) & 31) | ((((h) >> 11) & 31) << 5) | ((((h) >> 3) & 31) << 10) | 0x8000))

typedef struct {
    uint8_t n;                  // stripes, top to bottom
    uint8_t weight[7];          // their heights (0: all the same)
    uint32_t col[7];
} Stripes;

static const Stripes stripes[PRIDE_COUNT] = {
    [PRIDE_RAINBOW] = { 6, { 0 }, { 0xE40303, 0xFF8C00, 0xFFED00, 0x008026, 0x24408E, 0x732982 } },
    [PRIDE_PROGRESS] = { 6, { 0 }, { 0xE40303, 0xFF8C00, 0xFFED00, 0x008026, 0x24408E, 0x732982 } },
    [PRIDE_LESBIAN] = { 5, { 0 }, { 0xD52D00, 0xFF9A56, 0xFFFFFF, 0xD362A4, 0xA30262 } },
    [PRIDE_GAY] = { 7, { 0 }, { 0x078D70, 0x26CEAA, 0x98E8C1, 0xFFFFFF, 0x7BADE2, 0x5049CC, 0x3D1A78 } },
    [PRIDE_BI] = { 3, { 2, 1, 2 }, { 0xD60270, 0x9B4F96, 0x0038A8 } },
    [PRIDE_PAN] = { 3, { 0 }, { 0xFF218C, 0xFFD800, 0x21B1FF } },
    [PRIDE_ASEXUAL] = { 4, { 0 }, { 0x000000, 0xA3A3A3, 0xFFFFFF, 0x800080 } },
    [PRIDE_TRANS] = { 5, { 0 }, { 0x5BCEFA, 0xF5A9B8, 0xFFFFFF, 0xF5A9B8, 0x5BCEFA } },
    [PRIDE_NONBINARY] = { 4, { 0 }, { 0xFCF434, 0xFFFFFF, 0x9C59D1, 0x2C2C2C } },
    [PRIDE_AGENDER] = { 7, { 0 }, { 0x000000, 0xBCC4C7, 0xFFFFFF, 0xB7F684, 0xFFFFFF, 0xBCC4C7, 0x000000 } },
    [PRIDE_AROMANTIC] = { 5, { 0 }, { 0x3DA542, 0xA7D379, 0xFFFFFF, 0xA9A9A9, 0x000000 } },
    [PRIDE_INTERSEX] = { 1, { 0 }, { 0xFFD800 } },
    [PRIDE_GENDERFLUID] = { 5, { 0 }, { 0xFF76A4, 0xFFFFFF, 0xC011D7, 0x000000, 0x2F3CBE } },
    [PRIDE_GENDERQUEER] = { 3, { 0 }, { 0xB57EDC, 0xFFFFFF, 0x4A8123 } },
    [PRIDE_AROACE] = { 5, { 0 }, { 0xE28C00, 0xECCD00, 0xFFFFFF, 0x62AEDC, 0x203856 } },
    [PRIDE_DEMISEXUAL] = { 3, { 3, 1, 3 }, { 0xFFFFFF, 0x6E0070, 0xD2D2D2 } },
    [PRIDE_DEMIGENDER] = { 7, { 0 }, { 0x7F7F7F, 0xC3C3C3, 0xFBFF75, 0xFFFFFF, 0xFBFF75, 0xC3C3C3, 0x7F7F7F } },
    [PRIDE_ALLY] = { 6, { 0 }, { 0x000000, 0xFFFFFF, 0x000000, 0xFFFFFF, 0x000000, 0xFFFFFF } },
};

#define PURPLE 0x7902AA     // the intersex circle

static int sq(int a)
{
    return a * a;
}

uint16_t pride_color(int flag, int x, int y, int w, int h)
{
    if (flag <= PRIDE_NONE || flag >= PRIDE_COUNT || w <= 0 || h <= 0)
        return RGB(0x000000);
    // where in the flag, in thousandths: u across, v down (pixel centres)
    int u = (2 * x + 1) * 500 / w, v = (2 * y + 1) * 500 / h;
    int mid = abs(v - 500);
    if (flag == PRIDE_INTERSEX)
    {
        // a purple circle on yellow
        int r2 = sq(u - 500) + sq(v - 500);
        return r2 > sq(180) && r2 < sq(285) ? RGB(PURPLE) : RGB(0xFFD800);
    }
    if (flag == PRIDE_PROGRESS)
    {
        // the chevron from the left: the intersex triangle (yellow, the circle),
        // then white, pink, light blue, brown, black; the rainbow behind
        static const uint32_t bands[5] = { 0xFFFFFF, 0xFFAFC8, 0x74D7EE, 0x613915, 0x000000 };
        int d = u + mid * 3 / 4;
        if (d < 260)
        {
            int r2 = sq(u - 100) + sq(v - 500);
            return r2 > sq(45) && r2 < sq(88) ? RGB(PURPLE) : RGB(0xFFD800);
        }
        if (d < 560)
            return RGB(bands[(d - 260) / 60]);
    }
    if (flag == PRIDE_DEMISEXUAL && u + mid * 9 / 10 < 360)
        return RGB(0x000000);       // the black triangle
    if (flag == PRIDE_ALLY && v > 60)
    {
        // the rainbow Λ over the stripes: red outside, violet inside
        static const uint32_t rainbow[6] = { 0xE40303, 0xFF8C00, 0xFFED00, 0x008026, 0x24408E, 0x732982 };
        int outer = (v - 60) * 45 / 100 + 110, x = abs(u - 500);
        if (x <= outer && x > outer - 180)
            return RGB(rainbow[(outer - x) / 30]);
    }
    const Stripes *s = &stripes[flag];
    int total = 0;
    for (int i = 0; i < s->n; i++)
        total += s->weight[0] ? s->weight[i] : 1;
    int at = v * total, acc = 0;
    for (int i = 0; i < s->n; i++)
    {
        acc += s->weight[0] ? s->weight[i] : 1;
        if (at < acc * 1000)
            return RGB(s->col[i]);
    }
    return RGB(s->col[s->n - 1]);
}
