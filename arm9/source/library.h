// SPDX-License-Identifier: GPL-3.0-or-later
//
// library.idx (dsconv FORMAT.md, section 2): every song with its tags, loaded
// whole into RAM. Values are stored in sort order, so sorting by any tag is
// an integer sort.

#ifndef DSPLAYER_LIBRARY_H__
#define DSPLAYER_LIBRARY_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIB_NONE 0xFFFFFFFFu

typedef struct {
    uint32_t path;          // string offset, relative to the music root
    uint32_t duration_ms;
    uint32_t first_ref;
    uint16_t ref_count;
    uint16_t flags;
    uint32_t album;         // album id or LIB_NONE
    uint16_t disc, track;
    uint32_t title, title_sort;
    uint32_t artist, artist_sort;
    uint16_t year, reserved0;
    uint32_t reserved1;
} LibTrack;

typedef struct {
    uint32_t name, sort;
    uint32_t artist, artist_sort;
    uint32_t first;         // into the album track list
    uint16_t count, year;
    uint32_t cover_track;
    uint32_t reserved;
} LibAlbum;

_Static_assert(sizeof(LibTrack) == 48, "library.idx tracks are 48 bytes");
_Static_assert(sizeof(LibAlbum) == 32, "library.idx albums are 32 bytes");

bool lib_load(const char *path, char *err, int err_size);
bool lib_loaded(void);
// Changes whenever the converter rebuilt the index.
uint32_t lib_fingerprint(void);

uint32_t lib_track_count(void);
uint32_t lib_album_count(void);
const LibTrack *lib_track(uint32_t id);
const LibAlbum *lib_album(uint32_t id);
uint32_t lib_album_track(const LibAlbum *a, int i);
const char *lib_value(uint32_t value_id);    // "" for LIB_NONE
const char *lib_path(uint32_t track);
// First value of a tag (e.g. "genre"), or NULL.
// The first value of a tag (NULL: none).
const char *lib_tag(uint32_t track, const char *key);
// All values of a tag, as value ids in the file's order; returns how many.
int lib_tag_ids(uint32_t track, const char *key, uint32_t *out, int max);
// All values joined with sep (empty: none); returns how many.
int lib_tag_joined(uint32_t track, const char *key, const char *sep, char *out, size_t size);
// Track id for a path relative to the music root, or LIB_NONE.
uint32_t lib_find_path(const char *rel);

// ---- artists: tracks grouped by artist value, optionally ignoring case ----

void lib_group_artists(bool case_sensitive);
int lib_group_count(void);
// Spelling shown for a group (the most common one), LIB_NONE = "(no artist)".
uint32_t lib_group_name(int g);
int lib_group_size(int g);
int lib_track_group(uint32_t track);           // the group of its first artist
bool lib_track_in_group(uint32_t track, int g); // any of its artists
int lib_value_group(uint32_t value);            // an artist value's group, -1: none

// ---- lists for the browser ----

typedef enum { ITEM_ARTIST, ITEM_ALBUM, ITEM_TRACK, ITEM_FOLDER, ITEM_UP } ItemKind;

typedef struct {
    uint8_t kind;
    uint32_t id;    // group, album or track; for folders the first track inside
} Item;

enum { SORT_NAME_UP, SORT_NAME_DOWN, SORT_THIRD, SORT_FOURTH };
// Sort modes per list. Artist: name, name reversed.
// Album: name, reversed, year old->new, year new->old.
// Title: name, reversed, year old->new. Title inside an album: track order,
// name, reversed. Folder: name, reversed.

// Lists return the number of items written to out (room for every track,
// album or group is enough).
int lib_list_artists(Item *out, int sort);
int lib_list_albums(Item *out, int group, int sort);
int lib_list_tracks(Item *out, int group, uint32_t album, int sort, bool in_album);
// Folder contents: dir is relative to the music root ("" = the root).
int lib_list_folder(Item *out, const char *dir, int sort);
// Name of a folder item inside dir, copied into buf.
void lib_folder_name(const Item *it, const char *dir, char *buf, int size);

// All songs behind an item, in list order (artists by album then track).
int lib_item_tracks(const Item *it, const char *dir, uint32_t *out, int max);

// Compare two strings ignoring case (ASCII, Latin, Greek, Cyrillic, full-width).
bool casefold_equal(const char *a, const char *b);

#endif
