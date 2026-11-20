// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streaming: two ring buffers (left, right) of RING_BLOCKS blocks each. The
// ARM7 plays them in a loop and reports every finished block; we then load
// the next block of the file into the slot that was just freed.
//
// A .dsa block stores all left samples, then all right samples, so each half
// is read straight from the card into its ring with no copying.

#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "ipc.h"
#include "player.h"
#include "i18n.h"

#define RING_BLOCKS     8                       // 8 x 125 ms = 1 s of buffer
#define START_BLOCKS    4                       // read before starting; the rest follows
#define BLOCK_FRAMES    4096
#define BLOCK_BYTES     (BLOCK_FRAMES * 2)      // per channel
#define RING_BYTES      (RING_BLOCKS * BLOCK_BYTES)

static int16_t ring_l[RING_BLOCKS * BLOCK_FRAMES] __attribute__((aligned(32)));
static int16_t ring_r[RING_BLOCKS * BLOCK_FRAMES] __attribute__((aligned(32)));
static uint16_t cover[DSA_COVER_SIZE * DSA_COVER_SIZE] __attribute__((aligned(32)));

static FILE *song_file;
static char cur_path[512];
static DsaHeader hdr;
static DsaTags tags;
static bool has_cover;
static PlayerState state = PLAYER_IDLE;

static bool running;                // hardware is playing
static uint32_t base_block;         // song block that sits in ring slot 0 at start
static uint32_t loaded;             // blocks loaded since start
static uint32_t paused_block;       // where to continue after a pause
static volatile uint32_t played;    // blocks finished since start, from the ARM7
static volatile bool stop_ack;
static uint8_t generation;
static long file_pos = -1;
static uint32_t underruns;
static uint8_t volume = 127;
static uint32_t tick_played, tick_frames;   // last report seen, frames since: player_peek, smooth position

static void event_handler(u32 value, void *userdata)
{
    if (STREAM_EVT_GEN(value) != (generation & 0xF))
        return; // left over from a previous start
    played = STREAM_EVT_COUNT(value);
    if ((value & STREAM_EVT_TYPE_MASK) == STREAM_EVT_STOPPED)
        stop_ack = true;
}

void player_init(void)
{
    fifoSetValue32Handler(FIFO_STREAM_EVENT, event_handler, NULL);
}

static void send_cmd(const StreamCmd *c)
{
    fifoSendDatamsg(FIFO_STREAM_CMD, sizeof(*c), (u8 *)c);
}

// Stop the hardware and wait until the ARM7 has confirmed, so `played` is final
// and the rings may be overwritten.
static void hw_stop(void)
{
    if (!running)
        return;
    StreamCmd c = { 0 };
    c.cmd = STREAM_CMD_STOP;
    c.generation = generation;
    stop_ack = false;
    send_cmd(&c);
    for (int i = 0; i < 20000 && !stop_ack; i++)
        swiDelay(100);
    running = false;
}

static void load_block(uint32_t rel)
{
    uint32_t block = base_block + rel;
    int slot = rel % RING_BLOCKS;
    int16_t *l = ring_l + slot * BLOCK_FRAMES;
    int16_t *r = ring_r + slot * BLOCK_FRAMES;
    bool stereo = hdr.channels == 2;

    size_t got_l = 0, got_r = 0;
    if (block < hdr.block_count)
    {
        long off = (long)hdr.audio_offset + (long)block * BLOCK_BYTES * hdr.channels;
        if (off == file_pos || fseek(song_file, off, SEEK_SET) == 0)
        {
            got_l = fread(l, 1, BLOCK_BYTES, song_file);
            if (stereo && got_l == BLOCK_BYTES)
                got_r = fread(r, 1, BLOCK_BYTES, song_file);
            file_pos = off + (long)(got_l + got_r);
        }
        else
        {
            file_pos = -1;
        }
    }
    // Past the end, or a read error: silence.
    if (got_l < BLOCK_BYTES)
        memset((uint8_t *)l + got_l, 0, BLOCK_BYTES - got_l);
    if (stereo && got_r < BLOCK_BYTES)
        memset((uint8_t *)r + got_r, 0, BLOCK_BYTES - got_r);

    // The sound hardware reads main RAM, not the ARM9 cache.
    DC_FlushRange(l, BLOCK_BYTES);
    if (stereo)
        DC_FlushRange(r, BLOCK_BYTES);
}

