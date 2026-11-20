// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the real app (main.c) on the PC: taps, holds and drags through the
// UI like a person would, and writes screenshots of both screens.
//
//   ./screens /path/to/card/music out_dir

#include <math.h>
#include <stdio.h>

#define DSPLAYER_NO_MAIN
#include "../../arm9/source/main.c"
#include "../../arm9/source/viz.h"

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

// Run n vblanks; with filming on, save both screens every second one (on the
// DS each screen is drawn every second frame: 30 per second).
static int film_n;
static void film_both(const char *prefix, int n)
{
    for (int i = 0; i < n; i++)
    {
        tick(0);
        if (!film_on || (i & 1))
            continue;
        char name[64];
        snprintf(name, sizeof(name), "%s_%03d", prefix, film_n++);
        shot(name);
    }
}

// A tap, filmed.
static void film_tap(const char *prefix, int x, int y, int after)
{
    sim_set_touch(x, y);
    tick(KEY_TOUCH);
    tick(KEY_TOUCH);
    film_both(prefix, after);
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

// The bottom screen's rows y0..y1 as they are drawn right now.
static void bottom_rows(uint16_t *out, int y0, int y1)
{
    ui_draw_bottom();
    Canvas *c = gfx_canvas(SCREEN_BOTTOM);
    memcpy(out, c->px + y0 * SCREEN_W, (size_t)(y1 - y0) * SCREEN_W * 2);
}

// Tap a key of the search keyboard: a character, or a key kind (K_BACK, ...).
static bool key_pos(uint32_t cp, int kind, int *x, int *y)
{
    Key keys[KB_MAX_KEYS];
    int n = kb_layout(app.im, app.kb_num, app.set.keyboard == KB_QWERTZ, app.shift && !app.kb_num, keys);
    for (int i = 0; i < n; i++)
        if ((kind == K_CHAR && keys[i].kind == K_CHAR && keys[i].cp == cp) || (kind != K_CHAR && keys[i].kind == kind
            && (kind != K_KANA || keys[i].cp == cp)))
        {
            *x = keys[i].x + keys[i].w / 2;
            *y = KB_Y + keys[i].y + keys[i].h / 2;
            return true;
        }
    return false;
}

static void tap_key(uint32_t cp, int kind)
{
    int x, y;
    if (!key_pos(cp, kind, &x, &y))
    {
        printf("FAIL: no key U+%04X kind %d\n", (unsigned)cp, kind);
        failures++;
        return;
    }
    tap(x, y);
}

static void type_text(const char *s)
{
    while (*s)
        tap_key(utf8_next(&s), K_CHAR);
}

// The first result row of a section with this label, or -1.
static int result_row_named(const char *label)
{
    for (int i = 0; i < app.n_items; i++)
    {
        if (app.items[i].kind == ITEM_HEADER)
            continue;
        char buf[256];
        ui_item_label(&app.items[i], buf, sizeof(buf));
        if (strcmp(buf, label) == 0)
            return i;
    }
    return -1;
}

static int section_count(int sec)
{
    for (int i = 0; i < app.n_items; i++)
        if (app.items[i].kind == ITEM_HEADER && (int)app.items[i].id == sec)
            return app.marks[i].count;
    return 0;
}

// ---- README pictures, from the made-up library of tests/demo/make_demo_card.py
//      ./screens /tmp/demo/card/music shots demo

static uint32_t demo_track(const char *path)
{
    uint32_t t = lib_find_path(path);
    if (t == LIB_NONE)
    {
        printf("FAIL: demo card has no %s\n", path);
        failures++;
    }
    return t;
}

static void demo_tap_row(const char *prefix, const char *label, int after)
{
    int i = find_row(label);
    if (i < 0)
    {
        printf("FAIL: no row %s\n", label);
        failures++;
        return;
    }
    if (i >= app.scroll + list_rows())
        app.scroll = i - list_rows() + 2;
    film_tap(prefix, 100, list_y() + (i - app.scroll) * ROW_H + 8, after);
}

static void demo_im(int im)
{
    int x, y;
    key_pos(0, K_IM, &x, &y);
    hold(x, y, HOLD_TICKS + 2);
    release();
    run(OV_TICKS + 2);
    tap(60, ui_im_menu_y() + 20 + im * 16 + 8);
    run(IM_TICKS + 4);
}

static int demo(void)
{
    film_on = true;
    uint32_t first = demo_track("Bandcamp/Mira Kestrel/Coastline Tapes/01 Saltwater Radio.dsa");
    play_tracks(&first, 1, 0, false, SA_JUMP);
    run(120);

    // browsing: the artists, Nightglass's albums, a song, the player
    int k = find_row("Nightglass");
    select_index(k);
    run(20);
    shot("browse");
    film_n = 0;
    film_both("rd_browse", 16);
    film_tap("rd_browse", 100, LIST_Y + (k - app.scroll) * ROW_H + 8, 36);
    demo_tap_row("rd_browse", "Afterglow Arcade", 36);
    demo_tap_row("rd_browse", "Midnight Tram", 50);
    film_tap("rd_browse", 128, BAR_Y + 14, 60);

    // the player: covers of their own and the built-in one, next and back
    uint32_t q[4] = { demo_track("Bandcamp/Nightglass/Afterglow Arcade/03 Midnight Tram.dsa"),
                      demo_track("Bandcamp/Polar Moth/Frostbyte/01 Snowfield.dsa"),
                      demo_track("Bandcamp/Sölve/Orbit/01 Solstice.dsa"),
                      demo_track("Bandcamp/Mira Kestrel/Coastline Tapes/02 Harbor Lights.dsa") };
    play_tracks(q, 4, 0, false, SA_PUT);
    run(90);
    shot("player");
    film_n = 0;
    film_both("rd_player", 24);
    film_tap("rd_player", 178, 100, 70);
    film_tap("rd_player", 178, 100, 70);
    film_tap("rd_player", 78, 100, 70);
    run(30);
    rate_current(4);
    tap(20, 140);
    run(OV_TICKS + 6);
    shot("info");
    tap(240, 8);
    run(OV_TICKS + 4);
    tap(236, 146);
    run(OV_TICKS + 6);
    shot("queue");
    tap(240, 8);
    run(OV_TICKS + 4);
    uint32_t two = demo_track("Singles/Nightglass, Mira Kestrel - Two Lights.dsa");
    play_tracks(&two, 1, 0, false, SA_JUMP);
    run(90);
    shot("two_artists");

    // search: the 🔍 tab, typing "night", the D-pad, the keyboard down
    tap(128, 172);
    run(40);
    show_tab(TAB_ARTIST);
    run(30);
    film_n = 0;
    film_tap("rd_search", 240, 10, TAB_TICKS + KB_TICKS + 10);
    for (const char *c = "night"; *c; c++)
    {
        int kx, ky;
        key_pos((uint32_t)*c, K_CHAR, &kx, &ky);
        film_tap("rd_search", kx, ky, 12);
    }
    film_both("rd_search", 24);
    shot("search");
    sim_set_touch(0, 0);
    tick(KEY_DOWN);
    film_both("rd_search", 14);
    tick(KEY_DOWN);
    film_both("rd_search", 20);
    int dx, dy;
    key_pos(0, K_DONE, &dx, &dy);
    film_tap("rd_search", dx, dy, KB_TICKS + 30);
    tap(150, FIELD_Y + 10);
    run(KB_TICKS + 4);
    tap(240, FIELD_Y + 10);
    run(SHEET_TICKS + 4);

    // hold o: its other forms ("sö" finds Sölve)
    type_text("s");
    int ox, oy;
    key_pos('o', K_CHAR, &ox, &oy);
    hold(ox, oy, HOLD_TICKS + ACC_TICKS + 4);
    shot("search_accents");
    release();
    run(SHEET_TICKS + 4);
    tap(240, FIELD_Y + 10);
    run(SHEET_TICKS + 4);
    // かな: はな finds ハナビラ
    demo_im(IM_KANA);
    tap_key(0x306F, K_CHAR);
    tap_key(0x306A, K_CHAR);
    run(SHEET_TICKS + 4);
    shot("search_kana");
    tap(240, FIELD_Y + 10);
    // 拼音: lin finds 林星月
    demo_im(IM_PINYIN);
    type_text("lin");
    run(SHEET_TICKS + 4);
    shot("search_pinyin");
    tap(240, FIELD_Y + 10);
    // АБВ
    demo_im(IM_CYR);
    type_text("ви");
    run(SHEET_TICKS + 4);
    shot("search_cyrillic");
    tap(240, FIELD_Y + 10);
    demo_im(IM_ABC);
    run(SHEET_TICKS + 4);

    // + Filter: a genre, then the songs as a list
    bool on_x;
    int px = -1;
    for (int x = 4; x < 252 && px < 0; x++)
        if (ui_chip_at(x, CHIPS_Y + 8, &on_x) == F_COUNT)
            px = x;
    tap(px + 10, CHIPS_Y + 8);
    run(OV_TICKS + 4);
    tap(100, FMENU_Y + 20 + F_GENRE * 16 + 8);
    run(TAB_TICKS + 4);
    shot("search_filter");
    for (int i = 0; i < app.n_fopts; i++)
        if (strcmp(lib_value(app.fopts[i].value), "Synthwave") == 0)
            tap(100, FMENU_Y + 20 + (i - app.fscroll) * 16 + 8);
    run(OV_TICKS + SHEET_TICKS + 8);
    tap_key(0, K_DONE);
    run(KB_TICKS + 8);
    press(KEY_DOWN);
    run(10);
    shot("search_list");
    search_filter_clear(&app.sf);
    search_refresh();
    run(20);

    // settings, the theme: drag the accent round the ring, then the presets
    show_tab(TAB_ARTIST);
    run(40);
    tap(240, BAR_Y + 14);
    run(OV_TICKS + 10);
    shot("settings");
    tap(100, SET_Y + 3 * SET_H + 10);
    run(30);
    shot("theme");
    film_n = 0;
    film_both("rd_theme", 16);
    theme_select(0);
    {
        float a0 = (float)app.theme_hsv.h * 6.2831853f / CP_HUES;
        int rr = (CP_R_OUT + CP_R_IN) / 2;
        sim_set_touch(CP_CX + (int)(rr * cosf(a0)), CP_CY - (int)(rr * sinf(a0)));
        tick(KEY_TOUCH);
        for (int i = 0; i <= 90; i++)
        {
            float a = a0 + 6.2831853f * i / 90;
            sim_set_touch(CP_CX + (int)(rr * cosf(a)), CP_CY - (int)(rr * sinf(a)));
            tick(KEY_TOUCH);
            if (i % 2 == 0)
            {
                char name[64];
                snprintf(name, sizeof(name), "rd_theme_%03d", film_n++);
                shot(name);
            }
        }
        sim_set_touch(0, 0);
        tick(0);
    }
    tap(THEME_CHIP_X + 20, THEME_PRESETS_Y + 10);
    film_both("rd_theme", OV_TICKS + 20);
    shot("presets");
    film_tap("rd_theme", PRESET_X0 + 60, PRESET_Y + 20 + 1 * 16 + 8, 30);       // Midnight Rose
    film_tap("rd_theme", THEME_CHIP_X + 20, THEME_PRESETS_Y + 10, OV_TICKS + 16);
    film_tap("rd_theme", PRESET_X0 + 60, PRESET_Y + 20 + 8, 40);                // Ultraviolet
    tap(240, 8);
    run(30);

    // in German
    app.set.lang = i18n_lang = LANG_DE;
    play_tracks(q, 4, 0, false, SA_JUMP);
    slide_to(true);
    run(90);
    shot("german");
    slide_to(false);
    run(40);
    tap(240, 10);
    run(TAB_TICKS + KB_TICKS + 6);
    type_text("night");
    run(SHEET_TICKS + 6);
    shot("german_search");
    app.set.lang = i18n_lang = LANG_EN;

    printf("%s\n", failures ? "DEMO INCOMPLETE" : "demo pictures written");
    return failures != 0;
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
    char stats_file[600], stats_new[600];
    snprintf(stats_file, sizeof(stats_file), "%s/stats.bin", out_dir);
    snprintf(stats_new, sizeof(stats_new), "%s/stats.new", out_dir);
    remove(stats_file);
    remove(stats_new);
    stats_set_file(stats_file);

    gfx_init();
    player_init();
    snprintf(music_root, sizeof(music_root), "%s", argv[1]);
    app_init();
    if (argc > 3 && strcmp(argv[3], "demo") == 0)
        return demo();
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
    // the scroll bar: the queue stays where it's let go
    hold(250, 24, 2);
    drag_to(250, 20 + ROWS * ROW_H - 2, 4);
    release();
    run(10);
    check(app.q_scroll == queue_count() - ROWS, "queue scroll bar: stays where it's let go");
    press(KEY_DOWN);
    check(app.q_sel >= app.q_scroll && app.q_sel < app.q_scroll + ROWS, "then Down shows the selection again");
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

    // ---- sort: an artist option saved by an older version ("Most songs") falls back to name
    check(ui_sort_option_count(SLOT_ARTIST) == 2, "artists sort by name only");
    {
        int tab = app.tab;
        app.set.sort[SLOT_ARTIST] = 2;
        show_tab(TAB_ARTIST);
        check(app.set.sort[SLOT_ARTIST] == 0, "a removed sort option falls back to name");
        show_tab(tab);
        run(20);
    }

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

    // ---- settings: a top page leading to pages
    tap(240, BAR_Y + 14);
    check(app.overlay == OV_SETTINGS && app.ov_vis == OV_SETTINGS && app.ov_t < OV_TICKS,
          "settings open, sliding up");
    run(20);
    check(app.ov_t == OV_TICKS && app.set_page == PAGE_TOP, "settings in place, on the top page");
    check(!app.set.show_remaining, "length on the right by default");
    shot("13_settings");
    // Player page: slides in from the right
    tap(100, SET_Y + 2 * SET_H + 10);
    check(app.set_page == PAGE_PLAYER && app.page_t > 0 && app.page_t < TAB_TICKS, "Player page sliding in");
    shot("13a_page_sliding");
    run(20);
    check(app.page_t == TAB_TICKS, "Player page in place");
    shot("13b_player_page");
    tap(100, SET_Y + 0 * SET_H + 10);
    check(app.set.show_remaining, "time left can be chosen");
    tap(100, SET_Y + 0 * SET_H + 10);
    check(!app.set.plain_bg, "cover background on by default");
    tap(100, SET_Y + 1 * SET_H + 10);
    check(app.set.plain_bg, "cover background can be turned off");
    tap(100, SET_Y + 1 * SET_H + 10);
    check(!app.set.shuffle_weighted, "shuffle by rating off by default");
    tap(100, SET_Y + 2 * SET_H + 10);
    check(app.set.shuffle_weighted, "shuffle by rating is on the Player page");
    tap(100, SET_Y + 2 * SET_H + 10);
    tap(20, 8);                             // ‹
    check(app.set_page == PAGE_TOP, "‹ goes back");
    run(20);
    check(app.page_t == 0, "the top page is back in place");
    // Browsing page: case-sensitive names
    tap(100, SET_Y + 1 * SET_H + 10);
    run(20);
    tap(100, SET_Y + 0 * SET_H + 10);
    check(app.set.case_sensitive, "case-sensitive on (Browsing page)");
    press(KEY_B);
    check(app.set_page == PAGE_TOP && app.overlay == OV_SETTINGS, "B goes back, settings stay open");
    run(20);
    // Theme: three colours and the colour triangle
    tap(100, SET_Y + 3 * SET_H + 10);
    run(20);
    check(app.set_shown == PAGE_THEME && app.theme_sel == 0, "Theme page, the accent chosen");
    shot("13f_theme");
    tap(THEME_CHIP_X + 20, THEME_CHIP_Y + THEME_CHIP_PITCH + 10);
    check(app.theme_sel == 1, "the Background chip");
    uint16_t bg0 = app.theme.bg;
    tap(CP_CX + (CP_R_IN + CP_R_OUT) / 2, CP_CY);           // the ring, at the right: red
    check(app.theme_hsv.h < 16 || app.theme_hsv.h > CP_HUES - 16, "the ring's right side is red");
    check(app.theme.bg != bg0 && C_BG == (app.theme.bg | 0x8000) && (app.theme.bg & 31) > ((app.theme.bg >> 10) & 31),
          "the background turns reddish, and the app with it");
    tap(THEME_CHIP_X + 20, THEME_CHIP_Y + 10);
    check(app.theme_sel == 0, "the Accent chip");
    {
        // drag from the middle of the triangle to its white corner (120° on from the hue)
        float a = (app.theme_hsv.h + 512) * 6.2831853f / CP_HUES;
        int wx = CP_CX + (int)(CP_R_TRI * cosf(a)), wy = CP_CY - (int)(CP_R_TRI * sinf(a));
        hold(CP_CX, CP_CY, 2);
        uint16_t mid = app.theme.acc;
        drag_to(wx, wy, 6);
        release();
        uint16_t w = app.theme.acc;
        check(mid != w && (w & 31) >= 29 && ((w >> 5) & 31) >= 29 && ((w >> 10) & 31) >= 29,
              "dragging to the white corner: a white accent");
    }
    shot("13g_theme_changed");
    // Presets: the list rises out of its button; Midnight Rose, then Ultraviolet by D-pad
    tap(THEME_CHIP_X + 20, THEME_PRESETS_Y + 10);
    check(app.presets_open && app.menu_sel == -1, "Presets: the list, nothing marked (a theme of my own)");
    run(OV_TICKS / 2);
    shot("13h_presets_rising");
    run(OV_TICKS);
    shot("13i_presets");
    tap(PRESET_X0 + 60, PRESET_Y + 20 + 1 * 16 + 8);
    check(!app.presets_open && app.theme.bg == theme_presets[1].theme.bg && app.theme.acc == theme_presets[1].theme.acc
          && C_ACC == theme_presets[1].theme.acc, "Midnight Rose: the first theme");
    run(OV_TICKS + 2);
    shot("13j_midnight_rose");
    press(KEY_A);
    check(app.presets_open && app.menu_sel == 1, "A: the presets, Midnight Rose highlighted");
    run(OV_TICKS + 2);
    press(KEY_UP);
    press(KEY_A);
    check(!app.presets_open && app.theme.bg == theme_default.bg && app.theme.acc == theme_default.acc
          && C_ACC == theme_default.acc, "Up, A: Ultraviolet, the default theme");
    run(OV_TICKS + 2);
    press(KEY_DOWN);
    check(app.theme_sel == 1, "Down: the next colour");
    int h0 = app.theme_hsv.h;
    press(KEY_RIGHT);
    check(app.theme_hsv.h == (h0 + 16) % CP_HUES && app.set_page == PAGE_THEME, "Right turns the hue (and stays)");
    tap(THEME_CHIP_X + 20, THEME_PRESETS_Y + 10);
    run(OV_TICKS + 2);
    tap(PRESET_X0 + 60, PRESET_Y + 20 + 8);
    check(app.theme.acc == theme_default.acc, "back to Ultraviolet");
    press(KEY_B);
    run(20);
    check(app.set_page == PAGE_TOP, "B leaves the Theme page");
    tap(100, SET_Y + 4 * SET_H + 10);
    run(20);
    check(app.set_shown == PAGE_REGIONAL && ui_set_count(PAGE_REGIONAL) == 2, "Regional: language and keyboard");
    check(app.set.lang == LANG_EN && app.set.keyboard == KB_QWERTY, "English and QWERTY by default");
    {
        // findable without reading: every top row has an icon, the globe leads to Language
        bool all = true;
        for (int i = 0; i < ui_set_count(PAGE_TOP); i++)
            all &= ui_set_item(PAGE_TOP, i)->icon != 0;
        check(all, "every row of the top settings page has an icon");
        check(ui_set_item(PAGE_TOP, 4)->icon == 0x1F310 && ui_set_item(PAGE_REGIONAL, 0)->icon == 0x1F310
              && ui_set_item(PAGE_REGIONAL, 0)->id == SET_LANG, "the globe: Regional, then Language");
        // the icons are glyphs the player always has
        for (int i = 0; i < ui_set_count(PAGE_TOP); i++)
        {
            uint32_t cp = ui_set_item(PAGE_TOP, i)->icon;
            char u[8] = { 0 };
            if (cp < 0x10000)
                u[0] = 0xE0 | cp >> 12, u[1] = 0x80 | ((cp >> 6) & 63), u[2] = 0x80 | (cp & 63);
            else
                u[0] = 0xF0 | cp >> 18, u[1] = 0x80 | ((cp >> 12) & 63), u[2] = 0x80 | ((cp >> 6) & 63), u[3] = 0x80 | (cp & 63);
            all &= font_text_width(u) == 16;
        }
        check(all, "the icons are drawn 16 px wide");
    }
    tap(100, SET_Y + 1 * SET_H + 10);
    check(app.set.keyboard == KB_QWERTZ, "tap: QWERTZ");
    tap(100, SET_Y + 1 * SET_H + 10);
    check(app.set.keyboard == KB_QWERTY, "tap again: QWERTY");
    shot("13h_regional");
    press(KEY_B);
    run(20);
    // About
    tap(100, SET_Y + 5 * SET_H + 10);
    run(20);
    check(app.set_shown == PAGE_ABOUT && app.page_t == TAB_TICKS, "About in place");
    shot("13c_about");
    uint8_t lid_before = app.set.pause_on_lid;
    tap(100, SET_Y + 0 * SET_H + 10);
    check(app.set.pause_on_lid == lid_before, "About has no rows to tap");
    press(KEY_B);
    run(20);
    // D-pad: the highlight glides, A or Right opens a page, Left comes back to the same row
    press(KEY_DOWN);
    run(10);
    check(app.menu_sel == 0 && app.menu_vis == 0, "first D-pad press: highlight in place");
    press(KEY_DOWN);
    check(app.menu_vis > 0 && app.menu_vis < SET_H, "highlight gliding");
    run(10);
    check(app.menu_vis == SET_H, "highlight arrived");
    shot("13d_settings_sel");
    press(KEY_RIGHT);
    check(app.set_page == PAGE_BROWSING && app.menu_sel == 0, "Right opens Browsing, highlight on its first row");
    run(20);
    press(KEY_LEFT);
    check(app.set_page == PAGE_TOP && app.menu_sel == 1, "Left comes back to the Browsing row");
    run(20);
    press(KEY_A);
    run(20);
    shot("13e_browsing_page");
    press(KEY_B);
    run(20);
    press(KEY_B);
    check(app.overlay == OV_NONE, "B on the top page closes the settings");
    run(20);
    tap(36, 10);
    check(find_row("KIVARA") >= 0, "KIVARA separate when case-sensitive");
    tap(240, BAR_Y + 14);
    tap(100, SET_Y + 1 * SET_H + 10);       // Browsing
    run(20);
    tap(100, SET_Y + 0 * SET_H + 10);       // case-sensitive off again
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

    // ---- rating and plays in the song info
    check(!app.player_mode, "browsing");
    press(KEY_B);
    check(app.player_mode, "B in the browser: to the player");
    run(30);
    if (player_state() != PLAYER_PLAYING)
        press(KEY_START);
    press(KEY_X);
    run(20);
    check(app.overlay == OV_INFO, "X opens the song info");
    {
        uint32_t rated = app_current_track();
        int sx = ui_info_value_x(), sy = INFO_Y + INFO_RATING_ROW * 16;
        tap(sx + 3 * INFO_STAR_W + 8, sy + 8);
        check(app_song_rating(rated) == 4 && stats_rating(lib_path(rated)) == 4, "the 4th star: 4 stars");
        shot("06b_info_rated");
        tap(sx + 3 * INFO_STAR_W + 8, sy + 8);
        check(app_song_rating(rated) == 0 && stats_rating(lib_path(rated)) == 0, "the same star again: none");
        press(KEY_RIGHT);
        press(KEY_RIGHT);
        check(app_song_rating(rated) == 2, "Right: a star more");
        press(KEY_LEFT);
        check(app_song_rating(rated) == 1, "Left: a star less");

        // a play counts once, after half the song (or 4 minutes) of listening
        for (int i = 0; i < 10 && player_position_ms() <= 3500; i++)
            run(30);
        press(KEY_L);                       // back to the start: a new listen
        check(app_current_track() == rated && player_position_ms() < 1000, "L restarts the song");
        uint32_t dur = player_duration_ms(), need = dur / 2 < 240000 ? dur / 2 : 240000;
        uint32_t plays0 = app_song_plays(rated);
        run((int)(need * 60 / 1000) - 40);
        check(app_song_plays(rated) == plays0, "not counted before half the song");
        run(80);
        check(app_song_plays(rated) == plays0 + 1, "counted after half the song");
        run(120);
        check(app_song_plays(rated) == plays0 + 1, "only once per listen");
        check(!stats_dirty(), "saved a second later");

        stats_load();
        check(stats_rating(lib_path(rated)) == 1 && app_song_plays(rated) == plays0 + 1, "stats survive a restart");
        rename(stats_file, stats_new);      // as if the power went off between the two steps of a save
        stats_load();
        check(stats_rating(lib_path(rated)) == 1, "a save cut short: read from stats.new");
        rename(stats_new, stats_file);
    }
    press(KEY_B);
    check(app.overlay == OV_NONE && app.player_mode, "B closes the song info");
    // the artist's name in the player's top line: its albums in the browser
    {
        uint32_t cur = app_current_track();
        int ax = -1, tx = -1;
        for (int k = 0; k < 400 && ax < 0; k++)
        {
            for (int x = 8; x < 248 && ax < 0; x++)
                if (ui_player_artist_at(x, 12) >= 0 && ui_player_artist_at(x + 12, 12) >= 0)
                    ax = x;
            if (ax < 0)
                run(1);
        }
        for (int x = 247; x >= 8; x--)
            if (ui_player_artist_at(x, 12) < 0)
            {
                tx = x;
                break;
            }
        check(ax >= 0 && tx >= 0, "the artist's name is found in the player's top line");
        tap(tx, 12);
        check(app.player_mode, "a tap next to it does nothing");
        tap(ax + 6, 12);
        check(!app.player_mode && app.tab == TAB_ALBUM && app.artist == lib_track_group(cur),
              "tapping the artist: its albums in the browser");
        check(app.sel >= 0 && app.items[app.sel].id == lib_track(cur)->album, "with the playing song's album selected");
        run(30);
        shot("05c_artist_from_player");
        slide_to(true);
        run(30);
    }

    press(KEY_B);
    check(!app.player_mode, "B in the player: back to browsing");
    run(30);

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
    app.set.play_at_start = 1;
    save_now();
    player_stop();
    queue_clear();
    restore();
    check(player_state() == PLAYER_PLAYING && app_current_track() == saved_track,
          "Play on launch: launch goes on playing");
    player_toggle_pause();
    app.set.play_at_start = 0;
    save_now();
    Settings s2;
    Resume r2;
    queue_clear();
    Theme t2;
    check(state_load(&s2, &t2, &r2) && queue_count() == 15 && r2.path[0], "state saved and restored");

    // the theme is saved; a file from before themes still loads, with the default theme
    {
        Theme mine = { COLOR(3, 6, 2), COLOR(28, 30, 26), COLOR(10, 25, 31) }, kept = app.theme;
        app.theme = mine;
        save_now();
        Settings s3;
        Theme t3;
        Resume r3;
        check(state_load(&s3, &t3, &r3) && t3.bg == mine.bg && t3.text == mine.text && t3.acc == mine.acc,
              "the theme is saved");
        // older files from this one: version 2 has 16 bytes of settings, version 1 also no theme
        for (int version = 2; version >= 1; version--)
        {
            save_now();
            FILE *f = fopen(state, "rb");
            static uint8_t buf[1 << 20], out[1 << 20];
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            uint16_t hs;
            memcpy(&hs, buf + 6, 2);
            size_t entries = n - hs - 4;
            size_t old = hs - 16 - (version == 1 ? 8 : 0);
            memcpy(out, buf, 8 + 16);                               // up to the old settings' end
            memcpy(out + 24, buf + 40, old - 24);                   // the rest after today's settings
            uint16_t v = (uint16_t)version, os = (uint16_t)old;
            memcpy(out + 4, &v, 2);
            memcpy(out + 6, &os, 2);
            memcpy(out + old, buf + hs, entries);
            uint32_t crc = crc32(out, old) ^ crc32(out + old, entries);
            memcpy(out + old + entries, &crc, 4);
            f = fopen(state, "wb");
            fwrite(out, 1, old + entries + 4, f);
            fclose(f);
            bool ok = state_load(&s3, &t3, &r3);
            if (version == 2)
                check(ok && t3.bg == mine.bg && s3.brightness == app.set.brightness && s3.lang == app.set.lang
                      && s3.play_at_start == 0 && r3.path[0], "a version 2 state file: settings, theme, position");
            else
                check(ok && t3.bg == theme_default.bg && t3.acc == theme_default.acc && s3.brightness == app.set.brightness
                      && r3.path[0], "a state file from before themes: everything loads, the default theme");
        }
        app.theme = kept;
        save_now();
    }

    // ---- a song with two artists (two values of its artist tag): under both, both shown, either can be tapped
    {
        uint32_t duet = lib_find_path("Bandcamp/Ken Aoba, Aethel - Duet.dsa");
        check(duet != LIB_NONE, "the two-artist song is in the library");
        uint32_t ids[4];
        int n = lib_tag_ids(duet, "artist", ids, 4);
        check(n == 2 && strcmp(lib_value(ids[0]), "Ken Aoba") == 0 && strcmp(lib_value(ids[1]), "Aethel") == 0,
              "both artist values are kept");
        int gk = lib_value_group(ids[0]), ga = lib_value_group(ids[1]);
        check(gk >= 0 && ga >= 0 && gk != ga && lib_track_in_group(duet, gk) && lib_track_in_group(duet, ga),
              "it's in both artists' groups");
        Item list[64];
        int nk = lib_list_tracks(list, ga, LIB_NONE, 0, false), found = 0;
        for (int i = 0; i < nk; i++)
            found |= list[i].id == duet;
        check(found && lib_group_size(ga) == 2, "Aethel's songs: Sunward and Duet");
        char joined[64];
        lib_tag_joined(duet, "artist", ", ", joined, sizeof(joined));
        check(strcmp(joined, "Ken Aoba, Aethel") == 0, "shown as \"Ken Aoba, Aethel\"");
        // play it, tap the second name in the player
        uint32_t one[1] = { duet };
        play_tracks(one, 1, 0, false, SA_JUMP);
        slide_to(true);
        run(40);
        int bx = -1;
        for (int k = 0; k < 400 && bx < 0; k++)
        {
            for (int x = 8; x < 248 && bx < 0; x++)
                if (ui_player_artist_at(x, 12) == 1 && ui_player_artist_at(x + 10, 12) == 1)
                    bx = x;
            if (bx < 0)
                run(1);
        }
        check(bx >= 0, "the second name is found in the player's top line");
        shot("05d_two_artists_player");
        tap(bx + 4, 12);
        check(!app.player_mode && app.artist == ga, "tapping \"Aethel\": Aethel in the browser");
        run(30);
    }

    // ---- shuffle by rating: a 5-star song comes next far more often among 1-star ones
    {
        const LibAlbum *al = NULL;
        for (uint32_t i = 0; i < lib_album_count(); i++)
            if (strcmp(lib_value(lib_album(i)->name), "Paper Archive vol.1") == 0)
                al = lib_album(i);
        uint32_t tr[13];
        int n = al->count < 13 ? al->count : 13;
        for (int i = 0; i < n; i++)
        {
            tr[i] = lib_album_track(al, i);
            stats_set_rating(lib_path(tr[i]), 1);
        }
        stats_set_rating(lib_path(tr[5]), 5);
        bool was = queue_shuffle();
        queue_set_shuffle(true);
        int hits[2] = { 0, 0 };
        bool whole = true;
        for (int pass = 0; pass < 2; pass++)
        {
            app.set.shuffle_weighted = (uint8_t)pass;
            apply_weighting();
            for (int k = 0; k < 300; k++)
            {
                queue_set(tr, n, 0);
                hits[pass] += queue_track(1) == tr[5];
                for (int i = 0; i < n; i++)
                {
                    int seen = 0;
                    for (int j = 0; j < queue_count(); j++)
                        seen += queue_track(j) == tr[i];
                    whole &= seen == 1 && queue_count() == n;
                }
            }
        }
        printf("      (5-star song next: %d / 300 plain, %d / 300 by rating)\n", hits[0], hits[1]);
        check(hits[0] < 60 && hits[1] > 120, "shuffle by rating: 5 stars come next far more often");
        check(whole, "weighted shuffle keeps every song exactly once");
        app.set.shuffle_weighted = 0;
        apply_weighting();
        queue_set_shuffle(was);
    }

    // ---- a cover and its background stay apart in brightness, even a black cover
    {
        static uint16_t cov[128 * 128];
        const int greys[] = { 0, 3, 6, 9, 31 };
        int worst = 255;
        for (unsigned g = 0; g < sizeof(greys) / sizeof(greys[0]); g++)
        {
            for (int i = 0; i < 128 * 128; i++)
                cov[i] = COLOR(greys[g], greys[g], greys[g]);
            int d = ui_cover_bg_contrast(cov);
            printf("      (grey %2d cover: %d apart)\n", greys[g], d);
            worst = d < worst ? d : worst;
        }
        check(worst >= 11, "dark covers keep their distance from the background (above what looks black)");
    }
    {
        // how it looks: the playing song's cover turned black (test only)
        uint16_t *c = (uint16_t *)player_cover();
        static uint16_t saved[128 * 128];
        if (c)
        {
            memcpy(saved, c, sizeof(saved));
            for (int i = 0; i < 128 * 128; i++)
                c[i] = COLOR(0, 0, 0);
            palette_make(&app.theme);       // the background is made again
            slide_to(true);
            run(40);
            shot("17_black_cover");
            memcpy(c, saved, sizeof(saved));
            palette_make(&app.theme);
            slide_to(false);
            run(30);
        }
    }

    // ---- another theme, all over the app
    {
        Theme kept = app.theme;
        app.theme = (Theme){ COLOR(1, 4, 4), COLOR(27, 31, 30), COLOR(6, 27, 23) };
        palette_make(&app.theme);
        run(30);
        shot("16_theme_teal_list");
        slide_to(true);
        run(40);
        shot("16b_theme_teal_player");
        slide_to(false);
        run(30);
        app.theme = kept;
        palette_make(&app.theme);
        run(10);
    }

    // ---- player: the bars fall after pausing; a new song's texts scroll from their start
    {
        slide_to(true);
        run(40);
        if (player_state() != PLAYER_PLAYING)
            player_toggle_pause();
        run(30);
        int top = 0;
        for (int b = 0; b < VIZ_BANDS; b++)
            top = viz_level(b) > top ? viz_level(b) : top;
        player_toggle_pause();
        run(4);
        int mid = 0;
        for (int b = 0; b < VIZ_BANDS; b++)
            mid = viz_level(b) > mid ? viz_level(b) : mid;
        run(80);
        int end = 0;
        for (int b = 0; b < VIZ_BANDS; b++)
            end = viz_level(b) > end ? viz_level(b) : end;
        check(top > 0 && mid > 0 && mid < top && end == 0, "paused: the equalizer bars fall back down");
        uint32_t f0 = app.frame;
        next_song(false);
        check(app.text_t0 >= f0, "a new song: its texts scroll from the start");
        run(40);
        slide_to(false);
        run(40);
    }

    // ---- search (🔍): the engine first, then typing with the stylus
    {
        slide_to(false);
        run(40);
        Item *it = app.items;
        SearchMark *mk = app.marks;
        SearchFilter f;
        search_filter_clear(&f);
        int n = search_run("go", IM_ABC, &f, it, mk, item_cap);
        bool golmo = false, golden = false;
        for (int i = 0; i < n; i++)
        {
            char b[256];
            ui_item_label(&it[i], b, sizeof(b));
            golmo |= strcmp(b, "Gölmo") == 0;
            golden |= strcmp(b, "Golden Hours") == 0;
        }
        check(golmo && golden, "\"go\" finds Gölmo and Golden Hours (accents don't matter)");
        n = search_run("gö", IM_ABC, &f, it, mk, item_cap);
        golmo = golden = false;
        for (int i = 0; i < n; i++)
        {
            char b[256];
            ui_item_label(&it[i], b, sizeof(b));
            golmo |= strcmp(b, "Gölmo") == 0;
            golden |= strcmp(b, "Golden Hours") == 0;
        }
        check(golmo && !golden, "\"gö\" means exactly ö: Gölmo, not Golden Hours");
        n = search_run("GOLMO", IM_ABC, &f, it, mk, item_cap);
        check(n == 2 && it[1].kind == ITEM_ARTIST, "\"GOLMO\" finds Gölmo too (case and accents)");
        n = search_run("らび", IM_KANA, &f, it, mk, item_cap);
        check(n == 2 && it[1].kind == ITEM_TRACK && strcmp(lib_value(lib_track(it[1].id)->title), "ラビットロード") == 0,
              "hiragana らび finds katakana ラビットロード");
        n = search_run("おわり", IM_KANA, &f, it, mk, item_cap);
        check(n == 2 && mk[1].via == VIA_KANA && strcmp(lib_value(lib_track(it[1].id)->title), "終ワリノ鐘") == 0,
              "おわり finds 終ワリノ鐘 by its reading");
        n = search_run("luo", IM_ABC, &f, it, mk, item_cap);
        check(n == 0, "with ABC, \"luo\" doesn't look at pinyin");
        n = search_run("luo", IM_PINYIN, &f, it, mk, item_cap);
        check(n >= 2 && it[0].kind == ITEM_HEADER && it[0].id == SEC_ARTISTS && it[1].kind == ITEM_ARTIST
              && mk[1].via == VIA_PINYIN && mk[1].start == 0 && mk[1].len == 3, "pinyin \"luo\" finds 洛星涵 (luò)");
        n = search_run("luoxinghan", IM_PINYIN, &f, it, mk, item_cap);
        check(n >= 2 && it[1].kind == ITEM_ARTIST && mk[1].len == 12, "\"luoxinghan\" too, spaces don't matter");
        n = search_run("ви", IM_CYR, &f, it, mk, item_cap);
        check(n == 3 && mk[0].count == 2, "\"ви\" finds Винил and Витор");
        n = search_run("kiva", IM_ABC, &f, it, mk, item_cap);
        char first[256];
        ui_item_label(&it[1], first, sizeof(first));
        check(it[0].kind == ITEM_HEADER && strcmp(first, "Kivara") == 0, "\"kiva\": Kivara first (starts with it)");
        search_filter_set(&f, F_IN, IN_ALBUMS);
        n = search_run("e", IM_ABC, &f, it, mk, item_cap);
        bool albums_only = n > 0;
        for (int i = 0; i < n; i++)
            albums_only &= it[i].kind == ITEM_HEADER ? it[i].id == SEC_ALBUMS : it[i].kind == ITEM_ALBUM;
        check(albums_only, "search in: only albums");
        search_filter_clear(&f);
        SearchOpt opts[64];
        int no = search_options(F_GENRE, opts, 64);
        int voc = -1;
        for (int i = 0; i < no; i++)
            if (strcmp(lib_value(opts[i].value), "Vocal Synth") == 0)
                voc = i;
        check(voc >= 0 && opts[voc].count == 3, "genres with counts: Vocal Synth 3");
        search_filter_set(&f, F_GENRE, opts[voc].value);
        n = search_run("", IM_ABC, &f, it, mk, item_cap);
        check(n == 4 && it[0].id == SEC_SONGS && mk[0].count == 3, "genre Vocal Synth, nothing typed: its 3 songs");
        n = search_run("luo", IM_PINYIN, &f, it, mk, item_cap);
        check(n == 3, "with a filter, the query also looks at artists: 洛星涵's 2 Vocal Synth songs");
        search_filter_clear(&f);
        search_filter_set(&f, F_YEAR, 2020);
        n = search_run("", IM_ABC, &f, it, mk, item_cap);
        check(n == 19, "year: the 2020s (18 songs)");
        check(kana_mod(0x304B, KANA_DAKU) == 0x304C && kana_mod(0x306F, KANA_HANDAKU) == 0x3071
              && kana_mod(0x3064, KANA_SMALL) == 0x3063 && kana_mod(0x3063, KANA_SMALL) == 0x3064, "か゛ = が, は゜ = ぱ, つ 小 = っ");

        // the 🔍 tab: the search page slides in, then the keyboard comes up;
        // the tabs and the filters stay as they are meanwhile
        tap(240, 10);
        check(app.tab == TAB_SEARCH && app.kb_up && app.kb_t == 0, "🔍: search, the keyboard on its way");
        static uint16_t tabs_mid[TAB_H * SCREEN_W], tabs_end[TAB_H * SCREEN_W];
        static uint16_t chips_mid[18 * SCREEN_W], chips_end[18 * SCREEN_W];
        run(TAB_TICKS + 2 + KB_TICKS / 2);
        check(app.kb_t > 0 && app.kb_t < KB_TICKS, "halfway up");
        bottom_rows(tabs_mid, 0, TAB_H);
        bottom_rows(chips_mid, CHIPS_Y, CHIPS_Y + 18);
        run(KB_TICKS);
        check(app.kb_t == KB_TICKS, "keyboard up");
        bottom_rows(tabs_end, 0, TAB_H);
        bottom_rows(chips_end, CHIPS_Y, CHIPS_Y + 18);
        check(memcmp(tabs_mid, tabs_end, sizeof(tabs_mid)) == 0 && memcmp(chips_mid, chips_end, sizeof(chips_mid)) == 0,
              "while the keyboard comes up, the tabs and the filters don't flash");
        shot("30_search_open");
        type_text("k");
        int sh_a = app.sheet_t;
        run(SHEET_TICKS / 3);
        int sh_b = app.sheet_t;
        run(SHEET_TICKS / 3);
        int sh_c = app.sheet_t;
        check(sh_a < sh_b && sh_b < sh_c && sh_c < SHEET_TICKS, "the first letter: the results rise onto the top screen");
        type_text("iva");
        check(strcmp(app.query, "kiva") == 0, "typed \"kiva\" on the keys");
        run(SHEET_TICKS + 2);
        check(app.sheet_t == SHEET_TICKS && section_count(SEC_ARTISTS) >= 3, "the results are on the top screen");
        shot("31_search_typing");
        press(KEY_DOWN);
        check(app.sel_on && app.items[app.sel].kind == ITEM_ARTIST, "D-pad: the highlight comes on");
        press(KEY_DOWN);
        run(10);
        shot("31b_search_dpad");
        // ⌫ and the 123 keys
        tap_key(0, K_NUM);
        tap_key('2', K_CHAR);
        check(strcmp(app.query, "kiva2") == 0, "123: typed 2");
        tap_key(0, K_BACK);
        tap_key(0, K_NUM);
        check(strcmp(app.query, "kiva") == 0 && !app.kb_num, "⌫ deletes it, back to the letters");
        // ⮟: the keyboard goes down, the results onto the bottom screen
        tap_key(0, K_DONE);
        check(!app.kb_up, "⮟: keyboard down");
        run(KB_TICKS / 2);
        shot("32a_search_dropping");
        run(KB_TICKS);
        check(app.kb_t == 0, "the results are a list on the bottom screen");
        shot("32_search_results");
        {
            // hold a result: the row menu, next to the row
            int rr = result_row_named("Kivara feat. Ombo");
            hold(100, SEARCH_LIST_Y + (rr - app.scroll) * ROW_H + 8, HOLD_TICKS + 2);
            release();
            check(app.overlay == OV_ROWMENU && app.menu_item == rr, "holding a result: its row menu");
            run(OV_TICKS + 2);
            shot("32b_search_rowmenu");
            press(KEY_B);
            run(OV_TICKS + 2);
        }
        int r = result_row_named("Kivara");
        check(r >= 0 && r < app.scroll + SEARCH_ROWS, "Kivara in the list");
        tap(100, SEARCH_LIST_Y + (r - app.scroll) * ROW_H + 8);
        check(app.tab == TAB_ALBUM && app.artist >= 0 && strcmp(lib_value(lib_group_name(app.artist)), "Kivara") == 0,
              "tapping Kivara: Kivara's albums");
        run(40);
        // back: the query is kept, the keyboard comes up again
        tap(240, 10);
        run(TAB_TICKS + KB_TICKS + 4);
        check(strcmp(app.query, "kiva") == 0 && app.kb_t == KB_TICKS, "back in search: \"kiva\" kept, keyboard up");
        // the × clears it
        tap(240, FIELD_Y + 10);
        check(app.query[0] == 0 && app.n_items == 0, "× clears the query");
        run(SHEET_TICKS + 2);

        // hold o: its other forms, slide onto ö, let go
        type_text("g");
        int ox, oy2;
        key_pos('o', K_CHAR, &ox, &oy2);
        hold(ox, oy2, HOLD_TICKS + 2);
        check(app.acc_key >= 0, "holding o: its other forms");
        hold(ox, oy2, ACC_TICKS + 2);
        check(app.acc_sel == 0, "held still: the first form (ö) is chosen");
        shot("33_search_accents");
        int bx, by, bw;
        kb_forms_box(&kb_keys[app.acc_key], forms_count(&kb_keys[app.acc_key]), &bx, &by, &bw);
        int fy = KB_Y + by + 14;
        drag_to(bx + 2 + 3 * 25 + 12, fy, 6);         // over ô
        hold(bx + 2 + 3 * 25 + 12, fy, 10);
        check(app.acc_sel == 3 && app.acc_vis == bx + 2 + 3 * 25, "the highlight glides to ô");
        drag_to(bx + 2 + 12, fy, 6);                  // back to ö
        hold(bx + 2 + 12, fy, 10);
        release();
        check(strcmp(app.query, "gö") == 0 && app.acc_key < 0, "slid back to ö and let go: \"gö\"");
        run(10);
        check(result_row_named("Gölmo") >= 0 && result_row_named("Golden Hours") < 0, "gö: Gölmo only");
        shot("33b_search_go");
        tap(240, FIELD_Y + 10);

        app.set.keyboard = KB_QWERTZ;
        int zx, zy;
        check(key_pos('z', K_CHAR, &zx, &zy) && zy < KB_Y + 32, "QWERTZ (Settings › Regional): z in the top row");
        app.set.keyboard = KB_QWERTY;
        // the input key: かな pushes ABC out
        tap_key(0, K_IM);
        check(app.im == IM_KANA && app.im_t > 0, "input key: かな");
        run(IM_TICKS / 2);
        shot("34_search_im_push");
        run(IM_TICKS);
        tap_key(0x3089, K_CHAR);                    // ら
        tap_key(0x3072, K_CHAR);                    // ひ
        tap_key(KANA_DAKU, K_KANA);                 // ゛: び
        check(strcmp(app.query, "らび") == 0, "ら ひ ゛ typed らび");
        run(SHEET_TICKS + 2);
        check(result_row_named("ラビットロード") >= 0, "らび finds ラビットロード");
        shot("34b_search_kana");
        tap(240, FIELD_Y + 10);
        // hold the input key: the list; pinyin
        key_pos(0, K_IM, &ox, &oy2);
        hold(ox, oy2, HOLD_TICKS + 2);
        release();
        check(app.overlay == OV_IM, "holding the input key: the list of input methods");
        run(OV_TICKS + 2);
        shot("35_search_im_menu");
        tap(60, ui_im_menu_y() + 20 + IM_PINYIN * 16 + 8);
        run(IM_TICKS + 2);
        check(app.im == IM_PINYIN && app.overlay == OV_NONE, "pinyin chosen");
        type_text("luo");
        run(SHEET_TICKS + 2);
        check(app.n_items >= 2 && app.items[1].kind == ITEM_ARTIST, "pinyin \"luo\": 洛星涵");
        shot("35b_search_pinyin");
        tap(240, FIELD_Y + 10);
        key_pos(0, K_IM, &ox, &oy2);
        hold(ox, oy2, HOLD_TICKS + 2);
        release();
        run(OV_TICKS + 2);
        tap(60, ui_im_menu_y() + 20 + IM_CYR * 16 + 8);
        run(IM_TICKS + 2);
        type_text("ви");
        run(SHEET_TICKS + 2);
        check(app.im == IM_CYR && section_count(SEC_ARTISTS) == 2, "АБВ: \"ви\" finds two artists");
        shot("36_search_cyrillic");
        tap(240, FIELD_Y + 10);
        key_pos(0, K_IM, &ox, &oy2);
        hold(ox, oy2, HOLD_TICKS + 2);
        release();
        run(OV_TICKS + 2);
        tap(60, ui_im_menu_y() + 20 + IM_ABC * 16 + 8);
        run(IM_TICKS + 2);

        // + Filter: the menu, a genre, the chip
        bool on_x;
        int px = -1;
        for (int x = 4; x < 252 && px < 0; x++)
            if (ui_chip_at(x, CHIPS_Y + 8, &on_x) == F_COUNT)
                px = x;
        tap(px + 10, CHIPS_Y + 8);
        check(app.overlay == OV_FILTER && app.fpage < 0, "+ Filter: the filters");
        run(OV_TICKS + 2);
        shot("37_search_filter");
        tap(100, FMENU_Y + 20 + F_GENRE * 16 + 8);
        check(app.fpage == F_GENRE && app.n_fopts > 0, "Genre: its choices");
        run(TAB_TICKS / 2);
        shot("37a_search_genre_push");
        run(TAB_TICKS);
        shot("37b_search_genre");
        int vi = -1;
        for (int i = 0; i < app.n_fopts; i++)
            if (strcmp(lib_value(app.fopts[i].value), "Vocal Synth") == 0)
                vi = i;
        tap(100, FMENU_Y + 20 + (vi - app.fscroll) * 16 + 8);
        check(app.overlay == OV_NONE && search_filter_on(&app.sf, F_GENRE) && section_count(SEC_SONGS) == 3,
              "Vocal Synth: a chip, its 3 songs");
        run(4);
        check(app.chip_w[F_GENRE] > 0 && app.chip_w[F_GENRE] < ui_chip_full_w(F_GENRE), "the chip grows in");
        run(OV_TICKS + SHEET_TICKS + 4);
        shot("37c_search_filtered");
        tap_key(0, K_DONE);
        run(KB_TICKS + 2);
        shot("37d_search_filtered_list");
        int gx = -1;
        for (int x = 4; x < 252 && gx < 0; x++)
            if (ui_chip_at(x, CHIPS_Y + 8, &on_x) == F_GENRE && on_x)
                gx = x;
        tap(gx + 4, CHIPS_Y + 8);
        check(!search_filter_on(&app.sf, F_GENRE) && app.n_items == 0, "× on the chip: the filter is off");
        run(4);
        check(app.chip_w[F_GENRE] > 0, "the chip shrinks away");
        run(20);
        check(app.chip_w[F_GENRE] == 0, "gone");

        // in German
        tap(150, FIELD_Y + 10);
        run(KB_TICKS + 2);
        app.set.lang = i18n_lang = LANG_DE;
        tap(px + 10, CHIPS_Y + 8);
        run(OV_TICKS + 2);
        shot("38_search_de_filter");
        press(KEY_B);
        run(OV_TICKS + 2);
        type_text("go");
        run(SHEET_TICKS + 2);
        shot("38b_search_de");
        app.set.lang = i18n_lang = LANG_EN;
        tap(240, FIELD_Y + 10);
        press(KEY_B);
        run(KB_TICKS + 2);
        show_tab(TAB_ARTIST);
        run(30);
    }

    // ---- the German interface: every text there, every screen drawn, the texts fit
    {
        check(i18n_missing(LANG_DE) == S_NONE && i18n_missing(LANG_EN) == S_NONE, "every text in English and German");
        bool fit = true;
        for (int lang = 0; lang < LANG_COUNT; lang++)
        {
            i18n_lang = lang;
            for (int id = 1; id < S_COUNT; id++)
                if ((id < S_CANT_PLAY || id > S_ERR_BLOCK) && font_text_width(txt(id)) > SCREEN_W - 8)   // toasts scroll
                {
                    printf("      (too wide: %s)\n", txt(id));
                    fit = false;
                }
            for (int i = 0; i < 3; i++)
                fit &= font_text_width(txt(S_TAB_ARTIST + i)) <= tab_x[i + 1] - tab_x[i] - 8;
            fit &= font_text_width(txt(S_ACCENT)) <= THEME_CHIP_W - 4 && font_text_width(txt(S_BACKGROUND)) <= THEME_CHIP_W - 4
                   && font_text_width(txt(S_TEXT)) <= THEME_CHIP_W - 4 && font_text_width(txt(S_PRESETS)) <= THEME_CHIP_W - 4
                   && font_text_width(txt(S_PRESET_MIDNIGHT_ROSE)) <= PRESET_X1 - PRESET_X0 - 46 - 16
                   && font_text_width(txt(S_PRESET_ULTRAVIOLET)) <= PRESET_X1 - PRESET_X0 - 46 - 16;
            for (int i = 0; i < 3; i++)
                fit &= font_text_width(txt(S_PLAY_NOW + i)) <= 240 - 88 - 24 - 4;       // the row menu
            for (int i = S_SORT_ARTISTS; i <= S_SORT_FOLDERS; i++)
                fit &= font_text_width(txt(i)) <= 248 - 96 - 8;                        // sort menu title
            for (int i = S_SORT_NAME_UP; i <= S_SORT_TRACK; i++)
                fit &= font_text_width(txt(i)) <= 248 - 96 - 24 - 4;
            fit &= font_text_width(txt(S_NEXT)) <= 48 && 248 - ui_info_value_x() >= 150;
            fit &= font_text_width(txt(S_NO_MUSIC)) <= 240 && font_text_width(txt(S_MAKE_MUSIC)) <= 240
                   && font_text_width(txt(S_NO_SD)) <= 240 && font_text_width(txt(S_DLDI)) <= 240;
            for (int i = S_LIB_MISSING; i <= S_RUN_DSCONV; i++)
                fit &= font_text_width(txt(i)) <= SCREEN_W - 16;
        }
        i18n_lang = app.set.lang;
        check(fit, "the texts fit their places, in both languages");

        slide_to(false);
        run(40);
        tap(240, BAR_Y + 14);
        run(20);
        tap(100, SET_Y + 4 * SET_H + 10);
        run(20);
        tap(100, SET_Y + 0 * SET_H + 10);
        check(app.set.lang == LANG_DE && strcmp(txt(S_SETTINGS), "Einstellungen") == 0, "tap Language: Deutsch");
        shot("20_de_regional");
        press(KEY_B);
        run(20);
        shot("20_de_settings");
        static const char *const names[] = { "20_de_playback", "20_de_browsing", "20_de_player", "20_de_theme" };
        for (int i = 0; i < 4; i++)
        {
            tap(100, SET_Y + i * SET_H + 10);
            run(20);
            shot(names[i]);
            press(KEY_B);
            run(20);
        }
        tap(100, SET_Y + 5 * SET_H + 10);
        run(20);
        shot("20_de_about");
        press(KEY_B);
        run(20);
        save_now();
        Settings sd;
        Theme td;
        Resume rd;
        check(state_load(&sd, &td, &rd) && sd.lang == LANG_DE, "the language is saved");
        tap(240, 8);
        run(20);
        check(app.overlay == OV_NONE, "settings closed");

        show_tab(TAB_ARTIST);
        run(20);
        shot("21_de_artist");
        tap(176, BAR_Y + 14);
        run(20);
        shot("21_de_sort");
        press(KEY_B);
        run(20);
        app.artist = -1;
        app.album = LIB_NONE;
        show_tab(TAB_ALBUM);
        run(20);
        hold(100, LIST_Y + 1 * ROW_H + 8, 35);
        release();
        check(app.overlay == OV_ROWMENU, "German row menu");
        shot("21_de_rowmenu");
        tap(150, ui_rowmenu_y(LIST_Y + 1 * ROW_H) + 20 + 2 * 16 + 8);
        check(strcmp(app.toast, "Eingereiht") == 0 || strstr(app.toast, "Titel eingereiht") != NULL
              || strcmp(app.toast, "Die Warteschlange ist voll.") == 0, "German toast");
        shot("21_de_toast");
        run(200);
        slide_to(true);
        run(40);
        shot("22_de_player");
        tap(20, 140);
        run(10);
        shot("22_de_info");
        tap(240, 8);
        tap(236, 146);
        run(10);
        shot("22_de_queue");
        tap(240, 8);
        run(10);
        slide_to(false);
        run(40);

        // back to English
        tap(240, BAR_Y + 14);
        run(20);
        tap(100, SET_Y + 4 * SET_H + 10);
        run(20);
        tap(100, SET_Y + 0 * SET_H + 10);
        check(app.set.lang == LANG_EN && strcmp(txt(S_SETTINGS), "Settings") == 0, "and back to English");
        press(KEY_B);
        run(20);
        tap(240, 8);
        run(20);
    }

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
