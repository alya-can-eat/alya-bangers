// SPDX-License-Identifier: GPL-3.0-or-later
//
// The equalizer against an exact (double) reference of the same filters, its
// frequency response, Flat leaving samples alone, chunking not mattering, and
// boosts never clipping.
//
//   ./test_eq

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eq.h"
#include "eq_tables.h"

#define RATE (16756991.0 / 512)
#define N 32768

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

static int16_t in[N], out[N];
static double ref[N];

static void set3(int b, int m, int t)
{
    int8_t g[3] = { (int8_t)b, (int8_t)m, (int8_t)t };
    eq_set(g);
}

// music-like: a few tones, a chirp and noise
static void signal(int16_t *x, int n, double amp, unsigned seed)
{
    srand(seed);
    for (int i = 0; i < n; i++)
    {
        double t = i / RATE;
        double v = 0.3 * sin(2 * M_PI * 55 * t) + 0.2 * sin(2 * M_PI * 440 * t)
                   + 0.15 * sin(2 * M_PI * 3000 * t) + 0.1 * sin(2 * M_PI * (100 + 4000 * t) * t)
                   + 0.1 * ((rand() / (double)RAND_MAX) * 2 - 1);
        x[i] = (int16_t)lround(amp * v);
    }
}

static void ref_biquad(const int32_t *c, double *y, int n)
{
    double b0 = c[0] / 268435456.0, b1 = c[1] / 268435456.0, b2 = c[2] / 268435456.0;
    double a1 = c[3] / 268435456.0, a2 = c[4] / 268435456.0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; i++)
    {
        double x0 = y[i];
        double y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x0; y2 = y1; y1 = y0;
        y[i] = y0;
    }
}

// The same cascade in doubles, with the same coefficients and pregain
// (and the high cut's step, 0: off).
static void reference_hc(const int16_t *x, double *y, int n, const int8_t *g, int hc)
{
    double pre = eq_pregain() / 32768.0;
    for (int i = 0; i < n; i++)
        y[i] = x[i] * pre;
    for (int b = 0; b < 3; b++)
        if (g[b])
            ref_biquad(eq_coef[b][g[b] + 12], y, n);
    if (hc)
    {
        ref_biquad(eq_highcut[hc - 1][0], y, n);
        ref_biquad(eq_highcut[hc - 1][1], y, n);
    }
}

static void reference(const int16_t *x, double *y, int n, const int8_t *g)
{
    reference_hc(x, y, n, g, 0);
}

static double rms_db_of_sine(double f, int amp)
{
    for (int i = 0; i < N; i++)
        in[i] = (int16_t)lround(amp * sin(2 * M_PI * f * i / RATE));
    memcpy(out, in, sizeof(in));
    EqState st;
    eq_state_reset(&st);
    eq_run(&st, out, N, 0);
    double si = 0, so = 0;
    for (int i = N / 2; i < N; i++)     // after the filters settle
    {
        si += (double)in[i] * in[i];
        so += (double)out[i] * out[i];
    }
    return 10 * log10(so / si);
}

static void test_flat(void)
{
    set3(0, 0, 0);
    CHECK(!eq_active(), "Flat is not active");
    CHECK(eq_pregain() == 32768, "Flat: no pregain");
    signal(in, N, 20000, 1);
    memcpy(out, in, sizeof(in));
    EqState st;
    eq_state_reset(&st);
    eq_run(&st, out, N, 0);
    CHECK(memcmp(in, out, sizeof(in)) == 0, "Flat leaves the samples untouched");
}

static void test_reference(void)
{
    static const int8_t sets[][3] = {
        { 12, 0, 0 }, { -12, 0, 0 }, { 0, 12, 0 }, { 0, -12, 0 }, { 0, 0, 12 }, { 0, 0, -12 },
        { 6, -3, 4 }, { -6, 2, 3 }, { 12, 12, 12 }, { -12, -12, -12 }, { 1, 0, 0 }, { 5, -7, 9 },
    };
    for (unsigned k = 0; k < sizeof(sets) / sizeof(sets[0]); k++)
    {
        const int8_t *g = sets[k];
        eq_set(g);
        signal(in, N, 12000, 2 + k);
        memcpy(out, in, sizeof(in));
        EqState st;
        eq_state_reset(&st);
        eq_run(&st, out, N, k & 1);
        reference(in, ref, N, g);
        double worst = 0;
        for (int i = 0; i < N; i++)
        {
            double d = fabs(out[i] - ref[i]);
            if (d > worst)
                worst = d;
        }
        CHECK(worst <= 1.0, "%+d %+d %+d: off the exact result by %.2f", g[0], g[1], g[2], worst);
    }
}

