// SPDX-License-Identifier: MIT
//
// Streams real .dsa files through player.c with a simulated ARM7 and checks,
// sample by sample, that what the hardware would play is the file's audio.
//
//   ./test_player song.dsa [more.dsa ...]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hostsim.h"
#include "player.h"

#define BF 4096

static int failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
            failures++;                                     \
        }                                                   \
    } while (0)

typedef struct {
    DsaHeader h;
    int16_t *left, *right; // whole song, block_count * BF samples per channel
} Song;

// Independent reader: the expected audio, straight from the file.
static bool load_song(const char *path, Song *s)
{
    FILE *f = fopen(path, "rb");
    char err[64];
    if (f == NULL || !dsa_read_header(f, &s->h, err, sizeof(err)))
    {
        printf("  can't read %s\n", path);
        if (f)
            fclose(f);
        return false;
    }
    size_t n = (size_t)s->h.block_count * BF;
    s->left = calloc(n, 2);
    s->right = calloc(n, 2);
    for (uint32_t b = 0; b < s->h.block_count; b++)
    {
        fseek(f, s->h.audio_offset + (long)b * BF * 2 * s->h.channels, SEEK_SET);
        if (fread(s->left + (size_t)b * BF, 2, BF, f) != BF)
            printf("  short read in block %lu\n", (unsigned long)b);
        if (s->h.channels == 2)
        {
            if (fread(s->right + (size_t)b * BF, 2, BF, f) != BF)
                printf("  short read in block %lu\n", (unsigned long)b);
        }
        else
        {
            memcpy(s->right + (size_t)b * BF, s->left + (size_t)b * BF, BF * 2);
        }
    }
    fclose(f);
    return true;
}

static int16_t heard_l[BF], heard_r[BF];

// Play one block in the simulator; return which song block it was, or -1.
static long play_and_identify(const Song *s, long expected)
{
    if (!sim_play_block(heard_l, heard_r))
        return -2;
    if (expected >= 0 && expected < (long)s->h.block_count
        && memcmp(heard_l, s->left + expected * BF, sizeof(heard_l)) == 0
        && memcmp(heard_r, s->right + expected * BF, sizeof(heard_r)) == 0)
        return expected;
    for (uint32_t b = 0; b < s->h.block_count; b++)
    {
        if (memcmp(heard_l, s->left + (size_t)b * BF, sizeof(heard_l)) == 0
            && memcmp(heard_r, s->right + (size_t)b * BF, sizeof(heard_r)) == 0)
            return b;
    }
    return -1;
}

static void test_straight(const char *path, const Song *s)
{
    char err[64];
    CHECK(player_play(path, err, sizeof(err)), "play: %s", err);
    CHECK(sim_hw.timer_divider == 512, "timer divider %u", sim_hw.timer_divider);
    CHECK(s->h.channels == 2 ? sim_hw.left != sim_hw.right : sim_hw.left == sim_hw.right,
          "mono files play the same ring on both sides");
    long next = 0;
    int guard = 0;
    while (player_state() == PLAYER_PLAYING && guard++ < 1000000)
    {
        player_service();
        if (player_state() != PLAYER_PLAYING)
            break;
        long b = play_and_identify(s, next);
        CHECK(b == next, "heard block %ld, expected %ld", b, next);
        if (b != next)
            return;
        next++;
    }
    CHECK(next == (long)s->h.block_count, "played %ld of %lu blocks", next,
          (unsigned long)s->h.block_count);
    CHECK(player_state() == PLAYER_ENDED, "state %d at the end", player_state());
    CHECK(player_underruns() == 0, "%lu dropouts", (unsigned long)player_underruns());
    CHECK(!sim_hw.running, "hardware stopped at the end");
    CHECK(player_position_ms() == s->h.duration_ms
          || player_position_ms() + 1 == s->h.duration_ms,
          "position %lu at the end, duration %lu", (unsigned long)player_position_ms(),
          (unsigned long)s->h.duration_ms);
}

static void test_pause_seek(const char *path, const Song *s)
{
    char err[64];
    if (s->h.block_count < 200)
    {
        printf("  (pause/seek test needs a song longer than 25 s, skipped)\n");
        return;
    }
    CHECK(player_play(path, err, sizeof(err)), "play: %s", err);
    long next = 0;
    for (int i = 0; i < 20; i++, next++)
    {
        player_service();
        CHECK(play_and_identify(s, next) == next, "block %ld", next);
    }

    // Pause: no sound until resumed, then continue exactly where we were.
    player_toggle_pause();
    CHECK(player_state() == PLAYER_PAUSED && !sim_hw.running, "paused");
    uint32_t pos = player_position_ms();
    CHECK(pos == (uint32_t)((uint64_t)20 * BF * 1000 * 512 / 16756991), "pause position %lu",
          (unsigned long)pos);
    player_toggle_pause();
    CHECK(player_state() == PLAYER_PLAYING && sim_hw.running, "resumed");
    for (int i = 0; i < 5; i++, next++)
    {
        player_service();
        CHECK(play_and_identify(s, next) == next, "after resume: block %ld", next);
    }

    // +10 s = 80 blocks (327285 frames / 4096 = 79.9, rounded)
    player_seek_seconds(10);
    next += 80;
    for (int i = 0; i < 5; i++, next++)
    {
        player_service();
        long b = play_and_identify(s, next);
        CHECK(b == next, "after +10 s: heard %ld, expected %ld", b, next);
    }

    // -10 s
    player_seek_seconds(-10);
    next -= 80;
    player_service();
    long b = play_and_identify(s, next);
    CHECK(b == next, "after -10 s: heard %ld, expected %ld", b, next);
    next++;

    // Seeking while paused moves the resume point.
    player_toggle_pause();
    player_seek_seconds(-1000);
    player_toggle_pause();
    player_service();
    b = play_and_identify(s, 0);
    CHECK(b == 0, "seek far back clamps to the start: heard %ld", b);

    // Restart (L near the start of a song).
    for (int i = 0; i < 3; i++)
    {
        player_service();
        play_and_identify(s, -1);
    }
    player_restart();
    player_service();
    CHECK(play_and_identify(s, 0) == 0, "restart plays block 0");

    // A slow card: no refills for 6 blocks. Expect dropouts, then recovery.
    for (int i = 0; i < 6; i++)
        play_and_identify(s, -1);
    player_service();
    CHECK(player_underruns() > 0, "dropouts are counted");
    // Restart was at block 0, then 1 block (0) + 6 unserviced blocks were played.
    long cur = 7;
    for (int i = 0; i < 3; i++, cur++)
    {
        player_service();
        long got = play_and_identify(s, cur);
        CHECK(got == cur, "after a dropout: heard %ld, expected %ld", got, cur);
    }
    player_stop();
    CHECK(player_state() == PLAYER_IDLE && !sim_hw.running, "stopped");
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s song.dsa [...]\n", argv[0]);
        return 2;
    }
    player_init();
    for (int i = 1; i < argc; i++)
    {
        Song s;
        printf("%s\n", argv[i]);
        if (!load_song(argv[i], &s))
        {
            failures++;
            continue;
        }
        int before = failures;
        test_straight(argv[i], &s);
        test_pause_seek(argv[i], &s);
        printf("  %s (%lu blocks, %u ch)\n", failures == before ? "ok" : "FAILED",
               (unsigned long)s.h.block_count, s.h.channels);
        free(s.left);
        free(s.right);
    }
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
