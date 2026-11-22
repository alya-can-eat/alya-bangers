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
#include "eq.h"
#include "eqprof.h"
#include "font.h"
#include "keyboard.h"
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
        snprintf(buf, size, "%s", v == LIB_NONE ? txt(S_NO_ARTIST) : lib_value(v));
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

static const uint8_t sort_names[SORT_SLOTS][4] = {
    { S_SORT_NAME_UP, S_SORT_NAME_DOWN },
    { S_SORT_NAME_UP, S_SORT_NAME_DOWN, S_SORT_YEAR_UP, S_SORT_YEAR_DOWN },
    { S_SORT_NAME_UP, S_SORT_NAME_DOWN, S_SORT_YEAR_UP },
    { S_SORT_TRACK, S_SORT_NAME_UP, S_SORT_NAME_DOWN },
    { S_SORT_NAME_UP, S_SORT_NAME_DOWN },
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
    return txt(sort_names[slot][i]);
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
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char s[32];
    snprintf(s, sizeof(s), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    text(T, 4, 0, s, C_TEXT);
    snprintf(s, sizeof(s), "%s %02d.%02d.%04d", txt(S_SUN + tmv.tm_wday % 7), tmv.tm_mday, tmv.tm_mon + 1,
             tmv.tm_year + 1900);
    text_r(T, 252, 0, s, C_DIM);
    // the card was too slow somewhere (should never show)
    if (player_underruns())
    {
        snprintf(s, sizeof(s), txt(S_DROPS), (unsigned long)player_underruns());
        text(T, 52, 0, s, COLOR(31, 12, 10));
    }
}

typedef struct {
    char title[200], artist[200], album[200], extra[48];
} SongText;

#define ARTIST_SEP ", "     // between the artists of a song

// All artists of a song, e.g. "Ken Aoba, Aethel".
static void artists_of(uint32_t t, char *out, size_t size)
{
    if (lib_tag_joined(t, "artist", ARTIST_SEP, out, size) == 0)
    {
        const LibTrack *tr = lib_track(t);
        snprintf(out, size, "%s", tr->artist != LIB_NONE ? lib_value(tr->artist) : "");
    }
}

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
    artists_of(t, st->artist, sizeof(st->artist));
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
        font_draw_ex(T, x0, 24, x0, x0 + w, txt(S_NOTHING_1), C_DIM, 0, 1);
        font_draw_ex(T, x0, 40, x0, x0 + w, txt(S_NOTHING_2), C_DIM, 0, 1);
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
    font_draw_ex(T, x + (48 - font_text_width(txt(S_NEXT))) / 2, 24, x, SCREEN_W, txt(S_NEXT), C_DIM, 0, 1);
    int np = queue_next_pos(true);
    if (np < 0 || !lib_loaded())
    {
        font_draw_ex(T, x + 20, 80, x, x + w, "–", C_DIM, 0, 1);
        return false;
    }
    uint32_t nt = queue_track(np);
    bool own = app.has_thumb && app.thumb_track == nt;
    gfx_blit(T, x + 8, 44, 32, 32, own ? app.thumb : default_thumb);
    char title[200], artists[200];
    track_title(nt, title, sizeof(title));
    artists_of(nt, artists, sizeof(artists));
    bool anim = marquee1(T, x, 80, w, title, C_TEXT, t, true);
    anim |= marquee1(T, x, 96, w, artists, C_DIM, t, true);
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
    char title[200], artists[200];
    track_title(track, title, sizeof(title));
    artists_of(track, artists, sizeof(artists));
    font_set_alpha(alpha);
    marquee1(T, x, 80, 48, title, C_TEXT, t, true);
    marquee1(T, x, 96, 48, artists, C_DIM, t, true);
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

    font_draw_ex(T, 200 + (48 - font_text_width(txt(S_NEXT))) / 2, 24, 200, SCREEN_W, txt(S_NEXT), C_DIM, 0, 1);

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

static bool sheet_on_top(int *sy);
static bool draw_sheet_top(int sy);
static bool ui_draw_top_screen(void);

bool ui_draw_top(void)
{
    T = gfx_canvas(SCREEN_TOP);
    gfx_unclip(T);
    int sy;
    bool sheet = sheet_on_top(&sy);
    if (sheet && sy <= SHEET_Y)
    {
        // typing: the results cover the top screen
        header();
        return draw_sheet_top(sy);
    }
    bool anim = ui_draw_top_screen();
    if (sheet)
        anim |= draw_sheet_top(sy);
    return anim;
}

static bool ui_draw_top_screen(void)
{
    uint32_t t = app.frame - app.text_t0;      // the song's texts start scrolling from their start
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
        viz_update(playing);
        draw_viz(256);
        draw_song_change(t);
        return true;
    }
    if (p2 > 0)
    {
        // paused or stopped: the bars fall back down
        bool falling = viz_update(playing);
        draw_viz(p2);
        if (playing || app.vinyl_speed > 0 || falling)
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
    static const uint8_t names[TAB_COUNT] = { S_TAB_ARTIST, S_TAB_ALBUM, S_TAB_TITLE, S_NONE, S_NONE };
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
            const char *name = txt(names[i]);
            text(B, x + (w - font_text_width(name)) / 2, oy + 2, name, act ? C_TEXT : C_DIM);
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
    case ITEM_ARTIST: return lib_track_in_group(cur, (int)it->id);
    default: return false;
    }
}

static void centered(Canvas *c, int y, const char *s, uint16_t color)
{
    int w = font_text_width(s);
    text(c, (SCREEN_W - w) / 2, y, s, color);
}

static bool draw_search_page(int oy);

