// SPDX-License-Identifier: MIT
//
// Drawing of both screens, following the mockup pixel for pixel: 256x192 per
// screen, Unifont 16 px, BGR555 colors.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "font.h"
#include "player.h"
#include "queue.h"
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

static void mmss(char *buf, int size, uint32_t ms)
{
    format_time(buf, size, ms);
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
    for (int k = 0; k < 2; k++)
    {
        int pen = x - off + k * period;
        for (int i = 0; i < n; i++)
            pen += font_draw_ex(c, pen, y, x, x + w, s[i], col[i], jp, 1);
    }
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
    return app.slide * 256 / SLIDE_FRAMES;
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
    { "Name  0→9 A→Z", "Name  Z→A 9→0", "Most songs", NULL },
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

static const uint16_t *placeholder_cover(void)
{
    static uint16_t px[128 * 128];
    static bool made;
    if (!made)
    {
        for (int i = 0; i < 128 * 128; i++)
            px[i] = C_PANEL;
        made = true;
    }
    return px;
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

static bool top_text_column(int x0, const SongText *st, uint32_t t)
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
    anim |= marquee1(T, x0, 24, w, st->title, C_TEXT, t, false);
    anim |= marquee1(T, x0, 44, w, st->artist, C_ACC, t, false);
    anim |= marquee1(T, x0, 64, w, st->album, C_DIM, t, false);
    font_draw_ex(T, x0, 84, x0, x0 + w, st->extra, C_DIM, -1, 1);
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
    uint32_t pos = player_position_ms(), dur = player_duration_ms();
    int fill = dur ? (int)((uint64_t)240 * pos / dur) : 0;
    gfx_fill(T, 8, y, 240, 4, C_LINE);
    gfx_fill(T, 8, y, fill, 4, C_ACC);
    char s[16];
    mmss(s, sizeof(s), pos);
    text(T, 8, y + 8, s, C_TEXT);
    s[0] = '-';
    mmss(s + 1, sizeof(s) - 1, dur > pos ? dur - pos : 0);
    text_r(T, 248, y + 8, s, C_DIM);
}

// ---- the record: per-pixel distance and angle around its centre, computed once

#define VR 60
#define VD (2 * VR + 1)
static uint8_t *v_dist;
static uint16_t *v_angle;

static void vinyl_tables(void)
{
    if (v_dist)
        return;
    v_dist = malloc(VD * VD);
    v_angle = malloc(VD * VD * sizeof(uint16_t));
    if (!v_dist || !v_angle)
        return;
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
static void draw_vinyl(int cx, int cy, int clip_x1, int rot)
{
    vinyl_tables();
    if (!v_dist)
        return;
    const uint16_t c_vinyl = COLOR(3, 3, 4), c_edge = COLOR(8, 8, 10), c_groove = COLOR(5, 5, 7);
    const uint16_t c_mark = COLOR(13, 13, 16);
    for (int j = 0; j < VD; j++)
    {
        int y = cy - VR + j;
        if (y < T->cy0 || y >= T->cy1)
            continue;
        uint16_t *line = T->px + y * SCREEN_W;
        int i0 = cx - VR < 0 ? VR - cx : 0;
        int i1 = clip_x1 - (cx - VR);
        if (i1 > VD) i1 = VD;
        if (i1 > SCREEN_W - (cx - VR)) i1 = SCREEN_W - (cx - VR);
        for (int i = i0; i < i1; i++)
        {
            int x = cx - VR + i;
            int d = v_dist[j * VD + i];
            if (d > VR)
                continue;
            uint16_t col = c_vinyl;
            if (d == VR)
                col = c_edge;
            else if (d <= 20)
                col = C_ACC_DIM;
            else if (d == 52 || d == 42 || d == 32)
                col = c_groove;
            for (int m = 0; m < 5; m++)
            {
                if (d != marks[m].r)
                    continue;
                int a = (v_angle[j * VD + i] - marks[m].a - rot + 720) % 360;
                if (a <= marks[m].len)
                    col = c_mark;
            }
            line[x] = col;
        }
    }
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
    font_draw_ex(T, x, 24, x, SCREEN_W, "Next", C_DIM, 0, 1);
    int np = queue_next_pos(true);
    if (np < 0 || !lib_loaded())
    {
        font_draw_ex(T, x, 80, x, x + w, "–", C_DIM, 0, 1);
        return false;
    }
    uint32_t nt = queue_track(np);
    if (app.has_thumb && app.thumb_track == nt)
        gfx_blit(T, x + 8, 44, 32, 32, app.thumb);
    else
        gfx_fill(T, x + 8, 44, 32, 32, C_PANEL);
    char title[200];
    track_title(nt, title, sizeof(title));
    const LibTrack *tr = lib_track(nt);
    bool anim = marquee1(T, x, 80, w, title, C_TEXT, t, false);
    anim |= marquee1(T, x, 96, w, tr->artist != LIB_NONE ? lib_value(tr->artist) : "", C_DIM, t, false);
    return anim;
}

bool ui_draw_top(void)
{
    T = gfx_canvas(SCREEN_TOP);
    gfx_unclip(T);
    gfx_fill(T, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    header();

    uint32_t t = app.frame;
    int p = app_slide_p256();
    int p1 = smooth256(p * 256 / 154);
    int p2 = smooth256((p - 102) * 256 / 154);
    bool playing = player_state() == PLAYER_PLAYING;
    bool anim = playing || (app.slide > 0 && app.slide < SLIDE_FRAMES);

    SongText st;
    song_text(app_current_track(), &st);
    const uint16_t *cover = player_cover();
    int cover_x = 8 + 56 * p1 / 256;

    if (p2 > 0)
    {
        if (playing)
            viz_update();
        draw_viz(p2);
        int rot = (int)((app.frame * 20 / 3) % 360);    // 33 1/3 rpm at 30 fps
        draw_vinyl(cover_x + 24 + 40 * (256 - p2) / 256, 88, cover_x, rot);
    }
    gfx_blit(T, cover_x, 24, 128, 128, cover ? cover : placeholder_cover());
    if (!cover)
        icon(T, cover_x + 56, 80, 0x266A, C_FAINT);

    anim |= top_text_column(144 + 112 * p1 / 256, &st, t);
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

static void draw_tabs(int oy)
{
    gfx_fill(B, 0, oy, SCREEN_W, TAB_H, C_PANEL);
    static const char *const names[TAB_COUNT] = { "Artist", "Album", "Title", NULL, NULL };
    for (int i = 0; i < TAB_COUNT; i++)
    {
        int x = tab_x[i], w = tab_x[i + 1] - x;
        bool act = (int)app.tab == i;
        if (act)
        {
            gfx_fill(B, x, oy, w, TAB_H, C_BG);
            gfx_fill(B, x, oy, w, 2, C_ACC);
        }
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
            font_draw(B, x + 4, oy + 2, w - 18, label, act ? C_TEXT : C_ACC);
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
        gfx_fill(B, 0, y, SCROLL_X, ROW_H, sel ? C_SEL : (r % 2 ? C_ZEBRA : C_BG));
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

static void pill(int cx, int y, uint32_t cp)
{
    gfx_round_fill(B, cx - 20, y + 4, 40, 20, 6, C_LINE);
    icon(B, cx - 8, y + 6, cp, C_TEXT);
}

static void draw_bar(int oy)
{
    int y = oy + BAR_Y;
    gfx_fill(B, 0, y, SCREEN_W, 28, C_PANEL);
    gfx_fill(B, 0, y, SCREEN_W, 1, C_LINE);
    icon(B, 8, y + 6, player_state() == PLAYER_PLAYING ? 0x23F8 : 0x25B6, C_TEXT);
    icon(B, 40, y + 6, 0x23ED, C_TEXT);
    if (app.add_mode)
        gfx_round_fill(B, 66, y + 4, 28, 20, 6, C_ACC);
    icon(B, 72, y + 6, 0xE000, app.add_mode ? C_BG : C_TEXT);
    pill(128, y, 0x2B9F);
    bool sort_open = app.overlay == OV_SORT;
    if (sort_open)
        gfx_fill(B, 160, y + 1, 32, 27, C_BG);
    icon(B, 168, y + 6, 0x21C5, sort_open ? C_ACC : C_TEXT);
    icon(B, 200, y + 6, 0x1F506, C_TEXT);
    icon(B, 232, y + 6, 0x22EE, C_TEXT);
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
    for (int i = 0; i < n; i++)
    {
        int y = y0 + 20 + i * 16;
        bool cur = app.set.sort[slot] == i;
        if (cur || i == app.menu_sel)
            gfx_fill(B, x0 + 1, y, x1 - x0 - 2, 16, i == app.menu_sel ? C_SEL : C_PANEL);
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
    for (int i = 0; i < 3; i++)
    {
        int y = y0 + 20 + i * 16;
        if (i == app.menu_sel)
            gfx_fill(B, x0 + 1, y, x1 - x0 - 2, 16, C_SEL);
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

static bool draw_panel(int oy)
{
    gfx_clip(B, 0, oy, SCREEN_W, SCREEN_H);
    draw_tabs(oy);
    bool anim = false;
    if (oy < SCREEN_H - TAB_H)
    {
        anim |= draw_list(oy);
        draw_bar(oy);
        if (app.overlay == OV_SORT)
            draw_sort_menu(oy);
        if (app.overlay == OV_ROWMENU)
            draw_row_menu(oy);
        anim |= draw_toast(oy);
    }
    gfx_unclip(B);
    return anim;
}

// ------------------------------------------------------------ bottom: player area

static void big_next(int cx, int cy, uint16_t c, bool flip)
{
    int s = flip ? -1 : 1;
    gfx_triangle(B, cx - 10 * s, cy - 9, cx - 10 * s, cy + 9, cx - 1 * s, cy, c);
    gfx_triangle(B, cx - 1 * s, cy - 9, cx - 1 * s, cy + 9, cx + 8 * s, cy, c);
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
    uint32_t pos = app.seek_drag ? app.seek_preview_ms : player_position_ms();
    int y = 40;
    gfx_fill(B, 16, y, 224, 4, C_LINE);
    int kx = 16 + (dur ? (int)((uint64_t)224 * pos / dur) : 0);
    gfx_fill(B, 16, y, kx - 16, 4, C_ACC);
    if (player_path() != NULL)
        gfx_disc(B, kx, y + 2, 5, C_TEXT);
    char s[16];
    mmss(s, sizeof(s), pos);
    text(B, 16, y + 12, s, C_TEXT);
    s[0] = '-';
    mmss(s + 1, sizeof(s) - 1, dur > pos ? dur - pos : 0);
    text_r(B, 240, y + 12, s, C_DIM);

    int cy = 100;
    bool sh = queue_shuffle();
    icon(B, 20, cy - 8, 0x1F500, sh ? C_ACC : C_DIM);
    if (sh)
        gfx_fill(B, 26, cy + 10, 4, 2, C_ACC);
    big_next(78, cy, C_TEXT, true);
    gfx_disc(B, 128, cy, 24, C_ACC);
    if (player_state() == PLAYER_PLAYING)
    {
        gfx_fill(B, 120, cy - 9, 6, 19, C_BG);
        gfx_fill(B, 131, cy - 9, 6, 19, C_BG);
    }
    else
        gfx_triangle(B, 122, cy - 10, 122, cy + 10, 138, cy, C_BG);
    big_next(178, cy, C_TEXT, false);
    int rep = queue_repeat();
    icon(B, 220, cy - 8, rep == REPEAT_ONE ? 0x1F502 : 0x1F501, rep ? C_ACC : C_DIM);

    icon(B, 12, 140, 0x24D8, C_TEXT);
    icon(B, 228, 140, 0x2630, C_TEXT);
    pill(128, 140, 0x2B9D);
    return anim || player_state() == PLAYER_PLAYING;
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
    char track[24] = "–", year[8] = "–", rating[32] = "–", format[64];
    if (tr->track && tr->album != LIB_NONE)
        snprintf(track, sizeof(track), "%d / %d", tr->track, lib_album(tr->album)->count);
    else if (tr->track)
        snprintf(track, sizeof(track), "%d", tr->track);
    if (tr->year)
        snprintf(year, sizeof(year), "%d", tr->year);
    const char *r = lib_tag(t, "~#rating");
    if (r)
    {
        int stars = (int)(atof(r) * 5 + 0.5);
        int n = 0;
        for (int i = 0; i < 5; i++)
            n += utf8_put(rating + n, i < stars ? 0x2605 : 0x2606);
    }
    const char *genre = lib_tag(t, "genre"), *plays = lib_tag(t, "~#playcount");
    const char *codec = lib_tag(t, "~codec");
    const DsaHeader *h = player_header();
    unsigned rate = h ? h->sound_clock_hz / h->timer_divider : 32728;
    snprintf(format, sizeof(format), "%s%sDSA %u.%u kHz", codec ? codec : "", codec ? " → " : "",
             rate / 1000, (rate % 1000) / 100);
    const char *labels[9] = { "Title", "Artist", "Album", "Track", "Year", "Genre", "Rating", "Plays", "Format" };
    const char *vals[9] = { st.title, st.artist[0] ? st.artist : "–", st.album[0] ? st.album : "–",
                            track, year, genre ? genre : "–", rating, plays ? plays : "–", format };
    bool anim = false;
    for (int i = 0; i < 9; i++)
    {
        int y = 24 + i * 16;
        text(B, 8, y, labels[i], C_DIM);
        anim |= marquee1(B, 72, y, 176, vals[i], C_TEXT, app.frame, false);
    }
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
        gfx_fill(B, 0, y, SCROLL_X, ROW_H, cur ? C_SEL : (i == app.q_sel ? C_LINE : (r % 2 ? C_ZEBRA : C_BG)));
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

static bool draw_settings(void)
{
    gfx_fill(B, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    gfx_fill(B, 0, 0, SCREEN_W, 20, C_PANEL);
    text(B, 8, 2, "Settings", C_TEXT);
    text(B, 236, 2, "×", C_TEXT);
    static const char *const labels[5] = { "Scroll all long rows", "Case-sensitive names",
                                           "Pause when lid closes", "L/R skip, lid closed", "ReplayGain" };
    static const char *const rg[RG_MODES] = { "Off", "Track", "Album", "Auto" };
    const uint8_t on[4] = { app.set.scroll_all, app.set.case_sensitive, app.set.pause_on_lid, app.set.lr_skip_lid };
    bool anim = false;
    for (int i = 0; i < 5; i++)
    {
        int y = 24 + i * 28;
        if (i == app.menu_sel)
            gfx_fill(B, 0, y, SCREEN_W, 27, C_SEL);
        anim |= marquee1(B, 8, y + 6, 196, labels[i], C_TEXT, app.frame, false);
        if (i < 4)
            toggle(216, y + 7, on[i]);
        else
            text_r(B, 244, y + 6, rg[app.set.rg_mode], C_ACC);
        gfx_fill(B, 8, y + 27, 240, 1, C_ZEBRA);
    }
    return anim;
}

bool ui_draw_bottom(void)
{
    B = gfx_canvas(SCREEN_BOTTOM);
    gfx_unclip(B);
    if (app.overlay == OV_SETTINGS)
        return draw_settings();

    int oy = app_panel_offset();
    bool anim = app.slide > 0 && app.slide < SLIDE_FRAMES;
    if (oy > 0)
    {
        if (app.overlay == OV_INFO)
            anim |= draw_info();
        else if (app.overlay == OV_QUEUE)
            anim |= draw_queue();
        else
            anim |= draw_player_area();
    }
    anim |= draw_panel(oy);
    return anim;
}
