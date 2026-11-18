// SPDX-License-Identifier: MIT
//
// dsplayer: browse the library by artist, album, title or folder on the
// bottom screen, see what plays on the top screen.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fat.h>
#include <nds.h>

#include "app.h"
#include "font.h"
#include "player.h"
#include "queue.h"
#include "util.h"

#define TAP_SLOP        6       // pixels a tap may move
#define HOLD_TICKS      30      // 0.5 s at 60 Hz: hold for the row menu / to lift a queue row
#define TOAST_FRAMES    60      // 2 s at 30 fps
#define SAVE_DELAY      120     // ticks after a change before the state is written
#define SAVE_EVERY      (30 * 60)

App app;

static char music_root[64];
static int item_cap;
static uint32_t *tracks_buf;
static int tracks_cap;
static uint32_t ticks;              // vblanks (60 per second)
static bool redraw = true;
static uint32_t save_at, last_save;
static bool lid_closed, paused_by_lid;

// ------------------------------------------------------------------ helpers

__attribute__((format(printf, 1, 2)))
static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app.toast, sizeof(app.toast), fmt, ap);
    va_end(ap);
    utf8_trim_partial(app.toast);
    app.toast_until = app.frame + TOAST_FRAMES;
    redraw = true;
}

static void touch_state(void)
{
    save_at = ticks + SAVE_DELAY;
}

static void full_path(char *out, int size, uint32_t track)
{
    snprintf(out, size, "%s/%s", music_root, lib_path(track));
}

static void select_index(int i)
{
    if (i >= app.n_items) i = app.n_items - 1;
    if (i < 0) i = 0;
    if (i != app.sel)
        app.sel_frame = app.frame;
    app.sel = i;
    if (app.sel < app.scroll)
        app.scroll = app.sel;
    if (app.sel >= app.scroll + ROWS)
        app.scroll = app.sel - ROWS + 1;
    int max_scroll = app.n_items > ROWS ? app.n_items - ROWS : 0;
    if (app.scroll > max_scroll) app.scroll = max_scroll;
    if (app.scroll < 0) app.scroll = 0;
    redraw = true;
}

static void rebuild_list(void)
{
    app.n_items = 0;
    if (!lib_loaded())
        return;
    int slot = ui_sort_slot();
    int sort = app.set.sort[slot];
    switch (app.tab)
    {
    case TAB_ARTIST:
        app.n_items = lib_list_artists(app.items, sort);
        break;
    case TAB_ALBUM:
        app.n_items = lib_list_albums(app.items, app.artist, sort);
        break;
    case TAB_TITLE:
        app.n_items = lib_list_tracks(app.items, app.artist, app.album, sort, app.album != LIB_NONE);
        break;
    case TAB_FOLDER:
    {
        int n = 0;
        if (app.folder[0])
            app.items[n++] = (Item){ ITEM_UP, 0 };
        n += lib_list_folder(app.items + n, app.folder, sort);
        app.n_items = n;
        break;
    }
    default:
        break;
    }
}

// Select the row showing `want`, if it is in the list.
static bool select_item(ItemKind kind, uint32_t id)
{
    for (int i = 0; i < app.n_items; i++)
    {
        if (app.items[i].kind == kind && app.items[i].id == id)
        {
            app.scroll = i - ROWS / 2;
            select_index(i);
            return true;
        }
    }
    return false;
}

static void show_tab(Tab t)
{
    app.tab = t;
    app.overlay = OV_NONE;
    app.sel = app.scroll = 0;
    app.sel_frame = app.frame;
    rebuild_list();
    uint32_t cur = app_current_track();
    bool found = false;
    if (t == TAB_ARTIST && app.artist >= 0)
        found = select_item(ITEM_ARTIST, app.artist);
    else if (t == TAB_ALBUM && app.album != LIB_NONE)
        found = select_item(ITEM_ALBUM, app.album);
    if (!found && cur != LIB_NONE)
        found = select_item(ITEM_TRACK, cur);
    if (!found && t == TAB_FOLDER && cur != LIB_NONE)
    {
        // the folder the playing song is in
        const char *cp = lib_path(cur);
        size_t dl = strlen(app.folder);
        for (int i = 0; i < app.n_items && !found; i++)
        {
            if (app.items[i].kind != ITEM_FOLDER)
                continue;
            char name[256];
            lib_folder_name(&app.items[i], app.folder, name, sizeof(name));
            const char *rest = dl ? cp + dl + 1 : cp;
            size_t nl = strlen(name);
            if ((!dl || (strncmp(cp, app.folder, dl) == 0 && cp[dl] == '/'))
                && strncmp(rest, name, nl) == 0 && rest[nl] == '/')
            {
                app.scroll = i - ROWS / 2;
                select_index(i);
                found = true;
            }
        }
    }
    if (!found && app.n_items > 1 && app.items[0].kind == ITEM_UP)
        select_index(1);
    redraw = true;
}

