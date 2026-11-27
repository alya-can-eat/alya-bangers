// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playlists made on the DS: one .m3u file each in /_nds/alya-bangers/playlists/,
// the songs as paths below the music root, so they stay when the library is
// converted again. A song that isn't in the library (any more) is kept in the
// file but not shown, in case it comes back.

#ifndef DSPLAYER_PLAYLIST_H__
#define DSPLAYER_PLAYLIST_H__

#include <stdbool.h>
#include <stdint.h>

#define PL_MAX   64             // playlists
#define PL_NAME  32             // UTF-8, with its 0
#define PL_SONGS 4000           // songs in one

void pl_set_dir(const char *dir);
// Reads every playlist (after the library is loaded).
void pl_load(void);

int pl_count(void);             // sorted by name
const char *pl_name(int i);
int pl_size(int i);             // its songs that are in the library
const uint32_t *pl_tracks(int i);
// The one with this name (case doesn't matter), -1: none.
int pl_find(const char *name);

// A new empty one (saved): its index, -1 if there are PL_MAX already or the
// name is taken or empty.
int pl_create(const char *name);
// Songs at the end; ones already in it are left out. How many were added,
// -1 if it's full.
int pl_add(int i, const uint32_t *tracks, int n);
void pl_remove(int i, int pos);
// A song to another place (positions in the playlist).
void pl_move(int i, int from, int to);
// A new name (its file too): its new index, -1 if another one has the name
// or it's empty.
int pl_rename(int i, const char *name);
void pl_delete(int i);

#endif
