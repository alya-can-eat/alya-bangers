// SPDX-License-Identifier: GPL-3.0-or-later
//
// dsplayer: browse the library by artist, album, title or folder on the
// bottom screen, see what plays on the top screen.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <fat.h>
#include <nds.h>

#include "app.h"
#include "boot.h"
#include "default_cover.h"
#include "eq.h"
#include "eqprof.h"
#include "font.h"
#include "keyboard.h"
#include "player.h"
#include "playlist.h"
#include "queue.h"
#include "stats.h"
#include "util.h"

#define TAP_SLOP        6       // pixels a tap may move
#define HOLD_TICKS      30      // 0.5 s at 60 Hz: hold for the row menu / to lift a queue row
#define TOAST_FRAMES    60      // 2 s at 30 fps
#define SAVE_DELAY      120     // ticks after a change before the state is written
#define SAVE_EVERY      (30 * 60)
#define LAUNCH_FADE     40      // vblanks: the music and the screens fade out, then the launcher starts
#define HOLD_START      60      // START held this long (1 s) starts it (Settings › Misc)

App app;

static char music_root[256];        // the music folder in use, "" none found
static int item_cap;
static uint32_t *tracks_buf;
static int tracks_cap;
volatile uint32_t vblank_count;    // counted in the vblank interrupt: real time
static uint32_t ticks;              // vblanks (60 per second) at the start of this loop
static uint32_t last_vblank;
static bool redraw = true;
static uint32_t save_at, last_save;
static uint32_t stats_save_at;
static uint32_t song_starts;        // songs opened or restarted: each is a new listen
static bool lid_closed, paused_by_lid;
static int lid_fade, lid_fade_in = LID_IN;

static void music_switch(const char *dir);
static void launch_start(void);

const char *app_music_root(void)
{
    return music_root;
}

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

// Ratings and plays: saved a second after the last change.
static void touch_stats(void)
{
    stats_save_at = ticks + 60;
    redraw = true;
}

// Give the playing song 0..5 stars.
static void rate_current(int stars)
{
    uint32_t t = app_current_track();
    if (t == LIB_NONE)
        return;
    stats_set_rating(lib_path(t), stars < 0 ? 0 : (stars > 5 ? 5 : stars));
    touch_stats();
}

// Shuffle by rating: each star doubles the weight. Unrated songs count as
// 3 stars, so they don't get buried: 5 stars come up 4x as often, 1 star 1/4.
static uint32_t rating_weight(uint32_t t)
{
    static const uint8_t w[6] = { 1, 2, 4, 8, 16, 32 };
    int r = stats_rating(lib_path(t));
    if (r == STATS_UNRATED)
        r = lib_tag(t, "~#rating") ? app_song_rating(t) : 3;
    return w[r < 0 ? 0 : (r > 5 ? 5 : r)];
}

static void apply_weighting(void)
{
    queue_set_weight_fn(app.set.shuffle_weighted ? rating_weight : NULL);
}

// A song counts as played once it has been listened to for half its length
// or 4 minutes, whichever comes first, like Last.fm. Listening time, not the
// position: seeking ahead doesn't count. Once per listen.
static void count_plays(uint32_t elapsed)
{
    static uint32_t session = 0xFFFFFFFFu, heard;
    static bool counted;
    if (session != song_starts)
    {
        session = song_starts;
        heard = 0;
        counted = false;
    }
    if (counted || player_state() != PLAYER_PLAYING)
        return;
    heard += elapsed;
    uint32_t dur = player_duration_ms(), need = dur / 2 < 240000 ? dur / 2 : 240000;
    uint32_t t = app_current_track();
    if (need && (uint64_t)heard * 1000 / 60 >= need && t != LIB_NONE)
    {
        counted = true;
        stats_count_play(lib_path(t), (uint32_t)time(NULL));
        touch_stats();
    }
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
    int rows = list_rows();
    if (app.sel >= app.scroll + rows)
        app.scroll = app.sel - rows + 1;
    int max_scroll = app.n_items > rows ? app.n_items - rows : 0;
    if (app.scroll > max_scroll) app.scroll = max_scroll;
    if (app.scroll < 0) app.scroll = 0;
    redraw = true;
}

uint8_t *app_sort_of(int slot)
{
    return slot == SLOT_PLAYLIST ? &app.set.pl_sort : &app.set.sort[slot < SORT_SLOTS ? slot : 0];
}

// A playlist's songs sorted for showing: 1 name A→Z, 2 Z→A, 3 year old→new.
static int pl_sort_mode;
static int cmp_pl_songs(const void *pa, const void *pb)
{
    const LibTrack *a = lib_track(((const Item *)pa)->id), *b = lib_track(((const Item *)pb)->id);
    if (pl_sort_mode == 3 && a->year != b->year)
        return a->year < b->year ? -1 : 1;
    if (a->title_sort != b->title_sort)
        return (a->title_sort < b->title_sort ? -1 : 1) * (pl_sort_mode == 2 ? -1 : 1);
    return ((const Item *)pa)->id < ((const Item *)pb)->id ? -1 : 1;
}

static void rebuild_list(void)
{
    app.n_items = 0;
    if (!lib_loaded())
        return;
    int slot = ui_sort_slot();
    if (*app_sort_of(slot) >= ui_sort_option_count(slot))
        *app_sort_of(slot) = 0;     // an option that's gone (saved by an older version)
    int sort = *app_sort_of(slot);
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
        if (app.pl >= pl_count())
            app.pl = pl_count() ? PL_LIST : PL_FOLDERS;   // that playlist is gone
        if (app.pl == PL_LIST && pl_count() == 0)
            app.pl = PL_FOLDERS;
        if (app.pl == PL_LIST)
        {
            app.items[n++] = (Item){ ITEM_UP, 0 };
            for (int i = 0; i < pl_count(); i++)
            {
                int k = sort == SORT_NAME_DOWN ? pl_count() - 1 - i : i;   // (the folders' sort)
                app.items[n++] = (Item){ ITEM_PLAYLIST, (uint32_t)k };
            }
        }
        else if (app.pl >= 0)
        {
            // its songs: in its order, or sorted for showing (the order it keeps doesn't change)
            app.items[n++] = (Item){ ITEM_UP, 0 };
            const uint32_t *t = pl_tracks(app.pl);
            for (int i = 0; i < pl_size(app.pl) && n < item_cap; i++)
                app.items[n++] = (Item){ ITEM_TRACK, t[i] };
            if (sort > 0)
            {
                pl_sort_mode = sort;
                qsort(app.items + 1, n - 1, sizeof(Item), cmp_pl_songs);
            }
        }
        else
        {
            if (app.folder[0])
                app.items[n++] = (Item){ ITEM_UP, 0 };
            else if (pl_count())
                app.items[n++] = (Item){ ITEM_PLAYLISTS, 0 };   // on top of the folders
            n += lib_list_folder(app.items + n, app.folder, sort);
        }
        app.n_items = n;
        break;
    }
    case TAB_SEARCH:
        app.n_items = search_run(app.query, app.im, &app.sf, app.items, app.marks, item_cap);
        break;
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
            app.scroll = i - list_rows() / 2;
            select_index(i);
            return true;
        }
    }
    return false;
}

// Switch list. dir: +1 the new list slides in from the right, -1 from the
// left, 0 no slide (default: from the side of the new tab).
static void search_leave(void);
static int search_first_row(void);

static void show_tab_dir(Tab t, int dir)
{
    if (app.tab == TAB_SEARCH && t != TAB_SEARCH)
        search_leave();
    if (t == TAB_SEARCH && app.tab != TAB_SEARCH)
    {
        app.kb_t = 0;           // comes up once the search page is in place
        app.sheet_t = 0;
    }
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
    if (t == TAB_SEARCH)
        app.kb_up = true;       // ready to type
    rebuild_list();
    if (t == TAB_SEARCH)
    {
        app.sel = search_first_row();
        app.sel_on = false;
        app.sel_vis = (app.sel - app.scroll) * ROW_H;
        redraw = true;
        return;
    }
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
                app.scroll = i - list_rows() / 2;
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
    if (player && app.tab == TAB_SEARCH)
    {
        app.kb_up = false;
        app.kb_t = app.sheet_t = 0;
        app.acc_key = app.key_down = -1;
    }
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
            song_starts++;
            app.text_t0 = app.frame;        // its texts scroll from the start
            redraw = true;
            return true;
        }
        toast(txt(S_CANT_PLAY), path_basename(path), err);
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
        song_starts++;
        app.text_t0 = app.frame;
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
    if (it->kind == ITEM_PLAYLIST)
    {
        int n = pl_size((int)it->id);
        n = n < tracks_cap ? n : tracks_cap;
        if (n > 0)
            memcpy(tracks_buf, pl_tracks((int)it->id), n * sizeof(uint32_t));
        return n;
    }
    if (it->kind == ITEM_PLAYLISTS)
        return 0;
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
        toast("%s", txt(S_QUEUE_FULL));
        return;
    }
    for (int i = 0; i < n; i++)
        mark(tracks_buf[i]);
    if (n == 1)
        toast("%s", txt(next ? S_PLAYS_NEXT : S_ADDED));
    else
        toast(txt(next ? S_N_PLAY_NEXT : S_N_ADDED), n);
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

static void query_clear(void);

static void activate(int i)
{
    if (i < 0 || i >= app.n_items)
        return;
    if (app.items[i].kind == ITEM_HEADER)
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
    case ITEM_TAG:
        // a chip for it, the query cleared: its songs
        search_tag_toggle(&app.sf, (int)TAG_ITEM_KEY(it->id), TAG_ITEM_VALUE(it->id));
        query_clear();
        break;
    case ITEM_PLAYLISTS:
        app.pl = PL_LIST;
        show_tab_dir(TAB_FOLDER, 1);
        break;
    case ITEM_PLAYLIST:
        app.pl = (int)it->id;
        show_tab_dir(TAB_FOLDER, 1);
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
        if (app.pl != PL_FOLDERS)
        {
            // a playlist: back to the playlists; the playlists: back to the folders
            int was = app.pl;
            app.pl = was >= 0 ? PL_LIST : PL_FOLDERS;
            show_tab_dir(TAB_FOLDER, -1);
            if (was >= 0)
                select_item(ITEM_PLAYLIST, (uint32_t)was);
            else
                select_item(ITEM_PLAYLISTS, 0);
            app.sel_vis = (app.sel - app.scroll) * ROW_H;
            break;
        }
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
                app.scroll = k - list_rows() / 2;
                select_index(k);
                break;
            }
        }
        break;
    }
    }
}

// ---- playlists

// The rows after Play now, Play next, Add to queue: how many, which.
static int row_extras(int i, int *out)
{
    if (i < 0 || i >= app.n_items)
        return 0;
    const Item *it = &app.items[i];
    if (it->kind == ITEM_PLAYLIST)
    {
        out[0] = ROWX_RENAME;
        out[1] = ROWX_DELETE;
        return 2;
    }
    if (it->kind == ITEM_UP || it->kind == ITEM_HEADER || it->kind == ITEM_PLAYLISTS)
        return 0;
    out[0] = it->kind == ITEM_TRACK && app.tab == TAB_FOLDER && app.pl >= 0 ? ROWX_REMOVE : ROWX_ADD;
    return 1;
}

int app_row_action(int i, int row)
{
    int x[2];
    int n = row_extras(i, x);
    return row >= 3 && row - 3 < n ? x[row - 3] : ROWX_NONE;
}

int app_row_count(int i)
{
    int x[2];
    return 3 + row_extras(i, x);
}

// A playlist's song that can be moved by holding it: in its own order only.
static bool pl_draggable(int i)
{
    return app.tab == TAB_FOLDER && app.pl >= 0 && app.set.pl_sort == 0 && i >= 1 && i < app.n_items
           && app.items[i].kind == ITEM_TRACK;
}

// The list again after a playlist changed, staying where it was.
static void refresh_list(void)
{
    int sel = app.sel, scroll = app.scroll;
    rebuild_list();
    app.sel = sel < app.n_items ? sel : app.n_items - 1;
    if (app.sel < 0)
        app.sel = 0;
    int max = app.n_items > list_rows() ? app.n_items - list_rows() : 0;
    app.scroll = scroll < max ? scroll : max;
    app.sel_vis = (app.sel - app.scroll) * ROW_H;
    redraw = true;
}

