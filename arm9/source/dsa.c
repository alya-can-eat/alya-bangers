// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>

#include "dsa.h"
#include "util.h"

#define MAX_TAGS_SIZE   (64 * 1024)  // larger tag sections are skipped (long lyrics etc.)

static uint16_t rd16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool dsa_read_header(FILE *f, DsaHeader *h, char *err, int err_size)
{
    uint8_t raw[DSA_HEADER_SIZE];

    if (fseek(f, 0, SEEK_SET) != 0 || fread(raw, 1, sizeof(raw), f) != sizeof(raw))
    {
        snprintf(err, err_size, "file too short");
        return false;
    }
    if (memcmp(raw, "DSA1", 4) != 0)
    {
        snprintf(err, err_size, "not a .dsa file");
        return false;
    }
    if (rd32(raw + 508) != crc32(raw, 508))
    {
        snprintf(err, err_size, "damaged header");
        return false;
    }
    if (rd16(raw + 4) != DSA_VERSION)
    {
        snprintf(err, err_size, "format v%u, player needs v%u: reconvert",
                 rd16(raw + 4), DSA_VERSION);
        return false;
    }

    h->flags = rd32(raw + 8);
    h->sound_clock_hz = rd32(raw + 12);
    h->timer_divider = rd16(raw + 16);
    h->channels = raw[18];
    h->sample_format = raw[19];
    h->frame_count = rd32(raw + 20);
    h->block_frames = rd32(raw + 24);
    h->block_count = rd32(raw + 28);
    h->duration_ms = rd32(raw + 32);
    h->tags_offset = rd32(raw + 36);
    h->tags_size = rd32(raw + 40);
    h->cover_offset = rd32(raw + 44);
    h->cover_size = rd32(raw + 48);
    h->cover_width = rd16(raw + 52);
    h->cover_height = rd16(raw + 54);
    h->thumb_offset = rd32(raw + 56);
    h->thumb_size = rd32(raw + 60);
    h->thumb_width = rd16(raw + 64);
    h->thumb_height = rd16(raw + 66);
    h->audio_offset = rd32(raw + 68);
    h->audio_size = rd32(raw + 72);
    h->rg_track_gain_mdb = (int32_t)rd32(raw + 76);
    h->rg_track_peak_q16 = rd32(raw + 80);
    h->rg_album_gain_mdb = (int32_t)rd32(raw + 84);
    h->rg_album_peak_q16 = rd32(raw + 88);

    if (h->sample_format != 1 || h->channels < 1 || h->channels > 2)
    {
        snprintf(err, err_size, "unsupported audio format");
        return false;
    }
    if (h->audio_offset % 512 != 0 || h->timer_divider == 0 || h->block_frames == 0
        || h->audio_size != h->block_count * h->block_frames * 2 * h->channels)
    {
        snprintf(err, err_size, "inconsistent header");
        return false;
    }
    return true;
}

// Copy a tag value into dst (NUL-terminated), appending with " / " if dst
// already holds a value.
static void append_value(char *dst, size_t size, const uint8_t *v, size_t len)
{
    size_t used = strlen(dst);
    if (used > 0)
    {
        if (used + 3 >= size)
            return;
        memcpy(dst + used, " / ", 3);
        used += 3;
    }
    size_t room = size - 1 - used;
    if (len > room)
    {
        // Don't cut a UTF-8 sequence in half.
        len = room;
        while (len > 0 && (v[len] & 0xC0) == 0x80)
            len--;
    }
    memcpy(dst + used, v, len);
    dst[used + len] = '\0';
}

bool dsa_read_tags(FILE *f, const DsaHeader *h, DsaTags *t)
{
    memset(t, 0, sizeof(*t));
    if (h->tags_size < 4 || h->tags_size > MAX_TAGS_SIZE)
        return false;

    uint8_t *buf = malloc(h->tags_size);
    if (buf == NULL)
        return false;
    bool ok = fseek(f, h->tags_offset, SEEK_SET) == 0
              && fread(buf, 1, h->tags_size, f) == h->tags_size;
    if (ok)
    {
        uint32_t count = rd32(buf);
        size_t pos = 4;
        for (uint32_t i = 0; i < count; i++)
        {
            if (pos + 3 > h->tags_size)
                break;
            size_t klen = buf[pos];
            size_t vlen = rd16(buf + pos + 1);
            pos += 3;
            if (pos + klen + vlen > h->tags_size)
                break;
            const char *key = (const char *)buf + pos;
            const uint8_t *val = buf + pos + klen;
            pos += klen + vlen;

            if (klen == 5 && memcmp(key, "title", 5) == 0 && t->title[0] == '\0')
                append_value(t->title, sizeof(t->title), val, vlen);
            else if (klen == 6 && memcmp(key, "artist", 6) == 0)
                append_value(t->artist, sizeof(t->artist), val, vlen);
            else if (klen == 5 && memcmp(key, "album", 5) == 0 && t->album[0] == '\0')
                append_value(t->album, sizeof(t->album), val, vlen);
        }
    }
    free(buf);
    return ok;
}

bool dsa_read_cover(FILE *f, const DsaHeader *h, uint16_t *dst)
{
    if (!(h->flags & DSA_FLAG_COVER) || h->cover_width != DSA_COVER_SIZE
        || h->cover_height != DSA_COVER_SIZE
        || h->cover_size != DSA_COVER_SIZE * DSA_COVER_SIZE * 2)
        return false;
    return fseek(f, h->cover_offset, SEEK_SET) == 0
           && fread(dst, 1, h->cover_size, f) == h->cover_size;
}

bool dsa_read_thumb(FILE *f, const DsaHeader *h, uint16_t *dst)
{
    if (!(h->flags & DSA_FLAG_COVER) || h->thumb_width != DSA_THUMB_SIZE
        || h->thumb_height != DSA_THUMB_SIZE
        || h->thumb_size != DSA_THUMB_SIZE * DSA_THUMB_SIZE * 2)
        return false;
    return fseek(f, h->thumb_offset, SEEK_SET) == 0
           && fread(dst, 1, h->thumb_size, f) == h->thumb_size;
}