static void slide_to(bool player)
{
    app.player_mode = player;
    app.overlay = OV_NONE;
    redraw = true;
}

// ------------------------------------------------------------------ playback

static uint8_t replaygain_volume(void)
{
    const DsaHeader *h = player_header();
    if (h == NULL || app.set.rg_mode == RG_OFF)
        return 127;
    bool album = app.set.rg_mode == RG_ALBUM || (app.set.rg_mode == RG_AUTO && app.album_queue);
    int32_t mdb;
    if (album && (h->flags & DSA_FLAG_RG_ALBUM))
        mdb = h->rg_album_gain_mdb;
    else if (h->flags & DSA_FLAG_RG_TRACK)
        mdb = h->rg_track_gain_mdb;
    else
        return 127;
    // The hardware volume only turns down; louder than the file is not possible.
    float v = 127.0f * powf(10.0f, mdb / 20000.0f);
    return v >= 127.0f ? 127 : (v <= 0 ? 0 : (uint8_t)(v + 0.5f));
}

// Open the song at queue position pos. Skips songs that can't be opened.
static bool open_pos(int pos, uint32_t start_ms, bool play)
{
    for (int tries = 0; tries < queue_count() && pos >= 0; tries++)
    {
        queue_jump(pos);
        char path[600], err[96];
        full_path(path, sizeof(path), queue_track(pos));
        if (player_open(path, start_ms, false, err, sizeof(err)))
        {
            player_set_volume(replaygain_volume());
            if (play)
                player_toggle_pause();
            touch_state();
            redraw = true;
            return true;
        }
        toast("Can't play %s: %s", path_basename(path), err);
        start_ms = 0;
        pos = queue_next_pos(false);
    }
    player_stop();
    return false;
}

static void play_tracks(const uint32_t *t, int n, int start, bool album_queue)
{
    if (n <= 0 || !queue_set(t, n, start))
        return;
    app.album_queue = album_queue;
    open_pos(queue_pos(), 0, true);
}

static void next_song(bool automatic)
{
    int p = queue_next_pos(automatic);
    if (p < 0)
    {
        // end of the queue: stay on the last song, paused at its start
        if (queue_pos() >= 0)
            open_pos(queue_pos(), 0, false);
        return;
    }
    open_pos(p, 0, true);
}

static void previous_song(void)
{
    // like most players: first back to the start of this song
    if (player_position_ms() > 3000 || queue_count() == 0)
    {
        player_restart();
        redraw = true;
        return;
    }
    open_pos(queue_prev_pos(), 0, player_state() == PLAYER_PLAYING);
}

static void toggle_pause(void)
{
    if (player_state() == PLAYER_IDLE && queue_count() > 0)
        open_pos(queue_pos() < 0 ? 0 : queue_pos(), 0, true);
    else
        player_toggle_pause();
    touch_state();
    redraw = true;
}

static void update_thumb(void)
{
    int np = queue_next_pos(true);
    if (np < 0 || !lib_loaded())
        return;
    uint32_t t = queue_track(np);
    if (t == app.thumb_track)
        return;
    app.thumb_track = t;
    app.has_thumb = false;
    char path[600], err[64];
    full_path(path, sizeof(path), t);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return;
    DsaHeader h;
    if (dsa_read_header(f, &h, err, sizeof(err)))
        app.has_thumb = dsa_read_thumb(f, &h, app.thumb);
    fclose(f);
}

// ------------------------------------------------------------------ list actions

static void mark(uint32_t t) { app.added[t >> 3] |= 1 << (t & 7); }

