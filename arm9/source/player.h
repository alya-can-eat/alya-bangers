// SPDX-License-Identifier: MIT
//
// Streams one .dsa file from the card into the sound hardware.

#ifndef DSPLAYER_PLAYER_H__
#define DSPLAYER_PLAYER_H__

#include <stdbool.h>
#include <stdint.h>

#include "dsa.h"

typedef enum {
    PLAYER_IDLE,     // nothing loaded
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_ENDED,    // reached the end of the song (the caller picks the next one)
} PlayerState;

void player_init(void);

// Open a song and start playing it. On failure returns false and fills err.
bool player_play(const char *path, char *err, int err_size);

// Keep the ring buffers full. Call often: every frame, and inside long loops.
void player_service(void);

void player_toggle_pause(void);
void player_stop(void);

// Jump by a number of seconds (negative = back), clamped to the song.
void player_seek_seconds(int seconds);

// Restart the current song from the beginning.
void player_restart(void);

PlayerState player_state(void);
uint32_t player_position_ms(void);
uint32_t player_duration_ms(void);
const char *player_path(void);
const DsaTags *player_tags(void);
// 128x128 BGR555 cover, or NULL.
const uint16_t *player_cover(void);
uint32_t player_underruns(void);

#endif
