// SPDX-License-Identifier: GPL-3.0-or-later
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
#include "default_cover.h"
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
volatile uint32_t vblank_count;    // counted in the vblank interrupt: real time
static uint32_t ticks;              // vblanks (60 per second) at the start of this loop
static uint32_t last_vblank;
static bool redraw = true;
static uint32_t save_at, last_save;
static bool lid_closed, paused_by_lid;
static int lid_fade, lid_fade_in = LID_IN;

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
    if (app.set.sort[slot] >= ui_sort_option_count(slot))
        app.set.sort[slot] = 0;     // an option that's gone (saved by an older version)
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

// Switch list. dir: +1 the new list slides in from the right, -1 from the
// left, 0 no slide (default: from the side of the new tab).
static void show_tab_dir(Tab t, int dir)
{
    if (dir == 0)
        dir = t > app.tab ? 1 : (t < app.tab ? -1 : 0);
    if (dir != 0 && app.slide == 0 && !app.player_mode && app.overlay != OV_SETTINGS)
    {
        ui_capture_list();
        app.tab_prev = app.tab_anim ? app.tab_prev : app.tab;
        app.tab_dir = dir;
        app.tab_anim = TAB_TICKS;
    }
    app.tab = t;
    app.overlay = OV_NONE;
    app.sel = app.scroll = 0;
    app.sel_frame = app.frame;
    rebuild_list();
    uint32_t cur = app_current_track();
    bool found = false;
    app.sel_vis = 0;
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
    app.sel_vis = (app.sel - app.scroll) * ROW_H;     // a new list: no gliding
    redraw = true;
}

static void show_tab(Tab t)
{
    show_tab_dir(t, 0);
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

// ---- song change animation: remember what the top screen shows before the
// queue or the song changes, then start the animation once the new song is open

static bool sa_captured;
static uint32_t sa_old_track, sa_old_next;

static bool player_layout(void) { return app.player_mode && app.slide == SLIDE_TICKS; }
static bool browse_layout(void) { return !app.player_mode && app.slide == 0; }

// The animation for the layout the top screen shows right now.
static SongAnim by_layout(SongAnim player, SongAnim browse)
{
    return player_layout() ? player : (browse_layout() ? browse : SA_NONE);
}

static void sa_capture(void)
{
    sa_captured = false;
    if (!lib_loaded() || !app.sa_cover || lid_closed)
        return;
    if (!(player_layout() && app_current_track() != LIB_NONE) && !browse_layout())
        return;
    sa_old_track = app_current_track();
    int np = queue_next_pos(true);
    sa_old_next = np >= 0 ? queue_track(np) : LIB_NONE;
    memcpy(app.sa_cover, player_cover() ? player_cover() : default_cover, 128 * 128 * 2);
    app.sa_has_thumb = sa_old_next != LIB_NONE && app.has_thumb && app.thumb_track == sa_old_next;
    if (app.sa_has_thumb)
        memcpy(app.sa_thumb, app.thumb, sizeof(app.sa_thumb));
    sa_captured = true;
}

static void sa_start(SongAnim kind)
{
    if (!sa_captured || kind == SA_NONE)
    {
        sa_captured = false;
        return;
    }
    sa_captured = false;
    uint32_t cur = app_current_track();
    int np = queue_next_pos(true);
    uint32_t next = np >= 0 ? queue_track(np) : LIB_NONE;
    if (kind == SA_JUMP && cur == sa_old_next)
        kind = SA_FORWARD;      // it was already the one shown under Next
    if (SA_STACK(kind) && cur == sa_old_track)
        kind = SA_NONE;         // the same song again
    if (kind == SA_BACK)
    {
        app.sa_a = cur;
        app.sa_b = sa_old_track;
        app.sa_c = sa_old_next;
    }
    else
    {
        app.sa_a = sa_old_track;
        app.sa_b = cur;
        app.sa_c = next;
        app.sa_n = sa_old_next;
    }
    app.sa_kind = kind;
    app.sa_t = 0;
}

// Open the song at queue position pos. Skips songs that can't be opened.
static bool open_pos(int pos, uint32_t start_ms, bool play, SongAnim anim)
{
    if (anim != SA_NONE && !sa_captured)
        sa_capture();
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
            sa_start(anim);
            touch_state();
            redraw = true;
            return true;
        }
        toast("Can't play %s: %s", path_basename(path), err);
        start_ms = 0;
        pos = queue_next_pos(false);
    }
    sa_captured = false;
    player_stop();
    return false;
}

