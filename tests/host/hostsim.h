// SPDX-License-Identifier: GPL-3.0-or-later
//
// PC stand-in for the DS: mock libnds functions and a simulated ARM7 that
// "plays" the ring buffers block by block, recording what would be heard.

#ifndef HOSTSIM_H__
#define HOSTSIM_H__

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool running;
    const int16_t *left, *right;
    uint32_t ring_bytes;
    uint32_t block_frames;
    uint16_t timer_divider;
    uint8_t generation;
    uint32_t played;
    int starts, stops;
    uint32_t stop_frame;    // set by a test: the sample within the next block a stop reports
    uint16_t last_fade;     // fade_frames of the last stop
    int chimes;             // the chime played (STREAM_CMD_CHIME)
    const int16_t *chime;
    uint32_t chime_bytes;
} SimHardware;

extern SimHardware sim_hw;

// Play one block: copy the current ring slot into left/right (block_frames
// samples each) and send the "block finished" event. Returns false if the
// hardware is stopped.
bool sim_play_block(int16_t *left, int16_t *right);

// Key state for the next scanKeys().
void sim_set_keys(uint32_t down, uint32_t held);

// Stylus position for the next touchRead() (KEY_TOUCH goes in the held keys).
void sim_set_touch(int x, int y);
extern int sim_backlight;

// Write a 256x192 BGR555 canvas as a binary PPM image.
void sim_write_ppm(const char *path, const uint16_t *px);

#endif
