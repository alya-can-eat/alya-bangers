// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drawing of both screens, following the mockup pixel for pixel: 256x192 per
// screen, Unifont 16 px, BGR555 colors.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "default_cover.h"
#include "font.h"
#include "player.h"
#include "queue.h"
#include "stats.h"
#include "util.h"
#include "viz.h"

const int tab_x[TAB_COUNT + 1] = { 0, 72, 144, 192, 224, 256 };

static Canvas *T, *B;

#define HOLD_FRAMES 30      // scrolling text waits 1 s before it moves
#define GAP 32              // space between the end and the start of scrolling text

// ------------------------------------------------------------ small helpers

static int utf8_put(char *out, uint32_t cp)
{
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; return 1; }
    if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 63); out[2] = 0; return 2; }
    if (cp < 0x10000)
    {
        out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 63); out[2] = 0x80 | (cp & 63);
        out[3] = 0;
        return 3;
    }
    out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 63);
    out[2] = 0x80 | ((cp >> 6) & 63); out[3] = 0x80 | (cp & 63); out[4] = 0;
    return 4;
}

// A 16 px symbol centered in a 16x16 cell.
static void icon(Canvas *c, int x, int y, uint32_t cp, uint16_t color)
{
    char s[8];
    utf8_put(s, cp);
    int w = font_text_width(s);
    font_draw_ex(c, x + (16 - w) / 2, y, 0, SCREEN_W, s, color, 0, 1);
}

static void text_r(Canvas *c, int xr, int y, const char *s, uint16_t color)
{
    font_draw_ex(c, xr - font_text_width(s), y, 0, SCREEN_W, s, color, -1, 1);
}

static void text(Canvas *c, int x, int y, const char *s, uint16_t color)
{
    font_draw_ex(c, x, y, 0, SCREEN_W, s, color, -1, 1);
}

// A selection highlight at y (it glides between rows), cut to the rows y0..y1.
static void highlight(int x, int y, int w, int h, int y0, int y1, uint16_t color)
{
    int top = y < y0 ? y0 : y, bottom = y + h > y1 ? y1 : y + h;
    if (bottom > top)
        gfx_fill(B, x, top, w, bottom - top, color);
}

static void mmss(char *buf, int size, uint32_t ms)
{
    format_time(buf, size, ms);
}

// Right of a progress bar: the song's length, or the time left (setting).
static void right_time(char *buf, int size, uint32_t pos, uint32_t dur)
{
    if (!app.set.show_remaining)
    {
        mmss(buf, size, dur);
        return;
    }
    buf[0] = '-';
    mmss(buf + 1, size - 1, dur > pos ? dur - pos : 0);
}

// Scrolling text fades out over EDGE_W pixels at each end instead of being
// cut off: save the 16 px tall strips at both ends, draw the text, then blend
// the strips back towards what was under them, more the closer to the end.
// amount (0..256) scales the whole fade.
#define EDGE_W 8
#define EDGE_RAMP 16    // the left fade comes and goes over this many px of scrolling
enum { EDGE_SAVE, EDGE_LEFT, EDGE_RIGHT };

static void edge(Canvas *c, int x, int y, uint16_t *buf, int mode, int amount)
{
    for (int j = 0; j < ROW_H; j++)
    {
        int yy = y + j;
        if (yy < c->cy0 || yy >= c->cy1)
            continue;
        for (int i = 0; i < EDGE_W; i++)
        {
            int xx = x + i;
            if (xx < c->cx0 || xx >= c->cx1)
                continue;
            uint16_t *p = &c->px[yy * SCREEN_W + xx];
            uint16_t *b = &buf[j * EDGE_W + i];
            if (mode == EDGE_SAVE)
                *b = *p;
            else if (*p != *b)
            {
                int k = mode == EDGE_LEFT ? i + 1 : EDGE_W - i;   // 1 at the outer end
                *p = gfx_mix(*p, *b, 256 - (EDGE_W + 1 - k) * amount / (EDGE_W + 1));
            }
        }
    }
}

// Scrolling text made of colored pieces. Returns true if it scrolls.
static bool marquee(Canvas *c, int x, int y, int w, const char *const *s, const uint16_t *col,
                    int n, uint32_t t, bool center)
{
    if (w <= 0)
        return false;
    char all[600];
    all[0] = 0;
    for (int i = 0; i < n; i++)
        strncat(all, s[i], sizeof(all) - strlen(all) - 1);
    int jp = font_has_kana(all);
    int tw = 0;
    for (int i = 0; i < n; i++)
        tw += font_draw_ex(c, SCREEN_W, -100, 0, 0, s[i], 0, jp, 1);   // measure only
    if (tw <= w)
    {
        int pen = x + (center ? (w - tw) / 2 : 0);
        for (int i = 0; i < n; i++)
            pen += font_draw_ex(c, pen, y, x, x + w, s[i], col[i], jp, 1);
        return false;
    }
    int period = tw + GAP;
    uint32_t lap = HOLD_FRAMES + period;
    uint32_t tt = t % lap;
    int off = tt < HOLD_FRAMES ? 0 : (int)(tt - HOLD_FRAMES);
    // soft edges: keep what is under both ends, draw, then blend the ends back
    uint16_t left[EDGE_W * ROW_H], right[EDGE_W * ROW_H];
    edge(c, x, y, left, EDGE_SAVE, 0);
    edge(c, x + w - EDGE_W, y, right, EDGE_SAVE, 0);
    for (int k = 0; k < 2; k++)
    {
        int pen = x - off + k * period;
        for (int i = 0; i < n; i++)
            pen += font_draw_ex(c, pen, y, x, x + w, s[i], col[i], jp, 1);
    }
    // the start is cut off only while it's away: the fade grows as the text
    // leaves and shrinks as it comes back, instead of switching on and off
    int away = off < period - off ? off : period - off;
    if (away > 0)
        edge(c, x, y, left, EDGE_LEFT, (away > EDGE_RAMP ? EDGE_RAMP : away) * 256 / EDGE_RAMP);
    edge(c, x + w - EDGE_W, y, right, EDGE_RIGHT, 256);
    return true;
}

static bool marquee1(Canvas *c, int x, int y, int w, const char *s, uint16_t col, uint32_t t,
                     bool center)
{
    return marquee(c, x, y, w, &s, &col, 1, t, center);
}

static int smooth256(int p)
{
    if (p <= 0) return 0;
    if (p >= 256) return 256;
    return (int)((int64_t)p * p * (768 - 2 * p) >> 16);
}

int app_slide_p256(void)
{
    return app.slide * 256 / SLIDE_TICKS;
}

int app_panel_offset(void)
{
    return PANEL_DOWN * smooth256(app_slide_p256()) / 256;
}

uint32_t app_current_track(void)
{
    const char *p = player_path();
    if (p == NULL || !lib_loaded() || queue_pos() < 0)
        return LIB_NONE;
    return queue_track(queue_pos());
}

int app_song_rating(uint32_t t)
{
    int r = stats_rating(lib_path(t));
    if (r != STATS_UNRATED)
        return r;
    const char *tag = lib_tag(t, "~#rating");      // 0..1
    r = tag ? (int)(atof(tag) * 5 + 0.5) : 0;
    return r < 0 ? 0 : (r > 5 ? 5 : r);
}

uint32_t app_song_plays(uint32_t t)
{
    const char *tag = lib_tag(t, "~#playcount");
    long n = tag ? atol(tag) : 0;
    return (n > 0 ? (uint32_t)n : 0) + stats_plays(lib_path(t));
}

// ------------------------------------------------------------ text for things

static void basename_no_ext(const char *path, char *buf, int size)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    snprintf(buf, size, "%s", b);
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf)
        *dot = 0;
}

static void track_title(uint32_t t, char *buf, int size)
{
    const LibTrack *tr = lib_track(t);
    if (tr->title != LIB_NONE)
        snprintf(buf, size, "%s", lib_value(tr->title));
    else
        basename_no_ext(lib_path(t), buf, size);
}

void ui_item_label(const Item *it, char *buf, int size)
{
    switch (it->kind)
    {
    case ITEM_ARTIST:
    {
        uint32_t v = lib_group_name(it->id);
        snprintf(buf, size, "%s", v == LIB_NONE ? "(no artist)" : lib_value(v));
        break;
    }
    case ITEM_ALBUM:
        snprintf(buf, size, "%s", lib_value(lib_album(it->id)->name));
        break;
    case ITEM_TRACK:
        if (app.tab == TAB_FOLDER)
            basename_no_ext(lib_path(it->id), buf, size);
        else
            track_title(it->id, buf, size);
        break;
    case ITEM_FOLDER:
        lib_folder_name(it, app.folder, buf, size);
        break;
    case ITEM_UP:
    {
        // "Bandcamp › Kivara"
        int n = 0;
        buf[0] = 0;
        for (const char *p = app.folder; *p && n < size - 4; p++)
        {
            if (*p == '/')
                n += snprintf(buf + n, size - n, " › ");
            else
                buf[n++] = *p, buf[n] = 0;
        }
        utf8_trim_partial(buf);
        break;
    }
    }
}

static void item_meta(const Item *it, char *buf, int size)
{
    buf[0] = 0;
    switch (it->kind)
    {
    case ITEM_ARTIST:
        snprintf(buf, size, "%d", lib_group_size(it->id));
        break;
    case ITEM_ALBUM:
        if (lib_album(it->id)->year)
            snprintf(buf, size, "%d", lib_album(it->id)->year);
        break;
    case ITEM_TRACK:
        mmss(buf, size, lib_track(it->id)->duration_ms);
        break;
    default:
        break;
    }
}

int ui_sort_slot(void)
{
    switch (app.tab)
    {
    case TAB_ARTIST: return SLOT_ARTIST;
    case TAB_ALBUM: return SLOT_ALBUM;
    case TAB_TITLE: return app.album != LIB_NONE ? SLOT_TITLE_IN_ALBUM : SLOT_TITLE;
    default: return SLOT_FOLDER;
    }
}

static const char *const sort_names[SORT_SLOTS][4] = {
    { "Name  0→9 A→Z", "Name  Z→A 9→0", NULL, NULL },
    { "Name  0→9 A→Z", "Name  Z→A 9→0", "Year  old → new", "Year  new → old" },
    { "Name  0→9 A→Z", "Name  Z→A 9→0", "Year  old → new", NULL },
    { "Track order", "Name  0→9 A→Z", "Name  Z→A 9→0", NULL },
    { "Name  0→9 A→Z", "Name  Z→A 9→0", NULL, NULL },
};