// browse_anim: how the cover changes if the browser is showing.
static void play_tracks(const uint32_t *t, int n, int start, bool album_queue, SongAnim browse_anim)
{
    if (n <= 0)
        return;
    sa_capture();       // before the queue changes: what Next showed
    if (!queue_set(t, n, start))
        return;
    app.album_queue = album_queue;
    open_pos(queue_pos(), 0, true, by_layout(SA_JUMP, browse_anim));
}

static void next_song(bool automatic)
{
    int p = queue_next_pos(automatic);
    if (p < 0)
    {
        // end of the queue: stay on the last song, paused at its start
        if (queue_pos() >= 0)
            open_pos(queue_pos(), 0, false, SA_NONE);
        return;
    }
    open_pos(p, 0, true, by_layout(SA_FORWARD, SA_LIFT));
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
    open_pos(queue_prev_pos(), 0, player_state() == PLAYER_PLAYING, by_layout(SA_BACK, SA_PUT));
}

static void toggle_pause(void)
{
    if (player_state() == PLAYER_IDLE && queue_count() > 0)
        open_pos(queue_pos() < 0 ? 0 : queue_pos(), 0, true, SA_NONE);
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
        open_pos(0, 0, false, SA_NONE);
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
    // covers as a stack in list order: further up is put on, further down
    // lifts the old one off; a song from another list is put on
    SongAnim anim = SA_PUT;
    uint32_t cur = app_current_track();
    for (int i = 0; i < app.n_items && cur != LIB_NONE; i++)
        if (app.items[i].kind == ITEM_TRACK && app.items[i].id == cur)
            anim = index < i ? SA_PUT : SA_LIFT;
    play_tracks(tracks_buf, n, start, app.tab == TAB_TITLE && app.album != LIB_NONE, anim);
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
        show_tab_dir(TAB_FOLDER, 1);
        break;
    }
    case ITEM_UP:
    {
        char *slash = strrchr(app.folder, '/');
        char left[512];
        snprintf(left, sizeof(left), "%s", slash ? slash + 1 : app.folder);
        if (slash) *slash = 0; else app.folder[0] = 0;
        show_tab_dir(TAB_FOLDER, -1);
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
            play_tracks(tracks_buf, n, 0, it->kind == ITEM_ALBUM, SA_PUT);
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
    case 5: app.set.show_remaining ^= 1; break;
    case 6: app.set.plain_bg ^= 1; break;
    case 7: app.about = true; redraw = true; return;
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
    app.sel_vis = (app.sel - app.scroll) * ROW_H;
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
        else if (x >= 224) { app.overlay = OV_SETTINGS; app.menu_sel = -1; app.about = false; app.about_t = 0; }
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
                    open_pos(i, 0, true, SA_JUMP);
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
        else if (app.about && x < 112)
            app.about = false;          // ‹ back to the settings
    }
    else if (!app.about && y >= SET_Y && y < SET_Y + SET_ROWS * SET_H)
        settings_toggle((y - SET_Y) / SET_H);
    redraw = true;
}

