// SPDX-License-Identifier: GPL-3.0-or-later
//
// The colours of the app, all made from a theme of three: background, text
// and accent. Everything else is a mix of those, so any theme keeps the same
// contrasts and look.

#ifndef DSPLAYER_PALETTE_H__
#define DSPLAYER_PALETTE_H__

#include <stdint.h>

enum {
    P_BG,           // main background
    P_ZEBRA,        // every other list row
    P_PANEL,        // headers, tab bar, menus
    P_LINE,         // separators, switches off, the queue's cursor
    P_FAINT,        // hints ("later")
    P_DIM,          // secondary text, icons
    P_TEXT,
    P_ACC,          // highlights, switches on, stars, visualizer, glare
    P_ACC_DIM,      // the record's label
    P_SEL,          // selected row, playing song
    P_VINYL,        // the record: body, grooves, rim, light marks
    P_GROOVE,
    P_RIM,
    P_MARK,
    P_COUNT
};

typedef struct {
    uint16_t bg, text, acc;     // DS colours (5 bits per channel, bit 15 set)
} Theme;

extern uint16_t pal[P_COUNT];
extern const Theme theme_default;

void palette_make(const Theme *t);
// Goes up with every palette_make, so cached drawings know to redo themselves.
uint32_t palette_version(void);

#endif
