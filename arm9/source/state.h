// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings and where playback stopped, kept in one small file on the card
// (/_nds/alya-bangers/state.bin).

#ifndef DSPLAYER_STATE_H__
#define DSPLAYER_STATE_H__

#include <stdbool.h>
#include <stdint.h>

#include "palette.h"

enum { RG_OFF, RG_TRACK, RG_ALBUM, RG_AUTO, RG_MODES };
enum { KB_QWERTY, KB_QWERTZ, KB_LAYOUTS };
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
    uint8_t plain_bg;         // player mode: plain background instead of one from the cover
    uint8_t shuffle_weighted; // shuffle: higher rated songs come up more often
    uint8_t lang;             // LANG_EN, LANG_DE (i18n.h)
    uint8_t keyboard;         // search keyboard: KB_QWERTY, KB_QWERTZ
    // state file version 3 from here (older files: 0)
    uint8_t play_at_start;    // launching the app goes on playing where it stopped
    uint8_t eq_profile;       // 0: Flat, n: the n-th saved profile (eqprof.h)
    int8_t eq_gain[3];        // what the equalizer plays with (dB), maybe changed since
    uint8_t reserved[11];     // room for more settings without a new file version
} Settings;

_Static_assert(sizeof(Settings) == 32, "settings are 32 bytes in the file");

typedef struct {
    char path[512];           // current song, relative to the music root
    uint32_t pos_ms;
    bool album_queue;         // the queue is one album (ReplayGain auto uses album gain)
} Resume;

void state_defaults(Settings *s);
void state_set_file(const char *path);
// Restores settings, the theme, the position and, if the library is
// unchanged, the queue. A file from before themes gives the default theme.
bool state_load(Settings *s, Theme *t, Resume *r);
bool state_save(const Settings *s, const Theme *t, const Resume *r);

#endif