static void hw_start(uint32_t block)
{
    hw_stop();

    base_block = block;
    generation = (generation + 1) & 0xF; // first, so late events of the old run are ignored
    played = 0;
    tick_played = 0;
    tick_frames = 0;
    loaded = 0;

    while (loaded < START_BLOCKS)
        load_block(loaded++);

    StreamCmd c = { 0 };
    c.cmd = STREAM_CMD_START;
    c.left = (uint32_t)ring_l;
    c.right = (uint32_t)(hdr.channels == 2 ? ring_r : ring_l); // mono: same data on both sides
    c.ring_bytes = RING_BYTES;
    c.block_frames = BLOCK_FRAMES;
    c.timer_divider = hdr.timer_divider;
    c.volume = volume;
    c.generation = generation;
    send_cmd(&c);
    running = true;
}

static void close_file(void)
{
    hw_stop();
    if (song_file != NULL)
        fclose(song_file);
    song_file = NULL;
    file_pos = -1;
    has_cover = false;
    state = PLAYER_IDLE;
}

static uint32_t ms_to_block(uint32_t ms)
{
    uint64_t frames = (uint64_t)ms * hdr.sound_clock_hz / hdr.timer_divider / 1000;
    uint32_t block = (uint32_t)((frames + BLOCK_FRAMES / 2) / BLOCK_FRAMES);
    if (hdr.block_count && block >= hdr.block_count)
        block = hdr.block_count - 1;
    return block;
}

bool player_open(const char *path, uint32_t start_ms, bool play, char *err, int err_size)
{
    close_file();

    song_file = fopen(path, "rb");
    if (song_file == NULL)
    {
        snprintf(err, err_size, "%s", txt(S_ERR_OPEN));
        return false;
    }
    if (!dsa_read_header(song_file, &hdr, err, err_size))
    {
        close_file();
        return false;
    }
    if (hdr.block_frames != BLOCK_FRAMES)
    {
        snprintf(err, err_size, txt(S_ERR_BLOCK), (unsigned long)hdr.block_frames);
        close_file();
        return false;
    }
    dsa_read_tags(song_file, &hdr, &tags);
    has_cover = dsa_read_cover(song_file, &hdr, cover);
    file_pos = -1;

    snprintf(cur_path, sizeof(cur_path), "%s", path);
    underruns = 0;
    uint32_t block = start_ms ? ms_to_block(start_ms) : 0;
    if (play)
    {
        hw_start(block);
        state = PLAYER_PLAYING;
    }
    else
    {
        paused_block = block;
        state = PLAYER_PAUSED;
    }
    return true;
}

bool player_play(const char *path, char *err, int err_size)
{
    return player_open(path, 0, true, err, err_size);
}

void player_service(void)
{
    if (state != PLAYER_PLAYING)
        return;

    uint32_t p = played;
    if ((uint64_t)(base_block + p) * BLOCK_FRAMES >= hdr.frame_count)
    {
        hw_stop();
        state = PLAYER_ENDED;
        return;
    }
    if (loaded <= p)
    {
        // The card was too slow and the hardware is already playing a slot
        // that wasn't refilled. Skip ahead to the present, starting with the
        // slot being played, so the rest of it is already right again.
        underruns++;
        loaded = p;
    }
    // Playback comes first, but the reading is spread out: while at least
    // 3 blocks (0.375 s) are ready ahead, one block per call; below that, all
    // that's missing at once, however long drawing has to wait.
    while (loaded < p + RING_BLOCKS)
    {
        load_block(loaded++);
        p = played;
        if (loaded >= p + 4)
            break;
    }
}

static uint32_t current_block(void)
{
    if (state == PLAYER_PAUSED)
        return paused_block;
    return base_block + played;
}

void player_toggle_pause(void)
{
    if (state == PLAYER_PLAYING)
    {
        hw_stop();
        paused_block = base_block + played; // resumes at the start of the block that was playing
        state = PLAYER_PAUSED;
    }
    else if (state == PLAYER_PAUSED)
    {
        hw_start(paused_block);
        state = PLAYER_PLAYING;
    }
}

static void jump_to_block(int64_t block)
{
    if (block < 0)
        block = 0;
    if (block >= hdr.block_count)
        block = hdr.block_count ? hdr.block_count - 1 : 0;
    if (state == PLAYER_PAUSED)
    {
        paused_block = block;
    }
    else if (state == PLAYER_PLAYING || state == PLAYER_ENDED)
    {
        hw_start(block);
        state = PLAYER_PLAYING;
    }
}

