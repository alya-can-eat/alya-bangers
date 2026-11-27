// SPDX-License-Identifier: GPL-3.0-or-later
//
// Search: see search.h. Every keystroke searches the whole library again:
// with a few thousand names that's a few ms on the DS, and it keeps the
// code simple (no stale results when a letter is deleted).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "fold_tables.h"
#include "i18n.h"
#include "search.h"
#include "util.h"

// ------------------------------------------------------------ folding

// One character for matching: its "base" letters (no case, no accents,
// katakana as hiragana, small kana as big ones; ß gives two) and its "exact"
// form (only case and katakana folded). Returns how many base letters.
static int fold_cp(uint32_t c, uint16_t base[2], uint16_t *exact)
{
    uint32_t e = c;
    if (c < 0x80)
        e = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    else if (c >= 0xC0 && c <= 0x24F)
    {
        e = c + lat_lower[c - 0xC0];
        int x = lat_base[c - 0xC0];
        *exact = (uint16_t)e;
        if (x > 0 && x < 'a')
        {
            base[0] = (uint8_t)lat_pairs[x - 1][0];
            base[1] = (uint8_t)lat_pairs[x - 1][1];
            return 2;
        }
        base[0] = x ? (uint16_t)x : (uint16_t)e;
        return 1;
    }
    else if (c >= 0x1E00 && c <= 0x1EFF)
    {
        e = c + ext_lower[c - 0x1E00];
        int x = ext_base[c - 0x1E00];
        *exact = (uint16_t)e;
        base[0] = x ? (uint16_t)x : (uint16_t)e;
        return 1;
    }
    else if ((c >= 0x391 && c <= 0x3A9 && c != 0x3A2) || (c >= 0x410 && c <= 0x42F))
        e = c + 32;                                 // Greek, Cyrillic capitals
    else if (c >= 0x400 && c <= 0x40F)
        e = c + 80;                                 // Ѐ..Џ
    else if (c >= 0x30A1 && c <= 0x30F6)
        e = c - 0x60;                               // katakana as hiragana
    else if (c == 0x30FD || c == 0x30FE)
        e = c - 0x60;                               // ヽ ヾ
    else if (c >= 0xFF01 && c <= 0xFF5E)
    {
        e = c - 0xFEE0;                             // full width ASCII
        if (e >= 'A' && e <= 'Z')
            e += 32;
    }
    else if (c > 0xFFFF)
        e = 0xFFFF;
    *exact = (uint16_t)e;
    uint32_t b = e;
    if (b == 0x451)
        b = 0x435;                                  // ё as е
    else if (b == 0x3041 || b == 0x3043 || b == 0x3045 || b == 0x3047 || b == 0x3049 || b == 0x3083
             || b == 0x3085 || b == 0x3087 || b == 0x308E || b == 0x3063)
        b = b + 1;                                  // small kana as big ones
    else if (b == 0x3095)
        b = 0x304B;
    else if (b == 0x3096)
        b = 0x3051;
    base[0] = (uint16_t)b;
    return 1;
}

static bool is_word_char(uint32_t c)
{
    if (c < 0x80)
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return !(c >= 0x2000 && c <= 0x206F) && !(c >= 0x3000 && c <= 0x303F) && c != 0xA0 && c != 0xFF5E;
}

#define FMAX 256
typedef struct {
    uint16_t b[FMAX], e[FMAX];  // base and exact letters
    uint8_t src[FMAX];          // the character of the string each came from
    bool word[FMAX];            // a word starts here
    int n;
} Folded;

// drop_spaces: pinyin, where "luoxinghan" should find "luò xīng hán"
static void fold_str(const char *s, bool drop_spaces, Folded *f)
{
    f->n = 0;
    uint32_t prev = ' ';
    for (int k = 0; *s && f->n < FMAX - 1 && k < 255; k++)
    {
        uint32_t c = utf8_next(&s);
        if (drop_spaces && (c == ' ' || c == '\''))
        {
            prev = c;
            continue;
        }
        uint16_t base[2], exact;
        int nb = fold_cp(c, base, &exact);
        for (int j = 0; j < nb && f->n < FMAX; j++)
        {
            f->b[f->n] = base[j];
            f->e[f->n] = exact;
            f->src[f->n] = (uint8_t)k;
            f->word[f->n] = j == 0 && !is_word_char(prev);
            f->n++;
        }
        prev = c;
    }
}

