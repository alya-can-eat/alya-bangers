// SPDX-License-Identifier: MIT
//
// font.bin layout (dsconv FORMAT.md, section 3): 32-byte header, then a table
// of 12-byte glyph entries sorted by code point (and variant), then 1-bit
// bitmaps, 16 rows of 1 or 2 bytes, most significant bit = leftmost pixel.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin_font.h"
#include "font.h"
#include "util.h"

#define FLAG_COMBINING  1
#define FLAG_JAPANESE   2

typedef struct {
    uint32_t cp;
    uint32_t offset;
    uint8_t width;
    uint8_t flags;
    int8_t xoff;
    uint8_t reserved;
} FontEntry;

_Static_assert(sizeof(FontEntry) == 12, "font.bin glyph entries are 12 bytes");

typedef struct {
    const uint8_t *bitmap;
    int width;      // advance and bitmap width
    int xoff;       // combining marks are drawn shifted and don't advance
    bool combining;
} Glyph;

static uint8_t *font_data;
static const FontEntry *table;
static uint32_t glyph_count;
static const uint8_t *bitmaps;
static uint32_t bitmap_size;

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool font_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 32 || size > 4 * 1024 * 1024)
    {
        fclose(f);
        return false;
    }
    uint8_t *data = malloc(size);
    if (data == NULL)
    {
        fclose(f);
        return false;
    }
    bool ok = fread(data, 1, size, f) == (size_t)size;
    fclose(f);

    uint32_t count = rd32(data + 12), toff = rd32(data + 16);
    uint32_t boff = rd32(data + 20), bsize = rd32(data + 24);
    ok = ok && memcmp(data, "DSFT", 4) == 0 && (data[4] | (data[5] << 8)) == 1 && data[8] == 16
         && toff % 4 == 0 && (uint64_t)toff + (uint64_t)count * 12 <= (uint64_t)size
         && (uint64_t)boff + bsize <= (uint64_t)size;
    if (!ok)
    {
        free(data);
        return false;
    }

    free(font_data);
    font_data = data;
    table = (const FontEntry *)(data + toff);
    glyph_count = count;
    bitmaps = data + boff;
    bitmap_size = bsize;
    return true;
}

bool font_loaded(void)
{
    return font_data != NULL;
}

static bool lookup_file(uint32_t cp, bool japanese, Glyph *g)
{
    uint32_t lo = 0, hi = glyph_count;
    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2;
        if (table[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= glyph_count || table[lo].cp != cp)
        return false;

    const FontEntry *e = &table[lo];
    if (japanese && lo + 1 < glyph_count && table[lo + 1].cp == cp
        && (table[lo + 1].flags & FLAG_JAPANESE))
        e = &table[lo + 1];

    if ((e->width != 8 && e->width != 16) || e->offset + e->width * 2u > bitmap_size)
        return false;
    g->bitmap = bitmaps + e->offset;
    g->width = e->width;
    g->combining = e->flags & FLAG_COMBINING;
    g->xoff = e->xoff;
    return true;
}

static bool lookup_builtin(uint32_t cp, Glyph *g)
{
    int lo = 0, hi = builtin_glyph_count;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (builtin_glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= builtin_glyph_count || builtin_glyphs[lo].cp != cp)
        return false;
    g->bitmap = builtin_glyphs[lo].bitmap;
    g->width = builtin_glyphs[lo].width;
    g->combining = false;
    g->xoff = 0;
    return true;
}

static Glyph get_glyph(uint32_t cp, bool japanese)
{
    Glyph g;
    if (font_data != NULL && lookup_file(cp, japanese, &g))
        return g;
    if (lookup_builtin(cp, &g))
        return g;
    if (font_data != NULL && lookup_file(0xFFFD, false, &g))
        return g;
    lookup_builtin(0xFFFD, &g); // always present
    return g;
}

bool font_has_kana(const char *s)
{
    uint32_t c;
    while ((c = utf8_next(&s)) != 0)
    {
        if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF)
            || (c >= 0xFF66 && c <= 0xFF9F))
            return true;
    }
    return false;
}