int ui_sort_option_count(int slot)
{
    int n = 0;
    while (n < 4 && sort_names[slot][n])
        n++;
    return n;
}

const char *ui_sort_option(int slot, int i)
{
    return sort_names[slot][i];
}

// ------------------------------------------------------------ top screen

static void vinyl_tables(void);

void ui_init(void)
{
    // float math that takes a moment on the DS: do it before music plays
    vinyl_tables();
    viz_init();
}

static void header(void)
{
    gfx_fill(T, 0, 0, SCREEN_W, 16, C_PANEL);
    static const char *const days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char s[32];
    snprintf(s, sizeof(s), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    text(T, 4, 0, s, C_TEXT);
    snprintf(s, sizeof(s), "%s %02d.%02d.%04d", days[tmv.tm_wday % 7], tmv.tm_mday, tmv.tm_mon + 1,
             tmv.tm_year + 1900);
    text_r(T, 252, 0, s, C_DIM);
    // the card was too slow somewhere (should never show)
    if (player_underruns())
    {
        snprintf(s, sizeof(s), "drops: %lu", (unsigned long)player_underruns());
        text(T, 52, 0, s, COLOR(31, 12, 10));
    }
}

typedef struct {
    char title[200], artist[200], album[200], extra[48];
} SongText;

static void song_text(uint32_t t, SongText *st)
{
    memset(st, 0, sizeof(*st));
    if (t == LIB_NONE)
    {
        const DsaTags *tg = player_tags();
        if (tg)
        {
            snprintf(st->title, sizeof(st->title), "%s", tg->title);
            snprintf(st->artist, sizeof(st->artist), "%s", tg->artist);
            snprintf(st->album, sizeof(st->album), "%s", tg->album);
        }
        return;
    }
    const LibTrack *tr = lib_track(t);
    track_title(t, st->title, sizeof(st->title));
    snprintf(st->artist, sizeof(st->artist), "%s", tr->artist != LIB_NONE ? lib_value(tr->artist) : "");
    if (tr->album != LIB_NONE)
        snprintf(st->album, sizeof(st->album), "%s", lib_value(lib_album(tr->album)->name));
    int n = 0;
    if (tr->year)
        n += snprintf(st->extra + n, sizeof(st->extra) - n, "%d", tr->year);
    if (tr->track && tr->album != LIB_NONE)
        snprintf(st->extra + n, sizeof(st->extra) - n, "%s%d/%d", n ? " · " : "", tr->track,
                 lib_album(tr->album)->count);
    else if (tr->track)
        snprintf(st->extra + n, sizeof(st->extra) - n, "%s%d", n ? " · " : "", tr->track);
}

static bool top_text_column(int x0, const SongText *st, uint32_t t, int alpha)
{
    if (x0 >= SCREEN_W)
        return false;
    int w = 104 < SCREEN_W - x0 ? 104 : SCREEN_W - x0;
    bool anim = false;
    if (player_path() == NULL)
    {
        font_draw_ex(T, x0, 24, x0, x0 + w, "Nothing", C_DIM, 0, 1);
        font_draw_ex(T, x0, 40, x0, x0 + w, "playing", C_DIM, 0, 1);
        return false;
    }
    font_set_alpha(alpha);      // fading in or out on a song change
    anim |= marquee1(T, x0, 24, w, st->title, C_TEXT, t, false);
    anim |= marquee1(T, x0, 44, w, st->artist, C_ACC, t, false);
    anim |= marquee1(T, x0, 64, w, st->album, C_DIM, t, false);
    font_draw_ex(T, x0, 84, x0, x0 + w, st->extra, C_DIM, -1, 1);
    font_set_alpha(256);
    // shuffle and repeat, since the controls are hidden while browsing
    icon(T, x0, 136, 0x1F500, queue_shuffle() ? C_ACC : C_FAINT);
    icon(T, x0 + 20, 136, queue_repeat() == REPEAT_ONE ? 0x1F502 : 0x1F501,
         queue_repeat() != REPEAT_OFF ? C_ACC : C_FAINT);
    return anim;
}

static void top_progress(int y)
{
    if (y >= SCREEN_H || player_path() == NULL)
        return;
    uint32_t pos = player_position_smooth_ms(), dur = player_duration_ms();
    int fill = dur ? (int)((uint64_t)240 * pos / dur) : 0;
    gfx_fill(T, 8, y, 240, 4, C_LINE);
    gfx_fill(T, 8, y, fill, 4, C_ACC);
    char s[16];
    mmss(s, sizeof(s), pos);
    text(T, 8, y + 8, s, C_TEXT);
    right_time(s, sizeof(s), pos, dur);
    text_r(T, 248, y + 8, s, C_DIM);
}

// ---- the record: per-pixel distance and angle around its centre, computed once

#define VR 60
#define VD (2 * VR + 1)
static uint8_t *v_dist;
static uint16_t *v_angle;
static uint16_t v_groove[360], v_edge[360];     // their colors by angle, with the glare

// A soft glare on the grooves and the rim, brightest at the top left (225°).
// Called again when the theme colors change.
static void vinyl_glare(void)
{
    const uint16_t c_edge = pal[P_RIM], c_groove = pal[P_GROOVE];
    for (int a = 0; a < 360; a++)
    {
        int d = a - 225;
        if (d < 0) d = -d;
        if (d > 180) d = 360 - d;
        int w = 0;
        if (d < 56)
        {
            int t = 256 - d * d * 256 / (56 * 56);     // 1 - (d/56)^2, squared
            w = t * t / 256;
        }
        v_groove[a] = gfx_mix(C_GLARE, c_groove, w * 45 / 100);
        v_edge[a] = gfx_mix(C_GLARE, c_edge, w * 50 / 100);
    }
}

static void vinyl_tables(void)
{
    if (v_dist)
        return;
    v_dist = malloc(VD * VD);
    v_angle = malloc(VD * VD * sizeof(uint16_t));
    if (!v_dist || !v_angle)
        return;
    vinyl_glare();
    for (int j = 0; j < VD; j++)
    {
        for (int i = 0; i < VD; i++)
        {
            float dx = i - VR, dy = j - VR;
            float d = sqrtf(dx * dx + dy * dy);
            v_dist[j * VD + i] = d > 250 ? 250 : (uint8_t)(d + 0.5f);
            float a = atan2f(dy, dx) * (180.0f / 3.14159265f);
            v_angle[j * VD + i] = (uint16_t)((int)(a + 360.5f) % 360);
        }
    }
}

typedef struct { uint8_t r; uint16_t a, len; } Mark;
static const Mark marks[5] = { { 52, 5, 28 }, { 37, 62, 22 }, { 47, 156, 30 }, { 32, 213, 24 }, { 42, 296, 26 } };

// Draw the part of the record left of clip_x1.
static HOT void vinyl_pixels(uint16_t *px, int cx, int cy, int y0, int y1, int clip_x1, int rot,
                             const uint8_t *dist, const uint16_t *angle)
{
    const uint16_t c_vinyl = pal[P_VINYL], c_mark = pal[P_MARK];
    // which light mark sits on each radius (-1: none)
    int8_t mark_at[VR + 1];
    for (int r = 0; r <= VR; r++)
        mark_at[r] = -1;
    for (int m = 0; m < 5; m++)
        mark_at[marks[m].r] = m;
    for (int j = 0; j < VD; j++)
    {
        int y = cy - VR + j;
        if (y < y0 || y >= y1)
            continue;
        uint16_t *line = px + y * SCREEN_W;
        int i0 = cx - VR < 0 ? VR - cx : 0;
        int i1 = clip_x1 - (cx - VR);
        if (i1 > VD) i1 = VD;
        if (i1 > SCREEN_W - (cx - VR)) i1 = SCREEN_W - (cx - VR);
        for (int i = i0; i < i1; i++)
        {
            int d = dist[j * VD + i];
            if (d > VR)
                continue;
            uint16_t col = c_vinyl;
            if (d == VR)
                col = v_edge[angle[j * VD + i]];
            else if (d <= 20)
                col = C_ACC_DIM;
            else if (d == 52 || d == 42 || d == 32)
                col = v_groove[angle[j * VD + i]];
            int m = mark_at[d];
            if (m >= 0)
            {
                int a = angle[j * VD + i] - marks[m].a - rot;
                while (a < 0) a += 360;
                if (a <= marks[m].len)
                    col = c_mark;
            }
            line[cx - VR + i] = col;
        }
    }
}

static void draw_vinyl(int cx, int cy, int clip_x1, int rot)
{
    vinyl_tables();
    if (!v_dist)
        return;
    static uint32_t glare_for;
    if (glare_for != palette_version())
    {
        vinyl_glare();              // the theme changed
        glare_for = palette_version();
    }
    vinyl_pixels(T->px, cx, cy, T->cy0, T->cy1, clip_x1, rot, v_dist, v_angle);
    T->dirty = true;
}

static void draw_viz(int p2)
{
    for (int i = 0; i < VIZ_BANDS; i++)
    {
        int v = viz_level(i);
        int len = 24 * v / 256 * p2 / 256;
        int y = 28 + i * 6;
        gfx_fill(T, 0, y, len, 4, gfx_mix(C_ACC, C_BG, 115 + 102 * v / 256));
    }
}

static bool top_next_column(int x, uint32_t t)
{
    if (x >= SCREEN_W)
        return false;
    int w = 48 < SCREEN_W - x ? 48 : SCREEN_W - x;
    // everything centred in the 48 px column, so "Next" lines up with the picture
    font_draw_ex(T, x + (48 - font_text_width("Next")) / 2, 24, x, SCREEN_W, "Next", C_DIM, 0, 1);
    int np = queue_next_pos(true);
    if (np < 0 || !lib_loaded())
    {
        font_draw_ex(T, x + 20, 80, x, x + w, "–", C_DIM, 0, 1);
        return false;
    }
    uint32_t nt = queue_track(np);
    bool own = app.has_thumb && app.thumb_track == nt;
    gfx_blit(T, x + 8, 44, 32, 32, own ? app.thumb : default_thumb);
    char title[200];
    track_title(nt, title, sizeof(title));
    const LibTrack *tr = lib_track(nt);
    bool anim = marquee1(T, x, 80, w, title, C_TEXT, t, true);
    anim |= marquee1(T, x, 96, w, tr->artist != LIB_NONE ? lib_value(tr->artist) : "", C_DIM, t, true);
    return anim;
}

// ---- song change

static int clamp256(int v) { return v < 0 ? 0 : (v > 256 ? 256 : v); }
static int ease_in(int q) { return q * q / 256; }
static int ease_out(int q) { return 256 - (256 - q) * (256 - q) / 256; }

static const uint16_t *live_cover(void)
{
    const uint16_t *c = player_cover();
    return c ? c : default_cover;
}

// Cover of the song that leaves (b = false) or arrives (b = true). Backwards
// the roles swap: the song that was playing is the one going to "Next".
static const uint16_t *role_cover(bool b)
{
    bool saved = app.sa_kind == SA_BACK ? b : !b;
    return saved ? app.sa_cover : live_cover();
}

static const uint16_t *thumb_of(uint32_t track, bool saved)
{
    if (saved)
        return app.sa_has_thumb ? app.sa_thumb : default_thumb;
    return app.has_thumb && app.thumb_track == track ? app.thumb : default_thumb;
}

static void next_texts(int x, uint32_t track, uint32_t t, int alpha)
{
    if (alpha <= 0)
        return;
    if (track == LIB_NONE)
    {
        font_set_alpha(alpha);
        font_draw_ex(T, x + 20, 80, x, SCREEN_W, "\u2013", C_DIM, 0, 1);
        font_set_alpha(256);
        return;
    }
    char title[200];
    track_title(track, title, sizeof(title));
    const LibTrack *tr = lib_track(track);
    font_set_alpha(alpha);
    marquee1(T, x, 80, 48, title, C_TEXT, t, true);
    marquee1(T, x, 96, 48, tr->artist != LIB_NONE ? lib_value(tr->artist) : "", C_DIM, t, true);
    font_set_alpha(256);
}

// "Album · year" under the cover in player mode.
static void album_line(uint32_t track, uint32_t t, int alpha)
{
    if (alpha <= 0 || track == LIB_NONE)
        return;
    const LibTrack *tr = lib_track(track);
    if (tr->album == LIB_NONE)
        return;
    char line[260];
    const char *name = lib_value(lib_album(tr->album)->name);
    if (tr->year)
        snprintf(line, sizeof(line), "%s \u00b7 %d", name, tr->year);
    else
        snprintf(line, sizeof(line), "%s", name);
    font_set_alpha(alpha);
    marquee1(T, 8, 164, 240, line, C_DIM, t, true);
    font_set_alpha(256);
}

static void draw_song_change(uint32_t t)
{
    int pre = app.sa_kind == SA_JUMP ? SA_PRE_TICKS : 0;
    // position on the forward timeline (backwards runs it from the end)
    int u = app.sa_kind == SA_BACK ? SA_TICKS - app.sa_t : app.sa_t - pre;
    int a = clamp256(u * 256 / SA_OUT_TICKS);                                    // old cover out
    int fl = clamp256((u - SA_FLY_START) * 256 / SA_FLY_TICKS);                  // new cover in
    int b = clamp256((u - SA_FLY_START - SA_FLY_TICKS) * 256 / SA_B_TICKS);      // record out
    int rot = app.vinyl_rot / 3;
    bool back = app.sa_kind == SA_BACK;

    font_draw_ex(T, 200 + (48 - font_text_width("Next")) / 2, 24, 200, SCREEN_W, "Next", C_DIM, 0, 1);

    // the song leaving: its cover slides off to the left, over its record
    if (a < 256)
    {
        int ax = 64 - 200 * ease_in(a) / 256;
        draw_vinyl(88, 88, ax < 64 ? ax : 64, rot);
        gfx_blit(T, ax, 24, 128, 128, role_cover(false));
    }
    // the arriving song's record rolls out from behind its cover
    if (b > 0)
    {
        int travel = 40 * (256 - ease_out(b)) / 256;
        draw_vinyl(88 + travel, 88, 64, rot + travel * 955 / 1000);
    }

    const uint16_t *bc = role_cover(true);
    if (u < 0)
    {
        // jump: the chosen song slides in over the old Next first
        int q = ease_out(clamp256(app.sa_t * 256 / SA_PRE_TICKS));
        int bx = 256 + (208 - 256) * q / 256;
        gfx_blit(T, 208, 44, 32, 32, thumb_of(app.sa_n, true));
        gfx_blit_scaled(T, bx, 44, 32, bc);
        // the old Next name fades out, the chosen song's fades in
        int k = app.sa_t * 512 / SA_PRE_TICKS;    // 0..512: out, then in
        if (k < 256)
            next_texts(200, app.sa_n, t, 256 - k);
        else
            next_texts(200, app.sa_b, t, clamp256(k - 256));
        album_line(app.sa_a, t, 256);
        return;
    }
    // ... waits in the Next spot until the old cover is out of the way, then
    // grows into the middle
    int f = ease_out(fl);
    gfx_blit_scaled(T, 208 + (64 - 208) * f / 256, 44 + (24 - 44) * f / 256, 32 + 96 * f / 256, bc);

    // its name fades out of the Next column before it sets off
    int nq = clamp256((u - SA_FLY_START / 2) * 256 / (SA_FLY_START - SA_FLY_START / 2));
    next_texts(200, app.sa_b, t, 256 - nq);
    // and the song after it comes in: picture slides, name fades in
    if (b > 0)
    {
        int off = 64 * (256 - ease_out(b)) / 256;
        if (app.sa_c != LIB_NONE)
            gfx_blit(T, 208 + off, 44, 32, 32, thumb_of(app.sa_c, back));
        next_texts(200, app.sa_c, t, b);
    }
    // the album line under the cover: out with the old song, in with the new
    int out = 256 - clamp256(u * 512 / SA_OUT_TICKS);
    if (out > 0)
        album_line(app.sa_a, t, out);
    else
        album_line(app.sa_b, t, b);
}

// ---- player mode background: the cover, blurred and darkened

static uint16_t *bg_cur, *bg_new;       // shown; being made / faded in
static uint32_t bg_track = 0xFFFFFFFEu; // the song bg_cur belongs to (this value: none yet)
static uint32_t bg_job = LIB_NONE;      // the song bg_new is made for
static int bg_rows;                     // rows of bg_new done
static bool bg_fading;
static uint32_t bg_fade_from;           // frame (30 fps) the cross-fade began
static int16_t bg_grid[13][17][3];

#define BG_SLICE 32                     // rows made per drawn frame (~2 ms)
#define BG_FADE  12                     // frames of the cross-fade, 0.4 s

// On the DS screen everything darker than BG_FLOOR (of 255) looks black, so
// brightness counts from there. A cover and its background keep BG_APART of it
// between them.
#define BG_FLOOR 24
#define BG_APART 12

static int luma8(int r, int g, int b)
{
    return (2 * r + 5 * g + b) / 8;
}

// How bright the cover's outer 4 px are (where it meets the background), 0..255.
static int cover_edge_luma(const uint16_t *cover)
{
    int sum = 0, n = 0;
    for (int y = 0; y < 128; y++)
    {
        bool rim = y < 4 || y >= 124;
        for (int x = 0; x < 128; x += rim ? 1 : 124)
        {
            uint16_t p = cover[y * 128 + x];
            sum += luma8((p & 31) * 8, ((p >> 5) & 31) * 8, ((p >> 10) & 31) * 8);
            n++;
            if (!rim && x == 0)
            {
                // the 4 px on each side
                for (int k = 1; k < 4; k++)
                {
                    uint16_t q = cover[y * 128 + k], e = cover[y * 128 + 127 - k];
                    sum += luma8((q & 31) * 8, ((q >> 5) & 31) * 8, ((q >> 10) & 31) * 8);
                    sum += luma8((e & 31) * 8, ((e >> 5) & 31) * 8, ((e >> 10) & 31) * 8);
                    n += 2;
                }
            }
        }
    }
    return sum / n;
}

static int grid_luma(void)
{
    int sum = 0;
    for (int gy = 0; gy < 13; gy++)
        for (int gx = 0; gx < 17; gx++)
            sum += luma8(bg_grid[gy][gx][0], bg_grid[gy][gx][1], bg_grid[gy][gx][2]);
    return sum / (13 * 17);
}

static int above_floor(int luma)
{
    return luma > BG_FLOOR ? luma - BG_FLOOR : 0;
}

// A very dark cover on its dark background would melt into it (both look
// black): then the background gets lighter. Covers that stand out stay as they are.
static void bg_keep_apart(const uint16_t *cover)
{
    int edge = above_floor(cover_edge_luma(cover)), avg = grid_luma();
    int d = above_floor(avg) - edge;
    if (d >= BG_APART || d <= -BG_APART)
        return;
    int lift = BG_FLOOR + edge + BG_APART - avg;
    for (int gy = 0; gy < 13; gy++)
        for (int gx = 0; gx < 17; gx++)
            for (int k = 0; k < 3; k++)
            {
                int v = bg_grid[gy][gx][k] + lift;
                bg_grid[gy][gx][k] = (int16_t)(v > 240 ? 240 : v);
            }
}

static void bg_grid_from(const uint16_t *cover);

// For the tests: how far apart a cover's edge and its background end up
// (counted above the floor).
int ui_cover_bg_contrast(const uint16_t *cover)
{
    bg_grid_from(cover);
    int d = above_floor(grid_luma()) - above_floor(cover_edge_luma(cover));
    return d < 0 ? -d : d;
}

// 17 x 13 colors (one every 16 px) from 8x8 blocks of the cover, made dark.
static void bg_grid_from(const uint16_t *cover)
{
    int bgc[3];                                 // the background in 8 bit
    for (int k = 0; k < 3; k++)
    {
        int v = (C_BG >> (5 * k)) & 31;
        bgc[k] = (v << 3) | (v >> 2);
    }
    for (int gy = 0; gy < 13; gy++)
    {
        for (int gx = 0; gx < 17; gx++)
        {
            int cx = gx * 8 - 4, cy = gy * 128 / 12 - 4;
            if (cx < 0) cx = 0;
            if (cx > 120) cx = 120;
            if (cy < 0) cy = 0;
            if (cy > 120) cy = 120;
            int r = 0, g = 0, b = 0;
            for (int j = 0; j < 8; j++)
            {
                const uint16_t *p = cover + (cy + j) * 128 + cx;
                for (int i = 0; i < 8; i++)
                {
                    r += p[i] & 31;
                    g += (p[i] >> 5) & 31;
                    b += (p[i] >> 10) & 31;
                }
            }
            // average (x 8 for 8 bit), then 30 % of it on top of half the theme color
            int v[3] = { r / 8, g / 8, b / 8 };
            for (int k = 0; k < 3; k++)
                bg_grid[gy][gx][k] = (int16_t)(v[k] * 3 / 10 + bgc[k] / 2);
        }
    }
    bg_keep_apart(cover);
}

// Rows y0..y1 of the grid stretched smoothly over the screen, with a little
// dithering: down the columns first, then along the row by adding a step.
// (At most 90 in 8 bit, so no clamping.)
static HOT void bg_rows_make(uint16_t *out, int y0, int y1)
{
    static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    for (int y = y0; y < y1; y++)
    {
        int gy = y >> 4, fy = y & 15;
        int col[17][3];
        for (int gx = 0; gx < 17; gx++)
            for (int k = 0; k < 3; k++)
                col[gx][k] = bg_grid[gy][gx][k] * (16 - fy) + bg_grid[gy + 1][gx][k] * fy;
        const uint8_t *d4 = bayer[y & 3];
        uint16_t *o = out + y * SCREEN_W;
        for (int gx = 0; gx < 16; gx++)
        {
            int r = col[gx][0] * 16, g = col[gx][1] * 16, b = col[gx][2] * 16;
            int dr = col[gx + 1][0] - col[gx][0], dg = col[gx + 1][1] - col[gx][1], db = col[gx + 1][2] - col[gx][2];
            for (int fx = 0; fx < 16; fx++)
            {
                int d = d4[fx & 3] >> 1;
                *o++ = (uint16_t)((((r >> 8) + d) >> 3) | ((((g >> 8) + d) >> 3) << 5)
                                  | ((((b >> 8) + d) >> 3) << 10) | 0x8000);
                r += dr;
                g += dg;
                b += db;
            }
        }
    }
}

// The average of two color pairs, rounded down per channel (equal colors stay equal).
#define AVG(x, y) (((x) & (y)) + ((((x) ^ (y)) & 0x7BDE7BDE) >> 1))

// dst = a with k quarters of b (b = NULL: the color pair bc), two pixels at a time
static HOT void quarters(uint32_t *dst, const uint32_t *a, const uint32_t *b, uint32_t bc, int k, int words)
{
    for (int i = 0; i < words; i++)
    {
        uint32_t x = a[i], y = b ? b[i] : bc;
        uint32_t half = AVG(x, y);
        uint32_t r;
        if (k <= 0) r = x;
        else if (k == 1) r = AVG(x, half);
        else if (k == 2) r = half;
        else if (k == 3) r = AVG(y, half);
        else r = y;
        dst[i] = r | 0x80008000;
    }
}

static void bg_swap(void)
{
    uint16_t *t = bg_cur;
    bg_cur = bg_new;
    bg_new = t;
    bg_track = bg_job;
    bg_job = LIB_NONE;
    bg_fading = false;
}

// The background of the top screen. p2: how far the player layout is in (0..256).
// A new song's background is made a slice per frame while the covers move, and
// faded in once they have settled. True while that is going on.
static bool draw_background(int p2)
{
    bool on = !app.set.plain_bg && player_path() != NULL;
    if (on && !bg_cur)
    {
        bg_cur = malloc(SCREEN_W * SCREEN_H * 2);
        bg_new = malloc(SCREEN_W * SCREEN_H * 2);
    }
    if (!on || !bg_cur || !bg_new)
    {
        gfx_fill(T, 0, 0, SCREEN_W, SCREEN_H, C_BG);
        return false;
    }
    static uint32_t bg_pal;
    if (bg_pal != palette_version())
    {
        // the theme changed: start over (made at once below)
        bg_pal = palette_version();
        bg_track = 0xFFFFFFFEu;
        bg_job = LIB_NONE;
        bg_fading = false;
    }
    uint32_t track = app_current_track();
    if (bg_track == 0xFFFFFFFEu)
    {
        // the first one: made at once
        bg_grid_from(live_cover());
        bg_rows_make(bg_cur, 0, SCREEN_H);
        bg_track = track;
    }
    else if (track == bg_track)
    {
        bg_job = LIB_NONE;          // back to the song it shows
        bg_fading = false;
    }
    else if (track != bg_job)
    {
        if (bg_fading)
            bg_swap();              // a fade still going: finish it at once
        bg_grid_from(live_cover());
        bg_job = track;
        bg_rows = 0;
    }
    if (bg_job != LIB_NONE && bg_rows < SCREEN_H)
    {
        int y1 = bg_rows + BG_SLICE > SCREEN_H ? SCREEN_H : bg_rows + BG_SLICE;
        bg_rows_make(bg_new, bg_rows, y1);
        bg_rows = y1;
    }
    if (bg_job != LIB_NONE && bg_rows >= SCREEN_H && !bg_fading)
    {
        if (p2 <= 0)
            bg_swap();              // not on screen: no need to fade
        else if (!SA_PLAYER(app.sa_kind))
        {
            bg_fading = true;
            bg_fade_from = app.frame;
        }
    }
    if (p2 <= 0)
    {
        if (bg_fading)
            bg_swap();
        gfx_fill(T, 0, 0, SCREEN_W, SCREEN_H, C_BG);
        return bg_job != LIB_NONE;
    }
    bool busy = bg_job != LIB_NONE;
    const int words = SCREEN_W * SCREEN_H / 2;
    uint32_t *dst = (uint32_t *)(void *)T->px;
    const uint32_t *src = (const uint32_t *)bg_cur;
    if (bg_fading)
    {
        int k = 1 + (int)(app.frame - bg_fade_from) * 3 / BG_FADE;
        if (k >= 4)
        {
            bg_swap();
            src = (const uint32_t *)bg_cur;
        }
        else
        {
            quarters(dst, src, (const uint32_t *)bg_new, 0, k, words);
            src = dst;
        }
    }
    // sliding in from the browser: from the plain color, in quarters
    int k2 = p2 >= 256 ? 4 : p2 * 4 / 256;
    if (k2 < 4)
        quarters(dst, src, NULL, C_BG | ((uint32_t)C_BG << 16), 4 - k2, words);
    else if (src != dst)
        memcpy(dst, src, SCREEN_W * SCREEN_H * 2);
    T->dirty = true;
    return busy;
}

bool ui_draw_top(void)
{
    T = gfx_canvas(SCREEN_TOP);
    gfx_unclip(T);
    uint32_t t = app.frame;
    int p = app_slide_p256();
    int p1 = smooth256(p * 256 / 154);
    int p2 = smooth256((p - 102) * 256 / 154);
    bool bg_busy = draw_background(p2);
    header();
    bool playing = player_state() == PLAYER_PLAYING;
    // the record and the visualizer move while playing; the rest only if text scrolls
    bool anim = (app.slide > 0 && app.slide < SLIDE_TICKS) || bg_busy;

    SongText st;
    song_text(app_current_track(), &st);
    const uint16_t *cover = player_cover();
    int cover_x = 8 + 56 * p1 / 256;
    int text_alpha = 256;

    if (SA_PLAYER(app.sa_kind) && p2 >= 256)
    {
        if (playing)
            viz_update();
        draw_viz(256);
        draw_song_change(t);
        return true;
    }
    if (p2 > 0)
    {
        if (playing)
            viz_update();
        draw_viz(p2);
        if (playing || app.vinyl_speed > 0)
            anim = true;
        // rolls out (turning with the distance) when the player slides in
        int travel = 40 * (256 - p2) / 256;
        draw_vinyl(cover_x + 24 + travel, 88, cover_x, app.vinyl_rot / 3 + travel * 955 / 1000);
    }
    if (SA_STACK(app.sa_kind) && p == 0)
    {
        // browse mode: covers on a stack, one put on or lifted off to the left
        int q = clamp256(app.sa_t * 256 / SA_STACK_TICKS);
        bool put = app.sa_kind == SA_PUT;
        const uint16_t *under = put ? app.sa_cover : live_cover();
        const uint16_t *moving = put ? live_cover() : app.sa_cover;
        int mx = put ? -128 + 136 * ease_out(q) / 256 : 8 - 136 * ease_in(q) / 256;
        gfx_blit(T, 8, 24, 128, 128, under);
        gfx_blit(T, mx, 24, 128, 128, moving);
        // the old text fades out, the new one fades in
        if (q < 128)
        {
            if (app.sa_a != LIB_NONE)
                song_text(app.sa_a, &st);
            else
                memset(&st, 0, sizeof(st));
            text_alpha = 256 - 2 * q;
        }
        else
            text_alpha = 2 * q - 256;
        anim = true;
    }
    else
        gfx_blit(T, cover_x, 24, 128, 128, cover ? cover : default_cover);

    anim |= top_text_column(144 + 112 * p1 / 256, &st, t, text_alpha);
    if (p2 > 0)
        anim |= top_next_column(200 + 64 * (256 - p2) / 256, t);
    top_progress(162 + 40 * p1 / 256);
    if (p2 > 0 && player_path() != NULL)
    {
        char line[260];
        if (st.album[0] && lib_loaded() && app_current_track() != LIB_NONE && lib_track(app_current_track())->year)
            snprintf(line, sizeof(line), "%s · %d", st.album, lib_track(app_current_track())->year);
        else
            snprintf(line, sizeof(line), "%s", st.album);
        anim |= marquee1(T, 8, 164 + 32 * (256 - p2) / 256, 240, line, C_DIM, t, true);
    }
    return anim;
}

// ------------------------------------------------------------ bottom: browser panel

static int tab_progress(void);

static void draw_tabs(int oy)
{
    gfx_fill(B, 0, oy, SCREEN_W, TAB_H, C_PANEL);
    static const char *const names[TAB_COUNT] = { "Artist", "Album", "Title", NULL, NULL };
    // in the player the tabs just wait: their pink fades to gray as the browser slides down
    uint16_t acc = gfx_mix(C_DIM, C_ACC, clamp256(oy * 256 / PANEL_DOWN));
    // the highlight glides from the old tab to the new one
    int hx = tab_x[app.tab], hw = tab_x[app.tab + 1] - hx;
    if (app.tab_anim > 0 && app.tab_prev != app.tab)
    {
        int q = tab_progress();
        int px = tab_x[app.tab_prev], pw = tab_x[app.tab_prev + 1] - px;
        hx = px + (hx - px) * q / 256;
        hw = pw + (hw - pw) * q / 256;
    }
    gfx_fill(B, hx, oy, hw, TAB_H, C_BG);
    gfx_fill(B, hx, oy, hw, 2, acc);
    for (int i = 0; i < TAB_COUNT; i++)
    {
        int x = tab_x[i], w = tab_x[i + 1] - x;
        bool act = (int)app.tab == i;
        if (i)
            gfx_fill(B, x, oy + 3, 1, 14, C_LINE);
        char label[200];
        label[0] = 0;
        if (i == TAB_ARTIST && app.artist >= 0)
        {
            Item it = { ITEM_ARTIST, (uint32_t)app.artist };
            ui_item_label(&it, label, sizeof(label));
        }
        if (i == TAB_ALBUM && app.album != LIB_NONE)
            snprintf(label, sizeof(label), "%s", lib_value(lib_album(app.album)->name));
        if (label[0])
        {
            // the tab shows the choice; the small × clears it
            font_draw(B, x + 4, oy + 2, w - 18, label, act ? C_TEXT : acc);
            text(B, x + w - 12, oy + 2, "×", C_DIM);
        }
        else if (i == TAB_FOLDER)
            icon(B, x + (w - 16) / 2, oy + 2, 0x1F5C0, act ? C_TEXT : C_DIM);
        else if (i == TAB_SEARCH)
            icon(B, x + (w - 16) / 2, oy + 2, 0x1F50D, act ? C_TEXT : C_DIM);
        else
        {
            int tw = font_text_width(names[i]);
            text(B, x + (w - tw) / 2, oy + 2, names[i], act ? C_TEXT : C_DIM);
        }
    }
}

static void scrollbar(int y0, int rows, int first, int total, bool active)
{
    gfx_fill(B, SCROLL_X, y0, SCREEN_W - SCROLL_X, rows * ROW_H, C_PANEL);
    if (total <= 0)
        return;
    int h = rows * ROW_H;
    int th = total > rows ? h * rows / total : h;
    if (th < 12)
        th = 12;
    int ty = y0 + (total > rows ? (h - th) * first / (total - rows) : 0);
    gfx_fill(B, SCROLL_X + 3, ty, 6, th, active ? C_ACC : C_ACC_DIM);
}

static bool track_marked(uint32_t t)
{
    return app.added && (app.added[t >> 3] & (1 << (t & 7)));
}

static bool item_playing(const Item *it)
{
    uint32_t cur = app_current_track();
    if (cur == LIB_NONE)
        return false;
    switch (it->kind)
    {
    case ITEM_TRACK: return it->id == cur;
    case ITEM_ALBUM: return lib_track(cur)->album == it->id;
    case ITEM_ARTIST: return lib_track_group(cur) == (int)it->id;
    default: return false;
    }
}

static void centered(Canvas *c, int y, const char *s, uint16_t color)
{
    int w = font_text_width(s);
    text(c, (SCREEN_W - w) / 2, y, s, color);
}

static bool draw_list(int oy)
{
    bool anim = false;
    int y0 = oy + LIST_Y;
    if (!lib_loaded())
    {
        gfx_fill(B, 0, y0, SCREEN_W, ROWS * ROW_H, C_BG);
        centered(B, y0 + 48, app.lib_error[0] ? app.lib_error : "No library.idx", C_TEXT);
        centered(B, y0 + 72, "Run dsconv on your music.", C_DIM);
        return false;
    }
    if (app.tab == TAB_SEARCH)
    {
        gfx_fill(B, 0, y0, SCREEN_W, ROWS * ROW_H, C_BG);
        centered(B, y0 + 64, "Search comes later.", C_DIM);
        return false;
    }
    bool numbers = app.tab == TAB_TITLE && (app.album != LIB_NONE || app.add_mode);
    for (int r = 0; r < ROWS; r++)
    {
        int i = app.scroll + r;
        int y = y0 + r * ROW_H;
        bool sel = i == app.sel && i < app.n_items;
        if (r == 0)
        {
            // all row backgrounds first, then the highlight gliding to the selection
            for (int k = 0; k < ROWS; k++)
                gfx_fill(B, 0, y0 + k * ROW_H, SCROLL_X, ROW_H, k % 2 ? C_ZEBRA : C_BG);
            if (app.sel >= 0 && app.sel < app.n_items)
                highlight(0, y0 + app.sel_vis, SCROLL_X, ROW_H, y0, y0 + ROWS * ROW_H, C_SEL);
        }
        if (i >= app.n_items)
            continue;
        const Item *it = &app.items[i];
        char label[256], meta[16];
        ui_item_label(it, label, sizeof(label));
        item_meta(it, meta, sizeof(meta));
        bool playing = item_playing(it);
        int x = 4;
        if (numbers)
        {
            if (it->kind == ITEM_TRACK && track_marked(it->id))
                text(B, 8, y, "+", C_ACC);
            else if (app.album != LIB_NONE)
            {
                char num[12];
                const LibTrack *tr = lib_track(it->id);
                snprintf(num, sizeof(num), "%d", tr->track ? tr->track : i + 1);
                text_r(B, 20, y, num, C_DIM);
            }
            x = 28;
        }
        if (it->kind == ITEM_FOLDER || it->kind == ITEM_UP)
        {
            icon(B, x - 2, y, it->kind == ITEM_FOLDER ? 0x1F5C0 : 0x2B9D, C_DIM);
            x += 18;
        }
        int mw = 0;
        if (meta[0])
        {
            mw = font_text_width(meta);
            text_r(B, 240, y, meta, playing ? C_ACC : C_DIM);
        }
        int w = 240 - x - (meta[0] ? mw + 8 : 0);
        uint16_t col = playing ? C_ACC : (it->kind == ITEM_UP ? C_DIM : C_TEXT);
        if (sel || app.set.scroll_all)
        {
            uint32_t t = sel ? app.frame - app.sel_frame : app.frame;
            anim |= marquee1(B, x, y, w, label, col, t, false);
        }
        else
            font_draw(B, x, y, w, label, col);
    }
    scrollbar(y0, ROWS, app.scroll, app.n_items, app.scroll_drag);

    if (app.scroll_drag && app.n_items > 0)
    {
        // big first letter of the top row next to the stylus
        char label[256], first[8];
        ui_item_label(&app.items[app.scroll], label, sizeof(label));
        const char *p = label;
        uint32_t cp = utf8_next(&p);
        if (cp >= 'a' && cp <= 'z')
            cp -= 32;
        utf8_put(first, cp ? cp : ' ');
        int h = ROWS * ROW_H;
        int th = app.n_items > ROWS ? h * ROWS / app.n_items : h;
        if (th < 12) th = 12;
        int ty = y0 + (app.n_items > ROWS ? (h - th) * app.scroll / (app.n_items - ROWS) : 0);
        int by = ty + th / 2 - 20;
        if (by < oy + 22) by = oy + 22;
        if (by > oy + 120) by = oy + 120;
        gfx_fill(B, 188, by, 48, 40, C_PANEL);
        gfx_rect(B, 188, by, 48, 40, C_ACC);
        int lw = font_text_width(first) * 2;
        font_draw_ex(B, 212 - lw / 2, by + 4, 0, SCREEN_W, first, C_TEXT, -1, 2);
    }
    return anim;
}

// ---- sideways slide between lists

#define LIST_PX (SCREEN_W * ROWS * ROW_H)
static uint16_t old_list[LIST_PX];
static uint16_t new_list[LIST_PX];

void ui_capture_list(void)
{
    Canvas *c = gfx_canvas(SCREEN_BOTTOM);
    memcpy(old_list, c->px + LIST_Y * SCREEN_W, sizeof(old_list));
}

static int tab_progress(void)
{
    // ease out: fast at first, settling at the end
    int p = (TAB_TICKS - app.tab_anim) * 256 / TAB_TICKS;
    return 256 - (256 - p) * (256 - p) / 256;
}

// The list area holds the new list; shift it in and the old one out.
static void slide_lists(int oy)
{
    if (app.tab_anim <= 0 || oy != 0)
        return;
    uint16_t *area = B->px + LIST_Y * SCREEN_W;
    memcpy(new_list, area, sizeof(new_list));
    int off = app.tab_dir * (SCREEN_W - SCREEN_W * tab_progress() / 256);  // new list's x
    int old_off = off - app.tab_dir * SCREEN_W;
    // new list at x = off, old list at x = old_off; each is one piece per row
    int nx0 = off < 0 ? 0 : off, nx1 = off < 0 ? SCREEN_W + off : SCREEN_W;
    int ox0 = old_off < 0 ? 0 : old_off, ox1 = old_off < 0 ? SCREEN_W + old_off : SCREEN_W;
    for (int y = 0; y < ROWS * ROW_H; y++)
    {
        uint16_t *d = area + y * SCREEN_W;
        const uint16_t *n = new_list + y * SCREEN_W, *o = old_list + y * SCREEN_W;
        if (nx1 > nx0)
            memcpy(d + nx0, n + (nx0 - off), (nx1 - nx0) * 2);
        if (ox1 > ox0)
            memcpy(d + ox0, o + (ox0 - old_off), (ox1 - ox0) * 2);
    }
    B->dirty = true;
}

static void pill(int cx, int y, uint32_t cp, bool lit)
{
    gfx_round_fill(B, cx - 20, y + 4, 40, 20, 6, lit ? C_DIM : C_LINE);
    icon(B, cx - 8, y + 6, cp, C_TEXT);
}

// A button lit up by the stylus (and shortly after the tap).
static bool lit(int id)
{
    return app.press_id == id && (app.press_down || app.frame < app.press_until);
}

static void lit_box(int cx, int cy, int w, int h, int id)
{
    if (lit(id))
        gfx_round_fill(B, cx - w / 2, cy - h / 2, w, h, 6, C_LINE);
}

typedef struct { int x, y; } Pt;

static Pt lerp_pt(Pt a, Pt b, int t)
{
    return (Pt){ a.x + (b.x - a.x) * t / 256, a.y + (b.y - a.y) * t / 256 };
}

static void quad(Pt a, Pt b, Pt c, Pt d, uint16_t col)
{
    gfx_triangle(B, a.x, a.y, b.x, b.y, c.x, c.y, col);
    gfx_triangle(B, a.x, a.y, c.x, c.y, d.x, d.y, col);
}

// Play triangle (t = 0) turning into the pause bars (t = 256): each bar comes
// from one half of the triangle. Pause: bars x l0..l1 and r0..r1, half height
// h. Play: from pl to the tip pt, half height ph.
static void play_pause(int cy, int l0, int l1, int r0, int r1, int h, int pl, int pt, int ph, int t, uint16_t col)
{
    if (t >= 256)
    {
        gfx_fill(B, l0, cy - h, l1 - l0 + 1, 2 * h + 1, col);
        gfx_fill(B, r0, cy - h, r1 - r0 + 1, 2 * h + 1, col);
        return;
    }
    if (t <= 0)
    {
        gfx_arrow(B, pl, cy, ph, pt - pl, 1, col);
        return;
    }
    int xs = (pl + pt) / 2, hs = ph / 2;
    quad(lerp_pt((Pt){ pl, cy - ph }, (Pt){ l0, cy - h }, t), lerp_pt((Pt){ xs, cy - hs }, (Pt){ l1, cy - h }, t),
         lerp_pt((Pt){ xs, cy + hs }, (Pt){ l1, cy + h }, t), lerp_pt((Pt){ pl, cy + ph }, (Pt){ l0, cy + h }, t), col);
    quad(lerp_pt((Pt){ xs, cy - hs }, (Pt){ r0, cy - h }, t), lerp_pt((Pt){ pt, cy }, (Pt){ r1, cy - h }, t),
         lerp_pt((Pt){ pt, cy }, (Pt){ r1, cy + h }, t), lerp_pt((Pt){ xs, cy + hs }, (Pt){ r0, cy + h }, t), col);
}

static int pp_shape(void)
{
    return smooth256(app.pp_t * 256 / PP_TICKS);
}

static void draw_bar(int oy)
{
    int y = oy + BAR_Y;
    gfx_fill(B, 0, y, SCREEN_W, 28, C_PANEL);
    gfx_fill(B, 0, y, SCREEN_W, 1, C_LINE);
    lit_box(16, y + 14, 28, 20, BTN_PLAY);
    play_pause(y + 14, 13, 14, 17, 18, 5, 12, 20, 5, pp_shape(), C_TEXT);
    lit_box(48, y + 14, 28, 20, BTN_NEXT);
    icon(B, 40, y + 5, 0x23ED, C_TEXT);     // its glyph sits 1 px low
    if (app.add_mode)
        gfx_round_fill(B, 66, y + 4, 28, 20, 6, lit(BTN_ADD) ? gfx_mix(C_ACC, C_TEXT, 160) : C_ACC);
    else
        lit_box(80, y + 14, 28, 20, BTN_ADD);
    icon(B, 72, y + 6, 0xE000, app.add_mode ? C_BG : C_TEXT);
    pill(128, y, 0x2B9F, lit(BTN_DOWN));
    bool sort_open = app.overlay == OV_SORT;
    if (sort_open)
        gfx_fill(B, 160, y + 1, 32, 27, C_BG);
    lit_box(176, y + 14, 28, 20, BTN_SORT);
    icon(B, 168, y + 6, 0x21C5, sort_open ? C_ACC : C_TEXT);
    // the sun shows the level: circle, sun, half full, full (DS Lite levels 2..5)
    int level = app.set.brightness < 2 ? 2 : (app.set.brightness > 5 ? 5 : app.set.brightness);
    lit_box(208, y + 14, 28, 20, BTN_BRIGHT);
    icon(B, 200, y + 6, 0xE001 + (level - 2), C_TEXT);
    lit_box(240, y + 14, 28, 20, BTN_SETTINGS);
    icon(B, 232, y + 6, 0x22EE, C_TEXT);
}

// ---- menus slide up when they open and down when they close: a menu is drawn
// into a scratch screen, then its rows are copied onto the real one, moved down

static uint16_t scratch_px[SCREEN_W * SCREEN_H] __attribute__((aligned(4)));
static Canvas scratch = { scratch_px, true, 0, 0, SCREEN_W, SCREEN_H };

static int ov_progress(void)
{
    return ease_out(clamp256(app.ov_t * 256 / OV_TICKS));
}

static bool shifted(bool (*fn)(int), int arg, int x0, int y0, int x1, int y1, int dy, int clip_y1)
{
    Canvas *real = B;
    B = &scratch;
    gfx_unclip(B);
    bool a = fn(arg);
    B = real;
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < SCREEN_H; y++)
    {
        int ty = y + dy;
        if (ty < real->cy0 || ty >= clip_y1 || ty >= real->cy1)
            continue;
        memcpy(real->px + ty * SCREEN_W + x0, scratch_px + y * SCREEN_W + x0, (x1 - x0) * 2);
    }
    real->dirty = true;
    return a;
}