typedef struct {
    Folded f;
    bool acc[FMAX];             // typed with its accent: must be exactly this letter
} Query;

static void query_make(const char *s, bool drop_spaces, Query *q)
{
    fold_str(s, drop_spaces, &q->f);
    for (int i = 0; i < q->f.n; i++)
        q->acc[i] = q->f.e[i] != q->f.b[i];
}

// Where q is in h (its first letter), the best place: the start, then a
// word start, then anywhere. -1: not there.
static int find(const Folded *h, const Query *q, int *rank)
{
    int best = -1, best_rank = 3;
    for (int i = 0; i + q->f.n <= h->n && best_rank > 0; i++)
    {
        int j = 0;
        while (j < q->f.n && h->b[i + j] == q->f.b[j] && (!q->acc[j] || h->e[i + j] == q->f.e[j]))
            j++;
        if (j < q->f.n)
            continue;
        int r = i == 0 ? 0 : (h->word[i] ? 1 : 2);
        if (r < best_rank)
        {
            best = i;
            best_rank = r;
        }
    }
    *rank = best_rank;
    return best;
}

static Folded hay;      // one at a time: not on the small stack

static bool match_str(const char *s, const Query *q, bool drop_spaces, int *rank, SearchMark *m, int via)
{
    fold_str(s, drop_spaces, &hay);
    int i = find(&hay, q, rank);
    if (i < 0)
        return false;
    int last = hay.src[i + q->f.n - 1];
    m->start = hay.src[i];
    m->len = (uint8_t)(last - hay.src[i] + 1);
    m->via = (uint8_t)via;
    return true;
}

typedef struct {
    Query name, pinyin;
    bool use_pinyin;
} Searcher;

static Searcher sr;

// A name (a value id, or plain text with value = LIB_NONE), and its readings.
static bool match_value(uint32_t value, const char *text, int *rank, SearchMark *m)
{
    if (match_str(text, &sr.name, false, rank, m, VIA_NAME))
        return true;
    const char *kana, *pinyin;
    if (value == LIB_NONE || !lib_reading(value, &kana, &pinyin))
        return false;
    if (kana && match_str(kana, &sr.name, false, rank, m, VIA_KANA))
        return true;
    if (sr.use_pinyin && pinyin && sr.pinyin.f.n && match_str(pinyin, &sr.pinyin, true, rank, m, VIA_PINYIN))
        return true;
    return false;
}

const char *search_reading(const Item *it, int via)
{
    uint32_t v = LIB_NONE;
    if (it->kind == ITEM_ARTIST)
        v = lib_group_name((int)it->id);
    else if (it->kind == ITEM_ALBUM)
        v = lib_album(it->id)->name;
    else if (it->kind == ITEM_TRACK)
        v = lib_track(it->id)->title;
    const char *kana, *pinyin;
    if (v == LIB_NONE || via == VIA_NAME || !lib_reading(v, &kana, &pinyin))
        return NULL;
    return via == VIA_KANA ? kana : pinyin;
}

// ------------------------------------------------------------ tags

// Names of the common tags; any other key shows as itself ("Mood" for mood).
static const struct {
    const char *key;
    const char *name[LANG_COUNT], *plural[LANG_COUNT];
} tag_names[] = {
    { "genre", { "Genre", "Genre" }, { "genres", "Genres" } },
    { "albumartist", { "Album artist", "Albuminterpret" }, { "album artists", "Albuminterpreten" } },
    { "composer", { "Composer", "Komponist" }, { "composers", "Komponisten" } },
    { "lyricist", { "Lyricist", "Texter" }, { "lyricists", "Texter" } },
    { "performer", { "Performer", "Mitwirkende" }, { "performers", "Mitwirkende" } },
    { "conductor", { "Conductor", "Dirigent" }, { "conductors", "Dirigenten" } },
    { "arranger", { "Arranger", "Arrangeur" }, { "arrangers", "Arrangeure" } },
    { "remixer", { "Remixer", "Remixer" }, { "remixers", "Remixer" } },
    { "mood", { "Mood", "Stimmung" }, { "moods", "Stimmungen" } },
    { "style", { "Style", "Stil" }, { "styles", "Stile" } },
    { "label", { "Label", "Label" }, { "labels", "Labels" } },
    { "organization", { "Label", "Label" }, { "labels", "Labels" } },
    { "grouping", { "Grouping", "Gruppierung" }, { "groupings", "Gruppierungen" } },
    { "language", { "Language", "Sprache" }, { "languages", "Sprachen" } },
    { "initialkey", { "Key", "Tonart" }, { "keys", "Tonarten" } },
    { "bpm", { "BPM", "BPM" }, { "BPM", "BPM" } },
};
#define N_TAG_NAMES ((int)(sizeof(tag_names) / sizeof(tag_names[0])))