// The button under the stylus, to light it up.
static int button_at(int x, int y)
{
    if (app.overlay != OV_NONE)
        return BTN_NONE;
    int oy = app_panel_offset();
    if (oy == 0 && y >= BAR_Y)
    {
        if (x < 32) return BTN_PLAY;
        if (x < 64) return BTN_NEXT;
        if (x < 100) return BTN_ADD;
        if (x >= 104 && x < 152) return BTN_DOWN;
        if (x >= 160 && x < 192) return BTN_SORT;
        if (x >= 192 && x < 224) return BTN_BRIGHT;
        if (x >= 224) return BTN_SETTINGS;
        return BTN_NONE;
    }
    if (oy == PANEL_DOWN && y < PANEL_DOWN)
    {
        if (y >= 76 && y < 124)
        {
            if (x < 48) return BTN_SHUFFLE;
            if (x < 100) return BTN_PREV;
            if (x < 156) return BTN_PPLAY;
            if (x < 204) return BTN_PNEXT;
            return BTN_REPEAT;
        }
        if (y >= 132 && y < 164)
        {
            if (x < 48) return BTN_INFO;
            if (x >= 104 && x < 152) return BTN_UP;
            if (x >= 208) return BTN_QUEUE;
        }
    }
    return BTN_NONE;
}

// Light a button briefly (for the buttons that keys also press).
static void flash(int id)
{
    app.press_id = id;
    app.press_down = false;
    app.press_until = app.frame + 6;
    redraw = true;
}

static void touch_down(int x, int y)
{
    app.press_id = button_at(x, y);
    app.press_down = app.press_id != BTN_NONE;
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
    {
        tp.moved = true;
        app.press_down = false;
        app.press_id = BTN_NONE;    // slid off: no tap, no light
    }
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
    if (app.press_down)
    {
        app.press_down = false;
        app.press_until = app.frame + 6;    // stays lit a moment after the tap
    }
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
    if (down & KEY_L) { previous_song(); if (app.player_mode) flash(BTN_PREV); }
    if (down & KEY_R) { next_song(false); flash(app.player_mode ? BTN_PNEXT : BTN_NEXT); }
    if (down & KEY_START) { toggle_pause(); flash(app.player_mode ? BTN_PPLAY : BTN_PLAY); }
    if (down | rep)
        redraw = true;

    if (!(held & (KEY_UP | KEY_DOWN)))
        vert_since = ticks;
    int step = dpad_step(ticks - vert_since);

    switch (app.overlay)
    {
    case OV_SETTINGS:
        if (app.about)
        {
            if (down & KEY_B) app.about = false;
        }
        else
            menu_keys(rep, down, SET_ROWS, settings_toggle);
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
        if ((down & KEY_A) && n) open_pos(app.q_sel, 0, true, SA_JUMP);
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
    // left / right: the tab next to this one (no wrapping at the ends)
    if ((down & KEY_LEFT) && app.tab > TAB_ARTIST) show_tab(app.tab - 1);
    if ((down & KEY_RIGHT) && app.tab < TAB_SEARCH) show_tab(app.tab + 1);
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
            lid_fade = 0;
            if (app.set.pause_on_lid && player_state() == PLAYER_PLAYING)
            {
                player_toggle_pause();
                paused_by_lid = true;
            }
            save_at = ticks;    // the DS may be switched off while closed
        }
        // fade to black first, so a screen that keeps showing its last frame
        // shows black when the lid opens again; then the light goes off
        if (lid_fade <= LID_OUT)
        {
            lid_fade++;
            if (lid_fade <= LID_OUT)
                setBrightness(3, -16 * lid_fade / LID_OUT);
            if (lid_fade == LID_OUT)
            {
                // and then really black: both screens filled and shown, so the
                // last frame is black whatever the display keeps
                for (int s = 0; s < 2; s++)
                    gfx_fill(gfx_canvas((Screen)s), 0, 0, SCREEN_W, SCREEN_H, COLOR(0, 0, 0));
                gfx_present();
            }
            else if (lid_fade == LID_OUT + 1)
                powerOff(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);   // a frame later
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
        setBrightness(3, -16);
        lid_fade_in = 0;
        powerOn(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);
        redraw = true;      // the screens were filled black: draw them again
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
        open_pos(queue_pos(), r.pos_ms, false, SA_NONE);
        return;
    }
    queue_set(&t, 1, 0);
    open_pos(0, r.pos_ms, false, SA_NONE);
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
    app.sa_cover = malloc(128 * 128 * 2);

    ui_init();
    restore();
    systemSetBacklightLevel(app.set.brightness);
    show_tab(TAB_ARTIST);
    last_save = ticks;
}