static int item_tracks(const Item *it)
{
    return lib_item_tracks(it, app.folder, tracks_buf, tracks_cap);
}

static void add_to_queue(const Item *it, bool next)
{
    int n = item_tracks(it);
    if (n <= 0)
        return;
    bool was_empty = queue_count() == 0;
    if (next ? !queue_insert_next(tracks_buf, n) : !queue_append(tracks_buf, n))
    {
        toast("The queue is full.");
        return;
    }
    for (int i = 0; i < n; i++)
        mark(tracks_buf[i]);
    if (n == 1)
        toast("%s", next ? "Plays next" : "Added to the queue");
    else
        toast("%d %s", n, next ? "songs play next" : "added to the queue");
    if (was_empty)
        open_pos(0, 0, false);
    touch_state();
}

// The songs of the current list, for playing one of them.
static void play_from_list(int index)
{
    int n = 0, start = 0;
    for (int i = 0; i < app.n_items && n < tracks_cap; i++)
    {
        if (app.items[i].kind != ITEM_TRACK)
            continue;
        if (i == index)
            start = n;
        tracks_buf[n++] = app.items[i].id;
    }
    play_tracks(tracks_buf, n, start, app.tab == TAB_TITLE && app.album != LIB_NONE);
}

static void activate(int i)
{
    if (i < 0 || i >= app.n_items)
        return;
    select_index(i);
    const Item *it = &app.items[i];
    if (app.add_mode && it->kind != ITEM_UP)
    {
        add_to_queue(it, false);
        return;
    }
    switch (it->kind)
    {
    case ITEM_ARTIST:
        app.artist = it->id;
        app.album = LIB_NONE;
        show_tab(TAB_ALBUM);
        break;
    case ITEM_ALBUM:
        app.album = it->id;
        show_tab(TAB_TITLE);
        break;
    case ITEM_TRACK:
        play_from_list(i);
        break;
    case ITEM_FOLDER:
    {
        char name[256];
        lib_folder_name(it, app.folder, name, sizeof(name));
        size_t l = strlen(app.folder);
        snprintf(app.folder + l, sizeof(app.folder) - l, "%s%s", l ? "/" : "", name);
        show_tab(TAB_FOLDER);
        break;
    }
    case ITEM_UP:
    {
        char *slash = strrchr(app.folder, '/');
        char left[512];
        snprintf(left, sizeof(left), "%s", slash ? slash + 1 : app.folder);
        if (slash) *slash = 0; else app.folder[0] = 0;
        show_tab(TAB_FOLDER);
        // put the selection on the folder we came from
        for (int k = 0; k < app.n_items; k++)
        {
            char name[256];
            if (app.items[k].kind != ITEM_FOLDER)
                continue;
            lib_folder_name(&app.items[k], app.folder, name, sizeof(name));
            if (strcmp(name, left) == 0)
            {
                app.scroll = k - ROWS / 2;
                select_index(k);
                break;
            }
        }
        break;
    }
    }
}

static void go_back(void)
{
    if (app.tab == TAB_TITLE && app.album != LIB_NONE)
        show_tab(TAB_ALBUM);
    else if (app.tab == TAB_ALBUM && app.artist >= 0)
        show_tab(TAB_ARTIST);
    else if (app.tab == TAB_FOLDER && app.folder[0])
        activate(0);   // the ⮝ row
}

static void row_menu_choose(int option)
{
    int i = app.menu_item;
    app.overlay = OV_NONE;
    if (i < 0 || i >= app.n_items || app.items[i].kind == ITEM_UP)
        return;
    const Item *it = &app.items[i];
    if (option == 0)
    {
        if (it->kind == ITEM_TRACK)
            play_from_list(i);
        else
        {
            int n = item_tracks(it);
            play_tracks(tracks_buf, n, 0, it->kind == ITEM_ALBUM);
        }
    }
    else
        add_to_queue(it, option == 1);
    redraw = true;
}

static void open_row_menu(int i)
{
    if (i < 0 || i >= app.n_items || app.items[i].kind == ITEM_UP || !lib_loaded())
        return;
    select_index(i);
    app.menu_item = i;
    app.menu_sel = 0;
    app.overlay = OV_ROWMENU;
    redraw = true;
}