// Tags with a place of their own (tabs, Year, Rating, Played), numbers that
// mean nothing on their own, and the converter's own (~codec, ~#rating, ...).
static const char *const tag_hidden[] = {
    "title", "artist", "artists", "album", "albumartists", "date", "originaldate", "year",
    "originalyear", "releasedate", "tracknumber", "discnumber", "tracktotal", "disctotal",
    "totaltracks", "totaldiscs", "track", "disc", "discsubtitle", "isrc", "barcode", "upc",
    "asin", "catalognumber", "length", "website", "copyright", "license", "compilation",
};

static int known_tag(int key)
{
    const char *k = lib_key_name(key);
    for (int i = 0; i < N_TAG_NAMES; i++)
        if (strcmp(tag_names[i].key, k) == 0)
            return i;
    return -1;
}

bool search_tag_shown(int key)
{
    const char *k = lib_key_name(key);
    size_t l = strlen(k);
    if (key < 0 || key > 255 || l == 0 || k[0] == '~' || (l > 4 && strcmp(k + l - 4, "sort") == 0))
        return false;
    for (unsigned i = 0; i < sizeof(tag_hidden) / sizeof(tag_hidden[0]); i++)
        if (strcmp(tag_hidden[i], k) == 0)
            return false;
    return true;
}

void search_tag_name(int key, char *buf, int size)
{
    int i = known_tag(key);
    if (i >= 0)
    {
        snprintf(buf, size, "%s", tag_names[i].name[i18n_lang < LANG_COUNT ? i18n_lang : 0]);
        return;
    }
    snprintf(buf, size, "%s", lib_key_name(key));
    if (buf[0] >= 'a' && buf[0] <= 'z')
        buf[0] -= 32;
}

static void tag_plural(int key, char *buf, int size)
{
    int i = known_tag(key);
    if (i >= 0)
        snprintf(buf, size, "%s", tag_names[i].plural[i18n_lang < LANG_COUNT ? i18n_lang : 0]);
    else
        search_tag_name(key, buf, size);
}

// The tags that can filter, in order (the common ones first, as listed above,
// then the others by name), and every value they have: made once per library.
typedef struct { uint32_t value; uint16_t key, count; } TagValue;
static struct {
    bool made;
    uint32_t fingerprint;
    int n_keys;
    uint8_t keys[256];          // shown keys in order
    int songs[256];             // songs with each (by position)
    uint8_t rank[256];          // a key id's position
    TagValue *vals;
    int n_vals;
} tg;

static void tags_make(void)
{
    if (!lib_loaded() || (tg.made && tg.fingerprint == lib_fingerprint()))
        return;
    free(tg.vals);
    memset(&tg, 0, sizeof(tg));
    tg.made = true;
    tg.fingerprint = lib_fingerprint();
    int nk = lib_key_count() < 256 ? lib_key_count() : 256;
    bool taken[256] = { false };
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < (pass ? nk : N_TAG_NAMES); i++)
        {
            int k = pass ? i : lib_key_id(tag_names[i].key);
            if (k < 0 || k >= nk || taken[k] || !search_tag_shown(k))
                continue;
            taken[k] = true;
            int songs = lib_key_songs(k);
            if (songs == 0)
                continue;
            tg.rank[k] = (uint8_t)tg.n_keys;
            tg.songs[tg.n_keys] = songs;
            tg.keys[tg.n_keys++] = (uint8_t)k;
        }
    // their values
    int cap = 0;
    uint32_t *v = malloc(lib_value_count() * sizeof(uint32_t) + 4);
    int *c = malloc(lib_value_count() * sizeof(int) + 4);
    for (int i = 0; v && c && i < tg.n_keys; i++)
    {
        int n = lib_key_values(tg.keys[i], v, c, (int)lib_value_count());
        for (int j = 0; j < n; j++)
        {
            if (v[j] > 0xFFFFFF)
                continue;
            if (tg.n_vals == cap)
            {
                int ncap = cap ? cap * 2 : 256;
                TagValue *nv = realloc(tg.vals, ncap * sizeof(TagValue));
                if (nv == NULL)
                    break;
                tg.vals = nv;
                cap = ncap;
            }
            tg.vals[tg.n_vals++] = (TagValue){ v[j], tg.keys[i], (uint16_t)(c[j] > 0xFFFF ? 0xFFFF : c[j]) };
        }
    }
    free(v);
    free(c);
}

