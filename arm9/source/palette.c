// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx.h"
#include "palette.h"

// Alya's theme: a very dark purple, a pinkish white, a violet pink
// (tools/gen_icon.py draws the icon in the same colours)
const Theme theme_default = { COLOR(4, 0, 5), COLOR(31, 29, 31), COLOR(30, 17, 31) };

uint16_t pal[P_COUNT];
static uint32_t version;

// k/256 of the way from a to b, rounded
static uint16_t toward(uint16_t a, uint16_t b, int k)
{
    int c[3];
    for (int i = 0; i < 3; i++)
    {
        int x = (a >> (5 * i)) & 31, y = (b >> (5 * i)) & 31;
        c[i] = (x * (256 - k) + y * k + 128) >> 8;
    }
    return COLOR(c[0], c[1], c[2]);
}

void palette_make(const Theme *t)
{
    // how far from the background towards the text (or the accent), in 256ths;
    // fitted so the default theme looks as it did with fixed colours
    static const struct { uint8_t slot, from_text, k; } mixes[] = {
        { P_ZEBRA, 1, 5 },   { P_PANEL, 1, 15 },  { P_LINE, 1, 45 },    { P_FAINT, 1, 65 },
        { P_DIM, 1, 133 },   { P_VINYL, 1, 5 },   { P_GROOVE, 1, 25 },  { P_RIM, 1, 55 },
        { P_MARK, 1, 104 },  { P_SEL, 0, 76 },    { P_ACC_DIM, 0, 137 },
    };
    pal[P_BG] = t->bg | 0x8000;
    pal[P_TEXT] = t->text | 0x8000;
    pal[P_ACC] = t->acc | 0x8000;
    for (unsigned i = 0; i < sizeof(mixes) / sizeof(mixes[0]); i++)
        pal[mixes[i].slot] = toward(pal[P_BG], mixes[i].from_text ? pal[P_TEXT] : pal[P_ACC], mixes[i].k);
    version++;
}

uint32_t palette_version(void)
{
    return version;
}
