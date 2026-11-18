// SPDX-License-Identifier: MIT
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

// One vblank. The simulated ARM7 finishes a 4096-frame block every 7.5 vblanks.
static void tick(uint32_t held)
{
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
    int k = find_row("Kivara");
    check(k >= 0 && find_row("KIVARA") < 0, "KIVARA and Kivara grouped");
    check(k >= 0 && lib_group_size(app.items[k].id) == 20, "Kivara has 13+3+4 songs");
    app.scroll = k - 4;
    select_index(k);
    run(4);
    shot("01_artist");

    tap_row("Kivara");
    check(app.tab == TAB_ALBUM && app.artist >= 0, "tap artist -> Album tab");
    shot("02_album");

    tap_row("Paper Archive vol.1");
    check(app.tab == TAB_TITLE && app.album != LIB_NONE, "tap album -> Title tab");
    check(app.n_items == 13, "album has 13 songs in track order");

    tap(100, LIST_Y + 8);   // track 1
    check(player_state() == PLAYER_PLAYING, "tap song plays it");
    check(queue_count() == 13 && queue_pos() == 0, "queue = the album");
    run(90);
    shot("03_title_playing");

    // ---- slide down to the player
    tap(128, BAR_Y + 14);
    run(16);
    shot("04_slide_mid");
    run(30);
    check(app.slide == SLIDE_FRAMES, "slid to the player");
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
    check(app.overlay == OV_SETTINGS, "settings open");
    shot("13_settings");
    tap(100, 24 + 1 * 28 + 10);     // case-sensitive
    check(app.set.case_sensitive, "case-sensitive on");
    tap(240, 8);
    tap(36, 10);
    check(find_row("KIVARA") >= 0, "KIVARA separate when case-sensitive");
    tap(240, BAR_Y + 14);
    tap(100, 24 + 1 * 28 + 10);
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

    printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures != 0;
}