int search_tag_chip(const SearchFilter *f, int key)
{
    for (int i = 0; i < TAG_CHIPS; i++)
        if (f->tag[i].n && f->tag[i].key == key)
            return F_TAG + i;
    for (int i = 0; i < TAG_CHIPS; i++)
        if (f->tag[i].n == 0)
            return F_TAG + i;
    return F_TAG + TAG_CHIPS - 1;
}

void search_tag_toggle(SearchFilter *f, int key, uint32_t value)
{
    SearchTag *t = &f->tag[search_tag_chip(f, key) - F_TAG];
    if (t->n == 0 || t->key != key)
    {
        *t = (SearchTag){ (uint16_t)key, 1, 0, { value } };
        return;
    }
    for (int i = 0; i < t->n; i++)
        if (t->v[i] == value)
        {
            // on: off again
            memmove(&t->v[i], &t->v[i + 1], (t->n - i - 1) * sizeof(uint32_t));
            t->n--;
            return;
        }
    if (t->n < TAG_VALUES)
        t->v[t->n++] = value;
    else
        t->v[TAG_VALUES - 1] = value;   // full: the last one makes room
}

static bool tag_passes(uint32_t t, const SearchTag *tag)
{
    uint32_t ids[16];
    int n = lib_tag_ids_k(t, tag->key, ids, 16);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < tag->n; j++)
            if (ids[i] == tag->v[j])
                return true;
    return false;
}

// ------------------------------------------------------------ filters

static const uint8_t menu_kinds[FM_COUNT] = { F_IN, F_TAGS, F_RATING, F_YEAR, F_PLAYED, F_FOLDER };

int search_menu_kind(int row)
{
    return row >= 0 && row < FM_COUNT ? menu_kinds[row] : -1;
}

int search_menu_row(int kind)
{
    if (F_IS_TAG(kind))
        return FM_TAGS;
    for (int r = 0; r < FM_COUNT; r++)
        if (menu_kinds[r] == kind)
            return r;
    return -1;
}

void search_filter_clear(SearchFilter *f)
{
    memset(f, 0, sizeof(*f));
    f->rating = f->played = -1;
    f->decade = -1;
}

bool search_filter_on(const SearchFilter *f, int kind)
{
    if (F_IS_TAG(kind))
        return f->tag[kind - F_TAG].n > 0;
    switch (kind)
    {
    case F_IN: return f->in != IN_ALL;
    case F_RATING: return f->rating >= 0;
    case F_YEAR: return f->decade >= 0;
    case F_PLAYED: return f->played >= 0;
    case F_FOLDER: return f->folder[0] != 0;
    case F_TAGS:
        for (int i = 0; i < TAG_CHIPS; i++)
            if (f->tag[i].n)
                return true;
        return false;
    default: return false;
    }
}

void search_filter_off(SearchFilter *f, int kind)
{
    if (F_IS_TAG(kind))
    {
        f->tag[kind - F_TAG].n = 0;
        return;
    }
    switch (kind)
    {
    case F_IN: f->in = IN_ALL; break;
    case F_RATING: f->rating = -1; break;
    case F_YEAR: f->decade = -1; break;
    case F_PLAYED: f->played = -1; break;
    case F_FOLDER: f->folder[0] = 0; break;
    default: break;
    }
}

bool search_songs_filtered(const SearchFilter *f)
{
    for (int k = F_TAG; k < F_COUNT; k++)
        if (search_filter_on(f, k))
            return true;
    return false;
}

static bool rating_is(int r, int opt)
{
    return opt == RATE_NONE ? r == 0 : (opt == RATE_5 ? r == 5 : r >= 5 - opt);
}

static bool played_is(uint32_t p, int opt)
{
    return opt == PLAYED_NEVER ? p == 0 : (opt == PLAYED_ONCE ? p >= 1 : p >= 10);
}

