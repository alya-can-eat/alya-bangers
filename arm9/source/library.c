// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library.h"
#include "util.h"

#define INDEX_VERSION 1
#define GROUP_LOOKBACK 8    // case variants sit next to each other in value order

typedef struct {
    uint16_t key, reserved;
    uint32_t value;
} LibRef;

static uint8_t *data;
static uint32_t data_size;
static uint32_t n_tracks, n_keys, n_values, n_albums, n_refs;
static const char *strings;
static uint32_t strings_size;
static const uint32_t *keys, *values, *album_tracks;
static const LibTrack *tracks;
static const LibRef *refs;
static const LibAlbum *albums;
static uint32_t fingerprint;
typedef struct { uint32_t value, kana, pinyin; } LibReading;
static const LibReading *readings;     // sorted by value id (FORMAT.md 2.9)
static uint32_t n_readings;

static uint32_t *path_hash;     // track id + 1, 0 = empty
static uint32_t path_hash_mask;

static uint16_t *track_group;   // per track: the group of its first artist
static uint16_t *value_group;   // per value: its artist group (0xFFFF: not an artist)
static int artist_key = -1;     // the "artist" tag key
static int none_group = -1;     // "(no artist)"
static int n_groups;
static uint32_t *group_name;    // display value id
static uint32_t *group_rank;    // first value id, for sorting
static int *group_size;

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static bool table_ok(uint32_t off, uint32_t count, uint32_t size)
{
    return (off & 3) == 0 && off <= data_size && (uint64_t)count * size <= data_size - off;
}

bool lib_load(const char *path, int *err)
{
    *err = LIB_OK;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        *err = LIB_ERR_MISSING;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 64)
    {
        fclose(f);
        *err = LIB_ERR_DAMAGED;
        return false;
    }
    data = malloc(size);
    if (data == NULL)
    {
        fclose(f);
        *err = LIB_ERR_TOO_BIG;
        return false;
    }
    size_t got = fread(data, 1, size, f);
    fclose(f);
    data_size = size;

    const uint8_t *h = data;
    if (got != (size_t)size || memcmp(h, "DSIX", 4) != 0 || (h[4] | (h[5] << 8)) != INDEX_VERSION)
    {
        *err = LIB_ERR_VERSION;
        goto fail;
    }
    n_tracks = rd32(h + 8);
    n_keys = rd32(h + 12);
    n_values = rd32(h + 16);
    n_albums = rd32(h + 20);
    n_refs = rd32(h + 24);
    uint32_t s_off = rd32(h + 28);
    strings_size = rd32(h + 32);
    uint32_t k_off = rd32(h + 36), v_off = rd32(h + 40), t_off = rd32(h + 44);
    uint32_t r_off = rd32(h + 48), a_off = rd32(h + 52), at_off = rd32(h + 56);
    if (s_off > data_size || strings_size > data_size - s_off || strings_size == 0
        || data[s_off + strings_size - 1] != 0
        || !table_ok(k_off, n_keys, 4) || !table_ok(v_off, n_values, 4)
        || !table_ok(t_off, n_tracks, sizeof(LibTrack)) || !table_ok(r_off, n_refs, sizeof(LibRef))
        || !table_ok(a_off, n_albums, sizeof(LibAlbum)) || !table_ok(at_off, 0, 4))
    {
        *err = LIB_ERR_DAMAGED;
        goto fail;
    }
    strings = (const char *)data + s_off;
    keys = (const uint32_t *)(data + k_off);
    values = (const uint32_t *)(data + v_off);
    tracks = (const LibTrack *)(data + t_off);
    refs = (const LibRef *)(data + r_off);
    albums = (const LibAlbum *)(data + a_off);
    album_tracks = (const uint32_t *)(data + at_off);
    fingerprint = crc32(data, 64) ^ (uint32_t)size;

    // Check every reference once, so the rest of the code can trust them.
    for (uint32_t i = 0; i < n_keys; i++)
        if (keys[i] >= strings_size) goto damaged;
    for (uint32_t i = 0; i < n_values; i++)
        if (values[i] >= strings_size) goto damaged;
    uint32_t at_count = (data_size - at_off) / 4;
    for (uint32_t i = 0; i < n_albums; i++)
    {
        const LibAlbum *a = &albums[i];
        if (a->name >= n_values || a->sort >= n_values || a->first + a->count > at_count)
            goto damaged;
        if ((a->artist != LIB_NONE && a->artist >= n_values) || (a->artist_sort != LIB_NONE && a->artist_sort >= n_values))
            goto damaged;
        for (int j = 0; j < a->count; j++)
            if (album_tracks[a->first + j] >= n_tracks) goto damaged;
    }
    for (uint32_t i = 0; i < n_tracks; i++)
    {
        const LibTrack *t = &tracks[i];
        if (t->path >= strings_size || t->first_ref + t->ref_count > n_refs)
            goto damaged;
        if (t->album != LIB_NONE && t->album >= n_albums) goto damaged;
        const uint32_t v[4] = { t->title, t->title_sort, t->artist, t->artist_sort };
        for (int j = 0; j < 4; j++)
            if (v[j] != LIB_NONE && v[j] >= n_values) goto damaged;
    }
    for (uint32_t i = 0; i < n_refs; i++)
        if (refs[i].key >= n_keys || refs[i].value >= n_values) goto damaged;

    // readings for search: optional, a table that doesn't add up is left out
    readings = NULL;
    n_readings = 0;
    uint32_t rd_off = rd32(h + 60);
    if (rd_off && (rd_off & 3) == 0 && rd_off <= data_size - 4)
    {
        uint32_t n = rd32(data + rd_off);
        const LibReading *r = (const LibReading *)(data + rd_off + 4);
        bool ok = table_ok(rd_off + 4, n, sizeof(LibReading));
        for (uint32_t i = 0; ok && i < n; i++)
            ok = r[i].value < n_values && (i == 0 || r[i].value > r[i - 1].value)
                 && (r[i].kana == LIB_NONE || r[i].kana < strings_size)
                 && (r[i].pinyin == LIB_NONE || r[i].pinyin < strings_size);
        if (ok)
        {
            readings = r;
            n_readings = n;
        }
    }

    uint32_t buckets = 16;
    while (buckets < n_tracks * 2)
        buckets <<= 1;
    path_hash = calloc(buckets, sizeof(uint32_t));
    track_group = calloc(n_tracks ? n_tracks : 1, sizeof(uint16_t));
    if (path_hash == NULL || track_group == NULL)
    {
        *err = LIB_ERR_TOO_BIG;
        goto fail;
    }
    path_hash_mask = buckets - 1;
    for (uint32_t i = 0; i < n_tracks; i++)
    {
        uint32_t b = hash_str(strings + tracks[i].path) & path_hash_mask;
        while (path_hash[b])
            b = (b + 1) & path_hash_mask;
        path_hash[b] = i + 1;
    }
    return true;

