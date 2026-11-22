// SPDX-License-Identifier: GPL-3.0-or-later
//
// The equalizer profiles you saved (/_nds/alya-bangers/eq.bin), in the order
// you put them. Flat isn't stored: it's always there, above them.

#ifndef DSPLAYER_EQPROF_H__
#define DSPLAYER_EQPROF_H__

#include <stdbool.h>
#include <stdint.h>

#include "eq.h"

#define EQP_MAX  7                  // with Flat they fill the panel's list
#define EQP_NAME 32                 // UTF-8, with its 0

typedef struct {
    char name[EQP_NAME];
    int8_t gain[EQ_BANDS];
    uint8_t reserved[5];
} EqProfile;

_Static_assert(sizeof(EqProfile) == 40, "40 bytes per profile in the file");

void eqp_set_file(const char *path);
void eqp_load(void);
bool eqp_save(void);

int eqp_count(void);
const EqProfile *eqp_get(int i);
// Adds one at the end: its index, -1 if there are EQP_MAX already.
int eqp_add(const char *name, const int8_t gain[EQ_BANDS]);
void eqp_set_gain(int i, const int8_t gain[EQ_BANDS]);
void eqp_remove(int i);
// Moves profile `from` to place `to`, the others close up.
void eqp_move(int from, int to);

#endif
