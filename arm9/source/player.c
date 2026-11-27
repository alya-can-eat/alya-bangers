// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streaming: two ring buffers (left, right) of RING_BLOCKS blocks each. The
// ARM7 plays them in a loop and reports every finished block; we then load
// the next block of the file into the slot that was just freed.
//
// A .dsa block stores all left samples, then all right samples, so each half
// is read straight from the card into its ring with no copying.
//
// The equalizer runs over the samples in the ring, in place, at most
// EQ_LEAD blocks ahead of the one playing: reading stays up to 8 blocks
// ahead, but a change of the equalizer is heard within half a second.
//
// Pause fades out (the ARM7 turns the volume down) and tells us the exact
// sample it stopped at. Resume starts the hardware at that block's start, with
// silence up to that sample and a fade in from there, so nothing is heard twice.

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "eq.h"
#include "ipc.h"
#include "player.h"
#include "i18n.h"

#define RING_BLOCKS     8                       // 8 x 125 ms = 1 s of buffer
#define START_BLOCKS    4                       // read before starting; the rest follows
#define BLOCK_FRAMES    4096
#define BLOCK_BYTES     (BLOCK_FRAMES * 2)      // per channel
#define RING_BYTES      (RING_BLOCKS * BLOCK_BYTES)
#define EQ_LEAD         4                       // blocks equalized ahead of the one playing
#define EQ_URGENT       2                       // fewer ready: all that's missing at once
#define EQ_STEP         1024                    // frames per call otherwise
#define FADE_FRAMES     480                     // pause and resume: 15 ms

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
static uint32_t paused_frame;       // and the sample in that block
static volatile uint32_t stop_frame;    // from the ARM7: samples played of the block after `played`
static volatile uint32_t played;    // blocks finished since start, from the ARM7
static volatile bool stop_ack;
static uint8_t generation;
static long file_pos = -1;
static uint32_t underruns;
static uint8_t volume = 127;
static uint32_t tick_played, tick_frames;   // last report seen, frames since: player_peek, smooth position
static EqState eq_st;
static uint32_t eq_blk, eq_off;     // the next frame to equalize: block since start, frame in it
static bool eq_was_on;