damaged:
    *err = LIB_ERR_DAMAGED;
fail:
    free(data);
    data = NULL;
    n_tracks = n_albums = 0;
    return false;
}

bool lib_loaded(void) { return data != NULL; }
uint32_t lib_fingerprint(void) { return fingerprint; }
uint32_t lib_track_count(void) { return n_tracks; }
uint32_t lib_album_count(void) { return n_albums; }
const LibTrack *lib_track(uint32_t id) { return &tracks[id]; }
const LibAlbum *lib_album(uint32_t id) { return &albums[id]; }
uint32_t lib_album_track(const LibAlbum *a, int i) { return album_tracks[a->first + i]; }
const char *lib_path(uint32_t track) { return strings + tracks[track].path; }

bool lib_reading(uint32_t value, const char **kana, const char **pinyin)
{
    uint32_t lo = 0, hi = n_readings;
    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2;
        if (readings[mid].value < value)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= n_readings || readings[lo].value != value)
        return false;
    *kana = readings[lo].kana != LIB_NONE ? strings + readings[lo].kana : NULL;
    *pinyin = readings[lo].pinyin != LIB_NONE ? strings + readings[lo].pinyin : NULL;
    return true;
}

const char *lib_value(uint32_t v)
{
    return v < n_values ? strings + values[v] : "";
}