static int width_jp(const char *s, bool jp)
{
    int w = 0;
    uint32_t c;
    while ((c = utf8_next(&s)) != 0)
    {
        if (c < 0x20)
            continue;
        Glyph g = get_glyph(c, jp);
        if (!g.combining)
            w += g.width;
    }
    return w;
}

int font_text_width(const char *s)
{
    return width_jp(s, font_has_kana(s));
}

static void draw_glyph(Canvas *cv, int x, int y, int clip_x0, int clip_x1, const Glyph *g,
                       uint16_t color, int scale)
{
    if (clip_x0 < cv->cx0) clip_x0 = cv->cx0;
    if (clip_x1 > cv->cx1) clip_x1 = cv->cx1;
    if (x >= clip_x1 || x + g->width * scale <= clip_x0)
        return;
    int bytes = g->width / 8;
    for (int row = 0; row < FONT_HEIGHT; row++)
    {
        uint16_t bits = bytes == 1 ? (uint16_t)(g->bitmap[row] << 8)
                                   : (uint16_t)((g->bitmap[2 * row] << 8) | g->bitmap[2 * row + 1]);
        if (bits == 0)
            continue;
        for (int sy = 0; sy < scale; sy++)
        {
            int yy = y + row * scale + sy;
            if (yy < cv->cy0 || yy >= cv->cy1)
                continue;
            uint16_t *line = cv->px + yy * SCREEN_W;
            for (int col = 0; col < g->width; col++)
            {
                if (!(bits & (0x8000 >> col)))
                    continue;
                for (int sx = 0; sx < scale; sx++)
                {
                    int xx = x + col * scale + sx;
                    if (xx >= clip_x0 && xx < clip_x1)
                        line[xx] = color;
                }
            }
        }
    }
}

int font_draw_ex(Canvas *cv, int x, int y, int clip_x0, int clip_x1, const char *s,
                 uint16_t color, int jp, int scale)
{
    bool j = jp < 0 ? font_has_kana(s) : jp;
    int pen = x;
    uint32_t c;
    while ((c = utf8_next(&s)) != 0)
    {
        if (c < 0x20)
            continue;
        Glyph g = get_glyph(c, j);
        if (g.combining)
        {
            draw_glyph(cv, pen + g.xoff * scale, y, clip_x0, clip_x1, &g, color, scale);
            continue;
        }
        if (pen >= clip_x1)
        {
            pen += g.width * scale;     // keep counting the width
            continue;
        }
        draw_glyph(cv, pen, y, clip_x0, clip_x1, &g, color, scale);
        pen += g.width * scale;
    }
    cv->dirty = true;
    return pen - x;
}

int font_draw(Canvas *cv, int x, int y, int max_w, const char *s, uint16_t color)
{
    bool jp = font_has_kana(s);
    int total = width_jp(s, jp);
    int clip_x1 = x + max_w;
    int limit = max_w;
    Glyph ellipsis = get_glyph(0x2026, false);
    bool cut = total > max_w;
    if (cut)
        limit = max_w - ellipsis.width;

    int pen = 0;
    uint32_t c;
    while ((c = utf8_next(&s)) != 0)
    {
        if (c < 0x20)
            continue;
        Glyph g = get_glyph(c, jp);
        if (g.combining)
        {
            draw_glyph(cv, x + pen + g.xoff, y, x, clip_x1, &g, color, 1);
            continue;
        }
        if (pen + g.width > limit)
            break;
        draw_glyph(cv, x + pen, y, x, clip_x1, &g, color, 1);
        pen += g.width;
    }
    if (cut)
    {
        draw_glyph(cv, x + pen, y, x, clip_x1, &ellipsis, color, 1);
        pen += ellipsis.width;
    }
    cv->dirty = true;
    return pen;
}
