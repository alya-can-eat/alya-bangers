// SPDX-License-Identifier: MIT
//
// Visualizer: the loudness of 20 frequency bands (60 Hz to 11 kHz, spaced
// evenly on a log scale), measured with one Goertzel filter per band on the
// samples playing right now.

#include <math.h>
#include <stdint.h>

#include "gfx.h"
#include "player.h"
#include "viz.h"

#define N           512
#define RATE        32728
#define FLOOR_LOG2  (22 * 8)    // quietest level shown (power, 1/8 log2 steps)
#define TOP_LOG2    (40 * 8)    // full-scale sine
#define FALL        10          // per frame, out of 256

static int16_t window[N];
static int32_t coeff[VIZ_BANDS];    // 2 cos(w), Q14
static int levels[VIZ_BANDS];
static bool ready;

void viz_init(void)
{
    if (ready)
        return;
    for (int i = 0; i < N; i++)
        window[i] = (int16_t)(32767.0f * (0.5f - 0.5f * cosf(6.2831853f * i / (N - 1))));
    for (int b = 0; b < VIZ_BANDS; b++)
    {
        float f = 60.0f * powf(11000.0f / 60.0f, (float)b / (VIZ_BANDS - 1));
        coeff[b] = (int32_t)(16384.0f * 2.0f * cosf(6.2831853f * f / RATE));
    }
    ready = true;
}

// Power of one band. Input / 4 keeps the filter state in 32 bits
// (512 x 8192 < 2^31).
static HOT int64_t goertzel(const int16_t *x, const int16_t *win, int32_t c)
{
    int32_t s1 = 0, s2 = 0;
    for (int i = 0; i < N; i++)
    {
        int32_t in = ((int32_t)x[i] * win[i]) >> 17;
        int32_t s = in + (int32_t)(((int64_t)c * s1) >> 14) - s2;
        s2 = s1;
        s1 = s;
    }
    return (int64_t)s1 * s1 + (int64_t)s2 * s2 - (int64_t)(int32_t)(((int64_t)c * s1) >> 14) * s2;
}

// log2 of a 64-bit value in 1/8 steps
static int log2_8(uint64_t v)
{
    if (v == 0)
        return 0;
    int e = 63 - __builtin_clzll(v);
    // three more bits from below the top one
    uint32_t frac = e >= 3 ? (uint32_t)(v >> (e - 3)) & 7 : (uint32_t)(v << (3 - e)) & 7;
    return e * 8 + (int)frac;
}

void viz_update(void)
{
    viz_init();
    static int16_t x[N];
    bool live = player_peek(x, N);
    for (int b = 0; b < VIZ_BANDS; b++)
    {
        int target = 0;
        if (live)
        {
            int64_t p = goertzel(x, window, coeff[b]);
            int l = log2_8(p > 0 ? (uint64_t)p : 0);
            target = (l - FLOOR_LOG2) * 256 / (TOP_LOG2 - FLOOR_LOG2);
            if (target < 0) target = 0;
            if (target > 256) target = 256;
        }
        if (target > levels[b])
            levels[b] = target;
        else
            levels[b] = levels[b] > FALL ? levels[b] - FALL : 0;
    }
}

int viz_level(int band)
{
    return levels[band];
}