// Index of a tag key, -1 if no track has it (keys are sorted by name).
static int key_index(const char *key)
{
    uint32_t lo = 0, hi = n_keys;
    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2;
        if (strcmp(strings + keys[mid], key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n_keys && strcmp(strings + keys[lo], key) == 0 ? (int)lo : -1;
}

const char *lib_tag(uint32_t track, const char *key)
{
    uint32_t id;
    return lib_tag_ids(track, key, &id, 1) ? strings + values[id] : NULL;
}

uint32_t lib_value_count(void)
{
    return n_values;
}

int lib_key_count(void)
{
    return (int)n_keys;
}

const char *lib_key_name(int key)
{
    return key >= 0 && (uint32_t)key < n_keys ? strings + keys[key] : "";
}

int lib_key_id(const char *name)
{
    return key_index(name);
}

int lib_key_songs(int key)
{
    int n = 0;
    for (uint32_t t = 0; t < n_tracks; t++)
        for (uint32_t i = 0; i < tracks[t].ref_count; i++)
            if (refs[tracks[t].first_ref + i].key == key)
            {
                n++;
                break;
            }
    return n;
}

int lib_key_values(int key, uint32_t *out, int *counts, int max)
{
    // a counter per value id, then the values that have one, in value (= sort) order
    uint16_t *c = calloc(n_values ? n_values : 1, sizeof(uint16_t));
    if (c == NULL)
        return 0;
    for (uint32_t t = 0; t < n_tracks; t++)
    {
        uint32_t last = LIB_NONE;
        for (uint32_t i = 0; i < tracks[t].ref_count; i++)
        {
            const LibRef *r = &refs[tracks[t].first_ref + i];
            if (r->key == key && r->value != last && c[r->value] < 0xFFFF)
                c[r->value]++;      // (a song with the same value twice counts once)
            if (r->key == key)
                last = r->value;
        }
    }
    int n = 0;
    for (uint32_t v = 0; v < n_values && n < max; v++)
        if (c[v])
        {
            out[n] = v;
            counts[n++] = c[v];
        }
    free(c);
    return n;
}

int lib_tag_ids(uint32_t track, const char *key, uint32_t *out, int max)
{
    int k = key_index(key);
    return k < 0 ? 0 : lib_tag_ids_k(track, k, out, max);
}

int lib_tag_ids_k(uint32_t track, int k, uint32_t *out, int max)
{
    int n = 0;
    const LibTrack *t = &tracks[track];
    for (uint32_t i = 0; i < t->ref_count && n < max; i++)
    {
        const LibRef *r = &refs[t->first_ref + i];
        if (r->key == k)
            out[n++] = r->value;
    }
    return n;
}

int lib_tag_joined(uint32_t track, const char *key, const char *sep, char *out, size_t size)
{
    uint32_t ids[16];
    int n = lib_tag_ids(track, key, ids, 16);
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len + 1 < size; i++)
        len += snprintf(out + len, size - len, "%s%s", i ? sep : "", strings + values[ids[i]]);
    return n;
}

uint32_t lib_find_path(const char *rel)
{
    if (path_hash == NULL)
        return LIB_NONE;
    uint32_t b = hash_str(rel) & path_hash_mask;
    while (path_hash[b])
    {
        uint32_t id = path_hash[b] - 1;
        if (strcmp(strings + tracks[id].path, rel) == 0)
            return id;
        b = (b + 1) & path_hash_mask;
    }
    return LIB_NONE;
}

// ------------------------------------------------------------ case folding

static uint32_t fold(uint32_t c)
{
    if (c < 0x80)
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return c | 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c & 1) ? c + 1 : c;
    if (c == 0x178)
        return 0xFF;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2)
        return c + 32;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    if (c >= 0xFF21 && c <= 0xFF3A)
        return c + 32;
    return c;
}

bool casefold_equal(const char *a, const char *b)
{
    for (;;)
    {
        uint32_t ca = utf8_next(&a), cb = utf8_next(&b);
        if (fold(ca) != fold(cb))
            return false;
        if (ca == 0)
            return true;
    }
}

// ------------------------------------------------------------ artist groups