static void draw_sort_menu(int oy)
{
    int slot = ui_sort_slot();
    int n = ui_sort_option_count(slot);
    int x0 = 96, x1 = 248;
    int h = 20 + 16 * n + 4;
    int y0 = oy + BAR_Y - h;
    gfx_fill(B, x0, y0, x1 - x0, h + 1, C_PANEL);
    gfx_rect(B, x0, y0, x1 - x0, h + 1, C_LINE);
    static const char *const titles[SORT_SLOTS] = { "Sort artists", "Sort albums", "Sort songs", "Sort songs", "Sort folders" };
    text(B, x0 + 8, y0 + 2, titles[slot], C_DIM);
    if (app.menu_sel >= 0)
        highlight(x0 + 1, y0 + 20 + app.menu_vis, x1 - x0 - 2, 16, y0 + 20, y0 + 20 + 16 * n, C_SEL);
    for (int i = 0; i < n; i++)
    {
        int y = y0 + 20 + i * 16;
        bool cur = app.set.sort[slot] == i;
        icon(B, x0 + 4, y, cur ? 0x25CF : 0x25CB, cur ? C_ACC : C_DIM);
        text(B, x0 + 24, y, ui_sort_option(slot, i), C_TEXT);
    }
}

int ui_rowmenu_y(int row_y)
{
    int h = 20 + 16 * 3 + 4;
    return row_y + ROW_H + h <= BAR_Y ? row_y + ROW_H : row_y - h;
}