static void event_handler(u32 value, void *userdata)
{
    if (STREAM_EVT_GEN(value) != (generation & 0xF))
        return; // left over from a previous start
    uint32_t type = value & STREAM_EVT_TYPE_MASK;
    if (type == STREAM_EVT_FRAME)
    {
        stop_frame = STREAM_EVT_COUNT(value);
        return;
    }
    played = STREAM_EVT_COUNT(value);
    if (type == STREAM_EVT_STOPPED)
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

// Stop the hardware (after fading out over `fade` samples) and wait until the
// ARM7 has confirmed, so `played` and `stop_frame` are final and the rings may
// be overwritten.
static void hw_stop_fade(uint16_t fade)
{
    if (!running)
        return;
    StreamCmd c = { 0 };
    c.cmd = STREAM_CMD_STOP;
    c.generation = generation;
    c.fade_frames = fade;
    stop_ack = false;
    stop_frame = 0;
    send_cmd(&c);
    for (int i = 0; i < 60000 && !stop_ack; i++)
        swiDelay(100);
    running = false;
}

static void hw_stop(void)
{
    hw_stop_fade(0);
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

// Equalize n frames of a loaded block, both channels.
static void eq_part(uint32_t rel, uint32_t off, int n)
{
    int slot = rel % RING_BLOCKS;
    int16_t *l = ring_l + slot * BLOCK_FRAMES + off;
    eq_run(&eq_st, l, n, 0);
    DC_FlushRange(l, n * 2);
    if (hdr.channels == 2)
    {
        int16_t *r = ring_r + slot * BLOCK_FRAMES + off;
        eq_run(&eq_st, r, n, 1);
        DC_FlushRange(r, n * 2);
    }
}

// Keep the equalizer ahead of playback, over what's loaded. The block playing
// isn't touched: if the equalizer fell behind it, it goes on after it.
static void eq_ahead(void)
{
    uint32_t p = played;
    if (!eq_active())
    {
        eq_was_on = false;
        eq_blk = p + 1;
        eq_off = 0;
        return;
    }
    if (!eq_was_on || eq_blk < p + 1)
    {
        eq_state_reset(&eq_st);
        eq_blk = p + 1;
        eq_off = 0;
        eq_was_on = true;
    }
    uint32_t target = p + 1 + EQ_LEAD < loaded ? p + 1 + EQ_LEAD : loaded;
    int budget = eq_blk < p + 1 + EQ_URGENT ? INT_MAX : EQ_STEP;
    while (eq_blk < target && budget > 0)
    {
        int n = BLOCK_FRAMES - eq_off;
        if (n > EQ_STEP)
            n = EQ_STEP;
        eq_part(eq_blk, eq_off, n);
        budget -= n;
        eq_off += n;
        if (eq_off == BLOCK_FRAMES)
        {
            eq_blk++;
            eq_off = 0;
        }
    }
}

// Silence before sample `from` of the first block, then a fade in, in both
// rings (after the equalizer, which already ran over these blocks).
static void fade_in(uint32_t from)
{
    int16_t *rings[2] = { ring_l, ring_r };
    for (int c = 0; c < (hdr.channels == 2 ? 2 : 1); c++)
    {
        int16_t *x = rings[c];
        memset(x, 0, from * 2);
        for (uint32_t i = 0; i < FADE_FRAMES; i++)
            x[from + i] = (int16_t)((int32_t)x[from + i] * (int32_t)i / FADE_FRAMES);
        DC_FlushRange(x, (from + FADE_FRAMES) * 2);
    }
}

// Start at `block`; with `fade`, from sample `from` of it with a fade in.
static void hw_start_at(uint32_t block, uint32_t from, bool fade)
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
    // the first blocks equalized before the hardware starts on them
    eq_state_reset(&eq_st);
    eq_blk = eq_off = 0;
    eq_was_on = eq_active();
    if (eq_was_on)
        for (; eq_blk < EQ_URGENT; eq_blk++)
            eq_part(eq_blk, 0, BLOCK_FRAMES);
    if (fade)
        fade_in(from < BLOCK_FRAMES ? from : 0);

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

static void hw_start(uint32_t block)
{
    hw_start_at(block, 0, false);
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
        paused_frame = 0;
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
    eq_ahead();
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
        hw_stop_fade(FADE_FRAMES);
        paused_block = base_block + played;
        paused_frame = stop_frame < BLOCK_FRAMES ? stop_frame : 0;
        state = PLAYER_PAUSED;
    }
    else if (state == PLAYER_PAUSED)
    {
        hw_start_at(paused_block, paused_frame, true);
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
        paused_frame = 0;
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
    uint64_t frames = (uint64_t)current_block() * BLOCK_FRAMES + (state == PLAYER_PAUSED ? paused_frame : 0);
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

// ---- the chime: two bell-like notes, A5 then F5, as 16-bit samples at the
// mixer's rate. Made once, without floating point: each partial a resonator
// (y[n] = 2 cos(w) y[n-1] - y[n-2]), times a fast rise and an exponential decay.

#define CHIME_FRAMES    22912               // 0.7 s (a multiple of 2: whole words)
#define CHIME_SECOND    9819                // where the second note starts (0.3 s)
#define CHIME_RISE      164                 // 5 ms
#define CHIME_DECAY     1073523129          // per sample, Q30: a time constant of 0.15 s
#define CHIME_PEAK      14000

static int16_t chime[CHIME_FRAMES] __attribute__((aligned(32)));
static bool chime_made;

typedef struct { int64_t c; int64_t y1, y2; } Osc;     // Q30 coefficient, Q24 samples

static Osc osc(int64_t c, int64_t sin_w)
{
    return (Osc){ c, sin_w, 0 };      // y[0] = 0, y[1] = sin(w): a sine from the start
}

static int64_t osc_next(Osc *o)
{
    int64_t y = ((o->c * o->y1) >> 30) - o->y2;
    o->y2 = o->y1;
    o->y1 = y;
    return o->y2;
}

static void chime_make(void)
{
    // 2 cos(w) in Q30 and sin(w) in Q24 for 880, 1760, 698.46 and 1396.91 Hz at 32728.5 Hz
    Osc notes[2][2] = {
        { osc(2116910538, 2820905), osc(2026061731, 5561490) },
        { osc(2128206636, 2242916), osc(2070722775, 4445534) },
    };
    for (int n = 0; n < 2; n++)
    {
        int start = n ? CHIME_SECOND : 0;
        int64_t env = 0;
        for (int i = start; i < CHIME_FRAMES; i++)
        {
            int k = i - start;
            env = k < CHIME_RISE ? ((int64_t)k << 30) / CHIME_RISE
                                 : (k == CHIME_RISE ? (1 << 30) : (env * CHIME_DECAY) >> 30);
            int64_t tone = (osc_next(&notes[n][0]) * 4 + osc_next(&notes[n][1])) / 5;    // Q24
            int64_t e = env;
            if (CHIME_FRAMES - i < 656)
                e = e * (CHIME_FRAMES - i) / 656;           // the last 20 ms: down to silence
            int32_t v = (int32_t)(((tone * e) >> 30) * CHIME_PEAK >> 24);
            int32_t sum = chime[i] + v;
            chime[i] = (int16_t)(sum > 32767 ? 32767 : (sum < -32768 ? -32768 : sum));
        }
    }
    DC_FlushRange(chime, sizeof(chime));
    chime_made = true;
}

void player_chime(void)
{
    if (!chime_made)
        chime_make();
    StreamCmd c = { 0 };
    c.cmd = STREAM_CMD_CHIME;
    c.left = c.right = (uint32_t)(uintptr_t)chime;
    c.ring_bytes = sizeof(chime);
    c.timer_divider = 512;
    c.volume = 127;
    send_cmd(&c);
}

uint32_t player_underruns(void)
{
    return underruns;
}