// Moves a highlight 2/5 of the way to its target per vblank; jumps further
// than far are taken at once. True while it moves.
static bool glide(int *v, int target, uint32_t elapsed, int far)
{
    if (*v == target)
        return false;
    for (uint32_t k = 0; k < elapsed && *v != target; k++)
    {
        int d = target - *v;
        if (d > far || d < -far)
            *v = target;
        else
            *v += d * 2 / 5 ? d * 2 / 5 : (d > 0 ? 1 : -1);
    }
    return true;
}

// One step of the app, once per vblank: input, playback, then one screen
// drawn (they take turns). Animations follow the vblank counter, so they
// keep their speed even when drawing takes longer than a frame.
static void app_tick(void)
{
    uint32_t now = vblank_count;
    uint32_t elapsed = now - last_vblank ? now - last_vblank : 1;
    last_vblank = now;
    ticks = now;
    player_tick(elapsed);
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

    // slide between browsing and the player
    int target = app.player_mode ? SLIDE_TICKS : 0;
    if (app.slide != target)
    {
        int step = (int)elapsed;
        if (app.slide < target)
            app.slide = app.slide + step > target ? target : app.slide + step;
        else
            app.slide = app.slide - step < target ? target : app.slide - step;
        redraw = true;
    }
    if (app.tab_anim > 0)
    {
        app.tab_anim = app.tab_anim > (int)elapsed ? app.tab_anim - (int)elapsed : 0;
        redraw = true;
    }
    // song change on the top screen
    if (app.sa_kind != SA_NONE)
    {
        int total = SA_STACK(app.sa_kind) ? SA_STACK_TICKS
                    : (app.sa_kind == SA_JUMP ? SA_PRE_TICKS : 0) + SA_TICKS;
        app.sa_t += (int)elapsed;
        bool layout_left = SA_STACK(app.sa_kind) ? !browse_layout() : !app.player_mode;
        if (app.sa_t >= total || layout_left)
        {
            app.sa_kind = SA_NONE;
            redraw = true;
        }
    }
    // the record: spins up to 33 1/3 rpm while playing, spins down when paused,
    // and stands still while a new one rolls out
    if (player_state() == PLAYER_PLAYING && !SA_PLAYER(app.sa_kind))
        app.vinyl_speed = app.vinyl_speed + 9 * (int)elapsed > 256 ? 256 : app.vinyl_speed + 9 * (int)elapsed;
    else
        app.vinyl_speed = app.vinyl_speed > 15 * (int)elapsed ? app.vinyl_speed - 15 * (int)elapsed : 0;
    app.vinyl_rot = (app.vinyl_rot + 10 * (int)elapsed * app.vinyl_speed / 256) % 1080;

    // menus slide up when they open and down when they close
    static Overlay last_overlay;
    bool menu_opened = false;
    if (app.overlay != last_overlay)
    {
        if (app.overlay != OV_NONE)
        {
            app.ov_vis = app.overlay;
            app.ov_t = 0;
            menu_opened = true;
        }
        last_overlay = app.overlay;
        redraw = true;
    }
    int ov_target = app.overlay != OV_NONE ? OV_TICKS : 0;
    if (app.ov_t != ov_target)
    {
        if (app.ov_t < ov_target)
            app.ov_t = app.ov_t + (int)elapsed > ov_target ? ov_target : app.ov_t + (int)elapsed;
        else
            app.ov_t = app.ov_t - (int)elapsed < 0 ? 0 : app.ov_t - (int)elapsed;
        redraw = true;
    }
    if (app.ov_t == 0 && app.overlay == OV_NONE)
        app.ov_vis = OV_NONE;
    // About slides in over the settings and back out
    int about_target = app.about ? TAB_TICKS : 0;
    if (app.about_t != about_target)
    {
        if (app.about_t < about_target)
            app.about_t = app.about_t + (int)elapsed > about_target ? about_target : app.about_t + (int)elapsed;
        else
            app.about_t = app.about_t - (int)elapsed < 0 ? 0 : app.about_t - (int)elapsed;
        redraw = true;
    }

    // play <-> pause shape, seek knob size
    int pp_target = player_state() == PLAYER_PLAYING ? PP_TICKS : 0;
    if (app.pp_t != pp_target)
    {
        app.pp_t += app.pp_t < pp_target ? (int)elapsed : -(int)elapsed;
        if ((pp_target == PP_TICKS && app.pp_t > PP_TICKS) || (pp_target == 0 && app.pp_t < 0))
            app.pp_t = pp_target;
        redraw = true;
    }
    int knob_target = app.seek_drag ? KNOB_TICKS : 0;
    if (app.knob_t != knob_target)
    {
        app.knob_t += app.knob_t < knob_target ? (int)elapsed : -(int)elapsed;
        if ((knob_target && app.knob_t > KNOB_TICKS) || (!knob_target && app.knob_t < 0))
            app.knob_t = knob_target;
        redraw = true;
    }
    // the highlights in the list, the menus and the queue glide to the selection
    redraw |= glide(&app.sel_vis, (app.sel - app.scroll) * ROW_H, elapsed, ROWS * ROW_H);
    static int menu_was = -1, q_was = -1;
    int menu_target = app.menu_sel * (app.ov_vis == OV_SETTINGS ? SET_H : 16);
    if (menu_opened || app.menu_sel < 0 || menu_was < 0)
        app.menu_vis = menu_target;         // nothing to glide from
    else
        redraw |= glide(&app.menu_vis, menu_target, elapsed, 1000);
    menu_was = app.menu_sel;
    int q_target = (app.q_sel - app.q_scroll) * ROW_H;
    if (app.q_sel < 0 || q_was < 0)
        app.q_vis = q_target;
    else
        redraw |= glide(&app.q_vis, q_target, elapsed, ROWS * ROW_H);
    q_was = app.q_sel;
    if (lid_fade_in < LID_IN && !lid_closed)
    {
        lid_fade_in++;
        setBrightness(3, -16 + 16 * lid_fade_in / LID_IN);
    }

    uint32_t prev_frame = app.frame;
    app.frame = now / 2;
    // progress bars: redraw when they move a pixel or the time a second; the clock
    static int last_px = -1;
    static uint32_t last_sec;
    uint32_t pos = player_position_smooth_ms(), dur = player_duration_ms();
    int px = dur ? (int)((uint64_t)240 * pos / dur) : 0;
    if (px != last_px || pos / 1000 != last_sec || app.frame / 30 != prev_frame / 30)
    {
        last_px = px;
        last_sec = pos / 1000;
        redraw = true;
    }
    if (lid_closed)
        return;
    update_thumb();

    static bool anim_top = true, anim_bottom = true, dirty_top = true, dirty_bottom = true, top_turn = true;
    if (redraw)
    {
        dirty_top = dirty_bottom = true;
        redraw = false;
    }
    bool need_top = dirty_top || anim_top, need_bottom = dirty_bottom || anim_bottom;
    if (need_top && (top_turn || !need_bottom))
    {
        anim_top = ui_draw_top();
        dirty_top = false;
        top_turn = false;
    }
    else if (need_bottom)
    {
        anim_bottom = ui_draw_bottom();
        dirty_bottom = false;
        top_turn = true;
    }
    player_service();
}

#ifndef DSPLAYER_NO_MAIN
static void on_vblank(void)
{
    vblank_count++;
}
#endif

#ifndef DSPLAYER_NO_MAIN
int main(int argc, char **argv)
{
    defaultExceptionHandler();
    // the sound hardware (ARM7 side) reads the ring buffers from main RAM: give
    // it priority, so copying whole screens around can't make it wait
    REG_EXMEMCNT |= ARM7_MAIN_RAM_PRIORITY;
    gfx_init();
    player_init();
    keysSetRepeat(15, 4);
    disableSleep();
    irqSet(IRQ_VBLANK, on_vblank);
    irqEnable(IRQ_VBLANK);

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
