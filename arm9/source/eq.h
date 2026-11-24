// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 3-band equalizer: Bass (low shelf, 120 Hz), Mid (peak, 1 kHz), Treble
// (high shelf, 5 kHz), each -12..+12 dB. player.c runs it on the samples in
// the ring, a little ahead of playback. Bands at 0 dB are skipped, so Flat
// costs nothing and leaves the samples untouched.
//
// The input is first turned down by the loudest point of the whole curve
// (and 0.25 dB more), so a boost doesn't clip.
//
// After the bands, two optional stages for the DS's output, which holds each
// sample until the next (a tone at f also sounds as a mirror at 32.7 kHz - f)
// and has only 10 bits:
// - the high cut, a steep low-pass from 12.0 .. 15.5 kHz, so the mirrors of
//   very high sounds don't land where you hear them;
// - dither: each sample rounded to the 10 bits with a little noise added and
//   shaped away from 1..5 kHz, so the hardware's cut changes nothing and
//   quiet high sounds turn into soft hiss instead of distortion.

#ifndef DSPLAYER_EQ_H__
#define DSPLAYER_EQ_H__

#include <stdbool.h>
#include <stdint.h>

#define EQ_BANDS 3
#define EQ_MIN   (-12)
#define EQ_MAX   12
#define EQ_HIGHCUT_STEPS 8          // high cut edges 12.0, 12.5 .. 15.5 kHz (0: off)

typedef struct {
    int32_t x1, x2, y1, y2;     // the last inputs and outputs, samples x 4096
} EqBiquad;

// The filters' memory for one stream (left and right).
typedef struct {
    EqBiquad b[2][EQ_BANDS];
    EqBiquad hc[2][2];          // the high cut
    int32_t ns[2][3];           // the dither: the last rounding errors
    uint32_t rng[2];            // and its random numbers, per channel
    uint8_t gen;                // the output settings it was last run with
} EqState;

// The gains in dB, clamped to EQ_MIN..EQ_MAX. Applies to what is run next.
void eq_set(const int8_t gain[EQ_BANDS]);
void eq_get(int8_t gain[EQ_BANDS]);
// The output stages: the high cut (0: off, 1..EQ_HIGHCUT_STEPS) and dither.
void eq_set_output(int highcut, bool dither);
// Anything to run: a band not at 0 dB, the high cut, dither.
bool eq_active(void);
// A band not at 0 dB (the equalizer itself changes the sound).
bool eq_bands_active(void);
// A high cut step's edge and the mirrors it removes: below that (in 0.1 kHz).
int eq_highcut_edge10(int step);
int eq_highcut_clean10(int step);
// How much the input is turned down (Q15, 32768 = not at all).
int32_t eq_pregain(void);

void eq_state_reset(EqState *st);
// Run n samples of one channel (0 left, 1 right) in place.
void eq_run(EqState *st, int16_t *buf, int n, int ch);

#endif