static void draw_row_menu(int oy)
{
    if (app.menu_item < 0 || app.menu_item >= app.n_items)
        return;
    int row_y = LIST_Y + (app.menu_item - app.scroll) * ROW_H;
    int y0 = oy + ui_rowmenu_y(row_y);
    int x0 = 88, x1 = 240, h = 20 + 16 * 3 + 4;
    gfx_fill(B, x0, y0, x1 - x0, h, C_PANEL);
    gfx_rect(B, x0, y0, x1 - x0, h, C_ACC);
    char label[256];
    ui_item_label(&app.items[app.menu_item], label, sizeof(label));
    font_draw(B, x0 + 8, y0 + 2, x1 - x0 - 16, label, C_DIM);
    static const uint32_t icons[3] = { 0x25B6, 0x21B3, '+' };
    static const char *const names[3] = { "Play now", "Play next", "Add to queue" };
    if (app.menu_sel >= 0)
        highlight(x0 + 1, y0 + 20 + app.menu_vis, x1 - x0 - 2, 16, y0 + 20, y0 + 20 + 16 * 3, C_SEL);
    for (int i = 0; i < 3; i++)
    {
        int y = y0 + 20 + i * 16;
        icon(B, x0 + 4, y, icons[i], C_ACC);
        text(B, x0 + 24, y, names[i], C_TEXT);
    }
}

