// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 3-band equalizer: Bass (low shelf, 120 Hz), Mid (peak, 1 kHz), Treble
// (high shelf, 5 kHz), each -12..+12 dB. player.c runs it on the samples in
// the ring, a little ahead of playback. Bands at 0 dB are skipped, so Flat
// costs nothing and leaves the samples untouched.
//
// The input is first turned down by the loudest point of the whole curve
// (and 0.25 dB more), so a boost doesn't clip.

#ifndef DSPLAYER_EQ_H__
#define DSPLAYER_EQ_H__

#include <stdbool.h>
#include <stdint.h>

#define EQ_BANDS 3
#define EQ_MIN   (-12)
#define EQ_MAX   12

typedef struct {
    int32_t x1, x2, y1, y2;     // the last inputs and outputs, samples x 4096
} EqBiquad;

// The filters' memory for one stream (left and right).
typedef struct {
    EqBiquad b[2][EQ_BANDS];
} EqState;

// The gains in dB, clamped to EQ_MIN..EQ_MAX. Applies to what is run next.
void eq_set(const int8_t gain[EQ_BANDS]);
void eq_get(int8_t gain[EQ_BANDS]);
// Any band not at 0 dB.
bool eq_active(void);
// How much the input is turned down (Q15, 32768 = not at all).
int32_t eq_pregain(void);

void eq_state_reset(EqState *st);
// Run n samples of one channel (0 left, 1 right) in place.
void eq_run(EqState *st, int16_t *buf, int n, int ch);

#endif
