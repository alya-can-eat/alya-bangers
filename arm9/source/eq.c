// SPDX-License-Identifier: GPL-3.0-or-later
//
// Biquads in direct form I: 32-bit coefficients (Q28), samples carried with
// 12 extra bits (x 4096) through the three bands, 64-bit sums. (With fewer,
// the bass shelf's rounding grows to more than 1 at the lowest frequencies.) The inner loop
// runs from ITCM in ARM mode (the multiply-accumulate instructions), on a
// buffer in DTCM.

#include <string.h>

#include <nds.h>

#include "eq.h"
#include "eq_tables.h"

#ifndef ITCM_CODE
#define ITCM_CODE
#endif
#ifndef ARM_CODE
#define ARM_CODE
#endif
#ifndef DTCM_BSS
#define DTCM_BSS
#endif

#define WORK 512                    // samples per pass

#define STEP (64 << 12)             // one step of the DS's 10 bits, x 4096

static int8_t gains[EQ_BANDS];
static const int32_t *coef[EQ_BANDS];   // NULL: the band is at 0 dB, skipped
static int32_t band_pregain = 32768;    // Q15: what the bands need
static int32_t pregain = 32768;     // and with the high cut's margin
static const int32_t (*highcut)[5];     // the high cut's two biquads, NULL: off
static int highcut_step;
static bool dither;
static uint8_t out_gen;             // changes with the output stages
static int32_t work[WORK] DTCM_BSS;

// The high cut is steep: on loud songs it rings up to about +1 dB above
// full scale (measured on band-limited masters), so 1 dB more down then.
static void set_pregain(void)
{
    pregain = highcut ? (band_pregain * 29205) >> 15 : band_pregain;   // 29205 = 32768 x 10^(-1/20)
}

// |H(w)|^2 of one biquad from the grid's cos/sin.
static float power(const int32_t *c, const float *g)
{
    const float s = 1.0f / (float)(1 << EQ_Q);
    float b0 = c[0] * s, b1 = c[1] * s, b2 = c[2] * s, a1 = c[3] * s, a2 = c[4] * s;
    float br = b0 + b1 * g[0] + b2 * g[2], bi = b1 * g[1] + b2 * g[3];
    float ar = 1.0f + a1 * g[0] + a2 * g[2], ai = a1 * g[1] + a2 * g[3];
    return (br * br + bi * bi) / (ar * ar + ai * ai);
}

void eq_set(const int8_t gain[EQ_BANDS])
{
    bool on = false;
    for (int b = 0; b < EQ_BANDS; b++)
    {
        int g = gain[b] < EQ_MIN ? EQ_MIN : (gain[b] > EQ_MAX ? EQ_MAX : gain[b]);
        gains[b] = (int8_t)g;
        coef[b] = g ? eq_coef[b][g - EQ_MIN] : NULL;
        on |= g != 0;
    }
    band_pregain = 32768;
    if (!on)
    {
        set_pregain();
        return;
    }
    // the loudest point of the curve (cuts only: just above 1 here and there,
    // from rounding), and 0.25 dB more for the points between the grid's
    float peak = 1.0f;
    for (int i = 0; i < EQ_NF; i++)
    {
        float p = 1.0f;
        for (int b = 0; b < EQ_BANDS; b++)
            if (coef[b])
                p *= power(coef[b], eq_grid[i]);
        if (p > peak)
            peak = p;
    }
    // pregain = 1 / sqrt(peak) / 10^(0.25/20); sqrt by Newton (no libm needed)
    float r = peak > 4.0f ? peak / 4.0f : peak;
    for (int i = 0; i < 12; i++)
        r = 0.5f * (r + peak / r);
    band_pregain = (int32_t)(32768.0f / r / 1.0292f);
    set_pregain();
}

void eq_get(int8_t gain[EQ_BANDS])
{
    memcpy(gain, gains, sizeof(gains));
}

bool eq_bands_active(void)
{
    return coef[0] || coef[1] || coef[2];
}

bool eq_active(void)
{
    return eq_bands_active() || highcut || dither;
}

void eq_set_output(int step, bool dith)
{
    step = step < 0 || step > EQ_HIGHCUT_STEPS ? 0 : step;
    if (step == highcut_step && dith == dither)
        return;
    highcut_step = step;
    highcut = step ? eq_highcut[step - 1] : NULL;
    dither = dith;
    set_pregain();
    out_gen++;                      // a stage switched on starts from silence
}

