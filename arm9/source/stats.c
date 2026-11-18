// SPDX-License-Identifier: GPL-3.0-or-later
//
// A song is found by a 64-bit hash of its path below the music root, so its
// numbers stay when the library is converted again, as long as the file keeps
// its path.
//
// The file: a header ("ABST", version, size, record count), the records
// sorted by key, then a CRC-32. It is written to stats.new first and then
// renamed, so a save cut short (empty battery, power switch) can't lose what
// was there.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stats.h"
#include "util.h"

#define STATS_VERSION 1
#define ZERO_STARS    6     // rating byte: rated, with no stars

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t header_size;
    uint32_t count;
    uint32_t record_size;
} StatsHeader;

typedef struct {
    uint64_t key;
    uint32_t plays;
    uint32_t last_played;   // Unix time of the last counted play, 0 = never
    uint8_t rating;         // 0 = not rated, 1..5 stars, ZERO_STARS
    uint8_t reserved[7];
} StatRec;

_Static_assert(sizeof(StatRec) == 24, "24 bytes per song in the file");

static char file_path[256] = "/_nds/alya-bangers/stats.bin";
static StatRec *recs;
static uint32_t n_recs, cap_recs;
static bool dirty;

static uint64_t key_of(const char *rel)
{
    uint64_t h = 0xCBF29CE484222325ull;     // FNV-1a
    for (const uint8_t *p = (const uint8_t *)rel; *p; p++)
    {
        h ^= *p;
        h *= 0x100000001B3ull;
    }
    return h;
}

// Index of the key, or where it would go (with *found = false).
static uint32_t find(uint64_t key, bool *found)
{
    uint32_t lo = 0, hi = n_recs;
    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2;
        if (recs[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = lo < n_recs && recs[lo].key == key;
    return lo;
}

static StatRec *get(const char *rel)
{
    bool found;
    uint32_t i = find(key_of(rel), &found);
    return found ? &recs[i] : NULL;
}

static StatRec *get_or_add(const char *rel)
{
    uint64_t key = key_of(rel);
    bool found;
    uint32_t i = find(key, &found);
    if (found)
        return &recs[i];
    if (n_recs == cap_recs)
    {
        uint32_t cap = cap_recs ? cap_recs * 2 : 256;
        StatRec *r = realloc(recs, cap * sizeof(StatRec));
        if (!r)
            return NULL;
        recs = r;
        cap_recs = cap;
    }
    memmove(&recs[i + 1], &recs[i], (n_recs - i) * sizeof(StatRec));
    memset(&recs[i], 0, sizeof(StatRec));
    recs[i].key = key;
    n_recs++;
    return &recs[i];
}

void stats_set_file(const char *path)
{
    snprintf(file_path, sizeof(file_path), "%s", path);
}

static void new_path(char *out, int size)
{
    snprintf(out, size, "%s", file_path);
    char *dot = strrchr(out, '.');
    if (dot && !strchr(dot, '/'))
        *dot = 0;
    strncat(out, ".new", size - strlen(out) - 1);
}

static bool load_from(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    StatsHeader h;
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) && memcmp(h.magic, "ABST", 4) == 0
              && h.version == STATS_VERSION && h.header_size == sizeof(h)
              && h.record_size == sizeof(StatRec) && h.count <= 1000000;
    StatRec *r = NULL;
    if (ok && h.count)
    {
        r = malloc(h.count * sizeof(StatRec));
        ok = r && fread(r, sizeof(StatRec), h.count, f) == h.count;
    }
    uint32_t crc = 0;
    if (ok)
    {
        uint32_t c = crc32(&h, sizeof(h)) ^ (h.count ? crc32(r, h.count * sizeof(StatRec)) : 0);
        ok = fread(&crc, 1, 4, f) == 4 && crc == c;
    }
    for (uint32_t i = 1; ok && i < h.count; i++)
        ok = r[i - 1].key < r[i].key;
    fclose(f);
    if (!ok)
    {
        free(r);
        return false;
    }
    free(recs);
    recs = r;
    n_recs = cap_recs = h.count;
    return true;
}

void stats_load(void)
{
    char tmp[256];
    new_path(tmp, sizeof(tmp));
    if (!load_from(file_path))
        load_from(tmp);     // a save that stopped between the two steps
    dirty = false;
}

bool stats_dirty(void)
{
    return dirty;
}

bool stats_save(void)
{
    char tmp[256];
    new_path(tmp, sizeof(tmp));
    make_dirs_for(file_path);
    StatsHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "ABST", 4);
    h.version = STATS_VERSION;
    h.header_size = sizeof(h);
    h.count = n_recs;
    h.record_size = sizeof(StatRec);
    uint32_t crc = crc32(&h, sizeof(h)) ^ (n_recs ? crc32(recs, n_recs * sizeof(StatRec)) : 0);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h)
              && (n_recs == 0 || fwrite(recs, sizeof(StatRec), n_recs, f) == n_recs)
              && fwrite(&crc, 1, 4, f) == 4;
    ok = fclose(f) == 0 && ok;
    if (!ok)
        return false;
    remove(file_path);
    if (rename(tmp, file_path) != 0)
        return false;
    dirty = false;
    return true;
}

int stats_rating(const char *rel)
{
    const StatRec *r = get(rel);
    if (!r || r->rating == 0)
        return STATS_UNRATED;
    return r->rating == ZERO_STARS ? 0 : r->rating;
}

void stats_set_rating(const char *rel, int stars)
{
    if (stars > 5)
        stars = 5;
    StatRec *r = stars == STATS_UNRATED ? get(rel) : get_or_add(rel);
    if (!r)
        return;
    r->rating = stars == STATS_UNRATED ? 0 : (stars <= 0 ? ZERO_STARS : (uint8_t)stars);
    dirty = true;
}

uint32_t stats_plays(const char *rel)
{
    const StatRec *r = get(rel);
    return r ? r->plays : 0;
}

void stats_count_play(const char *rel, uint32_t unix_time)
{
    StatRec *r = get_or_add(rel);
    if (!r)
        return;
    r->plays++;
    r->last_played = unix_time;
    dirty = true;
}