// A song with several artists (several values of its artist tag) is in the
// group of each of them.
void lib_group_artists(bool case_sensitive)
{
    free(group_name);
    free(group_rank);
    free(group_size);
    free(value_group);
    group_name = group_rank = NULL;
    group_size = NULL;
    value_group = NULL;
    n_groups = 0;
    none_group = -1;
    if (data == NULL)
        return;
    artist_key = key_index("artist");

    uint32_t *count = calloc(n_values ? n_values : 1, sizeof(uint32_t));
    value_group = malloc((n_values ? n_values : 1) * sizeof(uint16_t));
    if (value_group)
        memset(value_group, 0xFF, (n_values ? n_values : 1) * sizeof(uint16_t));
    int cap = 64;
    group_name = malloc(cap * sizeof(uint32_t));
    group_rank = malloc(cap * sizeof(uint32_t));
    group_size = malloc(cap * sizeof(int));
    uint32_t *best = malloc(cap * sizeof(uint32_t));
    if (!count || !value_group || !group_name || !group_rank || !group_size || !best)
        goto out;

    bool no_artist = false;
    for (uint32_t i = 0; i < n_tracks; i++)
    {
        bool any = false;
        for (uint32_t k = 0; artist_key >= 0 && k < tracks[i].ref_count; k++)
        {
            const LibRef *r = &refs[tracks[i].first_ref + k];
            if (r->key == artist_key)
            {
                count[r->value]++;
                any = true;
            }
        }
        if (!any && tracks[i].artist != LIB_NONE)
        {
            count[tracks[i].artist]++;
            any = true;
        }
        if (!any)
            no_artist = true;
    }
    for (uint32_t v = 0; v < n_values; v++)
    {
        if (count[v] == 0)
            continue;
        int g = -1;
        if (!case_sensitive)
        {
            for (int k = n_groups - 1; k >= 0 && k >= n_groups - GROUP_LOOKBACK; k--)
            {
                if (casefold_equal(strings + values[group_rank[k]], strings + values[v]))
                {
                    g = k;
                    break;
                }
            }
        }
        if (g < 0)
        {
            if (n_groups + 1 >= cap || n_groups >= 0xFFFE)
            {
                if (n_groups >= 0xFFFE)
                    break;
                cap *= 2;
                group_name = realloc(group_name, cap * sizeof(uint32_t));
                group_rank = realloc(group_rank, cap * sizeof(uint32_t));
                group_size = realloc(group_size, cap * sizeof(int));
                best = realloc(best, cap * sizeof(uint32_t));
                if (!group_name || !group_rank || !group_size || !best)
                {
                    n_groups = 0;
                    goto out;
                }
            }
            g = n_groups++;
            group_rank[g] = v;
            group_name[g] = v;
            group_size[g] = 0;
            best[g] = 0;
        }
        value_group[v] = g;
        group_size[g] += count[v];
        if (count[v] > best[g])
        {
            best[g] = count[v];
            group_name[g] = v;   // the most common spelling wins
        }
    }
    if (no_artist)
    {
        none_group = n_groups++;
        group_name[none_group] = LIB_NONE;
        group_rank[none_group] = LIB_NONE;
        group_size[none_group] = 0;
    }
    for (uint32_t i = 0; i < n_tracks; i++)
    {
        uint32_t a = tracks[i].artist;
        int g = a == LIB_NONE ? none_group : value_group[a];
        track_group[i] = (uint16_t)g;
        if (a == LIB_NONE)
            group_size[g]++;
    }
out:
    free(count);
    free(best);
}

int lib_group_count(void) { return n_groups; }
uint32_t lib_group_name(int g) { return group_name[g]; }
int lib_group_size(int g) { return group_size[g]; }
int lib_track_group(uint32_t t) { return track_group[t]; }

int lib_value_group(uint32_t v)
{
    return value_group && v < n_values && value_group[v] != 0xFFFF ? value_group[v] : -1;
}

bool lib_track_in_group(uint32_t t, int g)
{
    if (track_group[t] == g)
        return true;
    for (uint32_t k = 0; value_group && artist_key >= 0 && k < tracks[t].ref_count; k++)
    {
        const LibRef *r = &refs[tracks[t].first_ref + k];
        if (r->key == artist_key && value_group[r->value] == g)
            return true;
    }
    return false;
}

// ------------------------------------------------------------ lists

static int sort_mode;
static bool sort_reverse;

static int cmp_u32(uint32_t a, uint32_t b) { return a < b ? -1 : a > b; }

static int cmp_groups(const void *pa, const void *pb)
{
    uint32_t a = ((const Item *)pa)->id, b = ((const Item *)pb)->id;
    // "(no artist)" always comes last
    if (group_rank[a] == LIB_NONE || group_rank[b] == LIB_NONE)
        return cmp_u32(group_rank[a], group_rank[b]);
    int c = cmp_u32(group_rank[a], group_rank[b]);
    return sort_reverse ? -c : c;
}

