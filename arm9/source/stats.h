// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ratings and plays made on the DS. The library index comes from dsconv and
// is only read, so these live in their own file (/_nds/alya-bangers/stats.bin).
// Songs are found by their path relative to the music root.

#ifndef DSPLAYER_STATS_H__
#define DSPLAYER_STATS_H__

#include <stdbool.h>
#include <stdint.h>

#define STATS_UNRATED (-1)

void stats_set_file(const char *path);
// Reads the file (a missing or damaged one: no stats yet).
void stats_load(void);
// Changed since the last save.
bool stats_dirty(void);
bool stats_save(void);

// Stars given on the DS, 0..5, or STATS_UNRATED.
int stats_rating(const char *rel);
// 0..5 stars, or STATS_UNRATED to forget the rating.
void stats_set_rating(const char *rel, int stars);

// Plays counted on the DS.
uint32_t stats_plays(const char *rel);
void stats_count_play(const char *rel, uint32_t unix_time);

#endif