void player_seek_seconds(int seconds)
{
    if (state == PLAYER_IDLE)
        return;
    // frames per second = clock / divider; round to the nearest block
    int64_t frames = (int64_t)seconds * hdr.sound_clock_hz / hdr.timer_divider;
    int64_t delta = (frames + (frames >= 0 ? BLOCK_FRAMES / 2 : -BLOCK_FRAMES / 2)) / BLOCK_FRAMES;
    jump_to_block((int64_t)current_block() + delta);
}

void player_seek_ms(uint32_t ms)
{
    if (state != PLAYER_IDLE)
        jump_to_block(ms_to_block(ms));
}

void player_set_volume(uint8_t v)
{
    if (v > 127)
        v = 127;
    if (v == volume)
        return;
    volume = v;
    if (state == PLAYER_PLAYING)
        hw_start(base_block + played);  // the new volume starts with the next block
}

void player_tick(uint32_t elapsed)
{
    uint32_t p = played;
    if (p != tick_played)
    {
        tick_played = p;
        tick_frames = 0;
    }
    else if (tick_frames < 1000)
        tick_frames += elapsed;
}

bool player_peek(int16_t *out, int n)
{
    if (state != PLAYER_PLAYING || n > BLOCK_FRAMES)
        return false;
    // The ARM7 reports whole blocks; within one, estimate from the frames
    // since the last report (60 frames per second).
    uint32_t rate = hdr.sound_clock_hz / hdr.timer_divider;
    uint32_t off = tick_frames * rate / 60;
    if (off > BLOCK_FRAMES - (uint32_t)n)
        off = BLOCK_FRAMES - n;
    uint32_t start = (tick_played % RING_BLOCKS) * BLOCK_FRAMES + off;
    const int16_t *r = hdr.channels == 2 ? ring_r : ring_l;
    for (int i = 0; i < n; i++)
    {
        uint32_t k = (start + i) % (RING_BLOCKS * BLOCK_FRAMES);
        out[i] = (int16_t)(((int32_t)ring_l[k] + r[k]) >> 1);
    }
    return true;
}

uint32_t player_position_smooth_ms(void)
{
    if (state != PLAYER_PLAYING)
        return player_position_ms();
    // the ARM7 reports whole 125 ms blocks; in between, count the frames. Both
    // from player_tick's snapshot: a report arriving in the middle of a frame
    // would otherwise move the bar a block ahead and then back again.
    uint64_t frames = (uint64_t)(base_block + tick_played) * BLOCK_FRAMES;
    if (frames > hdr.frame_count)
        frames = hdr.frame_count;
    uint32_t ms = (uint32_t)(frames * 1000 * hdr.timer_divider / hdr.sound_clock_hz);
    uint32_t in_block = tick_frames * 1000 / 60;
    uint32_t block_ms = (uint32_t)((uint64_t)BLOCK_FRAMES * 1000 * hdr.timer_divider / hdr.sound_clock_hz);
    if (in_block > block_ms)
        in_block = block_ms;
    uint32_t dur = player_duration_ms();
    return ms + in_block > dur ? dur : ms + in_block;
}

const DsaHeader *player_header(void)
{
    return state == PLAYER_IDLE ? NULL : &hdr;
}

void player_restart(void)
{
    if (state != PLAYER_IDLE)
        jump_to_block(0);
}

void player_stop(void)
{
    close_file();
}

PlayerState player_state(void)
{
    return state;
}

uint32_t player_position_ms(void)
{
    if (state == PLAYER_IDLE)
        return 0;
    uint64_t frames = (uint64_t)current_block() * BLOCK_FRAMES;
    if (frames > hdr.frame_count)
        frames = hdr.frame_count;
    return (uint32_t)(frames * 1000 * hdr.timer_divider / hdr.sound_clock_hz);
}

uint32_t player_duration_ms(void)
{
    return state == PLAYER_IDLE ? 0 : hdr.duration_ms;
}

const char *player_path(void)
{
    return state == PLAYER_IDLE ? NULL : cur_path;
}

const DsaTags *player_tags(void)
{
    return state == PLAYER_IDLE ? NULL : &tags;
}

const uint16_t *player_cover(void)
{
    return (state != PLAYER_IDLE && has_cover) ? cover : NULL;
}

uint32_t player_underruns(void)
{
    return underruns;
}