int lib_list_artists(Item *out, int sort)
{
    for (int g = 0; g < n_groups; g++)
        out[g] = (Item){ ITEM_ARTIST, (uint32_t)g };
    sort_mode = sort;
    sort_reverse = sort == SORT_NAME_DOWN;
    qsort(out, n_groups, sizeof(Item), cmp_groups);
    return n_groups;
}

static int cmp_albums(const void *pa, const void *pb)
{
    const LibAlbum *a = &albums[((const Item *)pa)->id], *b = &albums[((const Item *)pb)->id];
    int c = 0;
    if (sort_mode >= SORT_THIRD)
    {
        // unknown years go last either way
        uint32_t ya = a->year ? a->year : 0xFFFF, yb = b->year ? b->year : 0xFFFF;
        if (sort_mode == SORT_FOURTH && a->year && b->year)
            c = cmp_u32(yb, ya);
        else
            c = cmp_u32(ya, yb);
    }
    if (c == 0)
        c = cmp_u32(a->sort, b->sort);
    if (c == 0)
        c = cmp_u32(((const Item *)pa)->id, ((const Item *)pb)->id);
    return (sort_mode == SORT_NAME_DOWN) ? -c : c;
}

int lib_list_albums(Item *out, int group, int sort)
{
    int n = 0;
    for (uint32_t i = 0; i < n_albums; i++)
    {
        bool keep = group < 0;
        for (int j = 0; !keep && j < albums[i].count; j++)
            keep = lib_track_in_group(album_tracks[albums[i].first + j], group);
        if (keep)
            out[n++] = (Item){ ITEM_ALBUM, i };
    }
    sort_mode = sort;
    qsort(out, n, sizeof(Item), cmp_albums);
    return n;
}

static int cmp_tracks(const void *pa, const void *pb)
{
    uint32_t ia = ((const Item *)pa)->id, ib = ((const Item *)pb)->id;
    const LibTrack *a = &tracks[ia], *b = &tracks[ib];
    int c = 0;
    if (sort_mode == SORT_THIRD)
    {
        uint32_t ya = a->year ? a->year : 0xFFFF, yb = b->year ? b->year : 0xFFFF;
        c = cmp_u32(ya, yb);
    }
    if (c == 0)
        c = cmp_u32(a->title_sort, b->title_sort);  // no title (LIB_NONE) sorts last
    if (c == 0)
        c = cmp_u32(ia, ib);                         // path order
    return sort_mode == SORT_NAME_DOWN ? -c : c;
}

int lib_list_tracks(Item *out, int group, uint32_t album, int sort, bool in_album)
{
    int n = 0;
    if (album != LIB_NONE)
    {
        const LibAlbum *a = &albums[album];
        for (int j = 0; j < a->count; j++)
            out[n++] = (Item){ ITEM_TRACK, album_tracks[a->first + j] };
        if (in_album && sort == SORT_NAME_UP)
            return n;   // track order, as stored
        // inside an album: 1 = name, 2 = name reversed
        if (in_album)
            sort = sort == SORT_NAME_DOWN ? SORT_NAME_UP : SORT_NAME_DOWN;
    }
    else
    {
        for (uint32_t i = 0; i < n_tracks; i++)
            if (group < 0 || lib_track_in_group(i, group))
                out[n++] = (Item){ ITEM_TRACK, i };
    }
    sort_mode = sort;
    qsort(out, n, sizeof(Item), cmp_tracks);
    return n;
}

// Part of a track path below dir: NULL if the track is not inside dir.
static const char *below(uint32_t t, const char *dir, size_t dir_len)
{
    const char *p = strings + tracks[t].path;
    if (dir_len == 0)
        return p;
    if (strncmp(p, dir, dir_len) != 0 || p[dir_len] != '/')
        return NULL;
    return p + dir_len + 1;
}

static const char *list_dir;
static size_t list_dir_len;

static int name_len(const char *s)
{
    const char *slash = strchr(s, '/');
    return slash ? (int)(slash - s) : (int)strlen(s);
}

void lib_folder_name(const Item *it, const char *dir, char *buf, int size)
{
    const char *rest = below(it->id, dir, strlen(dir));
    if (rest == NULL)
        rest = "";
    int n = name_len(rest);
    if (n >= size)
        n = size - 1;
    memcpy(buf, rest, n);
    buf[n] = 0;
    utf8_trim_partial(buf);
}