static bool draw_toast(int oy)
{
    if (app.frame >= app.toast_until || !app.toast[0])
        return false;
    int y = oy + 140;
    gfx_fill(B, 8, y, 228, 20, C_PANEL);
    gfx_rect(B, 8, y, 228, 20, C_ACC);
    marquee1(B, 16, y + 2, 212, app.toast, C_TEXT, app.frame, false);
    return true;
}

static bool w_sort(int oy) { draw_sort_menu(oy); return false; }
static bool w_row(int oy) { draw_row_menu(oy); return false; }

// Both menus rise from behind the bottom bar.
static bool sort_menu_moving(int oy)
{
    int p = ov_progress();
    if (p >= 256)
    {
        draw_sort_menu(oy);
        return false;
    }
    int h = 20 + 16 * ui_sort_option_count(ui_sort_slot()) + 4;
    int y0 = oy + BAR_Y - h;
    shifted(w_sort, oy, 96, y0, 248, oy + BAR_Y + 1, (h + 1) * (256 - p) / 256, oy + BAR_Y + 1);
    return true;
}

static bool row_menu_moving(int oy)
{
    int p = ov_progress();
    if (p >= 256)
    {
        draw_row_menu(oy);
        return false;
    }
    int y0 = oy + ui_rowmenu_y(LIST_Y + (app.menu_item - app.scroll) * ROW_H);
    shifted(w_row, oy, 88, y0, 240, y0 + 72, (oy + BAR_Y - y0) * (256 - p) / 256, oy + BAR_Y);
    return true;
}

