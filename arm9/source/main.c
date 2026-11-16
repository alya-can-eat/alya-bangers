// SPDX-License-Identifier: MIT
//
// dsplayer: browse the music folder on the bottom screen, see what's playing
// on the top screen.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fat.h>
#include <nds.h>

#include "browser.h"
#include "font.h"
#include "gfx.h"
#include "player.h"
#include "util.h"

#define LIST_Y      16
#define ROW_H       16
#define ROWS        11

#define C_BG        COLOR(2, 2, 4)
#define C_HEADER    COLOR(5, 5, 9)
#define C_SELECT    COLOR(6, 9, 18)
#define C_TEXT      COLOR(28, 28, 29)
#define C_DIM       COLOR(16, 16, 19)
#define C_FOLDER    COLOR(30, 24, 10)
#define C_ACCENT    COLOR(12, 20, 31)
#define C_WARN      COLOR(31, 12, 10)
#define C_BAR_BG    COLOR(7, 7, 11)

static Browser br;
static char music_root[64];

static char **playlist;
static int playlist_count;
static int playlist_index = -1;

static char message[160];
static int message_frames;

static bool lid_closed;
static bool redraw_bottom = true;
static bool redraw_top = true;

static void service(void)
{
    player_service();
}

static void show_message(const char *fmt, const char *arg)
{
    snprintf(message, sizeof(message), fmt, arg);
    message_frames = 4 * 60;
    redraw_bottom = true;
}

// ------------------------------------------------------------------ playlist

static void playlist_clear(void)
{
    for (int i = 0; i < playlist_count; i++)
        free(playlist[i]);
    free(playlist);
    playlist = NULL;
    playlist_count = 0;
    playlist_index = -1;
}

// Make the songs of the current folder the playlist, starting at entry `sel`.
static void playlist_from_browser(int sel)
{
    playlist_clear();
    int files = 0;
    for (int i = 0; i < br.count; i++)
        files += !br.entries[i].is_dir;
    playlist = calloc(files ? files : 1, sizeof(char *));
    if (playlist == NULL)
        return;
    for (int i = 0; i < br.count; i++)
    {
        if (br.entries[i].is_dir)
            continue;
        char path[BROWSER_PATH_MAX];
        if (!browser_entry_path(&br, i, path, sizeof(path)))
            continue;
        if (i == sel)
            playlist_index = playlist_count;
        playlist[playlist_count] = strdup(path);
        if (playlist[playlist_count] != NULL)
            playlist_count++;
    }
}

// Play playlist entry i; if a file can't be played, try the following ones.
static void play_index(int i, int direction)
{
    char err[64];
    for (int tries = 0; tries < playlist_count; tries++, i += direction)
    {
        if (i < 0 || i >= playlist_count)
            break;
        if (player_play(playlist[i], err, sizeof(err)))
        {
            playlist_index = i;
            redraw_top = true;
            redraw_bottom = true;
            return;
        }
        char what[200];
        snprintf(what, sizeof(what), "%s: %s", path_basename(playlist[i]), err);
        show_message("%s", what);
    }
    player_stop();
    playlist_index = -1;
    redraw_top = true;
    redraw_bottom = true;
}

static void next_song(void)
{
    if (playlist_index >= 0 && playlist_index + 1 < playlist_count)
        play_index(playlist_index + 1, +1);
    else
    {
        player_stop();
        redraw_top = redraw_bottom = true;
    }
}

static void previous_song(void)
{
    if (player_position_ms() > 3000 || playlist_index <= 0)
        player_restart();
    else
        play_index(playlist_index - 1, -1);
    redraw_top = true;
}

// ------------------------------------------------------------------ browser

static bool at_root(void)
{
    return strcmp(br.path, music_root) == 0;
}

static void open_folder(const char *path, const char *select)
{
    Browser next = { 0 };
    if (!browser_open(&next, path, service))
    {
        show_message("Can't open %s", path_basename(path));
        browser_free(&next);
        return;
    }
    browser_free(&br);
    br = next;
    if (select != NULL)
        browser_select_name(&br, select);
    browser_clamp(&br, ROWS);
    redraw_bottom = true;
}