static bool in_folder(uint32_t t, const char *folder)
{
    size_t l = strlen(folder);
    const char *p = lib_path(t);
    return strncmp(p, folder, l) == 0 && p[l] == '/';
}

static bool song_passes(uint32_t t, const SearchFilter *f)
{
    for (int i = 0; i < TAG_CHIPS; i++)
        if (f->tag[i].n && !tag_passes(t, &f->tag[i]))
            return false;
    if (f->decade >= 0)
    {
        int y = lib_track(t)->year;
        if (f->decade == 0 ? y != 0 : (y / 10 * 10 != f->decade))
            return false;
    }
    if (f->folder[0] && !in_folder(t, f->folder))
        return false;
    if (f->rating >= 0 && !rating_is(app_song_rating(t), f->rating))
        return false;
    if (f->played >= 0 && !played_is(app_song_plays(t), f->played))
        return false;
    return true;
}

// ------------------------------------------------------------ results

typedef struct {
    uint8_t rank;
    uint16_t sec;
    uint32_t key, id;
    SearchMark m;
} Hit;

// Sections in order: artists, albums, the tags, songs.
static int sec_order(int sec)
{
    if (sec >= SEC_TAG)
        return 2 + tg.rank[sec - SEC_TAG];
    return sec == SEC_SONGS ? 0x7FFF : sec;
}

static int cmp_hits(const void *pa, const void *pb)
{
    const Hit *a = pa, *b = pb;
    if (a->sec != b->sec) return sec_order(a->sec) < sec_order(b->sec) ? -1 : 1;
    if (a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->id < b->id ? -1 : (a->id > b->id);
}

// A song's title, or its file name without the extension (no copy when it has a title).
static const char *title_of(uint32_t t, char *buf, int size)
{
    const LibTrack *tr = lib_track(t);
    if (tr->title != LIB_NONE)
        return lib_value(tr->title);
    const char *b = strrchr(lib_path(t), '/');
    snprintf(buf, size, "%s", b ? b + 1 : lib_path(t));
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf)
        *dot = 0;
    return buf;
}

// A song with filters on: does the query match where the search looks?
static bool song_matches(uint32_t t, int in, int *rank, SearchMark *m)
{
    char title[256];
    memset(m, 0, sizeof(*m));
    if (sr.name.f.n == 0)
    {
        *rank = 0;
        return true;
    }
    if (in == IN_ALL || in == IN_SONGS)
    {
        if (match_value(lib_track(t)->title, title_of(t, title, sizeof(title)), rank, m))
            return true;
    }
    SearchMark other;
    if (in == IN_ALL || in == IN_ARTISTS)
    {
        uint32_t ids[16];
        int n = lib_tag_ids(t, "artist", ids, 16);
        for (int i = 0; i < n; i++)
            if (match_value(ids[i], lib_value(ids[i]), rank, &other))
            {
                *rank += 3;     // after the songs whose title matches
                return true;
            }
    }
    if ((in == IN_ALL || in == IN_ALBUMS) && lib_track(t)->album != LIB_NONE)
    {
        uint32_t name = lib_album(lib_track(t)->album)->name;
        if (match_value(name, lib_value(name), rank, &other))
        {
            *rank += 6;
            return true;
        }
    }
    return false;
}