static int cmp_folder_items(const void *pa, const void *pb)
{
    const Item *a = pa, *b = pb;
    if (a->kind != b->kind)
        return a->kind == ITEM_FOLDER ? -1 : 1;
    char na[256], nb[256];
    const char *ra = below(a->id, list_dir, list_dir_len), *rb = below(b->id, list_dir, list_dir_len);
    int la = name_len(ra), lb = name_len(rb);
    snprintf(na, sizeof(na), "%.*s", la, ra);
    snprintf(nb, sizeof(nb), "%.*s", lb, rb);
    int c = natural_compare(na, nb);
    return sort_reverse ? -c : c;
}

int lib_list_folder(Item *out, const char *dir, int sort)
{
    size_t dl = strlen(dir);
    int n = 0, first_folder = 0;
    for (uint32_t i = 0; i < n_tracks; i++)
    {
        const char *rest = below(i, dir, dl);
        if (rest == NULL)
            continue;
        const char *slash = strchr(rest, '/');
        if (slash == NULL)
        {
            out[n++] = (Item){ ITEM_TRACK, i };
            continue;
        }
        int len = slash - rest;
        bool seen = false;
        for (int k = n - 1; k >= first_folder && !seen; k--)
        {
            if (out[k].kind != ITEM_FOLDER)
                continue;
            const char *other = below(out[k].id, dir, dl);
            seen = strncmp(other, rest, len) == 0 && other[len] == '/';
        }
        if (!seen)
            out[n++] = (Item){ ITEM_FOLDER, i };
    }
    list_dir = dir;
    list_dir_len = dl;
    sort_reverse = sort == SORT_NAME_DOWN;
    qsort(out, n, sizeof(Item), cmp_folder_items);
    return n;
}

static int cmp_artist_tracks(const void *pa, const void *pb)
{
    uint32_t ia = *(const uint32_t *)pa, ib = *(const uint32_t *)pb;
    const LibTrack *a = &tracks[ia], *b = &tracks[ib];
    uint32_t sa = a->album == LIB_NONE ? LIB_NONE : albums[a->album].sort;
    uint32_t sb = b->album == LIB_NONE ? LIB_NONE : albums[b->album].sort;
    int c = cmp_u32(sa, sb);
    if (c == 0) c = cmp_u32(a->album, b->album);
    if (c == 0) c = cmp_u32(a->disc, b->disc);
    if (c == 0) c = cmp_u32(a->track, b->track);
    if (c == 0) c = cmp_u32(a->title_sort, b->title_sort);
    if (c == 0) c = cmp_u32(ia, ib);
    return c;
}

int lib_item_tracks(const Item *it, const char *dir, uint32_t *out, int max)
{
    int n = 0;
    switch (it->kind)
    {
    case ITEM_TRACK:
        if (max > 0)
            out[n++] = it->id;
        break;
    case ITEM_ALBUM:
    {
        const LibAlbum *a = &albums[it->id];
        for (int j = 0; j < a->count && n < max; j++)
            out[n++] = album_tracks[a->first + j];
        break;
    }
    case ITEM_ARTIST:
        for (uint32_t i = 0; i < n_tracks && n < max; i++)
            if (lib_track_in_group(i, (int)it->id))
                out[n++] = i;
        qsort(out, n, sizeof(uint32_t), cmp_artist_tracks);
        break;
    case ITEM_TAG:
    {
        int key = (int)TAG_ITEM_KEY(it->id);
        uint32_t value = TAG_ITEM_VALUE(it->id);
        for (uint32_t i = 0; i < n_tracks && n < max; i++)
            for (uint32_t k = 0; k < tracks[i].ref_count; k++)
            {
                const LibRef *r = &refs[tracks[i].first_ref + k];
                if (r->key == key && r->value == value)
                {
                    out[n++] = i;
                    break;
                }
            }
        qsort(out, n, sizeof(uint32_t), cmp_artist_tracks);
        break;
    }
    case ITEM_FOLDER:
    {
        char name[256], sub[1024];
        lib_folder_name(it, dir, name, sizeof(name));
        if (dir[0])
            snprintf(sub, sizeof(sub), "%s/%s", dir, name);
        else
            snprintf(sub, sizeof(sub), "%s", name);
        size_t sl = strlen(sub);
        for (uint32_t i = 0; i < n_tracks && n < max; i++)   // path order
            if (below(i, sub, sl) != NULL)
                out[n++] = i;
        break;
    }
    default:
        break;
    }
    return n;
}
