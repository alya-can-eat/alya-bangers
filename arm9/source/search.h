// SPDX-License-Identifier: GPL-3.0-or-later
//
// Search (the 🔍 tab): matching names as you type, results in sections
// (artists, albums, songs), filters, and what the keyboards need.
//
// Matching ignores case and accents ("golmo" finds Gölmo), hiragana and
// katakana are the same, small kana match big ones. A letter typed with its
// accent must be exactly that ("gö" finds Gölmo, not Golden Hours). Names with kanji
// or hanzi are also found by their readings from dsconv: kana always, pinyin
// with the pinyin input (spaces don't matter there: "luoxinghan").

#ifndef DSPLAYER_SEARCH_H__
#define DSPLAYER_SEARCH_H__

#include <stdbool.h>
#include <stdint.h>

#include "library.h"

enum { IM_ABC, IM_KANA, IM_PINYIN, IM_CYR, IM_COUNT };     // input methods
enum { IN_ALL, IN_ARTISTS, IN_ALBUMS, IN_SONGS, IN_COUNT }; // "search in"
// filters (each becomes a chip); F_IN is "search in"
enum { F_IN, F_GENRE, F_RATING, F_YEAR, F_PLAYED, F_FOLDER, F_COUNT };
enum { RATE_5, RATE_4, RATE_3, RATE_2, RATE_1, RATE_NONE, RATE_COUNT };   // ★★★★★, ★★★★+ … unrated
enum { PLAYED_NEVER, PLAYED_ONCE, PLAYED_10, PLAYED_COUNT };
enum { SEC_ARTISTS, SEC_ALBUMS, SEC_SONGS };                // header rows (ITEM_HEADER ids)
enum { VIA_NAME, VIA_KANA, VIA_PINYIN };

typedef struct {
    uint8_t in;                 // IN_ALL: no chip
    int8_t rating;              // -1: off, else RATE_...
    int8_t played;              // -1: off, else PLAYED_...
    int16_t decade;             // -1: off, 0: songs without a year, else 1990, 2000, ...
    uint32_t genre;             // genre value id, LIB_NONE: off
    char folder[96];            // a folder at the top of the card, "": off
} SearchFilter;

// What a result row shows besides its name.
typedef struct {
    uint8_t start, len;         // the match, in characters of the name or of the reading
    uint8_t via;                // VIA_NAME, or the reading it matched (shown after the name)
    uint8_t unused;
    uint16_t count;             // header rows: how many results the section has
} SearchMark;

void search_filter_clear(SearchFilter *f);
bool search_filter_on(const SearchFilter *f, int kind);
void search_filter_off(SearchFilter *f, int kind);
// Filters on the songs themselves (not just where to search): the results
// are then songs only.
bool search_songs_filtered(const SearchFilter *f);

// The results for a query as list items: a header row, then its results, per
// section. marks[i] belongs to out[i]. Nothing typed and no filter: none.
int search_run(const char *query, int im, const SearchFilter *f, Item *out, SearchMark *marks, int cap);
// The query matched the reading of this row's name: the reading, else NULL.
const char *search_reading(const Item *it, int via);

// The choices of a filter, with how many songs each has.
typedef struct {
    uint32_t value;             // genre value id, RATE_ / PLAYED_ option, decade, or a folder's first song
    int count;
} SearchOpt;
int search_options(int kind, SearchOpt *out, int max);
void search_option_label(int kind, uint32_t value, char *buf, int size);
// The chip for a filter that is on.
void search_chip_label(const SearchFilter *f, int kind, char *buf, int size);
// Turn a filter on with one of its choices.
void search_filter_set(SearchFilter *f, int kind, uint32_t value);

// Keyboard helpers: upper case (shift), and the kana keys ゛ ゜ 小 applied to
// the last character typed (0: it has no such form).
uint32_t cp_upper(uint32_t c);
uint32_t cp_lower(uint32_t c);
enum { KANA_DAKU, KANA_HANDAKU, KANA_SMALL };
uint32_t kana_mod(uint32_t c, int mod);
// The other forms of a letter, shown when its key is held (UTF-8), or NULL.
const char *letter_forms(uint32_t c);

#endif