int eq_highcut_edge10(int step)
{
    return step >= 1 && step <= EQ_HIGHCUT_STEPS ? eq_highcut_edge[step - 1] : 0;
}

int eq_highcut_clean10(int step)
{
    return step >= 1 && step <= EQ_HIGHCUT_STEPS ? eq_highcut_clean[step - 1] : 0;
}

int32_t eq_pregain(void)
{
    return pregain;
}

void eq_state_reset(EqState *st)
{
    memset(st, 0, sizeof(*st));
    st->rng[0] = 0x9E3779B9u;
    st->rng[1] = 0x7F4A7C15u;
    st->gen = out_gen;
}

// Round to the DS's 10 bits with TPDF dither (two random numbers, together
// -1..+1 step) and the rounding error fed back, shaped (eq_tables.h).
// The samples come out as multiples of 64: the hardware's cut is exact.
ARM_CODE ITCM_CODE static void to_10bit(EqState *st, int ch, const int32_t *x, int16_t *out, int n)
{
    int32_t e1 = st->ns[ch][0], e2 = st->ns[ch][1], e3 = st->ns[ch][2];
    uint32_t r = st->rng[ch];
    for (int i = 0; i < n; i++)
    {
        int32_t v = x[i] - (int32_t)(((int64_t)EQ_NS_C1 * e1 + (int64_t)EQ_NS_C2 * e2
                                      + (int64_t)EQ_NS_C3 * e3) >> 12);
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        int32_t d = (int32_t)(((r & 0xFFFF) + (r >> 16)) << 2) - STEP;
        int32_t q = (v + d + STEP / 2) & ~(STEP - 1);
        if (q > 511 * STEP)
            q = 511 * STEP;
        else if (q < -512 * STEP)
            q = -512 * STEP;
        int32_t e = q - v;
        e = e > 2 * STEP ? 2 * STEP : (e < -2 * STEP ? -2 * STEP : e);   // clipped: don't let it run away
        e3 = e2;
        e2 = e1;
        e1 = e;
        out[i] = (int16_t)(q >> 12);
    }
    st->ns[ch][0] = e1;
    st->ns[ch][1] = e2;
    st->ns[ch][2] = e3;
    st->rng[ch] = r;
}

// One band over the work buffer.
ARM_CODE ITCM_CODE static void biquad(const int32_t *c, EqBiquad *s, int32_t *x, int n)
{
    const int32_t b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
    int32_t x1 = s->x1, x2 = s->x2, y1 = s->y1, y2 = s->y2;
    for (int i = 0; i < n; i++)
    {
        int32_t x0 = x[i];
        int64_t acc = (int64_t)b0 * x0 + (int64_t)b1 * x1 + (int64_t)b2 * x2
                      - (int64_t)a1 * y1 - (int64_t)a2 * y2 + (1 << (EQ_Q - 1));
        int32_t y0 = (int32_t)(acc >> EQ_Q);
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
        x[i] = y0;
    }
    s->x1 = x1;
    s->x2 = x2;
    s->y1 = y1;
    s->y2 = y2;
}

ARM_CODE ITCM_CODE void eq_run(EqState *st, int16_t *buf, int n, int ch)
{
    if (!eq_active())
        return;
    if (st->gen != out_gen)
    {
        // a stage was switched: the high cut and the dither start afresh
        memset(st->hc, 0, sizeof(st->hc));
        memset(st->ns, 0, sizeof(st->ns));
        st->gen = out_gen;
    }
    const int32_t g = pregain;
    while (n > 0)
    {
        int m = n < WORK ? n : WORK;
        // in, turned down, with 12 bits more: x * g / 32768 * 4096
        for (int i = 0; i < m; i++)
            work[i] = (buf[i] * g) >> 3;
        for (int b = 0; b < EQ_BANDS; b++)
            if (coef[b])
                biquad(coef[b], &st->b[ch][b], work, m);
        if (highcut)
        {
            biquad(highcut[0], &st->hc[ch][0], work, m);
            biquad(highcut[1], &st->hc[ch][1], work, m);
        }
        if (dither)
            to_10bit(st, ch, work, buf, m);
        else
            for (int i = 0; i < m; i++)
            {
                int32_t v = (work[i] + 2048) >> 12;
                buf[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        buf += m;
        n -= m;
    }
}
