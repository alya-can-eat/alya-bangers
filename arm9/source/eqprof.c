// SPDX-License-Identifier: GPL-3.0-or-later
//
// The file: a header ("AEQP", version, size, count, record size), the
// profiles, then a CRC-32. Written to eq.new first and then renamed, like
// stats.bin, so a save cut short can't lose them.

#include <stdio.h>
#include <string.h>

#include "eqprof.h"
#include "util.h"

#define EQP_VERSION 1

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t header_size;
    uint32_t count;
    uint32_t record_size;
} EqpHeader;

static char file_path[256] = "/_nds/alya-bangers/eq.bin";
static EqProfile profs[EQP_MAX];
static int n_profs;

void eqp_set_file(const char *path)
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
    EqpHeader h;
    static EqProfile p[EQP_MAX];
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) && memcmp(h.magic, "AEQP", 4) == 0
              && h.version == EQP_VERSION && h.header_size == sizeof(h)
              && h.record_size == sizeof(EqProfile) && h.count <= EQP_MAX;
    ok = ok && (h.count == 0 || fread(p, sizeof(EqProfile), h.count, f) == h.count);
    uint32_t crc = 0;
    ok = ok && fread(&crc, 1, 4, f) == 4
         && crc == (crc32(&h, sizeof(h)) ^ (h.count ? crc32(p, h.count * sizeof(EqProfile)) : 0));
    fclose(f);
    if (!ok)
        return false;
    for (uint32_t i = 0; i < h.count; i++)
    {
        p[i].name[EQP_NAME - 1] = 0;
        for (int b = 0; b < EQ_BANDS; b++)
            if (p[i].gain[b] < EQ_MIN || p[i].gain[b] > EQ_MAX)
                p[i].gain[b] = 0;
    }
    memcpy(profs, p, h.count * sizeof(EqProfile));
    n_profs = (int)h.count;
    return true;
}

void eqp_load(void)
{
    char tmp[256];
    new_path(tmp, sizeof(tmp));
    n_profs = 0;
    if (!load_from(file_path))
        load_from(tmp);     // a save that stopped between the two steps
}

bool eqp_save(void)
{
    char tmp[256];
    new_path(tmp, sizeof(tmp));
    make_dirs_for(file_path);
    EqpHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "AEQP", 4);
    h.version = EQP_VERSION;
    h.header_size = sizeof(h);
    h.count = n_profs;
    h.record_size = sizeof(EqProfile);
    uint32_t crc = crc32(&h, sizeof(h)) ^ (n_profs ? crc32(profs, n_profs * sizeof(EqProfile)) : 0);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h)
              && (n_profs == 0 || fwrite(profs, sizeof(EqProfile), n_profs, f) == (size_t)n_profs)
              && fwrite(&crc, 1, 4, f) == 4;
    ok = fclose(f) == 0 && ok;
    if (!ok)
        return false;
    remove(file_path);
    return rename(tmp, file_path) == 0;
}

int eqp_count(void)
{
    return n_profs;
}

const EqProfile *eqp_get(int i)
{
    return i >= 0 && i < n_profs ? &profs[i] : NULL;
}

int eqp_add(const char *name, const int8_t gain[EQ_BANDS])
{
    if (n_profs >= EQP_MAX)
        return -1;
    EqProfile *p = &profs[n_profs];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    utf8_trim_partial(p->name);
    memcpy(p->gain, gain, EQ_BANDS);
    return n_profs++;
}

void eqp_set_gain(int i, const int8_t gain[EQ_BANDS])
{
    if (i >= 0 && i < n_profs)
        memcpy(profs[i].gain, gain, EQ_BANDS);
}

void eqp_remove(int i)
{
    if (i < 0 || i >= n_profs)
        return;
    memmove(&profs[i], &profs[i + 1], (n_profs - i - 1) * sizeof(EqProfile));
    n_profs--;
}

void eqp_move(int from, int to)
{
    if (from < 0 || from >= n_profs || to < 0 || to >= n_profs || from == to)
        return;
    EqProfile p = profs[from];
    if (from < to)
        memmove(&profs[from], &profs[from + 1], (to - from) * sizeof(EqProfile));
    else
        memmove(&profs[to + 1], &profs[to], (from - to) * sizeof(EqProfile));
    profs[to] = p;
}
