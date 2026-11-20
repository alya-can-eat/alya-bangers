// SPDX-License-Identifier: GPL-3.0-or-later
//
// The play queue: track ids in play order. Shuffle reorders the list and
// remembers the original order, so turning it off restores it.

#ifndef DSPLAYER_QUEUE_H__
#define DSPLAYER_QUEUE_H__

#include <stdbool.h>
#include <stdint.h>

enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };

typedef struct {
    uint32_t track;
    uint32_t seq;       // position in the original (unshuffled) order
} QueueEntry;

void queue_clear(void);
// Replace everything; start = index in tracks of the song to play first.
bool queue_set(const uint32_t *tracks, int n, int start);
bool queue_insert_next(const uint32_t *tracks, int n);
bool queue_append(const uint32_t *tracks, int n);
void queue_remove(int pos);
void queue_move(int from, int to);

int queue_count(void);
int queue_pos(void);                // -1 = nothing current
uint32_t queue_track(int pos);
void queue_jump(int pos);

bool queue_shuffle(void);
void queue_set_shuffle(bool on);
// Shuffle weighted: a song with twice the weight is twice as likely to come
// next (weights 1..255; NULL = all the same).
void queue_set_weight_fn(uint32_t (*weight)(uint32_t track));
// Shuffle the songs after the current one again (shuffle on only).
void queue_reshuffle_rest(void);
int queue_repeat(void);
void queue_set_repeat(int mode);

// Position after the current song: automatic = the song ended by itself
// (repeat one plays it again). -1 = stop.
int queue_next_pos(bool automatic);
int queue_prev_pos(void);

// For saving and restoring.
const QueueEntry *queue_entries(void);
bool queue_restore(const QueueEntry *e, int n, int pos, bool shuffle, int repeat);

#endif