static bool draw_list(int oy)
{
    bool anim = false;
    int y0 = oy + LIST_Y;
    if (!lib_loaded())
    {
        gfx_fill(B, 0, y0, SCREEN_W, ROWS * ROW_H, C_BG);
        static const uint8_t why[] = { S_LIB_MISSING, S_LIB_MISSING, S_LIB_DAMAGED, S_LIB_TOO_BIG, S_LIB_VERSION };
        int e = app.lib_error >= 0 && app.lib_error < (int)sizeof(why) ? app.lib_error : 0;
        centered(B, y0 + 48, txt(why[e]), C_TEXT);
        centered(B, y0 + 72, txt(S_RUN_DSCONV), C_DIM);
        return false;
    }
    if (app.tab == TAB_SEARCH)
        return draw_search_page(oy);
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
    static const uint8_t titles[SORT_SLOTS] = { S_SORT_ARTISTS, S_SORT_ALBUMS, S_SORT_SONGS, S_SORT_SONGS, S_SORT_FOLDERS };
    text(B, x0 + 8, y0 + 2, txt(titles[slot]), C_DIM);
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
    int row_y = list_y() + (app.menu_item - app.scroll) * ROW_H;
    int y0 = oy + ui_rowmenu_y(row_y);
    int x0 = 88, x1 = 240, h = 20 + 16 * 3 + 4;
    gfx_fill(B, x0, y0, x1 - x0, h, C_PANEL);
    gfx_rect(B, x0, y0, x1 - x0, h, C_ACC);
    char label[256];
    ui_item_label(&app.items[app.menu_item], label, sizeof(label));
    font_draw(B, x0 + 8, y0 + 2, x1 - x0 - 16, label, C_DIM);
    static const uint32_t icons[3] = { 0x25B6, 0x21B3, '+' };
    static const uint8_t names[3] = { S_PLAY_NOW, S_PLAY_NEXT, S_ADD_QUEUE };
    if (app.menu_sel >= 0)
        highlight(x0 + 1, y0 + 20 + app.menu_vis, x1 - x0 - 2, 16, y0 + 20, y0 + 20 + 16 * 3, C_SEL);
    for (int i = 0; i < 3; i++)
    {
        int y = y0 + 20 + i * 16;
        icon(B, x0 + 4, y, icons[i], C_ACC);
        text(B, x0 + 24, y, txt(names[i]), C_TEXT);
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
    int y0 = oy + ui_rowmenu_y(list_y() + (app.menu_item - app.scroll) * ROW_H);
    shifted(w_row, oy, 88, y0, 240, y0 + 72, (oy + BAR_Y - y0) * (256 - p) / 256, oy + BAR_Y);
    return true;
}

// ------------------------------------------------------------ search (🔍)

static int ease_io(int q)
{
    q = clamp256(q);
    return q * q * (768 - 2 * q) / 65536;
}

static uint16_t key_fill(bool fn)
{
    return gfx_mix(C_TEXT, C_BG, fn ? 28 : 50);
}

// A rounded box with an outline.
static void round_box(Canvas *c, int x, int y, int w, int h, int r, uint16_t fill, uint16_t line)
{
    gfx_round_fill(c, x, y, w, h, r, line);
    gfx_round_fill(c, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 1, fill);
}

// Where the results' top edge is, in one tall picture of both screens (the
// bottom screen starts at SCREEN_H + HINGE): on the top screen while typing,
// in the list under the filters otherwise, moving with the keyboard.
static int sheet_y(void)
{
    int kp = app.tab == TAB_SEARCH ? ease_io(app.kb_t * 256 / KB_TICKS) : 256;
    int up = SCREEN_H - (SCREEN_H - SHEET_Y) * ease_io(app.sheet_t * 256 / SHEET_TICKS) / 256;
    int down = SCREEN_H + HINGE + SEARCH_LIST_Y;
    return down + (up - down) * kp / 256;
}

static void draw_field(int oy)
{
    int y = oy + FIELD_Y;
    bool typing = app.tab == TAB_SEARCH && app.kb_up;
    round_box(B, 4, y, 248, 20, 4, C_ZEBRA, typing ? C_ACC : C_LINE);
    int right = app.query[0] ? 228 : 246;
    int tw = font_text_width(app.query);
    int x = 10 + (tw > right - 16 ? right - 16 - tw : 0);     // long: its end shows
    gfx_clip(B, 6, y, right, y + 20);
    if (app.query[0])
        text(B, x, y + 2, app.query, C_TEXT);
    else
        text(B, 14, y + 2, txt(S_SEARCH_HINT), C_FAINT);     // after the cursor
    if (typing && (app.frame / 15) % 2 == 0)
        gfx_fill(B, (app.query[0] ? x + tw : 10) + 1, y + 3, 2, 14, C_ACC);
    gfx_clip(B, 0, oy, SCREEN_W, SCREEN_H);
    if (app.query[0])
        text(B, 236, y + 2, "×", C_DIM);
}

int ui_chip_full_w(int kind)
{
    char label[96];
    search_chip_label(&app.sf, kind, label, sizeof(label));
    int tw = font_text_width(label);
    return (tw > 96 ? 96 : tw) + 28;
}

static int plus_chip_w(int x)
{
    int w = font_text_width(txt(S_FILTER)) + 12;
    return x + w <= 252 ? w : 20;       // no room: just "+"
}

int ui_chip_at(int x, int y, bool *on_x)
{
    *on_x = false;
    if (y < CHIPS_Y - 2 || y >= CHIPS_Y + 20)
        return -1;
    int cx = 4;
    for (int k = 0; k < F_COUNT; k++)
    {
        int w = app.chip_w[k];
        if (w <= 0)
            continue;
        if (x >= cx && x < cx + w + 2)
        {
            *on_x = x >= cx + w - 20;
            return k;
        }
        cx += w + 4;
    }
    return x >= cx - 2 && x < cx + plus_chip_w(cx) + 4 ? F_COUNT : -1;
}

static void draw_chips(int oy)
{
    int y = oy + CHIPS_Y, x = 4;
    for (int k = 0; k < F_COUNT; k++)
    {
        int w = app.chip_w[k];
        if (w <= 0)
            continue;
        gfx_round_fill(B, x, y, w, 18, 8, C_SEL);
        if (search_filter_on(&app.sf, k) && w >= ui_chip_full_w(k) - 1)
        {
            char label[96];
            search_chip_label(&app.sf, k, label, sizeof(label));
            font_draw(B, x + 7, y + 1, w - 28, label, C_TEXT);
            text(B, x + w - 15, y + 1, "×", C_DIM);
        }
        x += w + 4;
    }
    int w = plus_chip_w(x);
    bool open = app.overlay == OV_FILTER;
    const char *s = w > 20 ? txt(S_FILTER) : "+";
    if (open)
        gfx_round_fill(B, x, y, w, 18, 8, C_ACC);
    else
        round_box(B, x, y, w, 18, 8, C_BG, C_LINE);
    text(B, x + 6, y + 1, s, open ? C_BG : C_DIM);
}

// ---- result rows (on either screen)

static void search_meta(const Item *it, char *buf, int size)
{
    buf[0] = 0;
    if (it->kind == ITEM_ARTIST)
        snprintf(buf, size, "%d", lib_group_size((int)it->id));
    else if (it->kind == ITEM_ALBUM)
        snprintf(buf, size, "%s", lib_value(lib_album(it->id)->artist));
    else if (it->kind == ITEM_TRACK)
        artists_of(it->id, buf, size);
}

// Byte offset of the n-th character.
static int char_at(const char *s, int n)
{
    const char *p = s;
    for (int i = 0; i < n && *p; i++)
        utf8_next(&p);
    return (int)(p - s);
}

// Pieces of text in w pixels, cut with "…" if they don't fit.
static void pieces_fit(Canvas *c, int x, int y, int w, const char *const *s, const uint16_t *col, int n)
{
    char all[600];
    all[0] = 0;
    for (int i = 0; i < n; i++)
        strncat(all, s[i], sizeof(all) - strlen(all) - 1);
    int jp = font_has_kana(all);
    if (font_text_width(all) <= w)
    {
        int pen = x;
        for (int i = 0; i < n; i++)
            pen += font_draw_ex(c, pen, y, x, x + w, s[i], col[i], jp, 1);
        return;
    }
    // the whole in the first colour with "…", then the other pieces over it up to the "…"
    int drawn = font_draw(c, x, y, w, all, col[0]);
    int cut = x + drawn - font_text_width("…");
    int pen = x;
    for (int i = 0; i < n; i++)
    {
        if (i > 0 && col[i] != col[0])
            font_draw_ex(c, pen, y, x, cut, s[i], col[i], jp, 1);
        pen += font_text_width(s[i]);
    }
}

static bool result_row(Canvas *c, int y, int i, int right, bool sel)
{
    const Item *it = &app.items[i];
    const SearchMark *m = &app.marks[i];
    if (it->kind == ITEM_HEADER)
    {
        static const uint8_t names[3] = { S_SEC_ARTISTS, S_SEC_ALBUMS, S_SEC_SONGS };
        char n[12];
        snprintf(n, sizeof(n), "%u", (unsigned)m->count);
        text(c, 4, y, txt(names[it->id < 3 ? it->id : 2]), C_DIM);
        text_r(c, right, y, n, C_DIM);
        gfx_fill(c, 4, y + 15, right - 4, 1, C_LINE);
        return false;
    }
    static char name[256], meta[200];
    ui_item_label(it, name, sizeof(name));
    search_meta(it, meta, sizeof(meta));
    bool playing = item_playing(it);
    int mw = 0;
    if (meta[0])
    {
        mw = font_text_width(meta);
        if (mw > 104)
            mw = 104;
        font_draw(c, right - mw, y, mw, meta, playing ? C_ACC : C_DIM);
    }
    int w = right - 4 - (meta[0] ? mw + 8 : 0);
    // the name (then the reading it was found by, dim), the match in the accent colour
    uint16_t base = playing ? C_ACC : C_TEXT;
    const char *reading = search_reading(it, m->via);
    const char *where = reading ? reading : name;
    uint16_t rest_col = reading ? C_DIM : base;
    int a = char_at(where, m->start), b = char_at(where, m->start + m->len);
    static char p0[300], p1[256], p2[256], p3[256];
    const char *pieces[4];
    uint16_t cols[4];
    int n = 0;
    if (reading)
    {
        snprintf(p0, sizeof(p0), "%s  ", name);
        pieces[n] = p0, cols[n++] = base;
    }
    snprintf(p1, sizeof(p1), "%.*s", a, where);
    pieces[n] = p1, cols[n++] = rest_col;
    snprintf(p2, sizeof(p2), "%.*s", b - a, where + a);
    pieces[n] = p2, cols[n++] = m->len ? C_ACC : rest_col;
    snprintf(p3, sizeof(p3), "%s", where + b);
    pieces[n] = p3, cols[n++] = rest_col;
    if (sel || app.set.scroll_all)
        return marquee(c, 4, y, w, pieces, cols, n, sel ? app.frame - app.sel_frame : app.frame, false);
    pieces_fit(c, 4, y, w, pieces, cols, n);
    return false;
}

// The results as a sheet with its top at y: rows from app.scroll, drawn
// only between clip0 and clip1. hl: the selection highlight shows.
static bool draw_sheet(Canvas *c, int y, int clip0, int clip1, int right, bool hl)
{
    int bottom = y + (SCREEN_H - SHEET_Y);
    if (bottom > clip1) bottom = clip1;
    int top = y > clip0 ? y : clip0;
    if (top >= bottom)
        return false;
    gfx_clip(c, 0, top, SCREEN_W, bottom);
    gfx_fill(c, 0, top, SCREEN_W, bottom - top, C_BG);
    bool anim = false;
    int rows = (bottom - y + ROW_H - 1) / ROW_H;
    int w = right == 240 ? SCROLL_X : SCREEN_W;
    for (int k = 0; k < rows; k++)
    {
        int i = app.scroll + k;
        if (i < app.n_items && app.items[i].kind != ITEM_HEADER)
            gfx_fill(c, 0, y + k * ROW_H, w, ROW_H, k % 2 ? C_ZEBRA : C_BG);
    }
    if (hl && app.sel >= 0 && app.sel < app.n_items)
        gfx_fill(c, 0, y + app.sel_vis, w, ROW_H, C_SEL);
    for (int k = 0; k < rows; k++)
    {
        int i = app.scroll + k;
        if (i >= app.n_items)
            break;
        anim |= result_row(c, y + k * ROW_H, i, right, hl && i == app.sel);
    }
    if (app.n_items == 0 && (app.query[0] || search_songs_filtered(&app.sf)))
    {
        const char *s = txt(S_NO_RESULTS);
        font_draw_ex(c, (SCREEN_W - font_text_width(s)) / 2, y + 24, 0, SCREEN_W, s, C_DIM, -1, 1);
    }
    gfx_unclip(c);
    return anim;
}

// Top screen: the results while typing (or on their way down).
static bool draw_sheet_top(int sy)
{
    return draw_sheet(T, sy, SHEET_Y, SCREEN_H, 252, app.sel_on || !app.kb_up);
}

static bool sheet_on_top(int *sy)
{
    if (app.player_mode || app.slide > 0 || (app.tab != TAB_SEARCH && app.sheet_t == 0))
        return false;
    *sy = sheet_y();
    return *sy < SCREEN_H;
}

// The search page under the tabs: the query, the filters, and (keyboard
// down) the results.
static bool draw_search_page(int oy)
{
    gfx_fill(B, 0, oy + TAB_H, SCREEN_W, BAR_Y - TAB_H, C_BG);
    draw_field(oy);
    draw_chips(oy);
    if (app.kb_t > 0)
        return false;
    int y0 = oy + SEARCH_LIST_Y;
    bool anim = draw_sheet(B, y0, y0, y0 + SEARCH_ROWS * ROW_H, 240, true);
    gfx_clip(B, 0, oy, SCREEN_W, SCREEN_H);
    scrollbar(y0, SEARCH_ROWS, app.scroll, app.n_items, app.scroll_drag);
    return anim;
}

// ---- keyboards

static void key_label(const Key *k, int im, char *buf)
{
    static const char *const kana[3] = { "゛", "゜", "小" };
    switch (k->kind)
    {
    case K_CHAR: buf[utf8_put(buf, k->cp)] = 0; break;
    case K_SPACE: strcpy(buf, txt(S_SPACE)); break;
    case K_IM: strcpy(buf, kb_im_label(im)); break;
    case K_NUM: strcpy(buf, "123"); break;
    case K_KANA: strcpy(buf, kana[k->cp < 3 ? k->cp : 0]); break;
    default: buf[0] = 0; break;
    }
}

static void draw_key(int x, int y, const Key *k, bool lit, int im)
{
    if (k->kind == K_DONE && app.ov_vis == OV_EQ_NAME)
    {
        // naming a profile: OK saves it (once it has a name)
        bool ok = app.eq_name[0] != 0;
        gfx_round_fill(B, x, y, k->w, k->h, 3, ok ? (lit ? gfx_mix(C_ACC, C_TEXT, 160) : C_ACC) : key_fill(true));
        const char *l = txt(S_EQ_OK);
        text(B, x + (k->w - font_text_width(l)) / 2, y + (k->h - 16) / 2, l, ok ? C_BG : C_FAINT);
        return;
    }
    gfx_round_fill(B, x, y, k->w, k->h, 3, lit ? C_ACC : key_fill(k->fn));
    uint16_t col = lit ? C_BG : (k->fn ? C_DIM : C_TEXT);
    uint32_t sym = k->kind == K_SHIFT ? 0x21E7 : (k->kind == K_BACK ? 0x232B : (k->kind == K_DONE ? 0x2B9F : 0));
    if (sym)
    {
        icon(B, x + (k->w - 16) / 2, y + (k->h - 16) / 2, sym, col);
        return;
    }
    char label[48];
    key_label(k, im, label);
    int jp = im == IM_KANA ? 1 : -1;
    int tw = font_text_width(label);
    font_draw_ex(B, x + (k->w - tw) / 2, y + (k->h - 16) / 2, x, x + k->w, label, col, jp, 1);
}

static void draw_layout(int dx, int y0, int im, bool current)
{
    Key keys[KB_MAX_KEYS];
    bool num = current && app.kb_num;
    int n = kb_layout(im, num, app.set.keyboard == KB_QWERTZ, current && app.shift && !num, keys);
    for (int i = 0; i < n; i++)
    {
        const Key *k = &keys[i];
        int x = dx + k->x;
        if (x + k->w <= 0 || x >= SCREEN_W)
            continue;
        bool lit = current && (i == app.key_down || i == app.acc_key
                               || (i == app.key_flash && app.frame < app.key_flash_until)
                               || (k->kind == K_SHIFT && app.shift) || (k->kind == K_NUM && app.kb_num));
        draw_key(x, y0 + k->y, k, lit, im);
    }
}

// A held letter: its other forms in a row above it.
static void draw_forms(int y0)
{
    Key keys[KB_MAX_KEYS];
    int n = kb_layout(app.im, app.kb_num, app.set.keyboard == KB_QWERTZ, app.shift && !app.kb_num, keys);
    if (app.acc_key < 0 || app.acc_key >= n)
        return;
    const Key *k = &keys[app.acc_key];
    const char *f = letter_forms(cp_lower(k->cp));
    if (f == NULL)
        return;
    uint32_t forms[12];
    int nf = 0;
    while (nf < 12 && (forms[nf] = utf8_next(&f)) != 0)
    {
        if (app.shift)
            forms[nf] = cp_upper(forms[nf]);
        nf++;
    }
    int bx, by, bw;
    kb_forms_box(k, nf, &bx, &by, &bw);
    int g = ease_out(clamp256(app.acc_t * 256 / ACC_TICKS));
    int w = k->w + (bw - k->w) * g / 256;
    int x0 = k->x + k->w / 2 - w / 2;
    if (x0 > SCREEN_W - 2 - w) x0 = SCREEN_W - 2 - w;
    if (x0 < 2) x0 = 2;
    gfx_unclip(B);
    round_box(B, x0, y0 + by, w, 32, 4, C_PANEL, C_ACC);
    if (g < 256)
        return;
    for (int i = 0; i < nf; i++)
        gfx_round_fill(B, bx + 2 + i * 25, y0 + by + 2, 23, 27, 3, key_fill(false));
    int hi = -1;
    if (app.acc_sel >= 0)
    {
        gfx_round_fill(B, app.acc_vis, y0 + by + 2, 23, 27, 3, C_ACC);
        hi = (app.acc_vis - bx - 2 + 12) / 25;
    }
    for (int i = 0; i < nf; i++)
    {
        char s[8];
        s[utf8_put(s, forms[i])] = 0;
        int tw = font_text_width(s);
        text(B, bx + 2 + i * 25 + (23 - tw) / 2, y0 + by + 7, s, i == hi ? C_BG : C_TEXT);
    }
}

static bool draw_keys(int kb_y, bool settled);

// Over the page and the bar: the results moving between the screens, the keyboard.
static bool search_layers(int oy)
{
    if (app.kb_t <= 0 || oy != 0)
        return false;
    int kb_y = SCREEN_H - (SCREEN_H - KB_Y) * ease_io(app.kb_t * 256 / KB_TICKS) / 256;
    bool anim = false;
    if (app.kb_t < KB_TICKS && app.n_items > 0)
    {
        // results on their way between the screens: over the query and the
        // filters, under the tabs (they stay put), the bar and the keyboard
        int sy = sheet_y() - (SCREEN_H + HINGE);
        draw_sheet(B, sy, TAB_H, kb_y < BAR_Y ? kb_y : BAR_Y, 240, !app.kb_up || app.sel_on);
    }
    if (app.kb_t < KB_TICKS)
        anim = true;
    return draw_keys(kb_y, app.kb_t == KB_TICKS) || anim;
}

// The keys at kb_y: the layout (pushed out by another while switching) and,
// once in place, a held letter's other forms.
static bool draw_keys(int kb_y, bool settled)
{
    bool anim = false;
    gfx_clip(B, 0, kb_y, SCREEN_W, SCREEN_H);
    gfx_fill(B, 0, kb_y, SCREEN_W, SCREEN_H - kb_y, C_BG);
    if (app.im_t > 0)
    {
        // another input method: the new keys push the old ones out to the left
        int p = ease_io((IM_TICKS - app.im_t) * 256 / IM_TICKS);
        int off = SCREEN_W * p / 256;
        draw_layout(-off, kb_y, app.im_from, false);
        draw_layout(SCREEN_W - off, kb_y, app.im, true);
        anim = true;
    }
    else
        draw_layout(0, kb_y, app.im, true);
    gfx_unclip(B);
    if (app.acc_key >= 0 && settled)
    {
        draw_forms(kb_y);
        anim |= app.acc_t < ACC_TICKS;
    }
    return anim;
}

// ---- menus: input methods, filters

int ui_im_menu_y(void)
{
    return 161 - (20 + 16 * IM_COUNT + 4) - 2;
}

static void draw_im_menu(int oy)
{
    int x0 = 4, x1 = 180, h = 20 + 16 * IM_COUNT + 4, y0 = oy + ui_im_menu_y();
    gfx_fill(B, x0, y0, x1 - x0, h, C_PANEL);
    gfx_rect(B, x0, y0, x1 - x0, h, C_ACC);
    text(B, x0 + 8, y0 + 2, txt(S_INPUT), C_DIM);
    if (app.menu_sel >= 0)
        highlight(x0 + 1, y0 + 20 + app.menu_vis, x1 - x0 - 2, 16, y0 + 20, y0 + 20 + 16 * IM_COUNT, C_SEL);
    for (int i = 0; i < IM_COUNT; i++)
    {
        int y = y0 + 20 + i * 16;
        bool cur = i == app.im;
        font_draw_ex(B, x0 + 8, y, 0, SCREEN_W, kb_im_label(i), cur ? C_ACC : C_TEXT, i == IM_KANA, 1);
        text(B, x0 + 48, y, txt(S_IM_LATIN + i), cur ? C_TEXT : C_DIM);
    }
}

static bool w_im(int oy) { draw_im_menu(oy); return false; }

static bool im_menu_moving(int oy)
{
    int p = ov_progress();
    if (p >= 256)
    {
        draw_im_menu(oy);
        return false;
    }
    int y0 = oy + ui_im_menu_y(), h = 20 + 16 * IM_COUNT + 4;
    shifted(w_im, oy, 4, y0, 180, y0 + h, (SCREEN_H - y0) * (256 - p) / 256, SCREEN_H);
    return true;
}

static const uint8_t filter_names[F_COUNT] = { S_SEARCH_IN, S_GENRE, S_INFO_RATING, S_INFO_YEAR, S_PLAYED, S_FOLDER };

// The filters (kind -1) or a filter's choices, with the left edge at dx.
static void filter_page(int kind, int dx, int oy)
{
    int y0 = oy + FMENU_Y;
    int x0 = dx < 0 ? 0 : dx, x1 = dx + SCREEN_W > SCREEN_W ? SCREEN_W : dx + SCREEN_W;
    if (x1 <= x0)
        return;
    gfx_clip(B, x0, y0, x1, SCREEN_H);
    gfx_fill(B, dx, y0, SCREEN_W, SCREEN_H - FMENU_Y, C_PANEL);
    gfx_rect(B, dx, y0, SCREEN_W, SCREEN_H - FMENU_Y, C_ACC);
    bool active = (kind < 0) == (app.fpage < 0);
    int r0 = y0 + 20;
    if (kind < 0)
    {
        text(B, dx + 8, y0 + 2, txt(S_ADD_FILTER), C_DIM);
        if (active && app.menu_sel >= 0)
            highlight(dx + 1, r0 + app.menu_vis, SCREEN_W - 2, 16, r0, r0 + 16 * FMENU_ROWS, C_SEL);
        for (int k = 0; k < F_COUNT; k++)
        {
            int y = r0 + k * 16;
            text(B, dx + 8, y, txt(filter_names[k]), C_TEXT);
            if (k == F_IN || search_filter_on(&app.sf, k))
            {
                char v[96];
                search_chip_label(&app.sf, k, v, sizeof(v));
                int vw = font_text_width(v);
                if (vw > 120) vw = 120;
                font_draw(B, dx + SCREEN_W - 8 - vw, y, vw, v, C_ACC);
            }
            else
                text_r(B, dx + SCREEN_W - 8, y, "›", C_DIM);
        }
    }
    else
    {
        char title[64];
        snprintf(title, sizeof(title), "‹ %s", txt(filter_names[kind]));
        text(B, dx + 8, y0 + 2, title, C_DIM);
        if (active && app.menu_sel >= 0)
            highlight(dx + 1, r0 + app.menu_vis, SCREEN_W - 2, 16, r0, r0 + 16 * FMENU_ROWS, C_SEL);
        char cur[96];
        bool on = search_filter_on(&app.sf, kind);
        search_chip_label(&app.sf, kind, cur, sizeof(cur));
        for (int r = 0; r < FMENU_ROWS; r++)
        {
            int i = app.fscroll + r;
            if (i >= app.n_fopts)
                break;
            int y = r0 + r * 16;
            char label[96], count[12];
            search_option_label(kind, app.fopts[i].value, label, sizeof(label));
            snprintf(count, sizeof(count), "%d", app.fopts[i].count);
            int cw = font_text_width(count);
            text_r(B, dx + SCREEN_W - 12, y, count, C_DIM);
            font_draw(B, dx + 8, y, SCREEN_W - 28 - cw, label, on && strcmp(label, cur) == 0 ? C_ACC : C_TEXT);
        }
        if (app.n_fopts > FMENU_ROWS)
        {
            // where in the choices this is
            int h = 16 * FMENU_ROWS, th = h * FMENU_ROWS / app.n_fopts;
            int ty = r0 + (h - th) * app.fscroll / (app.n_fopts - FMENU_ROWS);
            gfx_fill(B, dx + SCREEN_W - 5, ty, 2, th, C_ACC_DIM);
        }
        if (app.n_fopts == 0)
            text(B, dx + 8, r0, "–", C_DIM);
    }
    gfx_clip(B, 0, oy, SCREEN_W, SCREEN_H);
}

static bool draw_filter_menu(int oy)
{
    // a filter's choices push the list out to the left (and back)
    int q = ease_out(clamp256(app.fpage_t * 256 / TAB_TICKS));
    int dx = SCREEN_W - SCREEN_W * q / 256;
    if (dx > 0)
        filter_page(-1, dx - SCREEN_W, oy);
    if (dx < SCREEN_W && app.fkind >= 0)
        filter_page(app.fkind, dx, oy);
    return app.fpage_t > 0 && app.fpage_t < TAB_TICKS;
}

static bool w_filter(int oy) { return draw_filter_menu(oy); }

static bool filter_menu_moving(int oy)
{
    int p = ov_progress();
    if (p >= 256)
        return draw_filter_menu(oy);
    shifted(w_filter, oy, 0, oy + FMENU_Y, SCREEN_W, oy + SCREEN_H, (SCREEN_H - FMENU_Y) * (256 - p) / 256, SCREEN_H);
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
        if (app.tab == TAB_SEARCH)
            anim |= search_layers(oy);
        if (app.ov_vis == OV_SORT)
            anim |= sort_menu_moving(oy);
        if (app.ov_vis == OV_ROWMENU)
            anim |= row_menu_moving(oy);
        if (app.ov_vis == OV_FILTER)
            anim |= filter_menu_moving(oy);
        if (app.ov_vis == OV_IM)
            anim |= im_menu_moving(oy);
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

// Which of the artists' names in the player's top line is at (x, y), where
// it is drawn right now (the line scrolls)? -1: none. Same layout as
// draw_player_area; the names are the artist tag's values in order.
int ui_player_artist_at(int x, int y)
{
    if (y < 2 || y >= 28 || player_path() == NULL)
        return -1;
    uint32_t cur = app_current_track();
    SongText st;
    song_text(cur, &st);
    if (!st.artist[0])
        return -1;
    const char *segs[3] = { st.artist, " – ", st.title };
    char all[600];
    snprintf(all, sizeof(all), "%s%s%s", segs[0], segs[1], segs[2]);
    int jp = font_has_kana(all), aw = 0, tw = 0;
    for (int i = 0; i < 3; i++)
    {
        int w = font_draw_ex(B, SCREEN_W, -100, 0, 0, segs[i], 0, jp, 1);   // measure only
        if (i == 0)
            aw = w;
        tw += w;
    }
    // where each name starts and ends inside the artists' part
    uint32_t ids[16];
    int n_names = cur != LIB_NONE ? lib_tag_ids(cur, "artist", ids, 16) : 0;
    int from[16], to[16];
    if (n_names <= 1)
    {
        n_names = 1;
        from[0] = 0;
        to[0] = aw;
    }
    else
    {
        int sep = font_draw_ex(B, SCREEN_W, -100, 0, 0, ARTIST_SEP, 0, jp, 1), p = 0;
        for (int i = 0; i < n_names; i++)
        {
            from[i] = p;
            p += font_draw_ex(B, SCREEN_W, -100, 0, 0, lib_value(ids[i]), 0, jp, 1);
            to[i] = p;
            p += sep;
        }
    }
    const int lx = 8, lw = 240;
    int starts[2], n = 0;
    if (tw <= lw)
        starts[n++] = lx + (lw - tw) / 2;
    else
    {
        int period = tw + GAP;
        uint32_t tt = app.frame % (HOLD_FRAMES + period);
        int off = tt < HOLD_FRAMES ? 0 : (int)(tt - HOLD_FRAMES);
        starts[n++] = lx - off;
        starts[n++] = lx - off + period;
    }
    if (x < lx || x >= lx + lw)
        return -1;
    for (int i = 0; i < n; i++)
    {
        if (x < starts[i] - 4 || x >= starts[i] + aw + 4)
            continue;
        // the nearest name (a tap on ", " goes to the closer one)
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < n_names; k++)
        {
            int a = starts[i] + from[k], b = starts[i] + to[k];
            int d = x < a ? a - x : (x >= b ? x - b + 1 : 0);
            if (d < bd)
            {
                bd = d;
                best = k;
            }
        }
        return best;
    }
    return -1;
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
        centered(B, 8, txt(S_NOTHING_PLAYING), C_DIM);

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
    lit_box(48, 148, 28, 24, BTN_EQ);
    icon(B, 40, 140, ICON_EQ, eq_active() ? C_ACC : C_TEXT);     // lit while it changes the sound
    lit_box(236, 148, 28, 24, BTN_QUEUE);
    icon(B, 228, 140, 0x2630, C_TEXT);
    pill(128, 140, 0x2B9D, lit(BTN_UP));
    return anim;
}

// Song info: where the values (and the stars) start, after the longest label.
int ui_info_value_x(void)
{
    int w = 0;
    for (int i = 0; i < 9; i++)
    {
        int lw = font_text_width(txt(S_INFO_TITLE + i));
        if (lw > w)
            w = lw;
    }
    return 8 + w + 8 > INFO_STARS_X ? 8 + w + 8 : INFO_STARS_X;
}

static bool draw_info(void)
{
    gfx_fill(B, 0, 0, SCREEN_W, PANEL_DOWN, C_BG);
    gfx_fill(B, 0, 0, SCREEN_W, 20, C_PANEL);
    text(B, 8, 2, txt(S_SONG_INFO), C_TEXT);
    text(B, 236, 2, "×", C_TEXT);
    uint32_t t = app_current_track();
    if (t == LIB_NONE)
    {
        centered(B, 72, txt(S_NOTHING_PLAYING), C_DIM);
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
    char genre[160];
    lib_tag_joined(t, "genre", ", ", genre, sizeof(genre));
    const char *codec = lib_tag(t, "~codec");
    const DsaHeader *h = player_header();
    unsigned rate = h ? h->sound_clock_hz / h->timer_divider : 32728;
    snprintf(format, sizeof(format), "%s%sDSA %u.%u kHz", codec ? codec : "", codec ? " → " : "",
             rate / 1000, (rate % 1000) / 100);
    const char *vals[9] = { st.title, st.artist[0] ? st.artist : "–", st.album[0] ? st.album : "–",
                            track, year, genre[0] ? genre : "–", "", plays, format };
    bool anim = false;
    int vx = ui_info_value_x();
    for (int i = 0; i < 9; i++)
    {
        int y = INFO_Y + i * 16;
        text(B, 8, y, txt(S_INFO_TITLE + i), C_DIM);
        if (i != INFO_RATING_ROW)
            anim |= marquee1(B, vx, y, 248 - vx, vals[i], C_TEXT, app.frame, false);
    }
    // the stars can be tapped (Left / Right change them too)
    int stars = app_song_rating(t);
    for (int i = 0; i < 5; i++)
        icon(B, vx + i * INFO_STAR_W, INFO_Y + INFO_RATING_ROW * 16, i < stars ? 0x2605 : 0x2606,
             i < stars ? C_ACC : C_DIM);
    return anim;
}

static bool draw_queue(void)
{
    gfx_fill(B, 0, 0, SCREEN_W, PANEL_DOWN, C_BG);
    gfx_fill(B, 0, 0, SCREEN_W, 20, C_PANEL);
    char head[48];
    int n = queue_count();
    snprintf(head, sizeof(head), txt(n == 1 ? S_QUEUE_ONE : S_QUEUE_N), n);
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

// ---- equalizer

static const int8_t eq_flat[EQ_BANDS];

const int8_t *app_eq_profile_gain(int p)
{
    const EqProfile *pr = p > 0 ? eqp_get(p - 1) : NULL;
    return pr ? pr->gain : eq_flat;
}

bool app_eq_edited(void)
{
    return memcmp(app.set.eq_gain, app_eq_profile_gain(app.set.eq_profile), EQ_BANDS) != 0;
}

// +6, 0, −3
static void db_label(char *buf, int size, int db)
{
    if (db > 0)
        snprintf(buf, size, "+%d", db);
    else if (db < 0)
        snprintf(buf, size, "−%d", -db);
    else
        snprintf(buf, size, "0");
}

void ui_eq_buttons(int *save_x, int *save_w, int *del_x, int *del_w)
{
    *save_x = 4;
    *save_w = font_text_width(txt(S_EQ_SAVE)) + 12;
    *del_x = *save_x + *save_w + 6;
    *del_w = font_text_width(txt(S_EQ_DELETE)) + 12;
}

static void eq_button(int x, int w, const char *label, bool on, bool primary, int id)
{
    bool l = on && lit(id);
    if (on && primary)
    {
        gfx_round_fill(B, x, EQ_BTN_Y, w, EQ_BTN_H, 4, l ? gfx_mix(C_ACC, C_TEXT, 160) : C_ACC);
        text(B, x + 6, EQ_BTN_Y + 1, label, C_BG);
    }
    else
    {
        round_box(B, x, EQ_BTN_Y, w, EQ_BTN_H, 4, l ? C_LINE : C_BG, C_LINE);
        text(B, x + 6, EQ_BTN_Y + 1, label, on ? C_TEXT : C_FAINT);
    }
}

static void eq_row(int y, const char *name, bool chosen)
{
    if (chosen)
        gfx_fill(B, 0, y, EQ_LIST_W, ROW_H, C_SEL);
    font_draw(B, 6, y, EQ_LIST_W - 10, name, chosen ? C_ACC : C_TEXT);
}

// "Delete …?" over the panel, like the row menu.
static void eq_ask_box(void)
{
    const EqProfile *pr = eqp_get(app.set.eq_profile - 1);
    if (pr == NULL)
        return;
    int x = EQ_ASK_X, y = EQ_ASK_Y, w = EQ_ASK_W, h = 20 + 2 * ROW_H + 4;
    gfx_fill(B, x, y, w, h, C_ACC);
    gfx_fill(B, x + 1, y + 1, w - 2, h - 2, C_PANEL);
    char q[96];
    snprintf(q, sizeof(q), txt(S_EQ_ASK), pr->name);
    font_draw(B, x + 6, y + 2, w - 12, q, C_DIM);
    for (int i = 0; i < 2; i++)
    {
        int ry = y + 20 + i * ROW_H;
        bool hi = app.eq_ask_sel == i;
        if (hi)
            gfx_fill(B, x + 1, ry, w - 2, ROW_H, C_SEL);
        char a[48];
        snprintf(a, sizeof(a), "%s %s", i ? "‹" : "×", txt(i ? S_EQ_KEEP : S_EQ_DELETE));
        text(B, x + 6, ry, a, hi ? C_ACC : C_TEXT);
    }
}

static bool draw_eq(void)
{
    gfx_fill(B, 0, 0, SCREEN_W, PANEL_DOWN, C_BG);
    gfx_fill(B, 0, 0, SCREEN_W, 20, C_PANEL);
    text(B, 8, 2, txt(S_EQ), C_TEXT);
    if (app_eq_edited())
    {
        char e[48];
        snprintf(e, sizeof(e), " · %s", txt(S_EQ_EDITED));
        text(B, 8 + font_text_width(txt(S_EQ)), 2, e, C_DIM);
    }
    text(B, 236, 2, "×", C_TEXT);

    // the profiles: Flat, a line, the saved ones; one being moved floats
    // where it would land, the others close up around it
    int n = eqp_count();
    for (int r = 0; r < EQP_MAX; r++)
        gfx_fill(B, 0, EQ_OWN_Y + r * ROW_H, EQ_LIST_W, ROW_H, r % 2 ? C_ZEBRA : C_BG);
    eq_row(20, txt(S_EQ_FLAT), app.set.eq_profile == 0);
    gfx_fill(B, 4, EQ_OWN_Y - 1, EQ_LIST_W - 8, 1, C_LINE);
    bool dragging = app.eq_drag_from >= 0;
    for (int r = 0; r < n; r++)
    {
        int i = r;
        if (dragging)
        {
            if (r == app.eq_drag_to)
                continue;
            int k = r > app.eq_drag_to ? r - 1 : r;
            i = k >= app.eq_drag_from ? k + 1 : k;
        }
        eq_row(EQ_OWN_Y + r * ROW_H, eqp_get(i)->name, app.set.eq_profile == i + 1);
    }
    if (dragging)
    {
        int y = EQ_OWN_Y + app.eq_drag_to * ROW_H - 1;
        gfx_fill(B, 2, y, EQ_LIST_W - 4, 18, C_PANEL);
        gfx_rect(B, 2, y, EQ_LIST_W - 4, 18, C_ACC);
        font_draw(B, 6, y + 1, EQ_LIST_W - 12, eqp_get(app.eq_drag_from)->name, C_TEXT);
    }
    gfx_fill(B, EQ_LIST_W, 20, 1, EQ_LINE_Y - 20, C_PANEL);

    // the sliders: the value over, the band under; the one dragged or
    // moved with the D-pad in the accent
    for (int b = 0; b < EQ_BANDS; b++)
    {
        int cx = EQ_BAND_X(b), v = app.set.eq_gain[b];
        bool on = app.eq_drag_band == b || (app.eq_sliders && app.eq_band == b);
        char s[8];
        db_label(s, sizeof(s), v);
        text(B, cx - font_text_width(s) / 2, 21, s, on ? C_ACC : C_TEXT);
        gfx_fill(B, cx - 1, EQ_ZERO_Y + EQ_MIN * EQ_DB_PX, 2, (EQ_MAX - EQ_MIN) * EQ_DB_PX + 1, C_LINE);
        for (int db = EQ_MIN; db <= EQ_MAX; db += 6)
            if (db)
                gfx_fill(B, cx - 3, EQ_ZERO_Y - db * EQ_DB_PX, 7, 1, C_LINE);
        gfx_fill(B, cx - 6, EQ_ZERO_Y, 13, 1, C_DIM);
        int ky = EQ_ZERO_Y - v * EQ_DB_PX;
        if (v)
            gfx_fill(B, cx - 1, v > 0 ? ky : EQ_ZERO_Y, 2, (v > 0 ? v : -v) * EQ_DB_PX + 1, C_ACC);
        gfx_round_fill(B, cx - 8, ky - 3, 17, 7, 2, on ? C_ACC : C_TEXT);
        gfx_fill(B, cx - 4, ky, 9, 1, on ? C_SEL : C_LINE);
        const char *lab = txt(S_EQ_BASS + b);
        text(B, cx - font_text_width(lab) / 2, EQ_ZERO_Y + EQ_MAX * EQ_DB_PX + 2, lab, on ? C_ACC : C_DIM);
    }

    gfx_fill(B, 0, EQ_LINE_Y, SCREEN_W, 1, C_PANEL);
    int sx, sw, dx, dw;
    ui_eq_buttons(&sx, &sw, &dx, &dw);
    eq_button(sx, sw, txt(S_EQ_SAVE), app_eq_edited(), true, BTN_EQ_SAVE);
    eq_button(dx, dw, txt(S_EQ_DELETE), app.set.eq_profile > 0, false, BTN_EQ_DELETE);
    if (app.eq_ask)
        eq_ask_box();
    return false;
}

// Saving a new profile: its name on the search keyboard (the whole bottom screen).
static bool draw_eq_name(void)
{
    gfx_fill(B, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    gfx_fill(B, 0, 0, SCREEN_W, 20, C_PANEL);
    text(B, 8, 2, txt(S_EQ_NEW), C_TEXT);
    text(B, 236, 2, "×", C_TEXT);
    const char *nm = app.eq_name;
    int y = FIELD_Y;
    round_box(B, 4, y, 248, 20, 4, C_ZEBRA, C_ACC);
    int right = nm[0] ? 228 : 246;
    int tw = font_text_width(nm);
    int x = 10 + (tw > right - 16 ? right - 16 - tw : 0);     // long: its end shows
    gfx_clip(B, 6, y, right, y + 20);
    if (nm[0])
        text(B, x, y + 2, nm, C_TEXT);
    else
        text(B, 14, y + 2, txt(S_EQ_NAME), C_FAINT);
    if ((app.frame / 15) % 2 == 0)
        gfx_fill(B, (nm[0] ? x + tw : 10) + 1, y + 3, 2, 14, C_ACC);
    gfx_unclip(B);
    if (nm[0])
        text(B, 236, y + 2, "×", C_DIM);
    // what gets saved
    char sum[96] = "", v[8];
    for (int b = 0; b < EQ_BANDS; b++)
    {
        db_label(v, sizeof(v), app.set.eq_gain[b]);
        snprintf(sum + strlen(sum), sizeof(sum) - strlen(sum), "%s%s %s", b ? "  " : "", txt(S_EQ_BASS + b), v);
    }
    font_draw(B, 8, CHIPS_Y + 1, 240, sum, C_DIM);
    draw_keys(KB_Y, true);
    return true;    // the cursor blinks
}

static void toggle(int x, int y, bool on)
{
    gfx_round_fill(B, x, y, 28, 14, 7, on ? C_ACC : C_LINE);
    gfx_disc(B, on ? x + 20 : x + 7, y + 7, 5, on ? C_TEXT : C_DIM);
}

static const SetItem items_top[] = {
    { S_PLAYBACK, SI_PAGE, PAGE_PLAYBACK, ICON_PLAYBACK },
    { S_BROWSING, SI_PAGE, PAGE_BROWSING, ICON_BROWSING },
    { S_PLAYER, SI_PAGE, PAGE_PLAYER, ICON_PLAYER },
    { S_THEME, SI_PAGE, PAGE_THEME, ICON_THEME },
    { S_REGIONAL, SI_PAGE, PAGE_REGIONAL, ICON_REGIONAL },
    { S_ABOUT, SI_PAGE, PAGE_ABOUT, ICON_ABOUT },
};
static const SetItem items_playback[] = {
    { S_REPLAYGAIN, SI_VALUE, SET_RG, 0 },
    { S_PLAY_AT_START, SI_TOGGLE, SET_AUTOPLAY, 0 },
    { S_PAUSE_LID, SI_TOGGLE, SET_PAUSE_LID, 0 },
    { S_LR_LID, SI_TOGGLE, SET_LR_LID, 0 },
};
static const SetItem items_browsing[] = {
    { S_CASE, SI_TOGGLE, SET_CASE, 0 },
    { S_SCROLL_ALL, SI_TOGGLE, SET_SCROLL_ALL, 0 },
};
static const SetItem items_player[] = {
    { S_TIME_RIGHT, SI_VALUE, SET_TIME_RIGHT, 0 },
    { S_COVER_BG, SI_TOGGLE, SET_COVER_BG, 0 },
    { S_SHUFFLE_W, SI_TOGGLE, SET_SHUFFLE_W, 0 },
};
static const SetItem items_regional[] = {
    { S_LANGUAGE, SI_VALUE, SET_LANG, ICON_REGIONAL },
    { S_KEYBOARD, SI_VALUE, SET_KEYBOARD, ICON_KEYBOARD },
};
#define N_ITEMS(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const struct {
    uint16_t title;
    const SetItem *items;
    int n;
    uint16_t later;         // not there yet: what it will hold (S_NONE: it's there)
} pages[PAGE_COUNT] = {
    [PAGE_TOP] = { S_SETTINGS, items_top, N_ITEMS(items_top), S_NONE },
    [PAGE_PLAYBACK] = { S_PLAYBACK, items_playback, N_ITEMS(items_playback), S_NONE },
    [PAGE_BROWSING] = { S_BROWSING, items_browsing, N_ITEMS(items_browsing), S_NONE },
    [PAGE_PLAYER] = { S_PLAYER, items_player, N_ITEMS(items_player), S_NONE },
    [PAGE_THEME] = { S_THEME, NULL, 0, S_NONE },
    [PAGE_REGIONAL] = { S_REGIONAL, items_regional, N_ITEMS(items_regional), S_NONE },
    [PAGE_ABOUT] = { S_ABOUT, NULL, 0, S_NONE },
};

int ui_set_count(int page)
{
    return page >= 0 && page < PAGE_COUNT ? pages[page].n : 0;
}

const SetItem *ui_set_item(int page, int i)
{
    return &pages[page].items[i];
}

// A page shows the icon of its row on the top page.
static uint32_t page_icon(int page)
{
    for (int i = 0; i < N_ITEMS(items_top); i++)
        if (items_top[i].id == page)
            return items_top[i].icon;
    return 0;
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
    case SET_AUTOPLAY: return app.set.play_at_start;
    default: return false;
    }
}

static const char *set_value(int id)
{
    static const uint8_t rg[RG_MODES] = { S_OFF, S_RG_TRACK, S_RG_ALBUM, S_AUTO };
    switch (id)
    {
    case SET_RG: return txt(rg[app.set.rg_mode]);
    case SET_TIME_RIGHT: return txt(app.set.show_remaining ? S_LEFT : S_LENGTH);
    case SET_LANG: return lang_name(app.set.lang);
    case SET_KEYBOARD: return app.set.keyboard == KB_QWERTZ ? "QWERTZ" : "QWERTY";
    default: return "";
    }
}

static void centered_at(int cx, int y, const char *s, uint16_t color, int scale)
{
    font_draw_ex(B, cx - font_text_width(s) * scale / 2, y, 0, SCREEN_W, s, color, -1, scale);
}

static void about_body(int dx)
{
    int cx = dx + SCREEN_W / 2;
    centered_at(cx, 26, APP_NAME, C_ACC, 2);
    char version[48];
    snprintf(version, sizeof(version), "Version %s · %s", APP_VERSION, txt(S_EDITION));
    centered_at(cx, 60, version, C_DIM, 1);
    centered_at(cx, 84, txt(S_TAGLINE), C_TEXT, 1);
    centered_at(cx, 100, "© 2026 Alya (alya-can-eat)", C_TEXT, 1);
    centered_at(cx, 122, txt(S_LICENCE), C_TEXT, 1);
    centered_at(cx, 138, txt(S_FREE), C_DIM, 1);
    centered_at(cx, 158, APP_URL, C_ACC, 1);
    centered_at(cx, 174, txt(S_GLYPHS), C_DIM, 1);
}

// Black or white, whichever reads on c.
static uint16_t ink_on(uint16_t c)
{
    int y = (c & 31) * 3 + ((c >> 5) & 31) * 6 + ((c >> 10) & 31);     // brightness x 10
    return y > 160 ? COLOR(1, 1, 2) : COLOR(31, 31, 31);
}

// The colours to choose, Reset, and the colour triangle.
static bool theme_is(const Theme *a, const Theme *b)
{
    return ((a->bg ^ b->bg) & 0x7FFF) == 0 && ((a->text ^ b->text) & 0x7FFF) == 0 && ((a->acc ^ b->acc) & 0x7FFF) == 0;
}

// The preset themes, rising out of their button; fixed colours, each with
// its three colours, the one in use marked.
static void presets_list(int dx)
{
    int p = ease_out(clamp256(app.presets_t * 256 / OV_TICKS));
    int x0 = dx + PRESET_X0, x1 = dx + PRESET_X1, h = THEME_PRESETS_Y - 4 - PRESET_Y;
    int y0 = PRESET_Y + h * (256 - p) / 256;
    gfx_clip(B, x0 < 0 ? 0 : x0, PRESET_Y, x1, THEME_PRESETS_Y - 4);
    gfx_fill(B, x0, y0, x1 - x0, h, COLOR(5, 5, 6));
    gfx_rect(B, x0, y0, x1 - x0, h, COLOR(20, 20, 22));
    text(B, x0 + 8, y0 + 2, txt(S_PRESETS), COLOR(18, 18, 20));
    if (app.menu_sel >= 0)
        gfx_fill(B, x0 + 1, y0 + 20 + app.menu_vis, x1 - x0 - 2, 16, COLOR(11, 11, 13));
    for (int i = 0; i < THEME_PRESETS; i++)
    {
        const Theme *t = &theme_presets[i].theme;
        int y = y0 + 20 + i * 16;
        const uint16_t sw[3] = { t->bg, t->text, t->acc };
        for (int k = 0; k < 3; k++)
        {
            gfx_fill(B, x0 + 8 + k * 11, y + 3, 10, 10, COLOR(16, 16, 18));
            gfx_fill(B, x0 + 9 + k * 11, y + 4, 8, 8, sw[k] | 0x8000);
        }
        text(B, x0 + 46, y, txt(theme_presets[i].name), COLOR(31, 31, 31));
        if (theme_is(t, &app.theme))
            text_r(B, x1 - 8, y, "•", COLOR(31, 31, 31));
    }
    gfx_clip(B, dx < 0 ? 0 : dx, 0, SCREEN_W, SCREEN_H);
}

static void theme_body(int dx)
{
    static const uint8_t names[3] = { S_ACCENT, S_BACKGROUND, S_TEXT };
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
        const char *s = txt(names[i]);
        text(B, x + (THEME_CHIP_W - font_text_width(s)) / 2, y + 3, s, ink_on(c));
    }
    // Presets in fixed colours: findable whatever the theme, the way back
    // from one that went wrong
    int x = dx + THEME_CHIP_X;
    bool open = app.presets_open;
    gfx_round_fill(B, x, THEME_PRESETS_Y, THEME_CHIP_W, THEME_CHIP_H, 4, open ? COLOR(31, 31, 31) : COLOR(9, 9, 10));
    const char *ps = txt(S_PRESETS);
    text(B, x + (THEME_CHIP_W - font_text_width(ps)) / 2, THEME_PRESETS_Y + 3, ps,
         open ? COLOR(4, 4, 5) : COLOR(31, 31, 31));
    cp_draw(B, dx, app.theme_hsv);
    if (app.presets_t > 0)
        presets_list(dx);
}

// A settings page with its left edge at dx (pages slide in from the right).
// lit: the D-pad highlight is on this page.
static bool settings_page(int page, int dx, bool lit)
{
    gfx_clip(B, dx, 0, SCREEN_W, SCREEN_H);
    gfx_fill(B, dx, 0, SCREEN_W, SCREEN_H, C_BG);
    gfx_fill(B, dx, 0, SCREEN_W, 20, C_PANEL);
    if (page == PAGE_TOP)
        text(B, dx + 8, 2, txt(pages[page].title), C_TEXT);
    else
    {
        text(B, dx + 6, 2, "‹", C_TEXT);
        icon(B, dx + 18, 2, page_icon(page), C_ACC);
        text(B, dx + 38, 2, txt(pages[page].title), C_TEXT);
    }
    text(B, dx + 236, 2, "×", C_TEXT);
    bool anim = false;
    if (page == PAGE_ABOUT)
        about_body(dx);
    else if (page == PAGE_THEME)
        theme_body(dx);
    else if (pages[page].later)
    {
        centered_at(dx + SCREEN_W / 2, 76, txt(pages[page].later), C_TEXT, 1);
        centered_at(dx + SCREEN_W / 2, 96, txt(S_LATER_PAGE), C_DIM, 1);
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
            int lx = 8;
            if (it->icon)
            {
                icon(B, dx + 8, y + ty, it->icon, C_ACC);
                lx = 30;
            }
            int w = (value ? 232 - font_text_width(value) - 8 : 204) - lx;
            anim |= marquee1(B, dx + lx, y + ty, w, txt(it->label), C_TEXT, app.frame, false);
            if (it->kind == SI_TOGGLE)
                toggle(dx + 216, y + gy, set_on(it->id));
            else if (value)
                text_r(B, dx + 244, y + ty, value, C_ACC);
            else
            {
                text_r(B, dx + 244, y + ty, "›", C_DIM);
                if (it->kind == SI_PAGE && pages[it->id].later)
                    text_r(B, dx + 232, y + ty, txt(S_LATER), C_FAINT);
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
static bool w_eq(int arg) { (void)arg; return draw_eq(); }
static bool w_eq_name(int arg) { (void)arg; return draw_eq_name(); }

// The panel over the player area: song info, the queue or the equalizer.
static bool (*panel_fn(Overlay ov))(int)
{
    return ov == OV_INFO ? w_info : (ov == OV_QUEUE ? w_queue : (ov == OV_EQ ? w_eq : NULL));
}

bool ui_draw_bottom(void)
{
    B = gfx_canvas(SCREEN_BOTTOM);
    gfx_unclip(B);
    int p = ov_progress();
    if (app.ov_vis == OV_SETTINGS && p >= 256)
        return draw_settings();
    if (app.ov_vis == OV_EQ_NAME && p >= 256)
        return draw_eq_name();

    int oy = app_panel_offset();
    bool anim = (app.slide > 0 && app.slide < SLIDE_TICKS) || app.press_down || app.frame < app.press_until
                || (app.pp_t > 0 && app.pp_t < PP_TICKS) || (app.knob_t > 0 && app.knob_t < KNOB_TICKS);
    if (oy > 0)
    {
        bool (*panel)(int) = panel_fn(app.ov_vis);
        if (panel && p >= 256)
            anim |= panel(0);
        else
        {
            anim |= draw_player_area();
            if (panel)
            {
                // rises from behind the tabs
                shifted(panel, 0, 0, 0, SCREEN_W, PANEL_DOWN, PANEL_DOWN * (256 - p) / 256, PANEL_DOWN);
                anim = true;
            }
        }
    }
    anim |= draw_panel(oy);
    if (app.ov_vis == OV_SETTINGS || app.ov_vis == OV_EQ_NAME)
    {
        shifted(app.ov_vis == OV_SETTINGS ? w_settings : w_eq_name, 0, 0, 0, SCREEN_W, SCREEN_H,
                SCREEN_H * (256 - p) / 256, SCREEN_H);
        anim = true;
    }
    return anim;
}