int search_run(const char *query, int im, const SearchFilter *f, Item *out, SearchMark *marks, int cap)
{
    if (!lib_loaded() || cap < 4)
        return 0;
    query_make(query, false, &sr.name);
    query_make(query, true, &sr.pinyin);
    sr.use_pinyin = im == IM_PINYIN;
    bool songs_only = search_songs_filtered(f);
    if (sr.name.f.n == 0 && !songs_only)
        return 0;

    int max_hits = cap;
    Hit *hits = malloc(max_hits * sizeof(Hit));
    if (hits == NULL)
        return 0;
    int nh = 0, rank;
    SearchMark m;
    if (songs_only)
    {
        for (uint32_t t = 0; t < lib_track_count() && nh < max_hits; t++)
            if (song_passes(t, f) && song_matches(t, f->in, &rank, &m))
            {
                uint32_t key = lib_track(t)->title_sort;
                hits[nh++] = (Hit){ (uint8_t)rank, SEC_SONGS, key, t, m };
            }
    }
    else
    {
        if (f->in == IN_ALL || f->in == IN_ARTISTS)
            for (int g = 0; g < lib_group_count() && nh < max_hits; g++)
            {
                uint32_t v = lib_group_name(g);
                if (v != LIB_NONE && match_value(v, lib_value(v), &rank, &m))
                    hits[nh++] = (Hit){ (uint8_t)rank, SEC_ARTISTS, v, (uint32_t)g, m };
            }
        if (f->in == IN_ALL || f->in == IN_ALBUMS)
            for (uint32_t a = 0; a < lib_album_count() && nh < max_hits; a++)
            {
                uint32_t v = lib_album(a)->name;
                if (match_value(v, lib_value(v), &rank, &m))
                    hits[nh++] = (Hit){ (uint8_t)rank, SEC_ALBUMS, a, a, m };
            }
        if (f->in == IN_ALL)
        {
            tags_make();
            for (int i = 0; i < tg.n_vals && nh < max_hits; i++)
            {
                const TagValue *tv = &tg.vals[i];
                if (match_value(tv->value, lib_value(tv->value), &rank, &m))
                {
                    m.count = tv->count;
                    hits[nh++] = (Hit){ (uint8_t)rank, (uint16_t)(SEC_TAG + tv->key), tv->value,
                                        TAG_ITEM(tv->key, tv->value), m };
                }
            }
        }
        if (f->in == IN_ALL || f->in == IN_SONGS)
        {
            char title[256];
            for (uint32_t t = 0; t < lib_track_count() && nh < max_hits; t++)
            {
                if (match_value(lib_track(t)->title, title_of(t, title, sizeof(title)), &rank, &m))
                    hits[nh++] = (Hit){ (uint8_t)rank, SEC_SONGS, lib_track(t)->title_sort, t, m };
            }
        }
    }
    qsort(hits, nh, sizeof(Hit), cmp_hits);

    // a header row before each section
    static const uint8_t kinds[3] = { ITEM_ARTIST, ITEM_ALBUM, ITEM_TRACK };
    int n = 0;
    for (int i = 0; i < nh && n < cap; )
    {
        int sec = hits[i].sec, j = i;
        while (j < nh && hits[j].sec == sec)
            j++;
        memset(&marks[n], 0, sizeof(SearchMark));
        marks[n].count = (uint16_t)(j - i > 0xFFFF ? 0xFFFF : j - i);
        out[n++] = (Item){ ITEM_HEADER, (uint32_t)sec };
        for (; i < j && n < cap; i++)
        {
            marks[n] = hits[i].m;
            out[n++] = (Item){ sec >= SEC_TAG ? ITEM_TAG : kinds[sec], hits[i].id };
        }
        i = j;
    }
    free(hits);
    return n;
}

// ------------------------------------------------------------ filter choices

typedef struct { uint32_t v; int n; } Count;

static int cmp_count_desc(const void *pa, const void *pb)
{
    const SearchOpt *a = pa, *b = pb;
    if (a->count != b->count) return a->count > b->count ? -1 : 1;
    return strcmp(lib_value(a->value), lib_value(b->value));
}

