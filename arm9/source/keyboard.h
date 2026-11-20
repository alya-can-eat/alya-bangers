// SPDX-License-Identifier: GPL-3.0-or-later
//
// The search keyboards (bottom screen, y 64..192): where each key is and what
// it does. Drawn by ui.c, pressed in main.c. Positions as in the mockup.

#ifndef DSPLAYER_KEYBOARD_H__
#define DSPLAYER_KEYBOARD_H__

#include <stdbool.h>
#include <stdint.h>

#define KB_H 128                    // height of the keyboard

enum {
    K_CHAR,                         // types cp
    K_SHIFT, K_BACK, K_IM, K_NUM, K_SPACE, K_DONE,
    K_KANA,                         // ゛ ゜ 小 on the last kana (cp: KANA_DAKU, ...)
};

typedef struct {
    int16_t x, y, w, h;             // y from the keyboard's top
    uint8_t kind;
    bool fn;                        // drawn as a function key (darker, dim label)
    uint32_t cp;                    // K_CHAR: the character (already upper case with shift)
} Key;

#define KB_MAX_KEYS 64

// The keys of a layout: im (IM_...), num: the 123 keys, qwertz: Settings ›
// Regional, shift: letters in upper case. Returns how many.
int kb_layout(int im, bool num, bool qwertz, bool shift, Key *out);
// The key at x, y (keyboard coordinates), -1: none.
int kb_key_at(const Key *keys, int n, int x, int y);
// The input key's label: ABC, かな, 拼音, АБВ.
const char *kb_im_label(int im);
// The row of other forms over a held key (keyboard coordinates; it reaches
// above the keyboard): left edge, top, width; forms are 25 px apart from x + 2.
void kb_forms_box(const Key *k, int nforms, int *x, int *y, int *w);

#endif
