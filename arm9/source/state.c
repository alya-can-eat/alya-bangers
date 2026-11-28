// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library.h"
#include "queue.h"
#include "state.h"
#include "i18n.h"
#include "util.h"

#include <stddef.h>

#define STATE_VERSION 3     // 2: the theme at the end of the header; 3: 32 bytes of settings
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
    uint16_t theme[3];          // background, text, accent
    uint16_t reserved2;
} StateHeader;

// Versions 1 and 2: 16 bytes of settings (the start of today's); version 1
// ends before the theme.
typedef struct {
    char magic[4];
    uint16_t version, header_size;
    uint8_t settings[16];
    uint32_t fingerprint;
    uint32_t pos_ms;
    int32_t queue_pos;
    uint32_t queue_count;
    uint8_t shuffle, repeat, album_queue, reserved;
    char path[512];
    uint16_t theme[3];
    uint16_t reserved2;
} StateHeaderV2;

#define HEADER_V1 offsetof(StateHeaderV2, theme)
#define HEADER_V2 sizeof(StateHeaderV2)

#define DEFAULT_FILE "/_nds/alya-bangers/state.bin"
#define OLD_FILE     "/_nds/dsplayer/state.bin"     // before the app had its name

static char file_path[256] = DEFAULT_FILE;

void state_set_file(const char *path)
{
    snprintf(file_path, sizeof(file_path), "%s", path);
}

// paths.txt in the folder of the state file
static void paths_file(char *out, size_t size)
{
    const char *slash = strrchr(file_path, '/');
    int dir = slash ? (int)(slash - file_path + 1) : 0;
    snprintf(out, size, "%.*spaths.txt", dir, file_path);
}

void paths_load(Paths *p)
{
    memset(p, 0, sizeof(*p));
    char path[300], line[300];
    paths_file(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f))
    {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        // a path too long for its place is left out
        if (strncmp(line, "launcher=", 9) == 0 && l - 9 < sizeof(p->launcher))
            memcpy(p->launcher, line + 9, l - 9 + 1);
        else if (strncmp(line, "music=", 6) == 0 && l - 6 < sizeof(p->music))
            memcpy(p->music, line + 6, l - 6 + 1);
    }
    fclose(f);
}

bool paths_save(const Paths *p)
{
    char path[300];
    paths_file(path, sizeof(path));
    make_dirs_for(path);
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return false;
    bool ok = fprintf(f, "launcher=%s\nmusic=%s\n", p->launcher, p->music) > 0;
    return fclose(f) == 0 && ok;
}

void state_defaults(Settings *s)
{
    memset(s, 0, sizeof(*s));
    s->lr_skip_lid = 1;
    s->brightness = 5;
    s->rg_mode = RG_OFF;
}

bool state_load(Settings *s, Theme *t, Resume *r)
{
    memset(r, 0, sizeof(*r));
    *t = theme_default;
    FILE *f = fopen(file_path, "rb");
    if (f == NULL && strcmp(file_path, DEFAULT_FILE) == 0)
        f = fopen(OLD_FILE, "rb");      // saved under the old name: read it, save under the new
    if (f == NULL)
        return false;
    // the header as stored, whichever version
    static union { StateHeader v3; StateHeaderV2 v2; } raw;
    memset(&raw, 0, sizeof(raw));
    const size_t head = offsetof(StateHeader, settings);
    StateHeader *rh = &raw.v3;
    bool ok = fread(&raw, 1, head, f) == head && memcmp(rh->magic, "DSPS", 4) == 0
              && ((rh->version == STATE_VERSION && rh->header_size == sizeof(StateHeader))
                  || (rh->version == 2 && rh->header_size == HEADER_V2)
                  || (rh->version == 1 && rh->header_size == HEADER_V1));
    ok = ok && fread((char *)&raw + head, 1, rh->header_size - head, f) == rh->header_size - head;
    // in today's layout: older settings are the start of today's, the rest 0
    StateHeader h;
    memset(&h, 0, sizeof(h));
    if (ok && rh->version == STATE_VERSION)
        h = raw.v3;
    else if (ok)
    {
        const StateHeaderV2 *o = &raw.v2;
        memcpy(h.magic, o->magic, 4);
        h.version = o->version;
        h.header_size = o->header_size;
        memcpy(&h.settings, o->settings, sizeof(o->settings));
        h.fingerprint = o->fingerprint;
        h.pos_ms = o->pos_ms;
        h.queue_pos = o->queue_pos;
        h.queue_count = o->queue_count;
        h.shuffle = o->shuffle;
        h.repeat = o->repeat;
        h.album_queue = o->album_queue;
        memcpy(h.path, o->path, sizeof(h.path));
        memcpy(h.theme, o->theme, sizeof(h.theme));
    }
    ok = ok && h.queue_count <= MAX_SAVED;
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
        uint32_t c = crc32(&raw, h.header_size);
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
    if (h.version >= 2 && (h.theme[0] & h.theme[1] & h.theme[2] & 0x8000))
    {
        t->bg = h.theme[0];
        t->text = h.theme[1];
        t->acc = h.theme[2];
    }
    if (s->brightness < 2 || s->brightness > 5)
        s->brightness = 5;
    if (s->rg_mode >= RG_MODES)
        s->rg_mode = RG_OFF;
    if (s->lang >= LANG_COUNT)
        s->lang = LANG_EN;
    if (s->keyboard >= KB_LAYOUTS)
        s->keyboard = KB_QWERTY;
    if (s->date_fmt >= DATE_FORMATS)
        s->date_fmt = DATE_DMY;
    s->clock_12h = s->clock_12h != 0;
    s->play_at_start = s->play_at_start != 0;
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

bool state_save(const Settings *s, const Theme *t, const Resume *r)
{
    make_dirs_for(file_path);

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
    h.theme[0] = t->bg;
    h.theme[1] = t->text;
    h.theme[2] = t->acc;

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
