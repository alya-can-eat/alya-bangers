// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "library.h"
#include "queue.h"
#include "state.h"
#include "util.h"

#define STATE_VERSION 1
#define MAX_SAVED 65536

typedef struct {
    char magic[4];
    uint16_t version, header_size;
    Settings settings;
    uint32_t fingerprint;
    uint32_t pos_ms;
    int32_t queue_pos;
    uint32_t queue_count;
    uint8_t shuffle, repeat, album_queue, reserved;
    char path[512];
} StateHeader;

#define DEFAULT_FILE "/_nds/alya-bangers/state.bin"
#define OLD_FILE     "/_nds/dsplayer/state.bin"     // before the app had its name

static char file_path[256] = DEFAULT_FILE;

void state_set_file(const char *path)
{
    snprintf(file_path, sizeof(file_path), "%s", path);
}

void state_defaults(Settings *s)
{
    memset(s, 0, sizeof(*s));
    s->lr_skip_lid = 1;
    s->brightness = 5;
    s->rg_mode = RG_OFF;
}

bool state_load(Settings *s, Resume *r)
{
    memset(r, 0, sizeof(*r));
    FILE *f = fopen(file_path, "rb");
    if (f == NULL && strcmp(file_path, DEFAULT_FILE) == 0)
        f = fopen(OLD_FILE, "rb");      // saved under the old name: read it, save under the new
    if (f == NULL)
        return false;
    StateHeader h;
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) && memcmp(h.magic, "DSPS", 4) == 0
              && h.version == STATE_VERSION && h.header_size == sizeof(h) && h.queue_count <= MAX_SAVED;
    QueueEntry *e = NULL;
    if (ok && h.queue_count)
    {
        e = malloc(h.queue_count * sizeof(QueueEntry));
        ok = e != NULL && fread(e, sizeof(QueueEntry), h.queue_count, f) == h.queue_count;
    }
    uint32_t crc = 0;
    if (ok)
    {
        ok = fread(&crc, 1, 4, f) == 4;
        uint32_t c = crc32(&h, sizeof(h));
        if (h.queue_count)
            c ^= crc32(e, h.queue_count * sizeof(QueueEntry));
        ok = ok && c == crc;
    }
    fclose(f);
    if (!ok)
    {
        free(e);
        return false;
    }

    *s = h.settings;
    if (s->brightness < 2 || s->brightness > 5)
        s->brightness = 5;
    if (s->rg_mode >= RG_MODES)
        s->rg_mode = RG_OFF;
    h.path[sizeof(h.path) - 1] = 0;
    memcpy(r->path, h.path, sizeof(r->path));
    r->pos_ms = h.pos_ms;
    r->album_queue = h.album_queue;

    // Track ids only mean something for the same library.idx.
    bool same = lib_loaded() && h.fingerprint == lib_fingerprint();
    for (uint32_t i = 0; same && i < h.queue_count; i++)
        same = e[i].track < lib_track_count();
    if (same && h.queue_count)
        queue_restore(e, h.queue_count, h.queue_pos, h.shuffle, h.repeat);
    else
    {
        queue_set_shuffle(false);
        queue_set_shuffle(h.shuffle);
        queue_set_repeat(h.repeat <= REPEAT_ONE ? h.repeat : REPEAT_OFF);
    }
    free(e);
    return true;
}

bool state_save(const Settings *s, const Resume *r)
{
    // make the folders on the way (errors just mean they exist)
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", file_path);
    for (char *p = dir + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            mkdir(dir, 0777);
            *p = '/';
        }
    }

    StateHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "DSPS", 4);
    h.version = STATE_VERSION;
    h.header_size = sizeof(h);
    h.settings = *s;
    h.fingerprint = lib_fingerprint();
    h.pos_ms = r->pos_ms;
    h.queue_pos = queue_pos();
    h.queue_count = queue_count() <= MAX_SAVED ? queue_count() : 0;
    h.shuffle = queue_shuffle();
    h.repeat = queue_repeat();
    h.album_queue = r->album_queue;
    snprintf(h.path, sizeof(h.path), "%s", r->path);

    uint32_t crc = crc32(&h, sizeof(h));
    if (h.queue_count)
        crc ^= crc32(queue_entries(), h.queue_count * sizeof(QueueEntry));

    FILE *f = fopen(file_path, "wb");
    if (f == NULL)
        return false;
    bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h);
    if (ok && h.queue_count)
        ok = fwrite(queue_entries(), sizeof(QueueEntry), h.queue_count, f) == h.queue_count;
    ok = ok && fwrite(&crc, 1, 4, f) == 4;
    ok = (fclose(f) == 0) && ok;
    return ok;
}
