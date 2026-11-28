// SPDX-License-Identifier: GPL-3.0-or-later
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

// Open a song at a position, playing or paused.
bool player_open(const char *path, uint32_t start_ms, bool play, char *err, int err_size);

// Jump to a position (rounded to the nearest 125 ms block).
void player_seek_ms(uint32_t ms);

// Hardware volume 0..127 (ReplayGain is applied this way).
void player_set_volume(uint8_t v);

// Call every loop with the vblanks since the last call: tracks where inside
// a block playback is.
void player_tick(uint32_t elapsed);

// The n (<= 4096) mono samples playing right now, for the visualizer.
bool player_peek(int16_t *out, int n);

// Like player_position_ms(), but moving smoothly between block reports.
uint32_t player_position_smooth_ms(void);

const DsaHeader *player_header(void);

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

// The music fades in over this many vblanks (60 per second) from the next
// start on: the volume rises from 0, slowly at first.
void player_fade_in(uint32_t vblanks);
// The music fades out over this many vblanks and stays silent (0: back to
// the song's volume).
void player_fade_out(uint32_t vblanks);

// A short two-note sound over the music (the low battery warning), on a
// sound channel of its own: heard whether a song plays or not.
void player_chime(void);

#endif