int search_options(int kind, int key, SearchOpt *out, int max)
{
    int n = 0;
    uint32_t tc = lib_track_count();
    if (F_IS_TAG(kind))
    {
        // the values (of a tag that can filter), the most common first
        tags_make();
        int k = 0;
        for (int i = 0; i < tg.n_vals; i++)
            k += tg.vals[i].key == key;
        SearchOpt *all = malloc(k * sizeof(SearchOpt) + 8);
        if (all == NULL)
            return 0;
        k = 0;
        for (int i = 0; i < tg.n_vals; i++)
            if (tg.vals[i].key == key)
                all[k++] = (SearchOpt){ tg.vals[i].value, tg.vals[i].count };
        qsort(all, k, sizeof(SearchOpt), cmp_count_desc);
        n = k < max ? k : max;
        memcpy(out, all, n * sizeof(SearchOpt));
        free(all);
        return n;
    }
    switch (kind)
    {
    case F_TAGS:
        tags_make();
        for (int i = 0; i < tg.n_keys && n < max; i++)
            out[n++] = (SearchOpt){ tg.keys[i], tg.songs[i] };
        break;
    case F_RATING:
    case F_PLAYED:
    {
        int count = kind == F_RATING ? RATE_COUNT : PLAYED_COUNT;
        int c[RATE_COUNT] = { 0 };
        for (uint32_t t = 0; t < tc; t++)
        {
            int r = kind == F_RATING ? app_song_rating(t) : 0;
            uint32_t p = kind == F_PLAYED ? app_song_plays(t) : 0;
            for (int o = 0; o < count; o++)
                c[o] += kind == F_RATING ? rating_is(r, o) : played_is(p, o);
        }
        for (int o = 0; o < count && n < max; o++)
            out[n++] = (SearchOpt){ (uint32_t)o, c[o] };
        break;
    }
    case F_YEAR:
    {
        int c[21] = { 0 };      // 1900s .. 2090s, then no year
        for (uint32_t t = 0; t < tc; t++)
        {
            int y = lib_track(t)->year;
            c[y >= 1900 && y < 2100 ? (y - 1900) / 10 : 20] += 1;
        }
        for (int d = 19; d >= 0 && n < max; d--)    // newest first
            if (c[d])
                out[n++] = (SearchOpt){ (uint32_t)(1900 + d * 10), c[d] };
        if (c[20] && n < max)
            out[n++] = (SearchOpt){ 0, c[20] };
        break;
    }
    case F_FOLDER:
    {
        Item *items = malloc((tc + 1) * sizeof(Item));
        if (items == NULL)
            break;
        int ni = lib_list_folder(items, "", SORT_NAME_UP);
        for (int i = 0; i < ni && n < max; i++)
        {
            if (items[i].kind != ITEM_FOLDER)
                continue;
            char name[128];
            lib_folder_name(&items[i], "", name, sizeof(name));
            int c = 0;
            for (uint32_t t = 0; t < tc; t++)
                c += in_folder(t, name);
            out[n++] = (SearchOpt){ items[i].id, c };
        }
        free(items);
        break;
    }
    default:
        break;
    }
    return n;
}

void search_option_label(int kind, uint32_t value, char *buf, int size)
{
    if (F_IS_TAG(kind))
    {
        snprintf(buf, size, "%s", lib_value(value));
        return;
    }
    static const char *const stars[RATE_COUNT - 1] = { "★★★★★", "★★★★+", "★★★+", "★★+", "★+" };
    static const uint8_t played[PLAYED_COUNT] = { S_PLAYED_NEVER, S_PLAYED_ONCE, S_PLAYED_10 };
    static const uint8_t in[IN_COUNT] = { S_IN_ALL, S_ONLY_ARTISTS, S_ONLY_ALBUMS, S_ONLY_SONGS };
    switch (kind)
    {
    case F_IN: snprintf(buf, size, "%s", txt(in[value < IN_COUNT ? value : 0])); break;
    case F_TAGS: search_tag_name((int)value, buf, size); break;
    case F_RATING: snprintf(buf, size, "%s", value < RATE_NONE ? stars[value] : txt(S_UNRATED)); break;
    case F_PLAYED: snprintf(buf, size, "%s", txt(played[value < PLAYED_COUNT ? value : 0])); break;
    case F_YEAR:
        if (value == 0)
            snprintf(buf, size, "%s", txt(S_NO_YEAR));
        else
            snprintf(buf, size, txt(S_DECADE), (int)value);
        break;
    case F_FOLDER:
    {
        Item it = { ITEM_FOLDER, value };
        lib_folder_name(&it, "", buf, size);
        break;
    }
    default:
        buf[0] = 0;
        break;
    }
}

void search_chip_label(const SearchFilter *f, int kind, char *buf, int size)
{
    if (F_IS_TAG(kind))
    {
        const SearchTag *t = &f->tag[kind - F_TAG];
        if (t->n == 1)
            snprintf(buf, size, "%s", lib_value(t->v[0]));
        else
        {
            char p[64];
            tag_plural(t->key, p, sizeof(p));
            snprintf(buf, size, "%d %s", t->n, p);     // "2 genres"
        }
        return;
    }
    switch (kind)
    {
    case F_IN: search_option_label(F_IN, f->in, buf, size); break;
    case F_RATING: search_option_label(F_RATING, (uint32_t)f->rating, buf, size); break;
    case F_YEAR: search_option_label(F_YEAR, (uint32_t)f->decade, buf, size); break;
    case F_PLAYED: search_option_label(F_PLAYED, (uint32_t)f->played, buf, size); break;
    case F_FOLDER: snprintf(buf, size, "%s", f->folder); break;
    case F_TAGS:
    {
        // the menu's Tags row: the one chip, or how many
        int on = 0, last = -1;
        for (int i = 0; i < TAG_CHIPS; i++)
            if (f->tag[i].n)
                on++, last = F_TAG + i;
        if (on == 1)
            search_chip_label(f, last, buf, size);
        else
            snprintf(buf, size, txt(S_N_TAGS), on);
        break;
    }
    default: buf[0] = 0; break;
    }
}