// "Added to …", "Already in …"
static void pl_added_toast(int pl, int added, int n)
{
    const char *nm = pl_name(pl);
    if (added < 0)
        toast(txt(S_PL_FULL), nm);
    else if (added == 0)
        toast(txt(S_PL_ALREADY), nm);
    else if (n == 1)
        toast(txt(S_PL_ADDED), nm);
    else
        toast(txt(S_PL_N_ADDED), added, nm);
}

// Its songs to playlist pl (the songs of the queue for NAME_PL_QUEUE).
static void pl_add_songs(int pl, bool queue)
{
    int n = 0;
    if (queue)
        for (int i = 0; i < queue_count() && n < tracks_cap; i++)
            tracks_buf[n++] = queue_track(i);
    else
        n = item_tracks(&app.pl_item);
    pl_added_toast(pl, n ? pl_add(pl, tracks_buf, n) : 0, n);
    if (app.tab == TAB_FOLDER)
        refresh_list();
}

static void name_start(int kind)
{
    app.eq_name[0] = 0;
    app.key_down = app.acc_key = -1;
    app.shift = app.kb_num = false;
    app.name_for = (uint8_t)kind;
    app.overlay = OV_EQ_NAME;
}

// "Add to playlist": New playlist, then the playlists.
static void pl_picker_open(const Item *it)
{
    app.pl_item = *it;
    app.pl_scroll = 0;
    app.menu_sel = 0;
    app.overlay = OV_PL_ADD;
}

static void pl_picker_scroll_to_sel(void)
{
    if (app.menu_sel < app.pl_scroll)
        app.pl_scroll = app.menu_sel;
    if (app.menu_sel >= app.pl_scroll + PLADD_ROWS)
        app.pl_scroll = app.menu_sel - PLADD_ROWS + 1;
}

static void pl_picker_choose(int r)
{
    if (r == 0)
        name_start(NAME_PL);
    else if (r > 0 && r <= pl_count())
    {
        app.overlay = OV_NONE;
        pl_add_songs(r - 1, false);
    }
}

static void pl_picker_tap(int x, int y)
{
    int y0 = BAR_Y - PLADD_H;
    if (x < PLADD_X0 || x >= PLADD_X1 || y < y0 || y >= BAR_Y)
        app.overlay = OV_NONE;          // beside it: closed
    else if (y >= y0 + 20)
    {
        int r = app.pl_scroll + (y - y0 - 20) / 16;
        if ((y - y0 - 20) / 16 < PLADD_ROWS && r <= pl_count())
            pl_picker_choose(r);
    }
    redraw = true;
}

// A playlist's new name; one another playlist has: not taken.
static void pl_rename_done(void)
{
    int pl = (int)app.pl_item.id;
    int other = pl_find(app.eq_name);
    if (other >= 0 && other != pl)
    {
        toast(txt(S_PL_EXISTS), pl_name(other));
        return;                     // the keyboard stays, for another name
    }
    app.overlay = OV_NONE;
    int now = pl_rename(pl, app.eq_name);
    if (app.tab == TAB_FOLDER && app.pl == PL_LIST)
    {
        refresh_list();
        if (now >= 0)
            select_item(ITEM_PLAYLIST, (uint32_t)now);
        app.sel_vis = (app.sel - app.scroll) * ROW_H;
    }
}

// The typed name: a new playlist (or the one with that name) gets the songs.
static void pl_name_done(void)
{
    if (app.name_for == NAME_PL_RENAME)
    {
        pl_rename_done();
        return;
    }
    bool queue = app.name_for == NAME_PL_QUEUE;
    // a new one changes the others' places (they're sorted by name)
    char shown[PL_NAME] = "";
    if (app.pl >= 0)
        snprintf(shown, sizeof(shown), "%s", pl_name(app.pl));
    int pl = pl_find(app.eq_name);
    if (pl < 0)
    {
        if (pl_count() >= PL_MAX)
        {
            toast("%s", txt(S_PL_TOO_MANY));
            app.overlay = queue ? OV_QUEUE : OV_NONE;
            return;
        }
        pl = pl_create(app.eq_name);
        if (shown[0])
            app.pl = pl_find(shown);
    }
    app.overlay = queue ? OV_QUEUE : OV_NONE;
    if (pl >= 0)
        pl_add_songs(pl, queue);
}

// Deleting a playlist asks first.
static void pl_ask_open(const Item *it)
{
    app.pl_item = *it;
    app.eq_ask_sel = 0;
    app.overlay = OV_PL_ASK;
}

static void pl_ask_answer(bool del)
{
    app.overlay = OV_NONE;
    if (del)
    {
        pl_delete((int)app.pl_item.id);
        refresh_list();
    }
}

static void row_menu_choose(int option)
{
    int i = app.menu_item;
    app.overlay = OV_NONE;
    if (i < 0 || i >= app.n_items || app.items[i].kind == ITEM_UP)
        return;
    const Item *it = &app.items[i];
    if (option >= 3)
    {
        switch (app_row_action(i, option))
        {
        case ROWX_ADD: pl_picker_open(it); break;
        case ROWX_DELETE: pl_ask_open(it); break;
        case ROWX_RENAME:
            // the keyboard with its name
            app.pl_item = *it;
            name_start(NAME_PL_RENAME);
            snprintf(app.eq_name, sizeof(app.eq_name), "%s", pl_name((int)it->id));
            break;
        case ROWX_REMOVE:
        {
            char nm[PL_NAME];
            snprintf(nm, sizeof(nm), "%s", pl_name(app.pl));
            const uint32_t *t = pl_tracks(app.pl);
            for (int k = 0; k < pl_size(app.pl); k++)
                if (t[k] == it->id)
                {
                    pl_remove(app.pl, k);   // its place in the playlist (the list may be sorted)
                    break;
                }
            toast(txt(S_PL_REMOVED), nm);
            refresh_list();
            break;
        }
        default: break;
        }
        redraw = true;
        return;
    }
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
    if (i < 0 || i >= app.n_items || app.items[i].kind == ITEM_UP || app.items[i].kind == ITEM_HEADER
        || app.items[i].kind == ITEM_PLAYLISTS || !lib_loaded())
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
    app.menu_sel = *app_sort_of(ui_sort_slot());
    redraw = true;
}

static void choose_sort(int option)
{
    int slot = ui_sort_slot();
    app.overlay = OV_NONE;
    if (option < 0 || option >= ui_sort_option_count(slot))
        return;
    *app_sort_of(slot) = (uint8_t)option;
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

// Settings: change one.
// ------------------------------------------------------------------ search

static int search_first_row(void)
{
    for (int i = 0; i < app.n_items; i++)
        if (app.items[i].kind != ITEM_HEADER)
            return i;
    return 0;
}

// New results (the query or a filter changed): from the top, no highlight
// until the D-pad asks for it.
static void search_refresh(void)
{
    // searching takes a few frames on the DS: animations go on from where they
    // were instead of jumping over that time
    uint32_t before = vblank_count;
    rebuild_list();
    last_vblank += vblank_count - before;
    app.scroll = 0;
    app.sel = search_first_row();
    app.sel_frame = app.frame;
    app.sel_on = false;
    app.sel_vis = (app.sel - app.scroll) * ROW_H;
    redraw = true;
}

static bool search_has_content(void)
{
    return app.query[0] || search_songs_filtered(&app.sf);
}

static void search_leave(void)
{
    app.kb_up = false;
    app.kb_t = 0;
    app.key_down = app.acc_key = -1;
    app.shift = false;
}

static void kb_raise(void)
{
    app.kb_up = true;
    if (search_has_content())
        app.sheet_t = SHEET_TICKS;      // the results go straight up to the top screen
    select_index(app.sel);
}

static void kb_lower(void)
{
    app.kb_up = false;
    app.acc_key = app.key_down = -1;
    select_index(app.sel);              // fewer rows: the selection stays in view
}

// D-pad in the results: header rows are stepped over.
static void search_step(int d)
{
    if (app.n_items == 0)
        return;
    int dir = d > 0 ? 1 : -1, i = app.sel + d;
    if (i < 0) i = 0;
    if (i >= app.n_items) i = app.n_items - 1;
    while (i >= 0 && i < app.n_items && app.items[i].kind == ITEM_HEADER)
        i += dir;
    if (i < 0 || i >= app.n_items)
        return;
    select_index(i);
    // a section's first result shows its header too
    if (i > 0 && app.items[i - 1].kind == ITEM_HEADER && app.scroll == i)
        app.scroll = i - 1;
}

static int utf8_encode(uint32_t c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000)
    {
        out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | ((c >> 6) & 63)); out[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | c >> 18); out[1] = (char)(0x80 | ((c >> 12) & 63));
    out[2] = (char)(0x80 | ((c >> 6) & 63)); out[3] = (char)(0x80 | (c & 63));
    return 4;
}

// What the keys type into: the search, or the name of a new equalizer profile.
static char *kb_text(size_t *cap)
{
    if (app.overlay == OV_EQ_NAME)
    {
        *cap = sizeof(app.eq_name);
        return app.eq_name;
    }
    *cap = sizeof(app.query);
    return app.query;
}

static void kb_changed(void)
{
    if (app.overlay == OV_EQ_NAME)
        redraw = true;
    else
        search_refresh();
}

// The last character typed and where it starts.
static uint32_t query_last(int *at)
{
    size_t cap;
    char *q = kb_text(&cap);
    int n = (int)strlen(q), i = n;
    if (n == 0)
    {
        *at = 0;
        return 0;
    }
    do
        i--;
    while (i > 0 && (q[i] & 0xC0) == 0x80);
    *at = i;
    const char *p = q + i;
    return utf8_next(&p);
}

static void query_put(uint32_t c)
{
    char u[4];
    int l = utf8_encode(c, u);
    size_t cap;
    char *q = kb_text(&cap);
    size_t n = strlen(q);
    if (n + l >= cap)
        return;
    memcpy(q + n, u, l);
    q[n + l] = 0;
    kb_changed();
}

static void query_back(void)
{
    int at;
    if (query_last(&at) == 0)
        return;
    size_t cap;
    kb_text(&cap)[at] = 0;
    kb_changed();
}

static void query_clear(void)
{
    size_t cap;
    kb_text(&cap)[0] = 0;
    kb_changed();
}

// ゛ ゜ 小: change the last kana.
static void query_kana(int mod)
{
    int at;
    uint32_t c = kana_mod(query_last(&at), mod);
    if (c == 0)
        return;
    size_t cap;
    kb_text(&cap)[at] = 0;
    query_put(c);
}

static void im_switch(int im)
{
    if (im == app.im)
        return;
    app.im_from = app.im;
    app.im = im;
    app.im_t = IM_TICKS;
    app.kb_num = app.shift = false;
    kb_changed();                   // pinyin readings count only with pinyin
}

static Key kb_keys[KB_MAX_KEYS];
static int kb_n;
static bool kb_pressing;            // the stylus went down on the keyboard
static int acc_x0, acc_y0;          // where the stylus was when the forms popped up
static uint32_t kb_repeat_at;       // ⌫ held: when it last deleted

static void kb_load(void)
{
    kb_n = kb_layout(app.im, app.kb_num, app.set.keyboard == KB_QWERTZ, app.shift, kb_keys);
}

static void kb_flash(int k)
{
    app.key_flash = k;
    app.key_flash_until = app.frame + 5;
}

static void eq_name_done(void);

static void key_act(const Key *k)
{
    switch (k->kind)
    {
    case K_CHAR:
        app.shift = false;
        query_put(k->cp);
        break;
    case K_SPACE: query_put(' '); break;
    case K_SHIFT: app.shift = !app.shift; break;
    case K_BACK: query_back(); break;
    case K_NUM: app.kb_num = !app.kb_num; app.shift = false; break;
    case K_IM: im_switch((app.im + 1) % IM_COUNT); break;
    case K_DONE:
        if (app.overlay == OV_EQ_NAME)
            eq_name_done();
        else
            kb_lower();
        break;
    case K_KANA: query_kana((int)k->cp); break;
    default: break;
    }
    redraw = true;
}