static bool draw_panel(int oy)
{
    gfx_clip(B, 0, oy, SCREEN_W, SCREEN_H);
    draw_tabs(oy);
    bool anim = false;
    if (oy < SCREEN_H - TAB_H)
    {
        anim |= draw_list(oy);
        slide_lists(oy);
        anim |= app.tab_anim > 0;
        draw_bar(oy);
        if (app.ov_vis == OV_SORT)
            anim |= sort_menu_moving(oy);
        if (app.ov_vis == OV_ROWMENU)
            anim |= row_menu_moving(oy);
        anim |= draw_toast(oy);
    }
    gfx_unclip(B);
    return anim;
}

// ------------------------------------------------------------ bottom: player area

static void big_next(int cx, int cy, uint16_t c, bool flip)
{
    int s = flip ? -1 : 1;
    gfx_arrow(B, cx - 10 * s, cy, 9, 9, s, c);
    gfx_arrow(B, cx - 1 * s, cy, 9, 9, s, c);
    int bx = cx + 8 * s;
    gfx_fill(B, s > 0 ? bx : bx - 2, cy - 9, 3, 19, c);
}

static bool draw_player_area(void)
{
    gfx_fill(B, 0, 0, SCREEN_W, PANEL_DOWN, C_BG);
    bool anim = false;
    uint32_t cur = app_current_track();
    SongText st;
    song_text(cur, &st);
    if (player_path() != NULL)
    {
        const char *segs[3] = { st.artist, st.artist[0] ? " – " : "", st.title };
        const uint16_t cols[3] = { C_ACC, C_DIM, C_TEXT };
        anim |= marquee(B, 8, 8, 240, segs, cols, 3, app.frame, true);
    }
    else
        centered(B, 8, "Nothing playing", C_DIM);

    uint32_t dur = player_duration_ms();
    uint32_t pos = app.seek_drag ? app.seek_preview_ms : player_position_smooth_ms();
    int y = 40;
    gfx_fill(B, 16, y, 224, 4, C_LINE);
    int kx = 16 + (dur ? (int)((uint64_t)224 * pos / dur) : 0);
    gfx_fill(B, 16, y, kx - 16, 4, C_ACC);
    if (player_path() != NULL)
        gfx_disc(B, kx, y + 2, 5 + 2 * app.knob_t / KNOB_TICKS, C_TEXT);   // grows while dragged
    char s[16];
    mmss(s, sizeof(s), pos);
    text(B, 16, y + 12, s, C_TEXT);
    right_time(s, sizeof(s), pos, dur);
    text_r(B, 240, y + 12, s, C_DIM);

    int cy = 100;
    bool sh = queue_shuffle();
    lit_box(28, cy + 1, 32, 30, BTN_SHUFFLE);
    icon(B, 20, cy - 8, 0x1F500, sh ? C_ACC : C_DIM);
    if (sh)
        gfx_fill(B, 26, cy + 10, 4, 2, C_ACC);
    lit_box(78, cy, 36, 32, BTN_PREV);
    big_next(78, cy, C_TEXT, true);
    gfx_disc(B, 128, cy, 24, lit(BTN_PPLAY) ? gfx_mix(C_ACC, C_TEXT, 160) : C_ACC);
    play_pause(cy, 120, 125, 131, 136, 9, 122, 138, 10, pp_shape(), C_BG);
    lit_box(178, cy, 36, 32, BTN_PNEXT);
    big_next(178, cy, C_TEXT, false);
    int rep = queue_repeat();
    lit_box(228, cy + 1, 32, 30, BTN_REPEAT);
    icon(B, 220, cy - 8, rep == REPEAT_ONE ? 0x1F502 : 0x1F501, rep ? C_ACC : C_DIM);

    lit_box(20, 148, 28, 24, BTN_INFO);
    icon(B, 12, 140, 0x24D8, C_TEXT);
    lit_box(236, 148, 28, 24, BTN_QUEUE);
    icon(B, 228, 140, 0x2630, C_TEXT);
    pill(128, 140, 0x2B9D, lit(BTN_UP));
    return anim;
}