bool search_option_on(const SearchFilter *f, int kind, int key, uint32_t value)
{
    if (F_IS_TAG(kind))
    {
        const SearchTag *t = &f->tag[kind - F_TAG];
        for (int i = 0; t->key == key && i < t->n; i++)
            if (t->v[i] == value)
                return true;
        return false;
    }
    if (kind == F_TAGS)
    {
        for (int i = 0; i < TAG_CHIPS; i++)
            if (f->tag[i].n && f->tag[i].key == value)
                return true;
        return false;
    }
    if (!search_filter_on(f, kind))
        return false;
    char a[96], b[96];
    search_option_label(kind, value, a, sizeof(a));
    search_chip_label(f, kind, b, sizeof(b));
    return strcmp(a, b) == 0;
}

void search_filter_set(SearchFilter *f, int kind, uint32_t value)
{
    switch (kind)
    {
    case F_IN: f->in = (uint8_t)(value < IN_COUNT ? value : IN_ALL); break;
    case F_RATING: f->rating = (int8_t)value; break;
    case F_YEAR: f->decade = (int16_t)value; break;
    case F_PLAYED: f->played = (int8_t)value; break;
    case F_FOLDER: search_option_label(F_FOLDER, value, f->folder, sizeof(f->folder)); break;
    default: break;     // tags: search_tag_toggle
    }
}

// ------------------------------------------------------------ keyboard helpers

uint32_t cp_upper(uint32_t c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

uint32_t cp_lower(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

uint32_t kana_mod(uint32_t c, int mod)
{
    // か..ぢ, つ て と: + 1 voiced; は ひ ふ へ ほ: + 1 voiced, + 2 half voiced
    static const uint16_t voice[] = { 0x304B, 0x304D, 0x304F, 0x3051, 0x3053, 0x3055, 0x3057, 0x3059, 0x305B,
                                      0x305D, 0x305F, 0x3061, 0x3064, 0x3066, 0x3068 };
    static const uint16_t ha[] = { 0x306F, 0x3072, 0x3075, 0x3078, 0x307B };
    static const uint16_t small[] = { 0x3042, 0x3044, 0x3046, 0x3048, 0x304A, 0x3064, 0x3084, 0x3086, 0x3088, 0x308F };
    switch (mod)
    {
    case KANA_DAKU:
        if (c == 0x3046) return 0x3094;     // う ゔ
        if (c == 0x3094) return 0x3046;
        for (unsigned i = 0; i < sizeof(voice) / sizeof(voice[0]); i++)
        {
            if (c == voice[i]) return c + 1;
            if (c == voice[i] + 1u) return c - 1;
        }
        for (unsigned i = 0; i < sizeof(ha) / sizeof(ha[0]); i++)
        {
            if (c == ha[i] || c == ha[i] + 2u) return ha[i] + 1;
            if (c == ha[i] + 1u) return ha[i];
        }
        return 0;
    case KANA_HANDAKU:
        for (unsigned i = 0; i < sizeof(ha) / sizeof(ha[0]); i++)
        {
            if (c == ha[i] || c == ha[i] + 1u) return ha[i] + 2;
            if (c == ha[i] + 2u) return ha[i];
        }
        return 0;
    case KANA_SMALL:
        for (unsigned i = 0; i < sizeof(small) / sizeof(small[0]); i++)
        {
            uint32_t s = small[i] == 0x3064 ? 0x3063 : small[i] - 1;
            if (c == small[i]) return s;
            if (c == s) return small[i];
        }
        return 0;
    default:
        return 0;
    }
}

const char *letter_forms(uint32_t c)
{
    switch (c)
    {
    case 'a': return "äàáâãåāæ";
    case 'e': return "éèêëē";
    case 'i': return "íìîï";
    case 'o': return "öóòôõøōœ";
    case 'u': return "üúùûū";
    case 'c': return "çč";
    case 'n': return "ñ";
    case 's': return "šß";
    case 'z': return "ž";
    case 'y': return "ýÿ";
    case 0x435: return "ё";     // е
    case 0x44C: return "ъ";     // ь
    default: return NULL;
    }
}