// The n-th of a held letter's other forms (upper case with shift), 0: none.
static uint32_t form_at(const Key *k, int n)
{
    const char *f = k->kind == K_CHAR ? letter_forms(cp_lower(k->cp)) : NULL;
    uint32_t c = 0;
    for (int i = 0; f && i <= n; i++)
        if ((c = utf8_next(&f)) == 0)
            return 0;
    return app.shift ? cp_upper(c) : c;
}

static int forms_count(const Key *k)
{
    const char *f = k->kind == K_CHAR ? letter_forms(cp_lower(k->cp)) : NULL;
    int n = 0;
    while (f && utf8_next(&f))
        n++;
    return n;
}

static bool kb_ready(void)
{
    if (app.overlay == OV_EQ_NAME)
        return app.ov_t == OV_TICKS;
    return app.tab == TAB_SEARCH && app.kb_up && app.kb_t == KB_TICKS && app.overlay == OV_NONE
           && app_panel_offset() == 0;
}

static void kb_touch_down(int x, int y)
{
    kb_load();
    kb_pressing = true;
    app.key_down = kb_key_at(kb_keys, kb_n, x, y - KB_Y);
    app.key_since = ticks;
    if (app.key_down >= 0 && kb_keys[app.key_down].kind == K_BACK)
    {
        query_back();               // at once, then again while held
        kb_flash(app.key_down);
        kb_repeat_at = ticks + 24;
    }
}

static void acc_follow(int x, int y)
{
    const Key *k = &kb_keys[app.acc_key];
    int n = forms_count(k), bx, by, bw;
    kb_forms_box(k, n, &bx, &by, &bw);
    int ky = y - KB_Y;
    if (ky >= by - 16 && ky < k->y + k->h && x >= bx - 8 && x < bx + bw + 8)
    {
        int i = (x - bx - 2) / 25;
        app.acc_sel = i < 0 ? 0 : (i >= n ? n - 1 : i);
    }
    else
        app.acc_sel = -1;
}

static void kb_touch_move(int x, int y)
{
    if (app.acc_key >= 0)
    {
        // the first form stays chosen until the stylus moves
        if (abs(x - acc_x0) > 4 || abs(y - acc_y0) > 4)
        {
            acc_x0 = -100;
            acc_follow(x, y);
        }
        return;
    }
    int k = kb_key_at(kb_keys, kb_n, x, y - KB_Y);
    if (k != app.key_down)
    {
        // slid onto another key: that one counts now
        app.key_down = k;
        app.key_since = ticks;
        kb_repeat_at = ticks + 24;
        return;
    }
    if (k < 0)
        return;
    const Key *key = &kb_keys[k];
    uint32_t held = ticks - app.key_since;
    if (key->kind == K_BACK && ticks >= kb_repeat_at)
    {
        query_back();
        kb_flash(k);
        kb_repeat_at = ticks + 4;
    }
    else if (key->kind == K_IM && held >= HOLD_TICKS && app.overlay != OV_EQ_NAME)
    {
        // held: the list of input methods
        app.overlay = OV_IM;
        app.menu_sel = app.im;
        app.key_down = -1;
        kb_pressing = false;
    }
    else if (key->kind == K_CHAR && held >= HOLD_TICKS && forms_count(key) > 0)
    {
        app.acc_key = k;
        app.acc_t = 0;
        app.acc_sel = 0;
        acc_x0 = x;
        acc_y0 = y;
        int bx, by, bw;
        kb_forms_box(key, forms_count(key), &bx, &by, &bw);
        app.acc_vis = bx + 2;
    }
}

static void kb_touch_up(void)
{
    kb_pressing = false;
    if (app.acc_key >= 0)
    {
        const Key *k = &kb_keys[app.acc_key];
        if (app.acc_sel >= 0)
        {
            uint32_t c = form_at(k, app.acc_sel);
            app.shift = false;
            if (c)
                query_put(c);
        }
        kb_flash(app.acc_key);
        app.acc_key = -1;
    }
    else if (app.key_down >= 0 && kb_keys[app.key_down].kind != K_BACK)
    {
        int k = app.key_down;
        kb_flash(k);
        key_act(&kb_keys[k]);
    }
    app.key_down = -1;
    redraw = true;
}

// ---- filter menu: the filters, Tags (the tags), then a filter's choices

// The page in front: its list, how many rows, where it's scrolled to.
static int fpage_count(void)
{
    return app.fpage < 0 ? FM_COUNT : (app.fpage == F_TAGS ? app.n_tkeys : app.n_fopts);
}

static int *fpage_scroll(void)
{
    static int none;
    none = 0;
    return app.fpage < 0 ? &none : (app.fpage == F_TAGS ? &app.tscroll : &app.fscroll);
}

static const SearchOpt *fpage_opts(void)
{
    return app.fpage == F_TAGS ? app.tkeys : app.fopts;
}

// The highlight on the choice that's on, in view.
static void fpage_select_on(void)
{
    app.menu_sel = -1;
    const SearchOpt *o = fpage_opts();
    int *scroll = fpage_scroll();
    *scroll = 0;
    for (int i = 0; i < fpage_count(); i++)
        if (search_option_on(&app.sf, app.fpage, app.fkey, o[i].value))
        {
            app.menu_sel = i;
            *scroll = i >= FMENU_ROWS ? i - FMENU_ROWS + 1 : 0;
            break;
        }
}

static void tags_load(void)
{
    app.n_tkeys = search_options(F_TAGS, 0, app.tkeys, 256);
    app.tscroll = 0;
}

static void fpage_open(int kind)
{
    app.fpage = app.fkind = kind;
    if (kind == F_TAGS)
        tags_load();
    else
        app.n_fopts = search_options(kind, app.fkey, app.fopts, FOPTS_MAX);
    fpage_select_on();
}

static void filter_menu(int page)
{
    app.overlay = OV_FILTER;
    app.fpage = -1;
    app.fbase = -1;
    app.fpage_t = 0;
    app.menu_sel = -1;
    if (F_IS_TAG(page))
    {
        // a tag chip: its values, with the list of tags behind them
        app.fkey = app.sf.tag[page - F_TAG].key;
        tags_load();
        app.fbase = F_TAGS;
    }
    if (page > F_IN)
    {
        fpage_open(page);
        app.fpage_t = TAB_TICKS;    // straight to its choices
    }
    redraw = true;
}

static void filter_in_cycle(void)
{
    app.sf.in = (uint8_t)((app.sf.in + 1) % IN_COUNT);
    search_refresh();
}

static void filter_choose(int i)
{
    if (i < 0 || i >= app.n_fopts)
        return;
    uint32_t v = app.fopts[i].value;
    if (F_IS_TAG(app.fkind))
        search_tag_toggle(&app.sf, app.fkey, v);    // another value: "or"; one that's on: off
    else if (search_option_on(&app.sf, app.fkind, app.fkey, v))
        search_filter_off(&app.sf, app.fkind);      // the one that's on: off again
    else
        search_filter_set(&app.sf, app.fkind, v);
    app.overlay = OV_NONE;
    search_refresh();
}

// Tags: a tag's values push the list of tags out.
static void tags_choose(int i)
{
    if (i < 0 || i >= app.n_tkeys)
        return;
    app.fkey = (int)app.tkeys[i].value;
    app.fbase = F_TAGS;
    app.fpage_t = 0;
    fpage_open(search_tag_chip(&app.sf, app.fkey));
}

static void filter_list_choose(int r)
{
    int kind = search_menu_kind(r);
    if (kind == F_IN)
        filter_in_cycle();
    else if (kind > F_IN)
    {
        app.fbase = -1;
        fpage_open(kind);
    }
}

static void filter_scroll_to_sel(void);

static void filter_back(void)
{
    if (app.fpage >= 0 && app.fpage != F_TAGS && app.fbase == F_TAGS)
    {
        // a tag's values: back to the tags, on its row
        app.fpage = F_TAGS;
        app.menu_sel = -1;
        for (int i = 0; i < app.n_tkeys; i++)
            if ((int)app.tkeys[i].value == app.fkey)
                app.menu_sel = i;
        filter_scroll_to_sel();
    }
    else if (app.fpage == F_TAGS && app.fbase == F_TAGS)
    {
        // the tags (on the left, after a tag's values): now on the right, sliding out
        app.fbase = -1;
        app.fkind = F_TAGS;
        app.fpage_t = TAB_TICKS;
        app.fpage = -1;
        app.menu_sel = FM_TAGS;
    }
    else if (app.fpage >= 0)
    {
        app.menu_sel = search_menu_row(app.fpage);  // back on its row
        app.fpage = -1;
    }
    else
        app.overlay = OV_NONE;
    redraw = true;
}

static void filter_scroll_to_sel(void)
{
    if (app.fpage < 0 || app.menu_sel < 0)
        return;
    int *scroll = fpage_scroll();
    if (app.menu_sel < *scroll)
        *scroll = app.menu_sel;
    if (app.menu_sel >= *scroll + FMENU_ROWS)
        *scroll = app.menu_sel - FMENU_ROWS + 1;
}

static void filter_pick(int i)
{
    if (app.fpage < 0)
        filter_list_choose(i);
    else if (app.fpage == F_TAGS)
        tags_choose(i);
    else
        filter_choose(i);
}

static void filter_tap(int x, int y)
{
    (void)x;
    if (y < FMENU_Y)
    {
        app.overlay = OV_NONE;      // a tap above the menu closes it
        return;
    }
    if (y < FMENU_Y + 20)
    {
        if (app.fpage >= 0)
            filter_back();          // "‹ Genre"
        return;
    }
    int r = (y - FMENU_Y - 20) / 16;
    if (r >= FMENU_ROWS)
        return;
    filter_pick(*fpage_scroll() + r);
}

static void im_menu_choose(int i)
{
    app.overlay = OV_NONE;
    if (i >= 0 && i < IM_COUNT)
        im_switch(i);
}

static void im_menu_tap(int x, int y)
{
    int y0 = ui_im_menu_y();
    if (x >= 4 && x < 180 && y >= y0 + 20 && y < y0 + 20 + 16 * IM_COUNT)
        im_menu_choose((y - y0 - 20) / 16);
    else
        app.overlay = OV_NONE;
}

// Taps on the search page above the keyboard (or above the bar): the query,
// the chips, the results. True if it was one of those.
static bool tap_search(int x, int y)
{
    if (y < TAB_H || (y >= BAR_Y && !app.kb_up))
        return false;
    if (y < CHIPS_Y - 1)
    {
        if (x >= 228 && app.query[0])
            query_clear();
        else if (!app.kb_up)
            kb_raise();
        return true;
    }
    if (y < SEARCH_LIST_Y)
    {
        bool on_x;
        int k = ui_chip_at(x, y, &on_x);
        if (k == F_COUNT)
            filter_menu(-1);
        else if (k >= 0 && on_x)
        {
            search_filter_off(&app.sf, k);
            search_refresh();
        }
        else if (k >= 0)
            filter_menu(k == F_IN ? -1 : k);
        return true;
    }
    if (!app.kb_up && y < BAR_Y && x < SCROLL_X && lib_loaded())
        activate(app.scroll + (y - SEARCH_LIST_Y) / ROW_H);
    return true;
}

// ---- the file and folder picker: the launcher's .nds (Settings › Misc),
// the music folder (Settings › Browsing). A page past the one it's for.

static bool join_path(char *out, size_t size, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    return (size_t)snprintf(out, size, "%s%s%s", dir, l && dir[l - 1] == '/' ? "" : "/", name) < size;
}

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool has_library(const char *dir)
{
    char p[300];
    struct stat st;
    return join_path(p, sizeof(p), dir, ".dsmusic/library.idx") && stat(p, &st) == 0;
}

// The folder a path is in: "/a/b" -> "/a", "/a" -> "/", "fat:/a" -> "fat:/".
// False at the card's top.
static bool parent_dir(char *p)
{
    char *s = strrchr(p, '/');
    if (s == NULL || s[1] == 0)
        return false;
    if (s == p || s[-1] == ':')
        s[1] = 0;
    else
        *s = 0;
    return true;
}