static bool draw_info(void)
{
    gfx_fill(B, 0, 0, SCREEN_W, PANEL_DOWN, C_BG);
    gfx_fill(B, 0, 0, SCREEN_W, 20, C_PANEL);
    text(B, 8, 2, "Song info", C_TEXT);
    text(B, 236, 2, "×", C_TEXT);
    uint32_t t = app_current_track();
    if (t == LIB_NONE)
    {
        centered(B, 72, "Nothing playing", C_DIM);
        return false;
    }
    const LibTrack *tr = lib_track(t);
    SongText st;
    song_text(t, &st);
    char track[24] = "–", year[8] = "–", plays[16], format[64];
    if (tr->track && tr->album != LIB_NONE)
        snprintf(track, sizeof(track), "%d / %d", tr->track, lib_album(tr->album)->count);
    else if (tr->track)
        snprintf(track, sizeof(track), "%d", tr->track);
    if (tr->year)
        snprintf(year, sizeof(year), "%d", tr->year);
    snprintf(plays, sizeof(plays), "%lu", (unsigned long)app_song_plays(t));
    const char *genre = lib_tag(t, "genre");
    const char *codec = lib_tag(t, "~codec");
    const DsaHeader *h = player_header();
    unsigned rate = h ? h->sound_clock_hz / h->timer_divider : 32728;
    snprintf(format, sizeof(format), "%s%sDSA %u.%u kHz", codec ? codec : "", codec ? " → " : "",
             rate / 1000, (rate % 1000) / 100);
    const char *labels[9] = { "Title", "Artist", "Album", "Track", "Year", "Genre", "Rating", "Plays", "Format" };
    const char *vals[9] = { st.title, st.artist[0] ? st.artist : "–", st.album[0] ? st.album : "–",
                            track, year, genre ? genre : "–", "", plays, format };
    bool anim = false;
    for (int i = 0; i < 9; i++)
    {
        int y = INFO_Y + i * 16;
        text(B, 8, y, labels[i], C_DIM);
        if (i != INFO_RATING_ROW)
            anim |= marquee1(B, INFO_STARS_X, y, 176, vals[i], C_TEXT, app.frame, false);
    }
    // the stars can be tapped (Left / Right change them too)
    int stars = app_song_rating(t);
    for (int i = 0; i < 5; i++)
        icon(B, INFO_STARS_X + i * INFO_STAR_W, INFO_Y + INFO_RATING_ROW * 16, i < stars ? 0x2605 : 0x2606,
             i < stars ? C_ACC : C_DIM);
    return anim;
}

static bool draw_queue(void)
{
    gfx_fill(B, 0, 0, SCREEN_W, PANEL_DOWN, C_BG);
    gfx_fill(B, 0, 0, SCREEN_W, 20, C_PANEL);
    char head[48];
    int n = queue_count();
    snprintf(head, sizeof(head), "Queue · %d song%s", n, n == 1 ? "" : "s");
    text(B, 8, 2, head, C_TEXT);
    text(B, 236, 2, "×", C_TEXT);
    bool anim = false;
    bool dragging = app.q_drag_from >= 0;
    for (int r = 0; r < ROWS; r++)
        gfx_fill(B, 0, 20 + r * ROW_H, SCROLL_X, ROW_H, r % 2 ? C_ZEBRA : C_BG);
    if (!dragging && app.q_sel >= 0 && app.q_sel < n)
        highlight(0, 20 + app.q_vis, SCROLL_X, ROW_H, 20, 20 + ROWS * ROW_H, C_LINE);
    for (int r = 0; r < ROWS; r++)
    {
        int slot = app.q_scroll + r;
        int y = 20 + r * ROW_H;
        // while dragging, the other rows close up and a gap opens where it lands
        int i = slot;
        if (dragging)
        {
            if (slot == app.q_drag_to)
            {
                gfx_fill(B, 0, y, SCROLL_X, ROW_H, C_BG);
                continue;
            }
            int k = slot > app.q_drag_to ? slot - 1 : slot;
            i = k >= app.q_drag_from ? k + 1 : k;
        }
        bool cur = i == queue_pos();
        if (cur)
            gfx_fill(B, 0, y, SCROLL_X, ROW_H, C_SEL);
        if (i >= n)
            continue;
        uint32_t t = queue_track(i);
        char num[12], title[200], dur[16];
        snprintf(num, sizeof(num), "%d", slot + 1);
        text_r(B, 20, y, num, C_DIM);
        track_title(t, title, sizeof(title));
        mmss(dur, sizeof(dur), lib_track(t)->duration_ms);
        text_r(B, 216, y, dur, cur ? C_ACC : C_DIM);
        if (cur)
            anim |= marquee1(B, 28, y, 148, title, C_ACC, app.frame, false);
        else
        {
            font_draw(B, 28, y, 148, title, C_TEXT);
            text(B, 226, y, "×", C_DIM);
        }
    }
    scrollbar(20, ROWS, app.q_scroll, n, false);
    if (dragging && app.q_drag_to >= app.q_scroll && app.q_drag_to < app.q_scroll + ROWS)
    {
        int y = 20 + (app.q_drag_to - app.q_scroll) * ROW_H - 1;
        uint32_t t = queue_track(app.q_drag_from);
        char title[200], dur[16];
        gfx_fill(B, 4, y, 238, 18, C_PANEL);
        gfx_rect(B, 4, y, 238, 18, C_ACC);
        icon(B, 6, y + 1, 0x21C5, C_ACC);
        track_title(t, title, sizeof(title));
        font_draw(B, 28, y + 1, 148, title, C_TEXT);
        mmss(dur, sizeof(dur), lib_track(t)->duration_ms);
        text_r(B, 216, y + 1, dur, C_DIM);
    }
    return anim;
}

static void toggle(int x, int y, bool on)
{
    gfx_round_fill(B, x, y, 28, 14, 7, on ? C_ACC : C_LINE);
    gfx_disc(B, on ? x + 20 : x + 7, y + 7, 5, on ? C_TEXT : C_DIM);
}