static void test_response(void)
{
    // each band at +-12 dB: its gain where it acts, nothing where it doesn't
    struct { int band, db; double f, want; } cases[] = {
        { 0, 12, 40, 12 }, { 0, 12, 4000, 0 }, { 0, -12, 40, -12 }, { 0, -12, 4000, 0 },
        { 1, 12, 1000, 12 }, { 1, 12, 60, 0 }, { 1, -12, 1000, -12 }, { 1, -12, 12000, 0 },
        { 2, 12, 12000, 12 }, { 2, 12, 200, 0 }, { 2, -12, 12000, -12 }, { 2, -12, 200, 0 },
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++)
    {
        int8_t g[3] = { 0, 0, 0 };
        g[cases[k].band] = (int8_t)cases[k].db;
        eq_set(g);
        double pre = 20 * log10(eq_pregain() / 32768.0);
        double got = rms_db_of_sine(cases[k].f, 4000) - pre;
        CHECK(fabs(got - cases[k].want) < 0.6, "band %d at %+d dB, %.0f Hz: %+.2f dB, want %+.0f",
              cases[k].band, cases[k].db, cases[k].f, got, cases[k].want);
    }
    // the pregain: a boost of 12 dB turns the input down by about 12 dB
    set3(12, 0, 0);
    double pre = 20 * log10(eq_pregain() / 32768.0);
    CHECK(pre < -12.0 && pre > -12.6, "pregain for +12 dB bass: %.2f dB", pre);
    set3(-6, 0, -6);
    pre = 20 * log10(eq_pregain() / 32768.0);
    CHECK(pre < 0 && pre > -0.3, "cuts only: just the margin, %.2f dB", pre);
}

static void test_chunks(void)
{
    set3(7, -4, 5);
    eq_set_output(3, true);         // all stages: per channel, so pieces don't matter
    signal(in, N, 15000, 9);
    int16_t whole[N];
    memcpy(whole, in, sizeof(in));
    EqState a, b;
    eq_state_reset(&a);
    eq_state_reset(&b);
    eq_run(&a, whole, N, 1);
    memcpy(out, in, sizeof(in));
    srand(5);
    for (int i = 0; i < N;)
    {
        int m = 1 + rand() % 1500;
        if (i + m > N)
            m = N - i;
        eq_run(&b, out + i, m, 1);
        i += m;
    }
    CHECK(memcmp(whole, out, sizeof(out)) == 0, "the same result in pieces of any size (high cut, dither too)");
    eq_set_output(0, false);
}

static void test_no_clipping(void)
{
    // full-scale sines through boosted settings: the pregain keeps them in range
    srand(11);
    int worst_peak = 0;
    for (int k = 0; k < 40; k++)
    {
        int8_t g[3];
        for (int b = 0; b < 3; b++)
            g[b] = (int8_t)(k == 0 ? 12 : rand() % 25 - 12);
        eq_set(g);
        for (int j = 0; j < EQ_NF; j += 3)
        {
            double w = atan2(eq_grid[j][1], eq_grid[j][0]);
            // full scale after a 2 ms fade-in (an instant jump from silence is
            // no music; it may clip a sample, which is clamped, not wrapped)
            for (int i = 0; i < N / 4; i++)
                in[i] = (int16_t)lround(32767 * sin(w * i + 0.3) * (i < 64 ? i / 64.0 : 1.0));
            memcpy(out, in, sizeof(in));
            EqState st;
            eq_state_reset(&st);
            eq_run(&st, out, N / 4, 0);
            for (int i = 0; i < N / 4; i++)
            {
                int v = abs(out[i]);
                if (v > worst_peak)
                    worst_peak = v;
            }
        }
    }
    printf("  boosted full-scale sines, loudest sample: %d\n", worst_peak);
    CHECK(worst_peak < 32767, "boosted full-scale sines reach %d, no clipping", worst_peak);
}

