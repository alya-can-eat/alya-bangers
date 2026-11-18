// SPDX-License-Identifier: MIT
//
// Settings and where playback stopped, kept in one small file on the card
// (/_nds/dsplayer/state.bin).

#ifndef DSPLAYER_STATE_H__
#define DSPLAYER_STATE_H__

#include <stdbool.h>
#include <stdint.h>

enum { RG_OFF, RG_TRACK, RG_ALBUM, RG_AUTO, RG_MODES };
enum { SLOT_ARTIST, SLOT_ALBUM, SLOT_TITLE, SLOT_TITLE_IN_ALBUM, SLOT_FOLDER, SORT_SLOTS };

typedef struct {
    uint8_t scroll_all;       // scroll every long row, not just the selected one
    uint8_t case_sensitive;   // "KIVARA" and "Kivara" are different artists
    uint8_t pause_on_lid;
    uint8_t lr_skip_lid;      // L/R change songs while the lid is closed
    uint8_t rg_mode;
    uint8_t brightness;       // 2..5 (DS Lite levels)
    uint8_t sort[SORT_SLOTS];
    uint8_t show_remaining;   // right of the progress bar: time left instead of the length
    uint8_t reserved[4];
} Settings;

_Static_assert(sizeof(Settings) == 16, "settings are 16 bytes in the file");

typedef struct {
    char path[512];           // current song, relative to the music root
    uint32_t pos_ms;
    bool album_queue;         // the queue is one album (ReplayGain auto uses album gain)
} Resume;

void state_defaults(Settings *s);
void state_set_file(const char *path);
// Restores settings, the position and, if the library is unchanged, the queue.
bool state_load(Settings *s, Resume *r);
bool state_save(const Settings *s, const Resume *r);

#endif