static int pick_cmp(const void *a, const void *b)
{
    const PickEntry *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;         // folders first
    return strcasecmp(app.pick_names + x->name, app.pick_names + y->name);
}

static void keep_in_view(int *scroll, int top, int h, int max)
{
    int room = SCREEN_H - SET_Y;
    if (top < *scroll)
        *scroll = top;
    if (top + h > *scroll + room)
        *scroll = top + h - room;
    *scroll = *scroll < 0 ? 0 : (*scroll > max ? max : *scroll);
}

static void pick_keep_sel(void)
{
    if (app.menu_sel >= 0)
        keep_in_view(&app.pick_scroll, app.menu_sel * SET_H, SET_H, ui_pick_max_scroll());
}

// The picker shows a folder: its folders and, for the launcher, its .nds files.
static void pick_load(const char *path)
{
    char dir[sizeof(app.pick_dir)];
    snprintf(dir, sizeof(dir), "%s", path);
    snprintf(app.pick_dir, sizeof(app.pick_dir), "%s", dir);
    app.pick_n = app.pick_scroll = 0;
    app.menu_sel = app.menu_sel >= 0 ? 0 : -1;      // by D-pad: from the first row
    app.menu_vis = 0;
    app.pick_has_lib = app.pick_for == PICK_MUSIC && has_library(dir);
    redraw = true;
    if (app.pick == NULL)
        app.pick = malloc(PICK_MAX * sizeof(PickEntry));
    if (app.pick_names == NULL)
        app.pick_names = malloc(PICK_NAMES);
    DIR *d = app.pick && app.pick_names ? opendir(dir) : NULL;
    if (d == NULL)
        return;
    int used = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && app.pick_n < PICK_MAX)
    {
        const char *n = e->d_name;
        size_t l = strlen(n);
        if (n[0] == '.' || used + (int)l + 1 > PICK_NAMES)
            continue;                   // ., .., hidden ones (.dsmusic)
        bool folder;
        if (e->d_type == DT_DIR || e->d_type == DT_REG)
            folder = e->d_type == DT_DIR;
        else
        {
            char full[300];
            folder = join_path(full, sizeof(full), dir, n) && is_dir(full);
        }
        if (!folder && (app.pick_for != PICK_LAUNCHER || l < 5 || strcasecmp(n + l - 4, ".nds") != 0))
            continue;
        memcpy(app.pick_names + used, n, l + 1);
        app.pick[app.pick_n++] = (PickEntry){ (uint16_t)used, folder, 0 };
        used += (int)l + 1;
    }
    closedir(d);
    qsort(app.pick, app.pick_n, sizeof(PickEntry), pick_cmp);
}

static void pick_open(int what)
{
    app.pick_for = (uint8_t)what;
    app.pick_back_sel = app.menu_sel;
    // it starts in the launcher's folder, or the one the music folder is in
    char start[sizeof(app.pick_dir)];
    const char *cur = what == PICK_LAUNCHER ? app.paths.launcher : music_root;
    snprintf(start, sizeof(start), "%s", cur[0] ? cur : "/");
    parent_dir(start);
    if (!is_dir(start))
        snprintf(start, sizeof(start), "/");
    app.picking = true;
    pick_load(start);
}

static void pick_close(void)
{
    app.picking = false;
    app.menu_sel = app.pick_back_sel;
    redraw = true;
}

// Up a folder (the one come from highlighted); at the top, back to the page.
static void pick_up(void)
{
    char dir[sizeof(app.pick_dir)], child[sizeof(app.pick_dir)];
    snprintf(dir, sizeof(dir), "%s", app.pick_dir);
    const char *s = strrchr(dir, '/');
    snprintf(child, sizeof(child), "%s", s ? s + 1 : "");
    if (!parent_dir(dir))
    {
        pick_close();
        return;
    }
    bool by_dpad = app.menu_sel >= 0;
    pick_load(dir);
    for (int i = 0; i < app.pick_n; i++)
        if (app.pick[i].dir && strcmp(app.pick_names + app.pick[i].name, child) == 0)
        {
            int row = ui_pick_first() + i;
            if (by_dpad)
                app.menu_sel = row;
            app.menu_vis = by_dpad ? row * SET_H : 0;
            keep_in_view(&app.pick_scroll, row * SET_H, SET_H, ui_pick_max_scroll());
        }
}

static void pick_choose(int i)
{
    int first = ui_pick_first();
    if (i < 0 || i >= ui_pick_rows())
        return;
    if (i < first)
    {
        if (app.pick_for == PICK_LAUNCHER)
        {
            app.paths.launcher[0] = 0;      // No launcher
            paths_save(&app.paths);
            pick_close();
        }
        else if (!app.pick_has_lib)
            toast("%s", txt(S_NO_LIBRARY_THERE));
        else
        {
            char dir[sizeof(app.pick_dir)];
            snprintf(dir, sizeof(dir), "%s", app.pick_dir);
            pick_close();
            music_switch(dir);
        }
        return;
    }
    const PickEntry *e = &app.pick[i - first];
    char full[sizeof(app.pick_dir)];
    if (!join_path(full, sizeof(full), app.pick_dir, app.pick_names + e->name))
        return;                         // too long a path
    if (e->dir)
        pick_load(full);
    else
    {
        snprintf(app.paths.launcher, sizeof(app.paths.launcher), "%s", full);
        paths_save(&app.paths);
        pick_close();
    }
}

static void pick_tap(int x, int y)
{
    if (y < 20)
    {
        if (x >= 224)
            app.overlay = OV_NONE;      // ×: out of the settings
        else if (x < 112)
            pick_up();                  // ‹
    }
    else if (y + app.pick_scroll >= SET_Y)
        pick_choose((y + app.pick_scroll - SET_Y) / SET_H);
    redraw = true;
}

static void pick_keys(uint32_t rep, uint32_t down)
{
    int n = ui_pick_rows(), first = ui_pick_first(), was = app.menu_sel;
    if (down & (KEY_B | KEY_LEFT))
        pick_up();
    else if ((down & KEY_RIGHT) && app.menu_sel >= first && app.menu_sel < n && app.pick[app.menu_sel - first].dir)
        pick_choose(app.menu_sel);
    else if (n > 0)
    {
        if (rep & KEY_UP) app.menu_sel = app.menu_sel <= 0 ? n - 1 : app.menu_sel - 1;
        if (rep & KEY_DOWN) app.menu_sel = app.menu_sel + 1 >= n ? 0 : app.menu_sel + 1;
        if (down & KEY_A) pick_choose(app.menu_sel);
    }
    if (app.menu_sel != was)
        pick_keep_sel();                // moved by D-pad: in sight (a dragged list stays)
}

static void set_change(int id)
{
    switch (id)
    {
    case SET_SCROLL_ALL: app.set.scroll_all ^= 1; break;
    case SET_CASE:
        app.set.case_sensitive ^= 1;
        lib_group_artists(app.set.case_sensitive);
        app.artist = -1;    // group numbers changed
        rebuild_list();
        select_index(0);
        break;
    case SET_PAUSE_LID: app.set.pause_on_lid ^= 1; break;
    case SET_AUTOPLAY: app.set.play_at_start ^= 1; break;
    case SET_BATT:
        app.set.batt_sound_off ^= 1;
        if (!app.set.batt_sound_off)
            player_chime();         // on: what it sounds like
        break;
    case SET_DITHER:
        app.set.dither ^= 1;
        eq_set_output(app.set.highcut, app.set.dither);
        break;
    case SET_LR_LID: app.set.lr_skip_lid ^= 1; break;
    case SET_RG:
        app.set.rg_mode = (app.set.rg_mode + 1) % RG_MODES;
        player_set_volume(replaygain_volume());
        break;
    case SET_TIME_RIGHT: app.set.show_remaining ^= 1; break;
    case SET_COVER_BG: app.set.plain_bg ^= 1; break;
    case SET_LANG:
        app.set.lang = (app.set.lang + 1) % LANG_COUNT;
        i18n_lang = app.set.lang;
        break;
    case SET_KEYBOARD: app.set.keyboard = (app.set.keyboard + 1) % KB_LAYOUTS; break;
    case SET_CLOCK: app.set.clock_12h ^= 1; break;
    case SET_DATE: app.set.date_fmt = (app.set.date_fmt + 1) % DATE_FORMATS; break;
    case SET_MUSIC_DIR: pick_open(PICK_MUSIC); return;
    case SET_LAUNCHER: pick_open(PICK_LAUNCHER); return;
    case SET_LAUNCH_BUTTON:
    case SET_LAUNCH_HOLD:
    {
        uint8_t *on = id == SET_LAUNCH_BUTTON ? &app.set.launch_button : &app.set.launch_hold;
        *on ^= 1;
        if (*on && !app.paths.launcher[0])
            toast("%s", txt(S_NO_LAUNCHER));    // no use yet: which .nds
        break;
    }
    case SET_SHUFFLE_W:
        app.set.shuffle_weighted ^= 1;
        apply_weighting();
        queue_reshuffle_rest();     // the songs still to come, in the new way
        app.thumb_track = LIB_NONE;
        break;
    default: return;
    }
    touch_state();
    redraw = true;
}

// ---- theme page
static uint16_t *theme_slot(int i)
{
    return i == 0 ? &app.theme.acc : (i == 1 ? &app.theme.bg : &app.theme.text);
}

static void theme_select(int i)
{
    app.theme_sel = (i + 3) % 3;
    app.theme_hsv = cp_rgb_to_hsv(*theme_slot(app.theme_sel), app.theme_hsv.h);
    redraw = true;
}

// The picker's colour into the theme: the whole app changes at once.
static void theme_apply(void)
{
    *theme_slot(app.theme_sel) = cp_hsv_to_rgb(app.theme_hsv);
    palette_make(&app.theme);
    touch_state();
    redraw = true;
}

static void presets_open(bool open)
{
    app.presets_open = open;
    app.menu_sel = -1;
    // the one in use is highlighted
    for (int i = 0; open && i < THEME_PRESETS; i++)
    {
        const Theme *t = &theme_presets[i].theme;
        if (t->bg == app.theme.bg && t->text == app.theme.text && t->acc == app.theme.acc)
            app.menu_sel = i;
    }
    redraw = true;
}

static void preset_choose(int i)
{
    if (i >= 0 && i < THEME_PRESETS)
    {
        app.theme = theme_presets[i].theme;
        palette_make(&app.theme);
        theme_select(app.theme_sel);
        touch_state();
    }
    presets_open(false);
}

// The stylus on the theme page (below the header). down: it just touched.
static bool theme_touch(int x, int y, bool down)
{
    if (down && app.presets_open)
    {
        // a preset, or anywhere else: closed again
        app.theme_drag = CP_NONE;
        int r = (y - PRESET_Y - 20) / 16;
        if (x >= PRESET_X0 && x < PRESET_X1 && y >= PRESET_Y + 20 && r < THEME_PRESETS)
            preset_choose(r);
        else
            presets_open(false);
        return true;
    }
    if (down)
    {
        app.theme_drag = CP_NONE;
        if (x >= THEME_CHIP_X && x < THEME_CHIP_X + THEME_CHIP_W)
        {
            for (int i = 0; i < 3; i++)
            {
                int cy = THEME_CHIP_Y + i * THEME_CHIP_PITCH;
                if (y >= cy - 3 && y < cy + THEME_CHIP_H + 3)
                {
                    theme_select(i);
                    return true;
                }
            }
            if (y >= THEME_PRESETS_Y && y < THEME_PRESETS_Y + THEME_CHIP_H)
            {
                presets_open(true);
                return true;
            }
        }
        app.theme_drag = cp_part_at(x, y);
    }
    if (app.theme_drag == CP_RING)
        app.theme_hsv.h = cp_hue_at(x, y);
    else if (app.theme_drag == CP_TRIANGLE)
        cp_sv_at(app.theme_hsv.h, x, y, &app.theme_hsv.s, &app.theme_hsv.v);
    else
        return false;
    theme_apply();
    return true;
}