static void go_up(void)
{
    if (at_root())
        return;
    char child[256];
    snprintf(child, sizeof(child), "%s", path_basename(br.path));
    size_t len = strlen(child);
    if (len > 0 && child[len - 1] == '/')
        child[len - 1] = '\0';
    char parent[BROWSER_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", br.path);
    if (path_parent(parent))
        open_folder(parent, child);
}

static void activate_selection(void)
{
    if (br.count == 0)
        return;
    char path[BROWSER_PATH_MAX];
    if (!browser_entry_path(&br, br.selected, path, sizeof(path)))
    {
        show_message("%s", "Path too long");
        return;
    }
    if (br.entries[br.selected].is_dir)
    {
        open_folder(path, NULL);
    }
    else
    {
        playlist_from_browser(br.selected);
        play_index(playlist_index, +1);
    }
}

// Show the folder of the song that is playing, with the song selected.
static void reveal_playing(void)
{
    const char *p = player_path();
    if (p == NULL)
        return;
    char dir[BROWSER_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", p);
    char name[256];
    snprintf(name, sizeof(name), "%s", path_basename(p));
    if (path_parent(dir))
        open_folder(dir, name);
}

// ------------------------------------------------------------------ drawing

static void display_name(char *out, size_t size, const char *file)
{
    snprintf(out, size, "%s", file);
    if (ends_with_ci(out, ".dsa"))
        out[strlen(out) - 4] = '\0';
}

static void draw_bottom(void)
{
    Canvas *c = gfx_canvas(SCREEN_BOTTOM);
    gfx_fill(c, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    gfx_fill(c, 0, 0, SCREEN_W, LIST_Y, C_HEADER);

    if (message_frames > 0)
    {
        font_draw(c, 4, 0, SCREEN_W - 8, message, C_WARN);
    }
    else
    {
        char count[24];
        snprintf(count, sizeof(count), "%d/%d", br.count ? br.selected + 1 : 0, br.count);
        int cw = font_text_width(count);
        font_draw(c, SCREEN_W - 4 - cw, 0, cw, count, C_DIM);
        const char *title = at_root() ? "Music" : path_basename(br.path);
        font_draw(c, 4, 0, SCREEN_W - 16 - cw, title, C_TEXT);
    }

    const char *playing = player_path();
    for (int r = 0; r < ROWS; r++)
    {
        int i = br.scroll + r;
        if (i >= br.count)
            break;
        int y = LIST_Y + r * ROW_H;
        const BrowserEntry *e = &br.entries[i];
        if (i == br.selected)
            gfx_fill(c, 0, y, SCREEN_W, ROW_H, C_SELECT);

        char name[256];
        if (e->is_dir)
        {
            gfx_fill(c, 5, y + 4, 4, 2, C_FOLDER);   // folder icon
            gfx_fill(c, 5, y + 6, 10, 7, C_FOLDER);
            font_draw(c, 20, y, SCREEN_W - 26, e->name, C_FOLDER);
        }
        else
        {
            char full[BROWSER_PATH_MAX];
            if (playing != NULL && browser_entry_path(&br, i, full, sizeof(full))
                && strcmp(full, playing) == 0)
                font_draw(c, 4, y, 16, "♪", C_ACCENT);
            display_name(name, sizeof(name), e->name);
            font_draw(c, 20, y, SCREEN_W - 26, name, C_TEXT);
        }
    }
    if (br.count == 0)
        font_draw(c, 8, LIST_Y + 8, SCREEN_W - 16, "No songs or folders here", C_DIM);

    if (br.count > ROWS) // scroll bar
    {
        int track = ROWS * ROW_H;
        int h = track * ROWS / br.count;
        if (h < 6)
            h = 6;
        int y = LIST_Y + (track - h) * br.scroll / (br.count - ROWS);
        gfx_fill(c, SCREEN_W - 3, y, 2, h, C_DIM);
    }
}

static void draw_centered(Canvas *c, int y, const char *s, uint16_t color)
{
    int w = font_text_width(s);
    if (w > SCREEN_W - 8)
        w = SCREEN_W - 8;
    font_draw(c, (SCREEN_W - w) / 2, y, SCREEN_W - 8, s, color);
}

static void draw_top(void)
{
    Canvas *c = gfx_canvas(SCREEN_TOP);
    gfx_fill(c, 0, 0, SCREEN_W, SCREEN_H, C_BG);

    if (player_state() == PLAYER_IDLE)
    {
        font_draw(c, 8, 8, 240, "dsplayer", C_ACCENT);
        font_draw(c, 8, 40, 240, "A       play / open folder", C_TEXT);
        font_draw(c, 8, 56, 240, "B       back", C_TEXT);
        font_draw(c, 8, 72, 240, "START   pause / resume", C_TEXT);
        font_draw(c, 8, 88, 240, "L / R   previous / next song", C_TEXT);
        font_draw(c, 8, 104, 240, "X / Y   +10 s / -10 s", C_TEXT);
        font_draw(c, 8, 120, 240, "SELECT  show playing song", C_TEXT);
        if (!font_loaded())
            font_draw(c, 8, 160, 240, "font.bin missing: ASCII only", C_WARN);
        return;
    }

    const uint16_t *cover = player_cover();
    if (cover != NULL)
    {
        gfx_blit(c, 64, 4, DSA_COVER_SIZE, DSA_COVER_SIZE, cover);
    }
    else
    {
        gfx_fill(c, 64, 4, 128, 128, C_HEADER);
        font_draw(c, 124, 60, 16, "♪", C_DIM);
    }

    const DsaTags *t = player_tags();
    char title[256];
    if (t->title[0])
        snprintf(title, sizeof(title), "%s", t->title);
    else
        display_name(title, sizeof(title), path_basename(player_path()));
    draw_centered(c, 136, title, C_TEXT);

    char line[340];
    if (t->artist[0] && t->album[0])
        snprintf(line, sizeof(line), "%s · %s", t->artist, t->album);
    else
        snprintf(line, sizeof(line), "%s", t->artist[0] ? t->artist : t->album);
    draw_centered(c, 153, line, C_DIM);

    char pos[16], dur[16], times[48];
    format_time(pos, sizeof(pos), player_position_ms());
    format_time(dur, sizeof(dur), player_duration_ms());
    snprintf(times, sizeof(times), "%s%s / %s",
             player_state() == PLAYER_PAUSED ? "Paused  " : "", pos, dur);
    int tw = font_text_width(times);
    font_draw(c, SCREEN_W - 6 - tw, 174, tw, times, C_TEXT);

    int bar_w = SCREEN_W - 6 - tw - 8 - 6;
    if (bar_w > 8)
    {
        gfx_fill(c, 6, 181, bar_w, 3, C_BAR_BG);
        uint32_t d = player_duration_ms();
        int fill = d ? (int)((uint64_t)bar_w * player_position_ms() / d) : 0;
        gfx_fill(c, 6, 181, fill, 3, C_ACCENT);
    }

    uint32_t u = player_underruns();
    if (u)
    {
        char dropouts[24];
        snprintf(dropouts, sizeof(dropouts), "dropouts: %lu", (unsigned long)u);
        font_draw(c, 2, 0, 120, dropouts, C_WARN);
    }
}

static void fatal_screen(const char *line1, const char *line2)
{
    Canvas *c = gfx_canvas(SCREEN_TOP);
    gfx_fill(c, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    font_draw(c, 8, 8, 240, "dsplayer", C_ACCENT);
    font_draw(c, 8, 40, 240, line1, C_WARN);
    font_draw(c, 8, 56, 240, line2, C_TEXT);
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

// ------------------------------------------------------------------ main

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

static void handle_input(void)
{
    scanKeys();
    uint32_t down = keysDown();
    uint32_t rep = keysDownRepeat();
    uint32_t held = keysHeld();

    // Lid closed: backlights off, music keeps playing.
    if (held & KEY_LID)
    {
        if (!lid_closed)
        {
            lid_closed = true;
            powerOff(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);
        }
        return;
    }
    if (lid_closed)
    {
        lid_closed = false;
        powerOn(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);
        redraw_top = redraw_bottom = true;
    }

    int old_sel = br.selected;
    if (rep & KEY_UP)
        br.selected--;
    if (rep & KEY_DOWN)
        br.selected++;
    if (rep & KEY_LEFT)
        br.selected -= ROWS;
    if (rep & KEY_RIGHT)
        br.selected += ROWS;
    if (br.selected != old_sel)
    {
        browser_clamp(&br, ROWS);
        redraw_bottom = true;
    }

    if (down & KEY_A)
        activate_selection();
    if (down & KEY_B)
        go_up();
    if (down & KEY_SELECT)
        reveal_playing();
    if (down & KEY_START)
    {
        player_toggle_pause();
        redraw_top = true;
    }
    if (down & KEY_R)
        next_song();
    if (down & KEY_L)
        previous_song();
    if (rep & KEY_X)
    {
        player_seek_seconds(10);
        redraw_top = true;
    }
    if (rep & KEY_Y)
    {
        player_seek_seconds(-10);
        redraw_top = true;
    }
}

int main(int argc, char **argv)
{
    defaultExceptionHandler();
    gfx_init();
    player_init();
    keysSetRepeat(18, 5);
    disableSleep();

    if (!fatInitDefault())
        fatal_screen("Can't read the SD card.", "Is the flashcart's DLDI driver OK?");
    if (!find_music_root())
        fatal_screen("No /music folder on the card.", "Convert your music there with dsconv.");

    char font_path[128];
    snprintf(font_path, sizeof(font_path), "%s/.dsmusic/font.bin", music_root);
    font_load(font_path);

    open_folder(music_root, NULL);

    uint32_t last_pos_step = 0xFFFFFFFF;
    PlayerState last_state = PLAYER_IDLE;
    uint32_t last_underruns = 0;

    while (1)
    {
        swiWaitForVBlank();
        if (!lid_closed)
            gfx_present();

        player_service();
        handle_input();
        player_service();

        PlayerState st = player_state();
        if (st == PLAYER_ENDED)
        {
            next_song();
            st = player_state();
        }

        // Redraw the top screen when the state changes or every half second.
        uint32_t step = player_position_ms() / 500;
        if (st != last_state || step != last_pos_step || player_underruns() != last_underruns)
        {
            if (st != last_state)
                redraw_bottom = true; // the ♪ marker moves
            last_state = st;
            last_pos_step = step;
            last_underruns = player_underruns();
            redraw_top = true;
        }

        if (message_frames > 0 && --message_frames == 0)
            redraw_bottom = true;

        if (!lid_closed)
        {
            if (redraw_top)
            {
                draw_top();
                redraw_top = false;
            }
            if (redraw_bottom)
            {
                draw_bottom();
                redraw_bottom = false;
            }
        }
    }
}