static void open_sort_menu(void)
{
    if (app.tab == TAB_SEARCH)
        return;
    app.overlay = app.overlay == OV_SORT ? OV_NONE : OV_SORT;
    app.menu_sel = app.set.sort[ui_sort_slot()];
    redraw = true;
}

static void choose_sort(int option)
{
    int slot = ui_sort_slot();
    app.overlay = OV_NONE;
    if (option < 0 || option >= ui_sort_option_count(slot))
        return;
    app.set.sort[slot] = option;
    uint32_t keep = app.sel < app.n_items ? app.items[app.sel].id : LIB_NONE;
    ItemKind kind = app.sel < app.n_items ? app.items[app.sel].kind : ITEM_TRACK;
    rebuild_list();
    if (!select_item(kind, keep))
        select_index(0);
    touch_state();
}

static void toggle_add_mode(void)
{
    app.add_mode = !app.add_mode;
    if (app.add_mode && app.added)
        memset(app.added, 0, (lib_track_count() + 7) / 8);
    redraw = true;
}

static void cycle_brightness(void)
{
    app.set.brightness = app.set.brightness >= 5 ? 2 : app.set.brightness + 1;
    systemSetBacklightLevel(app.set.brightness);
    touch_state();
}

static void settings_toggle(int row)
{
    switch (row)
    {
    case 0: app.set.scroll_all ^= 1; break;
    case 1:
        app.set.case_sensitive ^= 1;
        lib_group_artists(app.set.case_sensitive);
        app.artist = -1;    // group numbers changed
        rebuild_list();
        select_index(0);
        break;
    case 2: app.set.pause_on_lid ^= 1; break;
    case 3: app.set.lr_skip_lid ^= 1; break;
    case 4:
        app.set.rg_mode = (app.set.rg_mode + 1) % RG_MODES;
        player_set_volume(replaygain_volume());
        break;
    default: return;
    }
    touch_state();
    redraw = true;
}

// ------------------------------------------------------------------ touch

static struct {
    bool down;
    int x0, y0, x, y;
    uint32_t start;
    bool moved, held;
} tp;

static void tab_tap(int x)
{
    for (int i = 0; i < TAB_COUNT; i++)
    {
        if (x < tab_x[i] || x >= tab_x[i + 1])
            continue;
        bool has_filter = (i == TAB_ARTIST && app.artist >= 0) || (i == TAB_ALBUM && app.album != LIB_NONE);
        if (has_filter && x >= tab_x[i + 1] - 18)
        {
            // the × clears this filter
            if (i == TAB_ARTIST)
                app.artist = -1;
            else
                app.album = LIB_NONE;
            show_tab(app.tab);
            return;
        }
        show_tab((Tab)i);
        return;
    }
}

static void scroll_to_y(int y)
{
    int max_scroll = app.n_items > ROWS ? app.n_items - ROWS : 0;
    int h = ROWS * ROW_H;
    int s = (y - LIST_Y) * (max_scroll + 1) / h;
    if (s < 0) s = 0;
    if (s > max_scroll) s = max_scroll;
    app.scroll = s;
    if (app.sel < s) app.sel = s;
    if (app.sel >= s + ROWS) app.sel = s + ROWS - 1;
    app.sel_frame = app.frame;
    redraw = true;
}

static uint32_t seek_ms_at(int x)
{
    uint32_t dur = player_duration_ms();
    if (x < 16) x = 16;
    if (x > 240) x = 240;
    return (uint32_t)((uint64_t)dur * (x - 16) / 224);
}

static int queue_row_at(int y)
{
    return app.q_scroll + (y - 20) / ROW_H;
}

