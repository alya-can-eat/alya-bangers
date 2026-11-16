// SPDX-License-Identifier: MIT
//
// Renders the real UI code (main.c) on the PC and writes screenshots.
//
//   ./screens /path/to/card/music out_dir

#include <stdio.h>

#define main dsplayer_main
#include "../../arm9/source/main.c"
#undef main

#include "hostsim.h"

static const char *out_dir;

static void shot(const char *name, Screen s)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
    if (s == SCREEN_TOP)
        draw_top();
    else
        draw_bottom();
    sim_write_ppm(path, gfx_canvas(s)->px);
}

static void play_blocks(int n)
{
    static int16_t l[4096], r[4096];
    for (int i = 0; i < n; i++)
    {
        player_service();
        sim_play_block(l, r);
    }
    player_service();
}

static void enter(const char *name)
{
    browser_select_name(&br, name);
    browser_clamp(&br, ROWS);
    activate_selection();
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s card/music out_dir\n", argv[0]);
        return 2;
    }
    out_dir = argv[2];
    gfx_init();
    player_init();
    snprintf(music_root, sizeof(music_root), "%s", argv[1]);
    char font_path[512];
    snprintf(font_path, sizeof(font_path), "%s/.dsmusic/font.bin", music_root);
    if (!font_load(font_path))
        printf("font.bin not loaded\n");
    open_folder(music_root, NULL);

    shot("01-top-idle", SCREEN_TOP);
    shot("02-bottom-root", SCREEN_BOTTOM);

    enter("YouTube Music");
    browser_select_name(&br, "VXLTRA - 00\xEF\xBC\x9A" "00.dsa");
    browser_clamp(&br, ROWS);
    shot("03-bottom-youtube", SCREEN_BOTTOM);

    enter("Kumoyuki - \xE6\x98\xA5\xE9\xA2\xA8\xE3\x83\xAC\xE3\x82\xBF\xE3\x83\xBC.dsa"); // 春風レター
    play_blocks(30);
    shot("04-top-kumoyuki-no-cover", SCREEN_TOP);
    shot("05-bottom-playing-marker", SCREEN_BOTTOM);

    go_up();
    enter("Soundcloud");
    browser_select_name(&br, "minto - \xE6\x98\x9F\xE4\xB9\x8B\xE6\xB2\xB3 (feat. \xE6\x9E\x97\xE6\x98\x9F\xE6\x9C\x88).dsa");
    activate_selection();
    play_blocks(2);
    shot("06-top-chinese-cover", SCREEN_TOP);

    player_toggle_pause();
    shot("07-top-paused", SCREEN_TOP);
    player_toggle_pause();

    go_up();
    enter("Bandcamp");
    enter("300 Lanterns");
    enter("300 Lanterns - HALCYON");
    shot("08-bottom-album", SCREEN_BOTTOM);
    activate_selection();
    play_blocks(3);
    shot("09-top-album-cover", SCREEN_TOP);

    show_message("Can't open %s", "some folder with a very long name that does not fit");
    shot("10-bottom-message", SCREEN_BOTTOM);

    // Let the song end: the next one in the folder starts by itself.
    char before[256];
    snprintf(before, sizeof(before), "%s", player_tags()->title);
    while (player_state() == PLAYER_PLAYING)
        play_blocks(1);
    if (player_state() == PLAYER_ENDED)
        next_song();
    printf("auto-next: '%s' -> '%s'\n", before, player_tags() ? player_tags()->title : "(stopped)");
    shot("11-top-after-auto-next", SCREEN_TOP);
    return 0;
}