static void set_open_page(int page)
{
    if (page == PAGE_THEME)
        theme_select(app.theme_sel);
    app.set_back_sel = app.menu_sel;
    // came by D-pad: the highlight starts on the first row (About has none)
    app.menu_sel = app.menu_sel >= 0 && ui_set_count(page) > 0 ? 0 : -1;
    app.set_page = app.set_shown = page;
    app.set_scroll[1] = 0;
    redraw = true;
}

static void set_back(void)
{
    if (app.set_page == PAGE_TOP)
        return;
    app.set_page = PAGE_TOP;
    app.menu_sel = app.set_back_sel;
    redraw = true;
}

// A row of the settings page that's showing: open its page or change it.
// The high cut's slider at a position: 0 .. 7 its edges (12.0 .. 15.5 kHz),
// 8 off (the right end).
static void highcut_to(int pos)
{
    pos = pos < 0 ? 0 : (pos > EQ_HIGHCUT_STEPS ? EQ_HIGHCUT_STEPS : pos);
    int k = pos == EQ_HIGHCUT_STEPS ? 0 : pos + 1;
    if (k == app.set.highcut)
        return;
    app.set.highcut = (uint8_t)k;
    eq_set_output(app.set.highcut, app.set.dither);
    touch_state();
    redraw = true;
}

static int highcut_pos(void)
{
    return app.set.highcut ? app.set.highcut - 1 : EQ_HIGHCUT_STEPS;
}

static void highcut_at(int x)
{
    int step = (SLIDER_X1 - SLIDER_X0) / EQ_HIGHCUT_STEPS;
    highcut_to((x - SLIDER_X0 + step / 2) / step);
}

static void set_activate(int i)
{
    if (i < 0 || i >= ui_set_count(app.set_page))
        return;
    const SetItem *it = ui_set_item(app.set_page, i);
    if (it->kind == SI_PAGE)
        set_open_page(it->id);
    else
        set_change(it->id);
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
    int rows = list_rows();
    int max_scroll = app.n_items > rows ? app.n_items - rows : 0;
    int h = rows * ROW_H;
    int s = (y - list_y()) * (max_scroll + 1) / h;
    if (s < 0) s = 0;
    if (s > max_scroll) s = max_scroll;
    app.scroll = s;
    if (app.sel < s) app.sel = s;
    if (app.sel >= s + rows) app.sel = s + rows - 1;
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
    if (app.overlay == OV_FILTER)
    {
        filter_tap(x, y);
        redraw = true;
        return;
    }
    if (app.overlay == OV_IM)
    {
        im_menu_tap(x, y);
        redraw = true;
        return;
    }
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
    if (app.overlay == OV_PL_ADD)
    {
        pl_picker_tap(x, y);
        return;
    }
    if (app.overlay == OV_PL_ASK)
    {
        int r = y - (PLASK_Y + 20);
        pl_ask_answer(x >= PLASK_X && x < PLASK_X + PLASK_W && r >= 0 && r < ROW_H);   // Delete, else Keep
        redraw = true;
        return;
    }
    if (app.overlay == OV_ROWMENU)
    {
        int row_y = list_y() + (app.menu_item - app.scroll) * ROW_H;
        int y0 = ui_rowmenu_y(row_y);
        if (x >= 88 && x < 240 && y >= y0 + 20 && y < y0 + 20 + 16 * app_row_count(app.menu_item))
            row_menu_choose((y - y0 - 20) / 16);
        else
            app.overlay = OV_NONE;
        redraw = true;
        return;
    }
    if (app.tab == TAB_SEARCH && tap_search(x, y))
    {
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
        else if (x >= 224)
        {
            app.overlay = OV_SETTINGS;
            app.menu_sel = app.set_back_sel = -1;
            app.set_page = app.set_shown = PAGE_TOP;
            app.page_t = 0;
            app.set_scroll[0] = app.set_scroll[1] = 0;
            app.picking = false;
            app.pick_t = 0;
        }
    }
    redraw = true;
}

// One of a song's artists (k: which of its artist tag's values) in the
// browser: their albums, the song's one selected.
static void browse_artist_of(uint32_t t, int k)
{
    if (t == LIB_NONE)
        return;
    uint32_t ids[16];
    int n = lib_tag_ids(t, "artist", ids, 16);
    int g = k >= 0 && k < n ? lib_value_group(ids[k]) : -1;
    if (g < 0)
        g = lib_track_group(t);
    if (g < 0)
        return;
    app.artist = g;
    app.album = LIB_NONE;
    show_tab(TAB_ALBUM);
    uint32_t album = lib_track(t)->album;
    for (int i = 0; i < app.n_items; i++)
        if (app.items[i].kind == ITEM_ALBUM && app.items[i].id == album)
        {
            select_index(i);
            break;
        }
    slide_to(false);
    redraw = true;
}

// ------------------------------------------------------------------ equalizer

static void eq_apply(void)
{
    eq_set(app.set.eq_gain);
    touch_state();
    redraw = true;
}

// A profile (0: Flat): its gains are heard within half a second.
static void eq_choose(int p)
{
    if (p < 0 || p > eqp_count())
        return;
    app.set.eq_profile = (uint8_t)p;
    memcpy(app.set.eq_gain, app_eq_profile_gain(p), EQ_BANDS);
    eq_apply();
}

static void eq_band_to(int b, int v)
{
    v = v < EQ_MIN ? EQ_MIN : (v > EQ_MAX ? EQ_MAX : v);
    if (app.set.eq_gain[b] == v)
        return;
    app.set.eq_gain[b] = (int8_t)v;
    eq_apply();
}

static int eq_db_at(int y)
{
    int d = EQ_ZERO_Y - y;
    return (d >= 0 ? d + EQ_DB_PX / 2 : d - EQ_DB_PX / 2) / EQ_DB_PX;
}

static int eq_band_at(int x)
{
    if (x < (EQ_BAND_X(0) + EQ_BAND_X(1)) / 2)
        return 0;
    return x < (EQ_BAND_X(1) + EQ_BAND_X(2)) / 2 ? 1 : 2;
}

static void eq_open(void)
{
    app.overlay = OV_EQ;
    app.eq_sliders = false;
    app.eq_drag_band = app.eq_drag_from = app.eq_drag_to = -1;
    app.eq_ask = false;
}

// Save: over the chosen profile; Flat changed: a new one, named first.
static void eq_save(void)
{
    if (!app_eq_edited())
        return;
    if (app.set.eq_profile > 0)
    {
        eqp_set_gain(app.set.eq_profile - 1, app.set.eq_gain);
        eqp_save();
        redraw = true;
        return;
    }
    if (eqp_count() >= EQP_MAX)
    {
        toast("%s", txt(S_EQ_FULL));
        return;
    }
    name_start(NAME_EQ);
}

static void eq_name_done(void)
{
    if (!app.eq_name[0])
        return;
    if (app.name_for != NAME_EQ)
    {
        pl_name_done();
        return;
    }
    int i = eqp_add(app.eq_name, app.set.eq_gain);
    if (i >= 0)
    {
        eqp_save();
        app.set.eq_profile = (uint8_t)(i + 1);
        touch_state();
    }
    eq_open();                      // back to the panel
}

static void eq_ask(void)
{
    if (app.set.eq_profile == 0)
        return;
    app.eq_ask = true;
    app.eq_ask_sel = 0;
}

static void eq_delete(void)
{
    eqp_remove(app.set.eq_profile - 1);
    eqp_save();
    app.eq_ask = false;
    eq_choose(0);
}

// A saved profile put in another place: the chosen one stays chosen.
static void eq_move(int from, int to)
{
    if (from == to)
        return;
    eqp_move(from, to);
    eqp_save();
    int c = app.set.eq_profile - 1;
    if (c == from)
        c = to;
    else if (c >= 0 && from < c && to >= c)
        c--;
    else if (c >= 0 && from > c && to <= c)
        c++;
    app.set.eq_profile = (uint8_t)(c + 1);
    touch_state();
}

static void tap_eq(int x, int y)
{
    redraw = true;
    if (app.eq_ask)
    {
        int ry = y - (EQ_ASK_Y + 20);
        if (x >= EQ_ASK_X && x < EQ_ASK_X + EQ_ASK_W && ry >= 0 && ry < ROW_H)
            eq_delete();
        else
            app.eq_ask = false;     // Keep, or beside the question
        return;
    }
    if (y < 20)
    {
        if (x >= 224)
            app.overlay = OV_NONE;
    }
    else if (x < EQ_LIST_W && y < EQ_LINE_Y)
    {
        if (y < EQ_OWN_Y - 1)
            eq_choose(0);
        else if (y >= EQ_OWN_Y && (y - EQ_OWN_Y) / ROW_H < eqp_count())
            eq_choose((y - EQ_OWN_Y) / ROW_H + 1);
    }
    else if (y >= EQ_LINE_Y)
    {
        int sx, sw, dx, dw;
        ui_eq_buttons(&sx, &sw, &dx, &dw);
        if (x >= sx && x < sx + sw)
            eq_save();
        else if (x >= dx && x < dx + dw)
            eq_ask();
    }
}

// ×, B: not saved, back to where it came from
static void name_cancel(void)
{
    if (app.name_for == NAME_EQ)
        eq_open();
    else
        app.overlay = app.name_for == NAME_PL_QUEUE ? OV_QUEUE : OV_NONE;
}

static void tap_eq_name(int x, int y)
{
    if (y < 20 && x >= 224)
        name_cancel();
    else if (y >= FIELD_Y && y < FIELD_Y + 20 && x >= 228)
        app.eq_name[0] = 0;
    redraw = true;
}