static void tap_browser(int x, int y)
{
    if (app.overlay == OV_SORT)
    {
        int n = ui_sort_option_count(ui_sort_slot());
        int h = 20 + 16 * n + 4, y0 = BAR_Y - h;
        if (x >= 96 && x < 248 && y >= y0 + 20 && y < y0 + 20 + 16 * n)
            choose_sort((y - y0 - 20) / 16);
        else
            app.overlay = OV_NONE;  // a tap anywhere else closes the menu
        redraw = true;
        return;
    }
    if (app.overlay == OV_ROWMENU)
    {
        int row_y = LIST_Y + (app.menu_item - app.scroll) * ROW_H;
        int y0 = ui_rowmenu_y(row_y);
        if (x >= 88 && x < 240 && y >= y0 + 20 && y < y0 + 20 + 48)
            row_menu_choose((y - y0 - 20) / 16);
        else
            app.overlay = OV_NONE;
        redraw = true;
        return;
    }
    if (y < TAB_H)
        tab_tap(x);
    else if (y < BAR_Y && x < SCROLL_X && lib_loaded())
        activate(app.scroll + (y - LIST_Y) / ROW_H);
    else if (y >= BAR_Y)
    {
        if (x < 32) toggle_pause();
        else if (x < 64) next_song(false);
        else if (x < 100) toggle_add_mode();
        else if (x >= 104 && x < 152) slide_to(true);
        else if (x >= 160 && x < 192) open_sort_menu();
        else if (x >= 192 && x < 224) cycle_brightness();
        else if (x >= 224) { app.overlay = OV_SETTINGS; app.menu_sel = -1; }
    }
    redraw = true;
}

static void tap_player(int x, int y)
{
    if (y >= PANEL_DOWN)
    {
        // a tab: back to browsing
        tab_tap(x);
        slide_to(false);
        return;
    }
    if (app.overlay == OV_INFO)
    {
        if (y < 20 && x >= 224)
            app.overlay = OV_NONE;
        redraw = true;
        return;
    }
    if (app.overlay == OV_QUEUE)
    {
        if (y < 20)
        {
            if (x >= 224)
                app.overlay = OV_NONE;
        }
        else if (y < 20 + ROWS * ROW_H && x < SCROLL_X)
        {
            int i = queue_row_at(y);
            if (i < queue_count())
            {
                if (x >= 220 && i != queue_pos())
                {
                    queue_remove(i);
                    app.thumb_track = LIB_NONE;
                    touch_state();
                }
                else
                    open_pos(i, 0, true);
            }
        }
        redraw = true;
        return;
    }
    int cy = 100;
    if (y >= cy - 24 && y < cy + 24)
    {
        if (x < 48) { queue_set_shuffle(!queue_shuffle()); app.thumb_track = LIB_NONE; touch_state(); }
        else if (x < 100) previous_song();
        else if (x < 156) toggle_pause();
        else if (x < 204) next_song(false);
        else { queue_set_repeat((queue_repeat() + 1) % 3); app.thumb_track = LIB_NONE; touch_state(); }
    }
    else if (y >= 132 && y < 164)
    {
        if (x < 48) app.overlay = OV_INFO;
        else if (x >= 104 && x < 152) slide_to(false);
        else if (x >= 208) { app.overlay = OV_QUEUE; app.q_sel = -1; app.q_drag_from = -1;
                             app.q_scroll = queue_pos() > 2 ? queue_pos() - 2 : 0; }
    }
    redraw = true;
}

static void tap_settings(int x, int y)
{
    if (y < 20)
    {
        if (x >= 224)
            app.overlay = OV_NONE;
    }
    else if (y >= 24 && y < 24 + 5 * 28)
        settings_toggle((y - 24) / 28);
    redraw = true;
}

static void touch_down(int x, int y)
{
    tp = (typeof(tp)){ true, x, y, x, y, ticks, false, false };
    int oy = app_panel_offset();
    if (app.overlay == OV_NONE && oy == 0 && x >= SCROLL_X && y >= LIST_Y && y < BAR_Y && app.n_items > ROWS)
    {
        app.scroll_drag = true;
        scroll_to_y(y);
    }
    else if (app.overlay == OV_NONE && oy == PANEL_DOWN && y >= 30 && y < 60 && player_path() != NULL)
    {
        app.seek_drag = true;
        app.seek_preview_ms = seek_ms_at(x);
    }
    else if (app.overlay == OV_QUEUE && oy == PANEL_DOWN && x >= SCROLL_X && y >= 20 && y < 20 + ROWS * ROW_H)
    {
        int max = queue_count() > ROWS ? queue_count() - ROWS : 0;
        app.q_scroll = (y - 20) * (max + 1) / (ROWS * ROW_H);
        tp.held = true;     // not a tap
    }
    redraw = true;
}