// The high cut: flat below its edge, 40 dB down where its mirrors would be
// heard, and exact like the bands.
static void test_highcut(void)
{
    set3(0, 0, 0);
    for (int k = 1; k <= EQ_HIGHCUT_STEPS; k++)
    {
        eq_set_output(k, false);
        double pre = 20 * log10(eq_pregain() / 32768.0);
        double edge = eq_highcut_edge10(k) * 100.0, stop = RATE - eq_highcut_clean10(k) * 100.0;
        double pass1 = rms_db_of_sine(1000, 8000) - pre, pass2 = rms_db_of_sine(edge - 500, 8000) - pre;
        double stop1 = rms_db_of_sine(stop + 50, 8000) - pre, stop2 = rms_db_of_sine(16200, 8000) - pre;
        CHECK(pass1 > -0.3 && pass1 < 0.1 && pass2 > -0.3 && pass2 < 0.1,
              "high cut %.1f kHz: 1 kHz %+.2f dB, %.1f kHz %+.2f dB (flat)", edge / 1000, pass1, (edge - 500) / 1000, pass2);
        CHECK(stop1 < -38 && stop2 < -38, "high cut %.1f kHz: %.2f kHz %+.1f dB, 16.2 kHz %+.1f dB (40 dB down)",
              edge / 1000, (stop + 50) / 1000, stop1, stop2);
    }
    CHECK(eq_highcut_clean10(7) >= 170 && eq_highcut_clean10(1) >= 187, "edge 15.0 kHz: no mirrors below 17.0 kHz");
    // exact, with bands too
    const int8_t g[3] = { 5, -3, 7 };
    eq_set(g);
    eq_set_output(4, false);
    signal(in, N, 12000, 21);
    memcpy(out, in, sizeof(in));
    EqState st;
    eq_state_reset(&st);
    eq_run(&st, out, N, 0);
    reference_hc(in, ref, N, g, 4);
    double worst = 0;
    for (int i = 0; i < N; i++)
        worst = fmax(worst, fabs(out[i] - ref[i]));
    CHECK(worst <= 1.0, "bands and high cut: off the exact result by %.2f", worst);
    double pre = 20 * log10(eq_pregain() / 32768.0);
    set3(0, 0, 0);
    eq_set_output(4, false);
    double pre_hc = 20 * log10(eq_pregain() / 32768.0);
    CHECK(pre_hc < -0.9 && pre_hc > -1.1 && pre < pre_hc, "the high cut turns down 1 dB (%.2f dB), with boosts more (%.2f dB)",
          pre_hc, pre);
    eq_set_output(0, false);
    CHECK(!eq_active(), "everything off: nothing to run");
}

// Power at f (Hann window), the result of a 12 kHz tone of 1.5 steps:
// rounded by the hardware (cut to 10 bits) its 3rd harmonic folds back to
// 3.27 kHz; dithered, it's gone into the noise.
static double tone_db(const int16_t *x, int n, double f)
{
    double re = 0, im = 0;
    for (int i = 0; i < n; i++)
    {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
        re += w * x[i] * cos(2 * M_PI * f * i / RATE);
        im += w * x[i] * sin(2 * M_PI * f * i / RATE);
    }
    return 10 * log10(re * re + im * im + 1e-9);
}

static double band_power(const int16_t *x, int n, double f0, double f1)
{
    double p = 0;
    int k = 0;
    for (double f = f0; f <= f1; f += 125, k++)
        p += pow(10, tone_db(x, n, f) / 10);
    return 10 * log10(p / k);
}

static void test_dither(void)
{
    set3(0, 0, 0);
    eq_set_output(0, true);
    CHECK(eq_active() && !eq_bands_active(), "dither alone: run, but the equalizer isn't on");
    // a quiet 12 kHz tone
    int n = 8192;
    for (int i = 0; i < n; i++)
        in[i] = (int16_t)lround(96 * sin(2 * M_PI * 12000 * i / RATE));
    int16_t hw[8192];
    for (int i = 0; i < n; i++)
        hw[i] = (int16_t)(in[i] & ~63);     // what the DS does: cut to 10 bits
    memcpy(out, in, n * 2);
    EqState st;
    eq_state_reset(&st);
    eq_run(&st, out, n, 0);
    bool steps = true;
    double mean = 0;
    for (int i = 0; i < n; i++)
    {
        steps &= (out[i] & 63) == 0;
        mean += out[i] - in[i];
    }
    mean /= n;
    CHECK(steps, "dithered: every sample a whole step of the 10 bits (the hardware's cut changes nothing)");
    CHECK(fabs(mean) < 2, "dithered: no offset (%.2f)", mean);
    double hw_spur = tone_db(hw, n, 36000 - RATE) - tone_db(hw, n, 12000);
    double dt_spur = tone_db(out, n, 36000 - RATE) - tone_db(out, n, 12000);
    printf("  12 kHz tone, 1.5 steps: folded-back 3rd harmonic cut %+.1f dB, dithered %+.1f dB\n", hw_spur, dt_spur);
    CHECK(dt_spur < hw_spur - 12, "dither: the folded-back distortion of a quiet high tone at least 12 dB lower");
    // silence: the noise alone, low where the ear hears best
    memset(out, 0, n * 2);
    eq_state_reset(&st);
    eq_run(&st, out, n, 1);
    int peak = 0;
    for (int i = 0; i < n; i++)
        peak = abs(out[i]) > peak ? abs(out[i]) : peak;
    double mid = band_power(out, n, 1000, 5000), high = band_power(out, n, 9000, 13000);
    double top = band_power(out, n, 15500, 16300);
    printf("  dither noise: 1..5 kHz %.1f dB, 9..13 kHz %.1f dB, 15.5..16.3 kHz %.1f dB, peak %d\n", mid, high, top, peak);
    CHECK(peak <= 4 * 64, "dither noise stays within a few steps (%d)", peak);
    CHECK(mid < high - 6 && top < high - 2, "dither noise: lower at 1..5 kHz and near 16 kHz than around 11 kHz");
    eq_set_output(0, false);
}

int main(void)
{
    test_flat();
    test_reference();
    test_response();
    test_chunks();
    test_no_clipping();
    test_highcut();
    test_dither();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