// D-pad: X switches between the profiles (Up / Down) and the sliders (Left /
// Right: which, Up / Down: 1 dB); A saves, Y deletes (asks first), B closes.
static void eq_keys(uint32_t down, uint32_t rep)
{
    if (app.eq_ask)
    {
        if (rep & (KEY_UP | KEY_DOWN))
            app.eq_ask_sel ^= 1;
        if (down & KEY_A)
        {
            if (app.eq_ask_sel == 0)
                eq_delete();
            else
                app.eq_ask = false;
        }
        if (down & KEY_B)
            app.eq_ask = false;
        return;
    }
    if (down & KEY_X)
        app.eq_sliders = !app.eq_sliders;
    if (app.eq_sliders)
    {
        if ((rep & KEY_LEFT) && app.eq_band > 0)
            app.eq_band--;
        if ((rep & KEY_RIGHT) && app.eq_band < EQ_BANDS - 1)
            app.eq_band++;
        if (rep & KEY_UP)
            eq_band_to(app.eq_band, app.set.eq_gain[app.eq_band] + 1);
        if (rep & KEY_DOWN)
            eq_band_to(app.eq_band, app.set.eq_gain[app.eq_band] - 1);
    }
    else if (rep & (KEY_UP | KEY_DOWN))
        eq_choose(app.set.eq_profile + ((rep & KEY_DOWN) ? 1 : -1));
    if (down & KEY_A)
        eq_save();
    if (down & KEY_Y)
        eq_ask();
    if (down & KEY_B)
        app.overlay = OV_NONE;
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
        int sy = INFO_Y + INFO_RATING_ROW * 16, sx = ui_info_value_x() - 1;
        if (y < 20 && x >= 224)
            app.overlay = OV_NONE;
        else if (y >= sy - 4 && y < sy + 20 && x >= sx && x < sx + 5 * INFO_STAR_W)
        {
            // a star: that many; the last lit one again: none
            int stars = (x - sx) / INFO_STAR_W + 1;
            uint32_t t = app_current_track();
            if (t != LIB_NONE)
                rate_current(app_song_rating(t) == stars ? 0 : stars);
        }
        redraw = true;
        return;
    }
    if (app.overlay == OV_QUEUE)
    {
        if (y < 20)
        {
            if (x >= 224)
                app.overlay = OV_NONE;
            else if (x >= Q_SAVE_X - 6 && x < Q_SAVE_X + 22 && queue_count() > 0)
                name_start(NAME_PL_QUEUE);  // 💾: the queue as a playlist
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
    if (app.overlay == OV_EQ)
    {
        tap_eq(x, y);
        return;
    }
    int who = ui_player_artist_at(x, y);
    if (who >= 0)
    {
        browse_artist_of(app_current_track(), who);
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
        if (x < 34) app.overlay = OV_INFO;
        else if (x < 62) eq_open();
        else if (x >= 104 && x < 152) slide_to(false);
        else if (x >= 208) { app.overlay = OV_QUEUE; app.q_sel = -1; app.q_drag_from = -1;
                             app.q_scroll = queue_pos() > 2 ? queue_pos() - 2 : 0; }
    }
    redraw = true;
}

static void tap_settings(int x, int y)
{
    if (app.picking)
    {
        if (app.pick_t == TAB_TICKS)
            pick_tap(x, y);
        return;
    }
    if (y < 20)
    {
        if (x >= 224)
            app.overlay = OV_NONE;
        else if (x >= LAUNCH_X - 8 && app.set.launch_button && app.paths.launcher[0])
            launch_start();             // 🚀
        else if (x < 112)
            set_back();                 // ‹
    }
    else if (ui_set_row_at(app.set_page, y) >= 0)
        set_activate(ui_set_row_at(app.set_page, y));
    redraw = true;
}

// The button under the stylus, to light it up.
static int button_at(int x, int y)
{
    if (app.overlay == OV_EQ && !app.eq_ask && app_panel_offset() == PANEL_DOWN && y >= EQ_LINE_Y
        && y < PANEL_DOWN)
    {
        int sx, sw, dx, dw;
        ui_eq_buttons(&sx, &sw, &dx, &dw);
        if (x >= sx && x < sx + sw) return BTN_EQ_SAVE;
        if (x >= dx && x < dx + dw) return BTN_EQ_DELETE;
    }
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
            if (x < 34) return BTN_INFO;
            if (x < 62) return BTN_EQ;
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

static int fdrag_from;              // filter choices: where the stylus started scrolling them

static int *set_scroll_now(void)
{
    return &app.set_scroll[app.set_page == PAGE_TOP ? 0 : 1];
}

// The settings page (or the picker) is in place, not sliding.
static bool settings_still(void)
{
    if (app.picking || app.pick_t)
        return app.picking && app.pick_t == TAB_TICKS;
    return app.page_t == (app.set_page != PAGE_TOP ? TAB_TICKS : 0);
}

static void touch_down(int x, int y)
{
    app.press_id = button_at(x, y);
    app.press_down = app.press_id != BTN_NONE;
    tp = (typeof(tp)){ true, x, y, x, y, ticks, false, false };
    int oy = app_panel_offset();
    if (kb_ready() && y >= KB_Y)
    {
        kb_touch_down(x, y);
        tp.held = true;             // keys are handled on their own, not as taps
        redraw = true;
        return;
    }
    fdrag_from = app.overlay == OV_PL_ADD ? app.pl_scroll
                 : (app.overlay == OV_SETTINGS ? (app.picking ? app.pick_scroll : *set_scroll_now())
                    : *fpage_scroll());
    if (app.overlay == OV_NONE && oy == 0 && x >= SCROLL_X && y >= list_y() && y < BAR_Y && app.n_items > list_rows()
        && !(app.tab == TAB_SEARCH && app.kb_up))
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
    else if (app.overlay == OV_SETTINGS && app.set_page == PAGE_THEME && app.page_t == TAB_TICKS && y >= 20
             && theme_touch(x, y, true))
        tp.held = true;
    else if (app.overlay == OV_SETTINGS && app.page_t == TAB_TICKS && !app.picking && ui_set_row_at(app.set_page, y) >= 0
             && ui_set_item(app.set_page, ui_set_row_at(app.set_page, y))->kind == SI_SLIDER)
    {
        // the slider: where the stylus is, then follows it
        app.slider_drag = true;
        highcut_at(x);
        tp.held = true;
    }
    else if (app.overlay == OV_EQ && oy == PANEL_DOWN && !app.eq_ask && x > EQ_LIST_W && y >= 20 && y < EQ_LINE_Y)
    {
        // a slider: where the stylus is, then follows it
        app.eq_drag_band = eq_band_at(x);
        eq_band_to(app.eq_drag_band, eq_db_at(y));
        tp.held = true;
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
    if (kb_pressing)
    {
        kb_touch_move(x, y);
        redraw = true;
        return;
    }
    if (app.overlay == OV_PL_ADD && tp.moved)
    {
        // drag the playlists
        int n = 1 + pl_count();
        int max = n > PLADD_ROWS ? n - PLADD_ROWS : 0;
        int s = fdrag_from + (tp.y0 - y) / 16;
        app.pl_scroll = s < 0 ? 0 : (s > max ? max : s);
        redraw = true;
        return;
    }
    if (app.overlay == OV_SETTINGS && tp.moved && !app.slider_drag && app.theme_drag == CP_NONE
        && tp.y0 >= 20 && settings_still())
    {
        // drag a page that's longer than the screen (or the picker)
        int max = app.picking ? ui_pick_max_scroll() : ui_set_max_scroll(app.set_page);
        int s = fdrag_from + (tp.y0 - y);
        *(app.picking ? &app.pick_scroll : set_scroll_now()) = s < 0 ? 0 : (s > max ? max : s);
        redraw = true;
        return;
    }
    if (app.overlay == OV_FILTER && app.fpage >= 0 && tp.moved)
    {
        // drag the choices
        int n = fpage_count();
        int max = n > FMENU_ROWS ? n - FMENU_ROWS : 0;
        int s = fdrag_from + (tp.y0 - y) / 16;
        *fpage_scroll() = s < 0 ? 0 : (s > max ? max : s);
        redraw = true;
        return;
    }
    if (app.scroll_drag)
        scroll_to_y(y);
    else if (app.seek_drag)
        app.seek_preview_ms = seek_ms_at(x);
    else if (app.theme_drag != CP_NONE)
        theme_touch(x, y, false);
    else if (app.eq_drag_band >= 0)
        eq_band_to(app.eq_drag_band, eq_db_at(y));
    else if (app.slider_drag)
        highcut_at(x);
    else if (app.eq_drag_from >= 0)
    {
        int to = y < EQ_OWN_Y ? 0 : (y - EQ_OWN_Y) / ROW_H;
        app.eq_drag_to = to >= eqp_count() ? eqp_count() - 1 : to;
    }
    else if (app.q_drag_from >= 0)
    {
        int to = queue_row_at(y < 20 ? 20 : y);
        if (y < 28 && app.q_scroll > 0) app.q_scroll--;
        if (y > 20 + ROWS * ROW_H - 8 && app.q_scroll + ROWS < queue_count()) app.q_scroll++;
        if (to >= queue_count()) to = queue_count() - 1;
        if (to < 0) to = 0;
        app.q_drag_to = to;
    }
    else if (app.pl_drag_from >= 0)
    {
        int rows = list_rows();
        if (y < list_y() + 8 && app.scroll > 1)
            app.scroll--;
        if (y >= list_y() + rows * ROW_H - 8 && app.scroll + rows < app.n_items)
            app.scroll++;
        int to = app.scroll + (y < list_y() ? 0 : (y - list_y()) / ROW_H);
        app.pl_drag_to = to < 1 ? 1 : (to >= app.n_items ? app.n_items - 1 : to);    // below ‹
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
        if (oy == 0 && app.overlay == OV_NONE && tp.y0 >= list_y() && tp.y0 < BAR_Y && tp.x0 < SCROLL_X
            && !(app.tab == TAB_SEARCH && app.kb_up))
        {
            int i = app.scroll + (tp.y0 - list_y()) / ROW_H;
            if (pl_draggable(i))
            {
                // a playlist's song: lifted; moved, it goes to another place, let go, its menu
                select_index(i);
                app.pl_drag_from = app.pl_drag_to = i;
            }
            else
                open_row_menu(i);
        }
        else if (oy == PANEL_DOWN && app.overlay == OV_QUEUE && tp.y0 >= 20 && tp.y0 < 20 + ROWS * ROW_H)
        {
            int i = queue_row_at(tp.y0);
            if (i < queue_count())
                app.q_drag_from = app.q_drag_to = i;
        }
        else if (oy == PANEL_DOWN && app.overlay == OV_EQ && !app.eq_ask && tp.x0 < EQ_LIST_W
                 && tp.y0 >= EQ_OWN_Y && (tp.y0 - EQ_OWN_Y) / ROW_H < eqp_count())
            app.eq_drag_from = app.eq_drag_to = (tp.y0 - EQ_OWN_Y) / ROW_H;     // Flat stays on top
    }
    redraw = true;
}

static void touch_up(void)
{
    tp.down = false;
    app.theme_drag = CP_NONE;
    if (app.press_down)
    {
        app.press_down = false;
        app.press_until = app.frame + 6;    // stays lit a moment after the tap
    }
    if (kb_pressing)
    {
        kb_touch_up();
        return;
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
    else if (app.pl_drag_from >= 0)
    {
        int from = app.pl_drag_from, to = app.pl_drag_to;
        app.pl_drag_from = app.pl_drag_to = -1;
        if (to != from)
        {
            pl_move(app.pl, from - 1, to - 1);      // the row above the songs is ‹
            refresh_list();
            select_index(to);
            app.sel_vis = (app.sel - app.scroll) * ROW_H;
        }
        else
            open_row_menu(from);
    }
    else if (app.eq_drag_band >= 0)
        app.eq_drag_band = -1;
    else if (app.slider_drag)
        app.slider_drag = false;
    else if (app.eq_drag_from >= 0)
    {
        eq_move(app.eq_drag_from, app.eq_drag_to);
        app.eq_drag_from = app.eq_drag_to = -1;
    }
    else if (!tp.held && !tp.moved)
    {
        int oy = app_panel_offset();
        if (app.overlay == OV_SETTINGS)
            tap_settings(tp.x0, tp.y0);
        else if (app.overlay == OV_EQ_NAME)
            tap_eq_name(tp.x0, tp.y0);
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

// The D-pad and buttons on a settings page.
static void settings_keys(uint32_t down, uint32_t rep)
{
    if (app.set_page == PAGE_THEME)
    {
        // Up / Down: which colour, Left / Right: its hue (the triangle is for
        // the stylus), A: the presets
        if (app.presets_open)
        {
            if (down & KEY_B)
                presets_open(false);
            else
                menu_keys(rep, down, THEME_PRESETS, preset_choose);
        }
        else if (down & KEY_A)
            presets_open(true);
        else if (down & KEY_B)
            set_back();
        else if (rep & (KEY_UP | KEY_DOWN))
            theme_select(app.theme_sel + ((rep & KEY_DOWN) ? 1 : -1));
        else if (rep & (KEY_LEFT | KEY_RIGHT))
        {
            app.theme_hsv.h = (app.theme_hsv.h + ((rep & KEY_RIGHT) ? 16 : -16) + CP_HUES) % CP_HUES;
            theme_apply();
        }
    }
    else if (app.menu_sel >= 0 && app.menu_sel < ui_set_count(app.set_page)
             && ui_set_item(app.set_page, app.menu_sel)->kind == SI_SLIDER && (rep & (KEY_LEFT | KEY_RIGHT)))
        highcut_to(highcut_pos() + ((rep & KEY_RIGHT) ? 1 : -1));     // the slider: Left reaches lower
    else if (app.set_page != PAGE_TOP && (down & (KEY_B | KEY_LEFT)))
        set_back();
    else if (ui_set_count(app.set_page) > 0)
    {
        if ((down & KEY_RIGHT) && app.menu_sel >= 0
            && ui_set_item(app.set_page, app.menu_sel)->kind == SI_PAGE)
            set_activate(app.menu_sel);
        else
            menu_keys(rep, down, ui_set_count(app.set_page), set_activate);
    }
}

// START: pause. With Settings › Misc › Hold START, held for a second it
// starts the launcher instead: a short press pauses when it's let go.
static void start_key(uint32_t down, uint32_t held)
{
    static bool pressed, used, hinted;
    static uint32_t since;
    if (down & KEY_START)
    {
        if (!(app.set.launch_hold && app.paths.launcher[0]))
        {
            toggle_pause();
            flash(app.player_mode ? BTN_PPLAY : BTN_PLAY);
            return;
        }
        pressed = true;
        used = hinted = false;
        since = ticks;
    }
    if (!pressed)
        return;
    if (!(held & KEY_START))
    {
        pressed = false;
        if (!used)
        {
            toggle_pause();
            flash(app.player_mode ? BTN_PPLAY : BTN_PLAY);
        }
    }
    else if (!used && ticks - since >= HOLD_START)
    {
        used = true;
        launch_start();
    }
    else if (!used && !hinted && ticks - since >= HOLD_START / 3)
    {
        hinted = true;
        toast(txt(S_KEEP_HOLDING), path_basename(app.paths.launcher));
    }
}

static uint32_t vert_since;

static void handle_keys(uint32_t down, uint32_t rep, uint32_t held)
{
    if (down & KEY_L) { previous_song(); if (app.player_mode) flash(BTN_PREV); }
    if (down & KEY_R) { next_song(false); flash(app.player_mode ? BTN_PNEXT : BTN_NEXT); }
    start_key(down, held);
    if (down | rep)
        redraw = true;

    if (!(held & (KEY_UP | KEY_DOWN)))
        vert_since = ticks;
    int step = dpad_step(ticks - vert_since);

    switch (app.overlay)
    {
    case OV_SETTINGS:
    {
        int sel_was = app.menu_sel, page_was = app.set_page;
        if (app.picking)
        {
            if (app.pick_t == TAB_TICKS)
                pick_keys(rep, down);
            return;
        }
        settings_keys(down, rep);
        // the highlighted row stays in sight on a page longer than the screen
        if (app.overlay == OV_SETTINGS && !app.picking && app.set_page == page_was && app.menu_sel != sel_was
            && app.menu_sel >= 0 && app.menu_sel < ui_set_count(app.set_page))
            keep_in_view(set_scroll_now(), ui_set_row_y(app.set_page, app.menu_sel) - SET_Y,
                         ui_set_row_h(app.set_page, app.menu_sel), ui_set_max_scroll(app.set_page));
        return;
    }
    case OV_SORT:
        menu_keys(rep, down, ui_sort_option_count(ui_sort_slot()), choose_sort);
        return;
    case OV_ROWMENU:
        menu_keys(rep, down, app_row_count(app.menu_item), row_menu_choose);
        return;
    case OV_PL_ADD:
    {
        int count = 1 + pl_count();
        if (rep & KEY_UP) app.menu_sel = app.menu_sel <= 0 ? count - 1 : app.menu_sel - 1;
        if (rep & KEY_DOWN) app.menu_sel = app.menu_sel + 1 >= count ? 0 : app.menu_sel + 1;
        pl_picker_scroll_to_sel();
        if (down & KEY_A) pl_picker_choose(app.menu_sel);
        if (down & KEY_B) app.overlay = OV_NONE;
        return;
    }
    case OV_PL_ASK:
        if (rep & (KEY_UP | KEY_DOWN)) app.eq_ask_sel ^= 1;
        if (down & KEY_A) pl_ask_answer(app.eq_ask_sel == 0);
        if (down & KEY_B) pl_ask_answer(false);
        return;
    case OV_IM:
        menu_keys(rep, down, IM_COUNT, im_menu_choose);
        return;
    case OV_FILTER:
    {
        int count = fpage_count();
        if ((rep & (KEY_UP | KEY_DOWN)) && count > 0)
        {
            if (app.menu_sel < 0)
                app.menu_sel = 0;
            else if (rep & KEY_UP)
                app.menu_sel = app.menu_sel <= 0 ? count - 1 : app.menu_sel - 1;
            else
                app.menu_sel = app.menu_sel + 1 >= count ? 0 : app.menu_sel + 1;
            filter_scroll_to_sel();
        }
        if ((down & (KEY_A | KEY_RIGHT)) && app.menu_sel >= 0)
        {
            // Right goes into a page (not "Search in", not a choice)
            bool into = app.fpage < 0 ? app.menu_sel != FM_IN : app.fpage == F_TAGS;
            if ((down & KEY_A) || into)
                filter_pick(app.menu_sel);
        }
        if (down & (KEY_B | KEY_LEFT))
            filter_back();
        return;
    }
    case OV_INFO:
    {
        uint32_t t = app_current_track();
        if (t != LIB_NONE && (rep & (KEY_LEFT | KEY_RIGHT)))
            rate_current(app_song_rating(t) + ((rep & KEY_RIGHT) ? 1 : -1));
        if (down & (KEY_B | KEY_X)) app.overlay = OV_NONE;
        return;
    }
    case OV_EQ:
        eq_keys(down, rep);
        return;
    case OV_EQ_NAME:
        if (down & KEY_B)
            name_cancel();          // not saved
        if (down & KEY_A)
            eq_name_done();
        return;
    case OV_QUEUE:
    {
        int n = queue_count();
        if (app.q_sel >= n) app.q_sel = n - 1;      // songs were removed
        // only keys move the selection and the view: the scroll bar's
        // position stays when it's let go
        if ((rep & (KEY_UP | KEY_DOWN)) || (down & (KEY_A | KEY_X)))
        {
            if (app.q_sel < 0) app.q_sel = queue_pos() < 0 ? 0 : queue_pos();
            if (rep & KEY_UP) app.q_sel -= step;
            if (rep & KEY_DOWN) app.q_sel += step;
            if (app.q_sel >= n) app.q_sel = n - 1;
            if (app.q_sel < 0) app.q_sel = 0;
            if (rep & (KEY_UP | KEY_DOWN))
            {
                if (app.q_sel < app.q_scroll) app.q_scroll = app.q_sel;
                if (app.q_sel >= app.q_scroll + ROWS) app.q_scroll = app.q_sel - ROWS + 1;
            }
        }
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
    if (app.tab == TAB_SEARCH)
    {
        // while typing the first press shows the highlight; header rows are stepped over
        if (rep & (KEY_UP | KEY_DOWN))
        {
            if (app.kb_up && !app.sel_on)
                app.sel_on = true;
            else
                search_step((rep & KEY_UP) ? -step : step);
        }
        if ((down & KEY_LEFT) && app.overlay == OV_NONE) show_tab(TAB_FOLDER);
        if (down & KEY_A)
        {
            if (app.kb_up && !app.sel_on)
                app.sel_on = app.n_items > 0;
            else
                activate(app.sel);
        }
        if ((down & KEY_Y) && !app.kb_up) open_row_menu(app.sel);
        if ((down & KEY_X) && !app.kb_up) toggle_add_mode();
        if (down & KEY_B)
        {
            if (app.kb_up) kb_lower();
            else slide_to(true);
        }
        if (down & KEY_SELECT) slide_to(true);
        return;
    }
    if (rep & KEY_UP) select_index(app.sel - step);
    if (rep & KEY_DOWN) select_index(app.sel + step);
    // left / right: the tab next to this one (no wrapping at the ends)
    if ((down & KEY_LEFT) && app.tab > TAB_ARTIST) show_tab(app.tab - 1);
    if ((down & KEY_RIGHT) && app.tab < TAB_SEARCH) show_tab(app.tab + 1);
    if (down & KEY_A) activate(app.sel);
    if (down & KEY_Y) open_row_menu(app.sel);
    if (down & KEY_X) toggle_add_mode();
    // B and SELECT: to the player and back (Left / Right do the tabs)
    if (down & (KEY_B | KEY_SELECT)) slide_to(true);
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
    state_save(&app.set, &app.theme, &r);
    last_save = ticks;
    save_at = 0;
}

// ---- starting the launcher (Settings › Misc): the music and the screens
// fade out, everything is saved, then Pico Loader starts the .nds (boot.c).

static void launch_start(void)
{
    struct stat st;
    if (app.launch_t)
        return;
    if (!app.paths.launcher[0] || stat(app.paths.launcher, &st) != 0)
    {
        toast("%s", txt(S_NO_LAUNCHER));
        return;
    }
    if (!boot_ready())
    {
        toast("%s", txt(S_NO_PICO));
        return;
    }
    toast(txt(S_LAUNCHING), path_basename(app.paths.launcher));
    player_fade_out(LAUNCH_FADE);
    app.launch_t = 1;
}

static void launch_tick(uint32_t elapsed)
{
    if (!app.launch_t)
        return;
    app.launch_t += (int)elapsed;
    int t = app.launch_t > LAUNCH_FADE ? LAUNCH_FADE : app.launch_t;
    setBrightness(3, -16 * t / LAUNCH_FADE);
    if (app.launch_t <= LAUNCH_FADE + 1)
        return;                         // a frame in black first
    if (player_state() == PLAYER_PLAYING)
        player_toggle_pause();          // silent already: where it was, for next time
    save_now();
    stats_save();
    boot_nds(app.paths.launcher);
    // still here: it couldn't start; the app comes back as it was
    app.launch_t = 0;
    player_fade_out(0);
    lid_fade_in = 0;
    toast("%s", txt(S_BOOT_FAILED));
    redraw = true;
}

static void restore(void)
{
    Resume r;
    state_defaults(&app.set);
    bool ok = state_load(&app.set, &app.theme, &r);
    i18n_lang = app.set.lang;
    // the equalizer as it was (a profile that's gone: Flat)
    if (app.set.eq_profile > eqp_count())
        app.set.eq_profile = 0;
    for (int b = 0; b < EQ_BANDS; b++)
        if (app.set.eq_gain[b] < EQ_MIN || app.set.eq_gain[b] > EQ_MAX)
            app.set.eq_gain[b] = 0;
    eq_set(app.set.eq_gain);
    if (app.set.highcut > EQ_HIGHCUT_STEPS)
        app.set.highcut = 0;
    app.set.dither = app.set.dither ? 1 : 0;
    eq_set_output(app.set.highcut, app.set.dither);
    palette_make(&app.theme);
    lib_group_artists(app.set.case_sensitive);
    if (!ok || !r.path[0] || !lib_loaded())
        return;
    app.album_queue = r.album_queue;
    uint32_t t = lib_find_path(r.path);
    if (t == LIB_NONE)
        return;
    // on launch: the last song at the same position, paused (or playing:
    // Settings › Playback › Play on launch)
    bool play = app.set.play_at_start;
    if (play)
        player_fade_in(START_FADE);     // the music fades in, like the screens
    if (queue_pos() >= 0 && queue_track(queue_pos()) == t)
    {
        open_pos(queue_pos(), r.pos_ms, play, SA_NONE);
        return;
    }
    queue_set(&t, 1, 0);
    open_pos(0, r.pos_ms, play, SA_NONE);
}

// ------------------------------------------------------------------ setup

static void fatal_screen(const char *line1, const char *line2)
{
    Canvas *c = gfx_canvas(SCREEN_TOP);
    gfx_fill(c, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    font_draw(c, 8, 8, 240, APP_NAME, C_ACC);
    font_draw(c, 8, 40, 240, line1, C_TEXT);
    font_draw(c, 8, 56, 240, line2, C_DIM);
    font_draw(c, 8, 160, 240, txt(S_PRESS_START), C_DIM);
    gfx_fill(gfx_canvas(SCREEN_BOTTOM), 0, 0, SCREEN_W, SCREEN_H, C_BG);
    for (int f = 0;; f++)
    {
        swiWaitForVBlank();
        gfx_present();
        if (f <= LID_IN)
            setBrightness(3, -16 + 16 * f / LID_IN);    // fading in, like the app
        scanKeys();
        if (keysDown() & KEY_START)
            exit(0);
    }
}

// The music folder: the one chosen in Settings › Browsing, else /music.
static bool find_music_root(void)
{
    static const char *candidates[] = { "/music", "fat:/music", "sd:/music" };
    if (app.paths.music[0] && is_dir(app.paths.music))
    {
        snprintf(music_root, sizeof(music_root), "%s", app.paths.music);
        return true;
    }
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        if (is_dir(candidates[i]))
        {
            snprintf(music_root, sizeof(music_root), "%s", candidates[i]);
            return true;
        }
    music_root[0] = 0;
    return false;
}

// The music folder's font and library (none found: the list says how to
// get music onto the card), and room in the lists for its songs.
static void load_library(void)
{
    char path[300];
    app.lib_error = LIB_ERR_NO_FOLDER;
    if (music_root[0])
    {
        snprintf(path, sizeof(path), "%s/.dsmusic/font.bin", music_root);
        font_load(path);
        snprintf(path, sizeof(path), "%s/.dsmusic/library.idx", music_root);
        lib_load(path, &app.lib_error);
    }
    // room for any list: in search, every artist, album and song can be a result
    item_cap = 2 * (int)lib_track_count() + (int)lib_album_count() + 8;
    tracks_cap = (int)lib_track_count() + 1;
    free(app.items);
    free(app.marks);
    free(tracks_buf);
    free(app.added);
    app.items = malloc(item_cap * sizeof(Item));
    app.marks = malloc(item_cap * sizeof(SearchMark));
    tracks_buf = malloc(tracks_cap * sizeof(uint32_t));
    app.added = calloc((lib_track_count() + 8) / 8, 1);
    app.n_items = 0;
}

// Another music folder (checked: it has a library.idx), right away: what
// played stops, the settings close and the lists start over at its artists.
static void music_switch(const char *dir)
{
    char d[sizeof(music_root)];
    snprintf(d, sizeof(d), "%s", dir);
    player_stop();
    queue_clear();
    stats_save();
    lib_unload();
    snprintf(music_root, sizeof(music_root), "%s", d);
    snprintf(app.paths.music, sizeof(app.paths.music), "%s", d);
    paths_save(&app.paths);
    load_library();
    lib_group_artists(app.set.case_sensitive);
    apply_weighting();
    ui_library_changed();
    app.artist = -1;
    app.album = LIB_NONE;
    app.folder[0] = 0;
    app.pl = PL_FOLDERS;
    app.album_queue = false;
    app.add_mode = false;
    app.thumb_track = LIB_NONE;
    app.menu_item = -1;
    app.sa_kind = SA_NONE;
    sa_captured = false;
    search_filter_clear(&app.sf);
    app.fpage = app.fkind = app.fbase = -1;
    pl_load();
    show_tab(TAB_ARTIST);
    toast(txt(S_MUSIC_NOW), path_basename(d));
    save_now();
}

static void app_init(void)
{
    paths_load(&app.paths);
    if (!music_root[0])
        find_music_root();
    load_library();
    app.fopts = malloc(FOPTS_MAX * sizeof(SearchOpt));
    app.tkeys = malloc(256 * sizeof(SearchOpt));
    search_filter_clear(&app.sf);
    app.key_down = app.acc_key = app.key_flash = -1;
    app.fpage = app.fkind = app.fbase = -1;
    app.pl = PL_FOLDERS;
    pl_load();
    app.artist = -1;
    app.album = LIB_NONE;
    app.q_drag_from = app.q_drag_to = -1;
    app.pl_drag_from = app.pl_drag_to = -1;
    app.eq_drag_band = app.eq_drag_from = app.eq_drag_to = -1;
    app.thumb_track = LIB_NONE;
    app.menu_item = -1;
    app.sa_cover = malloc(128 * 128 * 2);

    stats_load();
    eqp_load();
    ui_init();
    restore();
    apply_weighting();
    systemSetBacklightLevel(app.set.brightness);
    show_tab(TAB_ARTIST);
    last_save = ticks;
    // the screens fade in from black, like opening the lid
    setBrightness(3, -16);
    lid_fade_in = 0;
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
// ---- low battery: a chime over the music (the DS is often closed in a
// pocket, where the red LED isn't seen), when it gets low and again every
// few minutes while it stays low, and a toast.

#define BATT_EVERY      300                 // vblanks between looks: 5 s
#define BATT_AGAIN      (5 * 60 * 60)       // the warning again after 5 minutes

static uint32_t batt_looked, batt_warned;
static bool batt_pending;                   // warned while nothing played: the chime when it does

static void battery_look(void)
{
    if (!batt_looked || ticks - batt_looked >= BATT_EVERY)
    {
        batt_looked = ticks ? ticks : 1;
        u32 lvl = getBatteryLevel();
        bool low = !(lvl & BATTERY_CHARGER_CONNECTED) && (lvl & BATTERY_LEVEL_MASK) <= BATTERY_LEVEL_DS_LOW;
        if (low != app.batt_low)
        {
            app.batt_low = low;
            batt_pending = false;
            redraw = true;
            if (low)
                batt_warned = ticks - BATT_AGAIN;   // right away
        }
        if (low && ticks - batt_warned >= BATT_AGAIN)
        {
            batt_warned = ticks;
            toast("%s", txt(S_BATT_LOW));
            batt_pending = true;
        }
    }
    if (batt_pending && player_state() == PLAYER_PLAYING)
    {
        batt_pending = false;
        if (!app.set.batt_sound_off)
            player_chime();
    }
}

static void app_tick(void)
{
    uint32_t now = vblank_count;
    uint32_t elapsed = now - last_vblank ? now - last_vblank : 1;
    last_vblank = now;
    ticks = now;
    player_tick(elapsed);
    battery_look();
    scanKeys();
    uint32_t down = keysDown(), rep = keysDownRepeat(), held = keysHeld();

    handle_lid(held, down);
    launch_tick(elapsed);
    if (!lid_closed && !app.launch_t)
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

    count_plays(elapsed);
    if (stats_save_at && ticks >= stats_save_at)
    {
        stats_save();
        stats_save_at = 0;
    }
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
    // settings pages slide in over the top page and back out
    int page_target = app.set_page != PAGE_TOP ? TAB_TICKS : 0;
    if (app.page_t != page_target)
    {
        if (app.page_t < page_target)
            app.page_t = app.page_t + (int)elapsed > page_target ? page_target : app.page_t + (int)elapsed;
        else
            app.page_t = app.page_t - (int)elapsed < 0 ? 0 : app.page_t - (int)elapsed;
        redraw = true;
    }

    // the picker slides in over its page and back out; gone with the settings
    if (app.ov_vis != OV_SETTINGS && (app.picking || app.pick_t))
        app.picking = false, app.pick_t = 0;
    int pick_target = app.picking ? TAB_TICKS : 0;
    if (app.pick_t != pick_target)
    {
        app.pick_t += app.pick_t < pick_target ? (int)elapsed : -(int)elapsed;
        app.pick_t = app.pick_t < 0 ? 0 : (app.pick_t > TAB_TICKS ? TAB_TICKS : app.pick_t);
        redraw = true;
    }
    if (!app.picking && app.pick_t == 0 && app.pick)
    {
        free(app.pick);
        free(app.pick_names);
        app.pick = NULL;
        app.pick_names = NULL;
        app.pick_n = 0;
    }

    // the preset themes rise out of their button (and are closed with the page)
    if (app.presets_open && (app.overlay != OV_SETTINGS || app.set_page != PAGE_THEME))
        app.presets_open = false;
    {
        int target = app.presets_open ? OV_TICKS : 0;
        if (app.presets_t != target)
        {
            app.presets_t += app.presets_t < target ? (int)elapsed : -(int)elapsed;
            if (app.presets_t > OV_TICKS) app.presets_t = OV_TICKS;
            if (app.presets_t < 0) app.presets_t = 0;
            redraw = true;
        }
    }

    // search: the keyboard comes up once the search page is in place, the
    // results move with it; while typing they come up on the top screen
    {
        int kb_target = app.tab == TAB_SEARCH && app.kb_up && app.tab_anim == 0 ? KB_TICKS : 0;
        if (app.kb_t != kb_target)
        {
            app.kb_t += app.kb_t < kb_target ? (int)elapsed : -(int)elapsed;
            if ((kb_target && app.kb_t > KB_TICKS) || (!kb_target && app.kb_t < 0))
                app.kb_t = kb_target;
            redraw = true;
        }
        int sheet_target = app.tab == TAB_SEARCH && app.kb_up && search_has_content() ? SHEET_TICKS : 0;
        if (app.tab == TAB_SEARCH && !app.kb_up && app.kb_t > 0)
            sheet_target = app.sheet_t;     // goes down with the keyboard
        else if (app.tab == TAB_SEARCH && !app.kb_up)
            app.sheet_t = 0;                // on the bottom screen now
        if (app.sheet_t != sheet_target)
        {
            app.sheet_t += app.sheet_t < sheet_target ? (int)elapsed : -(int)elapsed;
            if (app.sheet_t > SHEET_TICKS) app.sheet_t = SHEET_TICKS;
            if (app.sheet_t < 0) app.sheet_t = 0;
            redraw = true;
        }
        if (app.im_t > 0)
        {
            app.im_t = app.im_t > (int)elapsed ? app.im_t - (int)elapsed : 0;
            redraw = true;
        }
        if (app.acc_key >= 0 && app.acc_t < ACC_TICKS)
        {
            app.acc_t = app.acc_t + (int)elapsed > ACC_TICKS ? ACC_TICKS : app.acc_t + (int)elapsed;
            redraw = true;
        }
        if (app.acc_key >= 0 && app.acc_sel >= 0)
        {
            const Key *k = &kb_keys[app.acc_key];
            int bx, by, bw;
            kb_forms_box(k, forms_count(k), &bx, &by, &bw);
            redraw |= glide(&app.acc_vis, bx + 2 + app.acc_sel * 25, elapsed, 1000);
        }
        int fp_target = app.fpage >= 0 && app.fpage == app.fkind ? TAB_TICKS : 0;
        if (app.fpage_t != fp_target)
        {
            app.fpage_t += app.fpage_t < fp_target ? (int)elapsed : -(int)elapsed;
            if (app.fpage_t > TAB_TICKS) app.fpage_t = TAB_TICKS;
            if (app.fpage_t < 0) app.fpage_t = 0;
            redraw = true;
        }
        for (int k = 0; k < F_COUNT; k++)
        {
            int w = search_filter_on(&app.sf, k) ? ui_chip_full_w(k) : 0;
            if (app.chip_w[k] != w && glide(&app.chip_w[k], w, elapsed, 1000))
                redraw = true;
        }
        static bool flash_was;
        bool flashing = app.frame < app.key_flash_until;
        if (flashing != flash_was)
            redraw = true;
        flash_was = flashing;
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
    redraw |= glide(&app.sel_vis, (app.sel - app.scroll) * ROW_H, elapsed, list_rows() * ROW_H);
    static int menu_was = -1, q_was = -1, page_was = PAGE_TOP;
    bool presets = app.ov_vis == OV_SETTINGS && app.set_page == PAGE_THEME && app.presets_open;
    int menu_target = app.ov_vis == OV_SETTINGS && app.picking ? app.menu_sel * SET_H
                      : app.ov_vis == OV_SETTINGS && !presets
                      ? (app.menu_sel >= 0 && app.menu_sel < ui_set_count(app.set_page)
                         ? ui_set_row_y(app.set_page, app.menu_sel) - SET_Y : app.menu_sel * SET_H)
                      : (app.ov_vis == OV_FILTER ? (app.menu_sel - *fpage_scroll()) * 16
                         : (app.ov_vis == OV_PL_ADD ? (app.menu_sel - app.pl_scroll) * 16 : app.menu_sel * 16));
    static int fpage_was = -1;
    static bool picking_was;
    bool page_changed = app.set_page != page_was || app.fpage != fpage_was || app.picking != picking_was;
    page_was = app.set_page;
    fpage_was = app.fpage;
    picking_was = app.picking;
    if (menu_opened || page_changed || app.menu_sel < 0 || menu_was < 0)
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
    bool blink = app.tab == TAB_SEARCH && app.kb_up && app.frame / 15 != prev_frame / 15;
    if (px != last_px || pos / 1000 != last_sec || app.frame / 30 != prev_frame / 30 || blink)
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
    palette_make(&theme_default);
    // the sound hardware (ARM7 side) reads the ring buffers from main RAM: give
    // it priority, so copying whole screens around can't make it wait
    REG_EXMEMCNT |= ARM7_MAIN_RAM_PRIORITY;
    setBrightness(3, -16);      // black until the first frame is ready, then it fades in
    gfx_init();
    player_init();
    keysSetRepeat(15, 4);
    disableSleep();
    irqSet(IRQ_VBLANK, on_vblank);
    irqEnable(IRQ_VBLANK);

    if (!fatInitDefault())
        fatal_screen(txt(S_NO_SD), txt(S_DLDI));
    app_init();         // no music folder: it starts all the same, the list says what to do

    while (1)
    {
        swiWaitForVBlank();
        if (!lid_closed)
            gfx_present();
        app_tick();
    }
}
#endif