static void touch_move(int x, int y)
{
    tp.x = x;
    tp.y = y;
    if (abs(x - tp.x0) > TAP_SLOP || abs(y - tp.y0) > TAP_SLOP)
        tp.moved = true;
    if (app.scroll_drag)
        scroll_to_y(y);
    else if (app.seek_drag)
        app.seek_preview_ms = seek_ms_at(x);
    else if (app.q_drag_from >= 0)
    {
        int to = queue_row_at(y < 20 ? 20 : y);
        if (y < 28 && app.q_scroll > 0) app.q_scroll--;
        if (y > 20 + ROWS * ROW_H - 8 && app.q_scroll + ROWS < queue_count()) app.q_scroll++;
        if (to >= queue_count()) to = queue_count() - 1;
        if (to < 0) to = 0;
        app.q_drag_to = to;
    }
    else if (app.overlay == OV_QUEUE && x >= SCROLL_X)
    {
        int max = queue_count() > ROWS ? queue_count() - ROWS : 0;
        int s = (y - 20) * (max + 1) / (ROWS * ROW_H);
        app.q_scroll = s < 0 ? 0 : (s > max ? max : s);
    }
    else if (!tp.moved && !tp.held && ticks - tp.start >= HOLD_TICKS)
    {
        tp.held = true;
        int oy = app_panel_offset();
        if (oy == 0 && app.overlay == OV_NONE && tp.y0 >= LIST_Y && tp.y0 < BAR_Y && tp.x0 < SCROLL_X)
            open_row_menu(app.scroll + (tp.y0 - LIST_Y) / ROW_H);
        else if (oy == PANEL_DOWN && app.overlay == OV_QUEUE && tp.y0 >= 20 && tp.y0 < 20 + ROWS * ROW_H)
        {
            int i = queue_row_at(tp.y0);
            if (i < queue_count())
                app.q_drag_from = app.q_drag_to = i;
        }
    }
    redraw = true;
}

static void touch_up(void)
{
    tp.down = false;
    if (app.scroll_drag)
        app.scroll_drag = false;
    else if (app.seek_drag)
    {
        app.seek_drag = false;
        player_seek_ms(app.seek_preview_ms);
        touch_state();
    }
    else if (app.q_drag_from >= 0)
    {
        queue_move(app.q_drag_from, app.q_drag_to);
        app.q_drag_from = app.q_drag_to = -1;
        app.thumb_track = LIB_NONE;
        touch_state();
    }
    else if (!tp.held && !tp.moved)
    {
        int oy = app_panel_offset();
        if (app.overlay == OV_SETTINGS)
            tap_settings(tp.x0, tp.y0);
        else if (oy == 0)
            tap_browser(tp.x0, tp.y0);
        else if (oy == PANEL_DOWN)
            tap_player(tp.x0, tp.y0);
    }
    redraw = true;
}

// ------------------------------------------------------------------ keys

static int dpad_step(uint32_t held_ticks)
{
    // holding the D-pad speeds up scrolling
    if (held_ticks > 150) return 9;
    if (held_ticks > 60) return 3;
    return 1;
}

static void menu_keys(uint32_t rep, uint32_t down, int count, void (*choose)(int))
{
    if (rep & KEY_UP) app.menu_sel = app.menu_sel <= 0 ? count - 1 : app.menu_sel - 1;
    if (rep & KEY_DOWN) app.menu_sel = app.menu_sel + 1 >= count ? 0 : app.menu_sel + 1;
    if (down & KEY_A) choose(app.menu_sel);
    if (down & KEY_B) app.overlay = OV_NONE;
}

static uint32_t vert_since;

