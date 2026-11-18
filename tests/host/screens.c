// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the real app (main.c) on the PC: taps, holds and drags through the
// UI like a person would, and writes screenshots of both screens.
//
//   ./screens /path/to/card/music out_dir

#include <stdio.h>

#define DSPLAYER_NO_MAIN
#include "../../arm9/source/main.c"

#include "hostsim.h"

static const char *out_dir;
static int16_t blk_l[4096], blk_r[4096];
static uint32_t play_clock;     // vblanks since the last simulated block
static int failures;
static bool film_on;        // ./screens card out frames: also write every frame of the song changes

// One vblank. The simulated ARM7 finishes a 4096-frame block every 7.5 vblanks.
static void tick(uint32_t held)
{
    vblank_count++;
    sim_set_keys(0, held);
    app_tick();
    play_clock += 2;
    if (play_clock >= 15)
    {
        play_clock -= 15;
        sim_play_block(blk_l, blk_r);
    }
}

static void run(int n)
{
    for (int i = 0; i < n; i++)
        tick(0);
}

static void press(uint32_t key)
{
    vblank_count++;
    sim_set_keys(key, key);
    app_tick();
    run(2);
}

static void tap(int x, int y)
{
    sim_set_touch(x, y);
    tick(KEY_TOUCH);
    tick(KEY_TOUCH);
    run(2);
}

static void hold(int x, int y, int ticks)
{
    sim_set_touch(x, y);
    for (int i = 0; i < ticks; i++)
        tick(KEY_TOUCH);
}

static void drag_to(int x, int y, int steps)
{
    int x0 = tp.x, y0 = tp.y;
    for (int i = 1; i <= steps; i++)
    {
        sim_set_touch(x0 + (x - x0) * i / steps, y0 + (y - y0) * i / steps);
        tick(KEY_TOUCH);
    }
}

static void release(void)
{
    run(2);
}

static void shot(const char *name)
{
    // draw now, whatever the frame timing
    redraw = true;
    ui_draw_top();
    ui_draw_bottom();
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n256 400\n255\n");
    const uint16_t *s[2] = { gfx_canvas(SCREEN_TOP)->px, gfx_canvas(SCREEN_BOTTOM)->px };
    for (int y = 0; y < 400; y++)
    {
        for (int x = 0; x < 256; x++)
        {
            uint16_t p = 0;
            if (y < 192) p = s[0][y * 256 + x];
            else if (y >= 208) p = s[1][(y - 208) * 256 + x];
            int r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
            if (y >= 192 && y < 208) r = g = b = 3;
            fputc((r << 3) | (r >> 2), f);
            fputc((g << 3) | (g >> 2), f);
            fputc((b << 3) | (b >> 2), f);
        }
    }
    fclose(f);
}

// Run n vblanks; with filming on, save the top (or bottom) screen of every one.
static void film_screen(const char *prefix, int n, Screen which)
{
    for (int i = 0; i < n; i++)
    {
        tick(0);
        if (!film_on)
            continue;
        if (which == SCREEN_TOP)
            ui_draw_top();
        else
            ui_draw_bottom();
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_%03d.ppm", out_dir, prefix, i);
        FILE *f = fopen(path, "wb");
        fprintf(f, "P6\n256 192\n255\n");
        const uint16_t *s = gfx_canvas(which)->px;
        for (int k = 0; k < 256 * 192; k++)
        {
            int r = s[k] & 31, g = (s[k] >> 5) & 31, b = (s[k] >> 10) & 31;
            fputc((r << 3) | (r >> 2), f);
            fputc((g << 3) | (g >> 2), f);
            fputc((b << 3) | (b >> 2), f);
        }
        fclose(f);
    }
}

static void film(const char *prefix, int n)
{
    film_screen(prefix, n, SCREEN_TOP);
}

static void check(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: %s\n", what);
        failures++;
    }
    else
        printf("ok:   %s\n", what);
}

static int find_row(const char *label)
{
    for (int i = 0; i < app.n_items; i++)
    {
        char buf[256];
        ui_item_label(&app.items[i], buf, sizeof(buf));
        if (strcmp(buf, label) == 0)
            return i;
    }
    return -1;
}

