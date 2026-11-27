// SPDX-License-Identifier: GPL-3.0-or-later
//
// Search (the 🔍 tab): matching names as you type, results in sections
// (artists, albums, songs), filters, and what the keyboards need.
//
// Matching ignores case and accents ("golmo" finds Gölmo), hiragana and
// katakana are the same, small kana match big ones. A letter typed with its
// accent must be exactly that ("gö" finds Gölmo, not Golden Hours). Names with kanji
// or hanzi are also found by their readings from alya-bangers-dsconv: kana always, pinyin
// with the pinyin input (spaces don't matter there: "luoxinghan").

#ifndef DSPLAYER_SEARCH_H__
#define DSPLAYER_SEARCH_H__

#include <stdbool.h>
#include <stdint.h>

#include "library.h"

enum { IM_ABC, IM_KANA, IM_PINYIN, IM_CYR, IM_COUNT };     // input methods
enum { IN_ALL, IN_ARTISTS, IN_ALBUMS, IN_SONGS, IN_COUNT }; // "search in"

// Tags (genre, album artist, mood, ...): up to TAG_CHIPS of them filter at
// once, each with its own chip, and up to TAG_VALUES values of one tag
// ("Drum & Bass or Breakcore"). Different chips: songs must pass all of them.
#define TAG_CHIPS 3
#define TAG_VALUES 4

// filters (each becomes a chip); F_IN is "search in", F_TAG .. the tag chips
enum { F_IN, F_TAG, F_RATING = F_TAG + TAG_CHIPS, F_YEAR, F_PLAYED, F_FOLDER, F_COUNT };
// pages of the filter menu that aren't one filter: the list of tags
enum { F_TAGS = F_COUNT };
// rows of the filter menu
enum { FM_IN, FM_TAGS, FM_RATING, FM_YEAR, FM_PLAYED, FM_FOLDER, FM_COUNT };
int search_menu_kind(int row);      // F_IN, F_TAGS, F_RATING ...
int search_menu_row(int kind);      // a tag chip: FM_TAGS
#define F_IS_TAG(k) ((k) >= F_TAG && (k) < F_TAG + TAG_CHIPS)

enum { RATE_5, RATE_4, RATE_3, RATE_2, RATE_1, RATE_NONE, RATE_COUNT };   // ★★★★★, ★★★★+ … unrated
enum { PLAYED_NEVER, PLAYED_ONCE, PLAYED_10, PLAYED_COUNT };
// header rows (ITEM_HEADER ids): artists, albums, then a section per tag
// (SEC_TAG + its key id), then songs
enum { SEC_ARTISTS, SEC_ALBUMS, SEC_SONGS, SEC_TAG = 16 };
enum { VIA_NAME, VIA_KANA, VIA_PINYIN };

typedef struct {
    uint16_t key;               // tag key id (library.h)
    uint8_t n;                  // values, 0: off
    uint8_t unused;
    uint32_t v[TAG_VALUES];     // value ids
} SearchTag;

typedef struct {
    uint8_t in;                 // IN_ALL: no chip
    int8_t rating;              // -1: off, else RATE_...
    int8_t played;              // -1: off, else PLAYED_...
    int16_t decade;             // -1: off, 0: songs without a year, else 1990, 2000, ...
    SearchTag tag[TAG_CHIPS];
    char folder[96];            // a folder at the top of the card, "": off
} SearchFilter;

// What a result row shows besides its name.
typedef struct {
    uint8_t start, len;         // the match, in characters of the name or of the reading
    uint8_t via;                // VIA_NAME, or the reading it matched (shown after the name)
    uint8_t unused;
    uint16_t count;             // header rows: how many results the section has;
                                // tag rows: how many songs have the value
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
    uint32_t value;             // RATE_ / PLAYED_ option, decade, a folder's first song,
                                // a tag key id (F_TAGS) or a tag value id (a tag chip)
    int count;
} SearchOpt;
// key: which tag a tag chip's choices are the values of (else unused)
int search_options(int kind, int key, SearchOpt *out, int max);
void search_option_label(int kind, uint32_t value, char *buf, int size);
// Is this choice the one on (for tags: one of the values on)?
bool search_option_on(const SearchFilter *f, int kind, int key, uint32_t value);
// The chip for a filter that is on.
void search_chip_label(const SearchFilter *f, int kind, char *buf, int size);
// Turn a filter on with one of its choices.
void search_filter_set(SearchFilter *f, int kind, uint32_t value);

// Tags: the chip that has this key, else a free one (else the last one).
int search_tag_chip(const SearchFilter *f, int key);
// A value of a tag on, as another "or" of its chip; one that is on: off again.
void search_tag_toggle(SearchFilter *f, int key, uint32_t value);
// The name of a tag key ("Album artist" for albumartist), and how many of its values.
void search_tag_name(int key, char *buf, int size);
// Tags that can filter: not the ones that have a place of their own (title,
// artist, album, date, track numbers, ...) or the converter's own.
bool search_tag_shown(int key);

// Keyboard helpers: upper case (shift), and the kana keys ゛ ゜ 小 applied to
// the last character typed (0: it has no such form).
uint32_t cp_upper(uint32_t c);
uint32_t cp_lower(uint32_t c);
enum { KANA_DAKU, KANA_HANDAKU, KANA_SMALL };
uint32_t kana_mod(uint32_t c, int mod);
// The other forms of a letter, shown when its key is held (UTF-8), or NULL.
const char *letter_forms(uint32_t c);

#endif