static const SetItem items_top[] = {
    { "Playback", SI_PAGE, PAGE_PLAYBACK },
    { "Browsing", SI_PAGE, PAGE_BROWSING },
    { "Player", SI_PAGE, PAGE_PLAYER },
    { "Theme", SI_PAGE, PAGE_THEME },
    { "Regional", SI_PAGE, PAGE_REGIONAL },
    { "About", SI_PAGE, PAGE_ABOUT },
};
static const SetItem items_playback[] = {
    { "ReplayGain", SI_VALUE, SET_RG },
    { "Pause when lid closes", SI_TOGGLE, SET_PAUSE_LID },
    { "L/R skip, lid closed", SI_TOGGLE, SET_LR_LID },
};
static const SetItem items_browsing[] = {
    { "Case-sensitive names", SI_TOGGLE, SET_CASE },
    { "Scroll all long rows", SI_TOGGLE, SET_SCROLL_ALL },
};
static const SetItem items_player[] = {
    { "Time on the right", SI_VALUE, SET_TIME_RIGHT },
    { "Cover background", SI_TOGGLE, SET_COVER_BG },
    { "Shuffle by rating", SI_TOGGLE, SET_SHUFFLE_W },
};
#define N_ITEMS(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const struct {
    const char *title;
    const SetItem *items;
    int n;
    const char *later;      // not there yet: what it will hold
} pages[PAGE_COUNT] = {
    [PAGE_TOP] = { "Settings", items_top, N_ITEMS(items_top), NULL },
    [PAGE_PLAYBACK] = { "Playback", items_playback, N_ITEMS(items_playback), NULL },
    [PAGE_BROWSING] = { "Browsing", items_browsing, N_ITEMS(items_browsing), NULL },
    [PAGE_PLAYER] = { "Player", items_player, N_ITEMS(items_player), NULL },
    [PAGE_THEME] = { "Theme", NULL, 0, NULL },
    [PAGE_REGIONAL] = { "Regional", NULL, 0, "Language, date and time format" },
    [PAGE_ABOUT] = { "About", NULL, 0, NULL },
};

int ui_set_count(int page)
{
    return page >= 0 && page < PAGE_COUNT ? pages[page].n : 0;
}

const SetItem *ui_set_item(int page, int i)
{
    return &pages[page].items[i];
}

static bool set_on(int id)
{
    switch (id)
    {
    case SET_SCROLL_ALL: return app.set.scroll_all;
    case SET_CASE: return app.set.case_sensitive;
    case SET_PAUSE_LID: return app.set.pause_on_lid;
    case SET_LR_LID: return app.set.lr_skip_lid;
    case SET_COVER_BG: return !app.set.plain_bg;
    case SET_SHUFFLE_W: return app.set.shuffle_weighted;
    default: return false;
    }
}

static const char *set_value(int id)
{
    static const char *const rg[RG_MODES] = { "Off", "Track", "Album", "Auto" };
    if (id == SET_RG)
        return rg[app.set.rg_mode];
    if (id == SET_TIME_RIGHT)
        return app.set.show_remaining ? "Left" : "Length";
    return "";
}

static void centered_at(int cx, int y, const char *s, uint16_t color, int scale)
{
    font_draw_ex(B, cx - font_text_width(s) * scale / 2, y, 0, SCREEN_W, s, color, -1, scale);
}

static void about_body(int dx)
{
    int cx = dx + SCREEN_W / 2;
    centered_at(cx, 26, APP_NAME, C_ACC, 2);
    centered_at(cx, 60, "Version " APP_VERSION, C_DIM, 1);
    centered_at(cx, 84, "Music player for the DS Lite", C_TEXT, 1);
    centered_at(cx, 100, "© 2026 Alya (alya-can-eat)", C_TEXT, 1);
    centered_at(cx, 122, "Licence: GNU GPL v3", C_TEXT, 1);
    centered_at(cx, 138, "Free software, no warranty", C_DIM, 1);
    centered_at(cx, 158, APP_URL, C_ACC, 1);
    centered_at(cx, 174, "Glyphs: GNU Unifont", C_DIM, 1);
}

// Black or white, whichever reads on c.
static uint16_t ink_on(uint16_t c)
{
    int y = (c & 31) * 3 + ((c >> 5) & 31) * 6 + ((c >> 10) & 31);     // brightness x 10
    return y > 160 ? COLOR(1, 1, 2) : COLOR(31, 31, 31);
}

// The colours to choose, Reset, and the colour triangle.
static void theme_body(int dx)
{
    static const char *const names[3] = { "Accent", "Background", "Text" };
    const uint16_t cols[3] = { app.theme.acc, app.theme.bg, app.theme.text };
    for (int i = 0; i < 3; i++)
    {
        int x = dx + THEME_CHIP_X, y = THEME_CHIP_Y + i * THEME_CHIP_PITCH;
        uint16_t c = cols[i] | 0x8000;
        if (i == app.theme_sel)
        {
            // a frame that shows on any colour
            gfx_rect(B, x - 2, y - 2, THEME_CHIP_W + 4, THEME_CHIP_H + 4, COLOR(31, 31, 31));
            gfx_rect(B, x - 1, y - 1, THEME_CHIP_W + 2, THEME_CHIP_H + 2, COLOR(0, 0, 0));
        }
        else
            gfx_rect(B, x - 1, y - 1, THEME_CHIP_W + 2, THEME_CHIP_H + 2, COLOR(14, 14, 16));   // even on its own colour
        gfx_fill(B, x, y, THEME_CHIP_W, THEME_CHIP_H, c);
        const char *s = names[i];
        text(B, x + (THEME_CHIP_W - font_text_width(s)) / 2, y + 3, s, ink_on(c));
    }
    // Reset in fixed colours: findable whatever the theme
    int x = dx + THEME_CHIP_X;
    gfx_round_fill(B, x, THEME_RESET_Y, THEME_CHIP_W, THEME_CHIP_H, 4, COLOR(9, 9, 10));
    text(B, x + (THEME_CHIP_W - font_text_width("Reset")) / 2, THEME_RESET_Y + 3, "Reset", COLOR(31, 31, 31));
    cp_draw(B, dx, app.theme_hsv);
}

// A settings page with its left edge at dx (pages slide in from the right).
// lit: the D-pad highlight is on this page.
static bool settings_page(int page, int dx, bool lit)
{
    gfx_clip(B, dx, 0, SCREEN_W, SCREEN_H);
    gfx_fill(B, dx, 0, SCREEN_W, SCREEN_H, C_BG);
    gfx_fill(B, dx, 0, SCREEN_W, 20, C_PANEL);
    if (page == PAGE_TOP)
        text(B, dx + 8, 2, pages[page].title, C_TEXT);
    else
    {
        text(B, dx + 6, 2, "‹", C_TEXT);
        text(B, dx + 18, 2, pages[page].title, C_TEXT);
    }
    text(B, dx + 236, 2, "×", C_TEXT);
    bool anim = false;
    if (page == PAGE_ABOUT)
        about_body(dx);
    else if (page == PAGE_THEME)
        theme_body(dx);
    else if (pages[page].later)
    {
        centered_at(dx + SCREEN_W / 2, 76, pages[page].later, C_TEXT, 1);
        centered_at(dx + SCREEN_W / 2, 96, "Coming in a later version", C_DIM, 1);
    }
    else
    {
        int n = pages[page].n;
        for (int i = 0; i < n - 1; i++)
            gfx_fill(B, dx + 8, SET_Y + i * SET_H + SET_H - 1, 240, 1, C_ZEBRA);
        if (lit && app.menu_sel >= 0)
            highlight(dx, SET_Y + app.menu_vis, SCREEN_W, SET_H - 1, SET_Y, SET_Y + n * SET_H, C_SEL);
        const int ty = (SET_H - 1 - 16) / 2, gy = (SET_H - 1 - 14) / 2;     // text, switch in the row
        for (int i = 0; i < n; i++)
        {
            const SetItem *it = &pages[page].items[i];
            int y = SET_Y + i * SET_H;
            const char *value = it->kind == SI_VALUE ? set_value(it->id) : NULL;
            int w = value ? 232 - 8 - font_text_width(value) - 8 : 196;
            anim |= marquee1(B, dx + 8, y + ty, w, it->label, C_TEXT, app.frame, false);
            if (it->kind == SI_TOGGLE)
                toggle(dx + 216, y + gy, set_on(it->id));
            else if (value)
                text_r(B, dx + 244, y + ty, value, C_ACC);
            else
            {
                text_r(B, dx + 244, y + ty, "›", C_DIM);
                if (it->kind == SI_PAGE && pages[it->id].later)
                    text_r(B, dx + 232, y + ty, "later", C_FAINT);
            }
        }
    }
    gfx_unclip(B);
    return anim;
}

static bool draw_settings(void)
{
    // a page pushes the top page out to the left (and back)
    int q = ease_out(clamp256(app.page_t * 256 / TAB_TICKS));
    int dx = SCREEN_W - SCREEN_W * q / 256;
    bool anim = false;
    if (dx > 0)
    {
        anim = settings_page(PAGE_TOP, 0, app.set_page == PAGE_TOP);
        if (dx < SCREEN_W)
            for (int y = 0; y < SCREEN_H; y++)
                memmove(B->px + y * SCREEN_W, B->px + y * SCREEN_W + (SCREEN_W - dx), dx * 2);
    }
    if (dx < SCREEN_W)
        anim |= settings_page(app.set_shown, dx, app.set_page != PAGE_TOP);
    return anim || (app.page_t > 0 && app.page_t < TAB_TICKS);
}

static bool w_settings(int arg) { (void)arg; return draw_settings(); }
static bool w_info(int arg) { (void)arg; return draw_info(); }
static bool w_queue(int arg) { (void)arg; return draw_queue(); }

bool ui_draw_bottom(void)
{
    B = gfx_canvas(SCREEN_BOTTOM);
    gfx_unclip(B);
    int p = ov_progress();
    if (app.ov_vis == OV_SETTINGS && p >= 256)
        return draw_settings();

    int oy = app_panel_offset();
    bool anim = (app.slide > 0 && app.slide < SLIDE_TICKS) || app.press_down || app.frame < app.press_until
                || (app.pp_t > 0 && app.pp_t < PP_TICKS) || (app.knob_t > 0 && app.knob_t < KNOB_TICKS);
    if (oy > 0)
    {
        bool panel = app.ov_vis == OV_INFO || app.ov_vis == OV_QUEUE;
        if (panel && p >= 256)
            anim |= app.ov_vis == OV_INFO ? draw_info() : draw_queue();
        else
        {
            anim |= draw_player_area();
            if (panel)
            {
                // rises from behind the tabs
                shifted(app.ov_vis == OV_INFO ? w_info : w_queue, 0, 0, 0, SCREEN_W, PANEL_DOWN,
                        PANEL_DOWN * (256 - p) / 256, PANEL_DOWN);
                anim = true;
            }
        }
    }
    anim |= draw_panel(oy);
    if (app.ov_vis == OV_SETTINGS)
    {
        shifted(w_settings, 0, 0, 0, SCREEN_W, SCREEN_H, SCREEN_H * (256 - p) / 256, SCREEN_H);
        anim = true;
    }
    return anim;
}