// Tap the row with this label, scrolling it into view first.
static void tap_row(const char *label)
{
    int i = find_row(label);
    if (i < 0)
    {
        printf("FAIL: no row %s\n", label);
        failures++;
        return;
    }
    app.scroll = i - 2 < 0 ? 0 : i - 2;
    if (app.n_items > ROWS && app.scroll > app.n_items - ROWS)
        app.scroll = app.n_items - ROWS;
    tap(100, LIST_Y + (i - app.scroll) * ROW_H + 8);
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s card/music out_dir\n", argv[0]);
        return 2;
    }
    out_dir = argv[2];
    film_on = argc > 3 && strcmp(argv[3], "frames") == 0;
    char state[600];
    snprintf(state, sizeof(state), "%s/state.bin", out_dir);
    remove(state);
    state_set_file(state);

    gfx_init();
    player_init();
    snprintf(music_root, sizeof(music_root), "%s", argv[1]);
    app_init();
    check(lib_loaded(), "library.idx loads");
    check(sim_backlight == 5, "backlight set at start");

    // ---- Artist tab: KIVARA and Kivara are one artist by default
    check(app.tab == TAB_ARTIST, "starts on the Artist tab");
    // D-pad left/right switch tabs and stop at the ends
    press(KEY_LEFT);
    check(app.tab == TAB_ARTIST, "left on the first tab stays there");
    press(KEY_RIGHT);
    check(app.tab == TAB_ALBUM && app.tab_dir == 1, "right goes to the next tab, sliding in from the right");
    for (int i = 0; i < 6; i++)
        press(KEY_RIGHT);
    check(app.tab == TAB_SEARCH, "right on the last tab stays there");
    press(KEY_LEFT);
    check(app.tab == TAB_FOLDER && app.tab_dir == -1, "left goes back, sliding in from the left");
    for (int i = 0; i < 6; i++)
        press(KEY_LEFT);
    run(30);
    int k = find_row("Kivara");
    check(k >= 0 && find_row("KIVARA") < 0, "KIVARA and Kivara grouped");
    check(k >= 0 && lib_group_size(app.items[k].id) == 20, "Kivara has 13+3+4 songs");
    app.scroll = k - 4;
    select_index(k);
    run(4);
    shot("01_artist");

    tap_row("Kivara");
    check(app.tab == TAB_ALBUM && app.artist >= 0, "tap artist -> Album tab");
    check(app.tab_anim > 0 && app.tab_dir == 1, "the album list slides in from the right");
    shot("02a_album_sliding");
    run(20);
    check(app.tab_anim == 0, "the slide ends");
    shot("02_album");

    tap_row("Paper Archive vol.1");
    check(app.tab == TAB_TITLE && app.album != LIB_NONE, "tap album -> Title tab");
    check(app.n_items == 13, "album has 13 songs in track order");

    tap(100, LIST_Y + 8);   // track 1
    check(player_state() == PLAYER_PLAYING, "tap song plays it");
    check(queue_count() == 13 && queue_pos() == 0, "queue = the album");
    check(app.sa_kind == SA_PUT, "nothing was playing: the cover is put on from the left");
    run(30);
    check(app.sa_kind == SA_NONE, "the cover is in place");
    // the ARM7's reports arrive between frames: the position never goes back
    {
        uint32_t last = player_position_smooth_ms();
        bool back = false;
        for (int i = 0; i < 120; i++)
        {
            tick(0);
            uint32_t now = player_position_smooth_ms();
            back |= now < last;
            last = now;
        }
        check(!back, "the progress never moves backwards");
    }

    // covers as a stack in list order
    tap(100, LIST_Y + 3 * ROW_H + 8);
    check(app.sa_kind == SA_LIFT, "a song further down: the old cover is lifted off");
    run(30);
    tap(100, LIST_Y + 1 * ROW_H + 8);
    check(app.sa_kind == SA_PUT, "a song further up: the new cover is put on");
    run(30);
    press(KEY_R);
    check(app.sa_kind == SA_LIFT, "next song in browse mode lifts the cover off");
    run(30);
    press(KEY_L);
    check(app.sa_kind == SA_PUT, "previous song in browse mode puts it back on");
    run(30);
    tap(100, LIST_Y + 8);   // back to track 1 for the rest
    run(30);
    check(queue_pos() == 0, "track 1 again");
    run(90);
    shot("03_title_playing");

    // ---- small things: lit buttons, play/pause morph, gliding highlight
    sim_set_touch(48, BAR_Y + 14);
    tick(KEY_TOUCH);
    check(app.press_id == BTN_NEXT && app.press_down, "a button lights up under the stylus");
    sim_set_touch(48, BAR_Y - 40);      // slid off: no tap
    tick(KEY_TOUCH);
    check(app.press_id == BTN_NONE, "sliding off a button cancels it");
    run(4);
    check(app.pp_t == PP_TICKS, "playing: the pause shape");
    press(KEY_START);
    run(4);
    check(app.pp_t > 0 && app.pp_t < PP_TICKS, "pausing: the shape is morphing");
    run(20);
    check(app.pp_t == 0, "paused: the play shape");
    press(KEY_START);
    select_index(0);
    run(20);
    select_index(3);
    tick(0);
    check(app.sel_vis > 0 && app.sel_vis < 3 * ROW_H, "the row highlight glides to the selection");
    run(20);
    check(app.sel_vis == 3 * ROW_H, "and arrives");
    select_index(0);
    run(20);

    // ---- lid: fade to black, then the light goes off; fade in when opened
    sim_set_keys(KEY_LID, KEY_LID);
    vblank_count++;
    app_tick();
    for (int i = 0; i < LID_OUT + 2; i++)
        tick(KEY_LID);
    check(lid_closed && sim_brightness == -16, "lid closed: faded to black");
    bool all_black = true;
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < 256 * 192; k++)
            all_black &= (gfx_canvas((Screen)s)->px[k] & 0x7FFF) == 0;
    check(all_black, "and both screens filled black");
    run(2);
    check(sim_brightness < 0, "lid opened: still dark at first");
    run(LID_IN + 2);
    check(sim_brightness == 0, "then faded in");
    check((gfx_canvas(SCREEN_TOP)->px[100 * 256 + 100] & 0x7FFF) != 0, "and drawn again");

    // ---- slide down to the player
    tap(128, BAR_Y + 14);
    run(16);
    shot("04_slide_mid");
    run(30);
    check(app.slide == SLIDE_TICKS, "slid to the player");
    shot("05_player");
    run(9);
    shot("05_player_b");

    tap(20, 140);
    check(app.overlay == OV_INFO, "info opens");
    shot("06_info");
    tap(240, 8);
    check(app.overlay == OV_NONE, "info closes");

    tap(236, 146);
    check(app.overlay == OV_QUEUE, "queue opens");
    shot("07_queue");
    // remove song 3, then drag song 2 to position 5
    tap(230, 20 + 2 * ROW_H + 8);
    check(queue_count() == 12, "x removes a song");
    hold(100, 20 + 1 * ROW_H + 8, 35);
    check(app.q_drag_from == 1, "hold lifts a queue row");
    drag_to(100, 20 + 4 * ROW_H + 8, 6);
    shot("08_queue_drag");
    release();
    char t[200];
    snprintf(t, sizeof(t), "%s", lib_value(lib_track(queue_track(4))->title));
    check(strcmp(t, "Dawnline") == 0, "drag moved Dawnline to 5");
    tap(240, 8);

    // seek by dragging the bar
    hold(16 + 112, 42, 2);
    drag_to(16 + 168, 42, 3);
    release();
    check(player_position_ms() > 3500, "seek bar drag seeks");

    // ---- song changes on the top screen
    run(40);
    uint32_t before = app_current_track();
    tap(178, 100);      // next
    check(app.sa_kind == SA_FORWARD && app.sa_a == before && app.sa_b == app_current_track(),
          "next song: the cover leaves left, the next one grows in");
    film("anim_next", 70);
    check(app.sa_kind == SA_NONE, "the animation ends");
    check(app.vinyl_speed > 0, "the new record spins up after rolling out");
    uint32_t second = app_current_track();
    tap(78, 100);       // previous (the song just began, so it goes back)
    check(app.sa_kind == SA_BACK && app_current_track() == before && app.sa_b == second,
          "previous song: the same animation backwards");
    film("anim_prev", 70);
    tap(236, 146);      // queue
    tap(100, 20 + 5 * ROW_H + 8);
    check(app.sa_kind == SA_JUMP, "a song picked in the queue slides over Next first");
    tap(240, 8);
    film("anim_jump", 80);
    check(app.sa_kind == SA_NONE, "the jump animation ends");
    press(KEY_START);   // pause
    run(40);
    check(app.vinyl_speed == 0, "paused: the record spins down and stops");
    press(KEY_START);


    // ---- back up with a tab, then the folder tab
    tap(210, PANEL_DOWN + 10);
    run(40);
    check(app.tab == TAB_FOLDER && app.slide == 0, "tab tap slides back up");
    tap_row("Bandcamp");
    tap_row("Kivara");
    shot("09_folder");

    // ---- sort menu on the album list
    tap(36, 10);    // Artist tab label -> artist list
    tap_row("Kivara");
    tap(176, BAR_Y + 14);
    check(app.overlay == OV_SORT, "sort menu opens");
    shot("10_sort");
    int n = ui_sort_option_count(SLOT_ALBUM);
    int y0 = BAR_Y - (20 + 16 * n + 4);
    tap(150, y0 + 20 + 3 * 16 + 8);     // year new -> old
    char first[64];
    ui_item_label(&app.items[0], first, sizeof(first));
    check(strcmp(first, "Paper Archive vol.1") == 0, "newest album first");

    // ---- hold a row: menu
    tap(108, 10);   // Album tab
    tap_row("Paper Archive vol.1");
    hold(100, LIST_Y + 3 * ROW_H + 8, 35);
    release();
    check(app.overlay == OV_ROWMENU, "hold opens the row menu");
    shot("11_rowmenu");
    int ry = ui_rowmenu_y(LIST_Y + 3 * ROW_H);
    tap(150, ry + 20 + 2 * 16 + 8);
    check(queue_count() == 13, "add to queue appends");

    // ---- add mode
    tap(80, BAR_Y + 14);
    check(app.add_mode, "add mode on");
    tap(100, LIST_Y + 6 * ROW_H + 8);
    tap(100, LIST_Y + 4 * ROW_H + 8);
    check(queue_count() == 15, "tapping rows adds them");
    shot("12_addmode");
    tap(80, BAR_Y + 14);

    // ---- settings
    tap(240, BAR_Y + 14);
    check(app.overlay == OV_SETTINGS && app.ov_vis == OV_SETTINGS && app.ov_t < OV_TICKS,
          "settings open, sliding up");
    run(20);
    check(app.ov_t == OV_TICKS, "settings in place");
    check(!app.set.show_remaining, "length on the right by default");
    shot("13_settings");
    tap(100, SET_Y + 5 * SET_H + 10);
    check(app.set.show_remaining, "time left can be chosen");
    tap(100, SET_Y + 5 * SET_H + 10);
    check(!app.set.plain_bg, "cover background on by default");
    tap(100, SET_Y + 6 * SET_H + 10);
    check(app.set.plain_bg, "cover background can be turned off");
    tap(100, SET_Y + 6 * SET_H + 10);
    tap(100, SET_Y + 1 * SET_H + 10);     // case-sensitive
    check(app.set.case_sensitive, "case-sensitive on");
    // About slides in from the right, ‹ goes back
    tap(100, SET_Y + 7 * SET_H + 10);
    check(app.about && app.about_t > 0 && app.about_t < TAB_TICKS, "About sliding in");
    shot("13a_about_sliding");
    run(20);
    check(app.about_t == TAB_TICKS, "About in place");
    shot("13b_about");
    uint8_t lid_before = app.set.pause_on_lid;
    tap(100, SET_Y + 2 * SET_H + 10);
    check(app.set.pause_on_lid == lid_before, "rows can't be tapped through About");
    press(KEY_B);
    check(!app.about && app.overlay == OV_SETTINGS, "B leaves About, settings stay open");
    run(20);
    tap(240, 8);
    tap(240, BAR_Y + 14);
    tap(100, SET_Y + 7 * SET_H + 10);
    run(20);
    tap(20, 8);
    run(20);
    check(!app.about && app.about_t == 0, "‹ goes back");
    // the highlight glides between rows
    press(KEY_DOWN);
    run(10);
    check(app.menu_sel == 0 && app.menu_vis == 0, "first D-pad press: highlight in place");
    press(KEY_DOWN);
    check(app.menu_vis > 0 && app.menu_vis < SET_H, "highlight gliding");
    run(10);
    check(app.menu_vis == SET_H, "highlight arrived");
    shot("13c_settings_sel");
    tap(240, 8);
    tap(36, 10);
    check(find_row("KIVARA") >= 0, "KIVARA separate when case-sensitive");
    tap(240, BAR_Y + 14);
    tap(100, SET_Y + 1 * SET_H + 10);
    tap(240, 8);

    // ---- scroll bar drag with the letter bubble
    tap(36, 10);
    hold(250, LIST_Y + 60, 3);
    drag_to(250, LIST_Y + 90, 4);
    shot("14_scrolldrag");
    release();

    // ---- brightness cycles 5 -> 2
    tap(208, BAR_Y + 14);
    check(sim_backlight == 2, "brightness cycles to the lowest level");

    // ---- state survives a restart; launch loads the song paused at the same spot
    uint32_t saved_pos = player_position_ms();
    uint32_t saved_track = app_current_track();
    save_now();
    player_stop();
    queue_clear();
    restore();
    check(player_state() == PLAYER_PAUSED && app_current_track() == saved_track
          && (saved_pos > player_position_ms() ? saved_pos - player_position_ms() : player_position_ms() - saved_pos) <= 125,
          "launch resumes paused at the same song and position");
    save_now();
    Settings s2;
    Resume r2;
    queue_clear();
    check(state_load(&s2, &r2) && queue_count() == 15 && r2.path[0], "state saved and restored");

    if (film_on)
    {
        // a queue with different covers, filmed for a look at the animations
        uint32_t mix[4];
        int k = 0;
        for (uint32_t i = 0; i < lib_album_count() && k < 4; i++)
        {
            const LibAlbum *al = lib_album(i);
            if (strcmp(lib_value(al->name), "Paper Archive vol.1") == 0 || strcmp(lib_value(al->name), "Echoes Of Quiet Worlds") == 0)
            {
                mix[k++] = lib_album_track(al, 0);
                mix[k++] = lib_album_track(al, 1);
            }
        }
        uint32_t order[4] = { mix[0], mix[2], mix[1], mix[3] };
        slide_to(true);
        run(40);
        play_tracks(order, 4, 0, false, SA_PUT);
        run(80);
        tap(178, 100);
        film("demo_next", 60);
        run(40);
        tap(78, 100);
        film("demo_prev", 60);
        run(40);
        tap(236, 146);
        tap(100, 20 + 3 * ROW_H + 8);
        tap(240, 8);
        film("demo_jump", 75);

        // browse mode: the cover stack, in Kivara's songs
        slide_to(false);
        run(40);
        app.artist = lib_track_group(lib_find_path(lib_path(order[0])));
        app.album = LIB_NONE;
        show_tab(TAB_TITLE);
        run(20);
        tap_row("Persist");
        run(30);
        tap_row("Eternal");     // further up: put on
        film("stack_put", 24);
        run(20);
        tap_row("Gone");        // further down: lifted off
        film("stack_lift", 24);

        // menus sliding up and down
        run(40);
        tap(240, BAR_Y + 14);                       // settings
        film_screen("menu_settings_open", 18, SCREEN_BOTTOM);
        tap(240, 8);
        film_screen("menu_settings_close", 18, SCREEN_BOTTOM);
        tap(176, BAR_Y + 14);                       // sort
        film_screen("menu_sort_open", 18, SCREEN_BOTTOM);
        tap(20, LIST_Y + 8);
        film_screen("menu_sort_close", 18, SCREEN_BOTTOM);
        slide_to(true);
        run(40);
        tap(236, 146);                              // queue
        film_screen("menu_queue_open", 18, SCREEN_BOTTOM);
        tap(240, 8);
        film_screen("menu_queue_close", 18, SCREEN_BOTTOM);
        run(20);

        // play/pause morph with the lit button
        sim_set_touch(128, 100);
        tick(KEY_TOUCH);
        film_screen("fx_pause", 4, SCREEN_BOTTOM);
        film_screen("fx_pause_b", 16, SCREEN_BOTTOM);
        run(20);
        tap(128, 100);
        film_screen("fx_play", 16, SCREEN_BOTTOM);

        // the row highlight gliding
        slide_to(false);
        run(40);
        select_index(0);
        run(20);
        for (int k = 1; k <= 3; k++)
        {
            select_index(k * 2);
            char nm[24];
            snprintf(nm, sizeof(nm), "fx_glide%d", k);
            film_screen(nm, 10, SCREEN_BOTTOM);
        }
    }

    printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures != 0;
}
