// SPDX-License-Identifier: GPL-3.0-or-later
//
// Messages between the ARM9 (reads the card, runs the UI) and the ARM7 (drives
// the sound hardware).

#ifndef DSPLAYER_IPC_H__
#define DSPLAYER_IPC_H__

#include <stdint.h>

#include <nds/fifocommon.h>

// ARM9 -> ARM7: StreamCmd as a datamsg.
#define FIFO_STREAM_CMD     FIFO_USER_01
// ARM7 -> ARM9: 32-bit events (see STREAM_EVT_*).
#define FIFO_STREAM_EVENT   FIFO_USER_02

typedef enum {
    STREAM_CMD_START  = 1, // start both channels and the block timer
    STREAM_CMD_STOP   = 2, // stop (after fading out, if fade_frames); ARM7 answers with
                           // STREAM_EVT_FRAME, then STREAM_EVT_STOPPED
    STREAM_CMD_VOLUME = 3, // change channel volume while playing
    STREAM_CMD_CHIME  = 4, // play a short sound once on a channel of its own, over the music:
                           // left = its 16-bit samples (main RAM), ring_bytes = their size,
                           // timer_divider, volume
    STREAM_CMD_BOOT   = 5, // another .nds starts (arm9 boot.c): stop the sound, wait for VRAM C/D
                           // and the card, then jump into Pico Loader's ARM7 part
} StreamCmdType;

typedef struct {
    uint32_t cmd;           // StreamCmdType
    uint32_t left;          // address of the left ring buffer (main RAM, 16-bit PCM)
    uint32_t right;         // address of the right ring buffer (same as left for mono)
    uint32_t ring_bytes;    // size of each ring buffer in bytes
    uint32_t block_frames;  // samples per block: the ARM7 reports every finished block
    uint16_t timer_divider; // sound clock divider: 512 = 32728.5 Hz
    uint8_t  volume;        // channel volume, 0..127
    uint8_t  generation;    // echoed back in events, so stale events can be ignored
    uint16_t fade_frames;   // STOP: turn the volume down to 0 over this many samples first
    uint16_t reserved;
} StreamCmd;

// ARM7 -> ARM9 event word: [31:28] type, [27:24] generation, [23:0] count.
#define STREAM_EVT_BLOCK        (0u << 28)  // count = blocks fully played since START
#define STREAM_EVT_STOPPED      (1u << 28)  // count = blocks fully played when stopped
#define STREAM_EVT_FRAME        (2u << 28)  // count = samples played of the next block when stopped
#define STREAM_EVT_TYPE_MASK    (0xFu << 28)
#define STREAM_EVT_GEN(v)       (((v) >> 24) & 0xF)
#define STREAM_EVT_COUNT(v)     ((v) & 0xFFFFFF)
#define STREAM_EVT_MAKE(type, gen, count) \
    ((type) | (((uint32_t)(gen) & 0xF) << 24) | ((uint32_t)(count) & 0xFFFFFF))

#endif // DSPLAYER_IPC_H__