static void handle_keys(uint32_t down, uint32_t rep, uint32_t held)
{
    if (down & KEY_L) previous_song();
    if (down & KEY_R) next_song(false);
    if (down & KEY_START) toggle_pause();
    if (down | rep)
        redraw = true;

    if (!(held & (KEY_UP | KEY_DOWN)))
        vert_since = ticks;
    int step = dpad_step(ticks - vert_since);

    switch (app.overlay)
    {
    case OV_SETTINGS:
        menu_keys(rep, down, 5, settings_toggle);
        return;
    case OV_SORT:
        menu_keys(rep, down, ui_sort_option_count(ui_sort_slot()), choose_sort);
        return;
    case OV_ROWMENU:
        menu_keys(rep, down, 3, row_menu_choose);
        return;
    case OV_INFO:
        if (down & (KEY_B | KEY_X)) app.overlay = OV_NONE;
        return;
    case OV_QUEUE:
    {
        int n = queue_count();
        if (app.q_sel < 0) app.q_sel = queue_pos() < 0 ? 0 : queue_pos();
        if (rep & KEY_UP) app.q_sel -= step;
        if (rep & KEY_DOWN) app.q_sel += step;
        if (app.q_sel >= n) app.q_sel = n - 1;
        if (app.q_sel < 0) app.q_sel = 0;
        if (app.q_sel < app.q_scroll) app.q_scroll = app.q_sel;
        if (app.q_sel >= app.q_scroll + ROWS) app.q_scroll = app.q_sel - ROWS + 1;
        if ((down & KEY_A) && n) open_pos(app.q_sel, 0, true);
        if ((down & KEY_X) && n && app.q_sel != queue_pos()) { queue_remove(app.q_sel); touch_state(); }
        if (down & (KEY_B | KEY_Y)) app.overlay = OV_NONE;
        return;
    }
    default:
        break;
    }

    if (app.player_mode)
    {
        if (rep & KEY_LEFT) { player_seek_seconds(-10); touch_state(); }
        if (rep & KEY_RIGHT) { player_seek_seconds(10); touch_state(); }
        if (down & KEY_A) toggle_pause();
        if (down & KEY_X) app.overlay = OV_INFO;
        if (down & KEY_Y) { app.overlay = OV_QUEUE; app.q_sel = -1; app.q_drag_from = -1;
                            app.q_scroll = queue_pos() > 2 ? queue_pos() - 2 : 0; }
        if (down & (KEY_B | KEY_SELECT)) slide_to(false);
        return;
    }
    if (rep & KEY_UP) select_index(app.sel - step);
    if (rep & KEY_DOWN) select_index(app.sel + step);
    if (rep & KEY_LEFT) select_index(app.sel - ROWS);
    if (rep & KEY_RIGHT) select_index(app.sel + ROWS);
    if (down & KEY_A) activate(app.sel);
    if (down & KEY_B) go_back();
    if (down & KEY_Y) open_row_menu(app.sel);
    if (down & KEY_X) toggle_add_mode();
    if (down & KEY_SELECT) slide_to(true);
}

static void handle_lid(uint32_t held, uint32_t down)
{
    if (held & KEY_LID)
    {
        if (!lid_closed)
        {
            lid_closed = true;
            powerOff(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);
            if (app.set.pause_on_lid && player_state() == PLAYER_PLAYING)
            {
                player_toggle_pause();
                paused_by_lid = true;
            }
            save_at = ticks;    // the DS may be switched off while closed
        }
        if (app.set.lr_skip_lid)
        {
            if (down & KEY_L) previous_song();
            if (down & KEY_R) next_song(false);
        }
        return;
    }
    if (lid_closed)
    {
        lid_closed = false;
        powerOn(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);
        if (paused_by_lid && player_state() == PLAYER_PAUSED)
            player_toggle_pause();
        paused_by_lid = false;
        redraw = true;
    }
}

// ------------------------------------------------------------------ state

static void save_now(void)
{
    Resume r;
    memset(&r, 0, sizeof(r));
    uint32_t cur = app_current_track();
    if (cur != LIB_NONE)
        snprintf(r.path, sizeof(r.path), "%s", lib_path(cur));
    r.pos_ms = player_position_ms();
    r.album_queue = app.album_queue;
    state_save(&app.set, &r);
    last_save = ticks;
    save_at = 0;
}

static void restore(void)
{
    Resume r;
    state_defaults(&app.set);
    bool ok = state_load(&app.set, &r);
    lib_group_artists(app.set.case_sensitive);
    if (!ok || !r.path[0] || !lib_loaded())
        return;
    app.album_queue = r.album_queue;
    uint32_t t = lib_find_path(r.path);
    if (t == LIB_NONE)
        return;
    // on launch: the last song at the same position, paused
    if (queue_pos() >= 0 && queue_track(queue_pos()) == t)
    {
        open_pos(queue_pos(), r.pos_ms, false);
        return;
    }
    queue_set(&t, 1, 0);
    open_pos(0, r.pos_ms, false);
}

// ------------------------------------------------------------------ setup

static void fatal_screen(const char *line1, const char *line2)
{
    Canvas *c = gfx_canvas(SCREEN_TOP);
    gfx_fill(c, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    font_draw(c, 8, 8, 240, "dsplayer", C_ACC);
    font_draw(c, 8, 40, 240, line1, C_TEXT);
    font_draw(c, 8, 56, 240, line2, C_DIM);
    font_draw(c, 8, 160, 240, "Press START to exit", C_DIM);
    gfx_fill(gfx_canvas(SCREEN_BOTTOM), 0, 0, SCREEN_W, SCREEN_H, C_BG);
    while (1)
    {
        swiWaitForVBlank();
        gfx_present();
        scanKeys();
        if (keysDown() & KEY_START)
            exit(0);
    }
}

static bool find_music_root(void)
{
    static const char *candidates[] = { "/music", "fat:/music", "sd:/music" };
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
    {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode))
        {
            snprintf(music_root, sizeof(music_root), "%s", candidates[i]);
            return true;
        }
    }
    return false;
}

static void app_init(void)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/.dsmusic/font.bin", music_root);
    font_load(path);
    snprintf(path, sizeof(path), "%s/.dsmusic/library.idx", music_root);
    lib_load(path, app.lib_error, sizeof(app.lib_error));

    item_cap = (int)lib_track_count() + (int)lib_album_count() + 2;
    tracks_cap = (int)lib_track_count() + 1;
    app.items = malloc(item_cap * sizeof(Item));
    tracks_buf = malloc(tracks_cap * sizeof(uint32_t));
    app.added = calloc((lib_track_count() + 8) / 8, 1);
    app.artist = -1;
    app.album = LIB_NONE;
    app.q_drag_from = app.q_drag_to = -1;
    app.thumb_track = LIB_NONE;
    app.menu_item = -1;

    ui_init();
    restore();
    systemSetBacklightLevel(app.set.brightness);
    show_tab(TAB_ARTIST);
    last_save = ticks;
}

// One step of the app: input at 60 Hz, drawing at 30 fps.
static void app_tick(void)
{
    ticks++;
    player_tick();
    scanKeys();
    uint32_t down = keysDown(), rep = keysDownRepeat(), held = keysHeld();

    handle_lid(held, down);
    if (!lid_closed)
    {
        handle_keys(down, rep, held);
        if (held & KEY_TOUCH)
        {
            touchPosition t;
            touchRead(&t);
            if (!tp.down)
                touch_down(t.px, t.py);
            else
                touch_move(t.px, t.py);
        }
        else if (tp.down)
            touch_up();
    }

    player_service();
    if (player_state() == PLAYER_ENDED)
        next_song(true);

    if (save_at && ticks >= save_at)
        save_now();
    else if (player_state() == PLAYER_PLAYING && ticks - last_save > SAVE_EVERY)
        save_now();

    if (ticks & 1)
        return;     // draw every other frame

    // slide between browsing and the player
    int target = app.player_mode ? SLIDE_FRAMES : 0;
    if (app.slide != target)
    {
        app.slide += app.slide < target ? 1 : -1;
        redraw = true;
    }
    app.frame++;
    if (app.frame % 30 == 0)
        redraw = true;      // the clock
    if (lid_closed)
        return;
    update_thumb();
    static bool anim_top = true, anim_bottom = true;
    if (redraw || anim_top)
        anim_top = ui_draw_top();
    player_service();
    if (redraw || anim_bottom)
        anim_bottom = ui_draw_bottom();
    redraw = false;
}

#ifndef DSPLAYER_NO_MAIN
int main(int argc, char **argv)
{
    defaultExceptionHandler();
    gfx_init();
    player_init();
    keysSetRepeat(15, 4);
    disableSleep();

    if (!fatInitDefault())
        fatal_screen("Can't read the SD card.", "Is the flashcart's DLDI driver OK?");
    if (!find_music_root())
        fatal_screen("No /music folder on the card.", "Convert your music there with dsconv.");
    app_init();

    while (1)
    {
        swiWaitForVBlank();
        if (!lid_closed)
            gfx_present();
        app_tick();
    }
}
#endif
