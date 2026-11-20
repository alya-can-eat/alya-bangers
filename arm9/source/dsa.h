// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reader for .dsa files (see FORMAT.md in the dsconv repository).

#ifndef DSPLAYER_DSA_H__
#define DSPLAYER_DSA_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define DSA_VERSION         1
#define DSA_HEADER_SIZE     512
#define DSA_FLAG_COVER      (1u << 0)
#define DSA_FLAG_RG_TRACK   (1u << 1)
#define DSA_FLAG_RG_ALBUM   (1u << 2)
#define DSA_COVER_SIZE      128
#define DSA_THUMB_SIZE      32

typedef struct {
    uint32_t flags;
    uint32_t sound_clock_hz;
    uint16_t timer_divider;
    uint8_t channels;
    uint8_t sample_format;
    uint32_t frame_count;
    uint32_t block_frames;
    uint32_t block_count;
    uint32_t duration_ms;
    uint32_t tags_offset, tags_size;
    uint32_t cover_offset, cover_size;
    uint16_t cover_width, cover_height;
    uint32_t thumb_offset, thumb_size;
    uint16_t thumb_width, thumb_height;
    uint32_t audio_offset, audio_size;
    int32_t rg_track_gain_mdb;
    uint32_t rg_track_peak_q16;
    int32_t rg_album_gain_mdb;
    uint32_t rg_album_peak_q16;
} DsaHeader;

// Reads and checks the header (magic, version, checksum, layout). On failure
// returns false and writes a short reason to err.
bool dsa_read_header(FILE *f, DsaHeader *h, char *err, int err_size);

// Text shown for a song. Strings are empty when the tag is missing.
typedef struct {
    char title[192];
    char artist[160];
    char album[160];
} DsaTags;

// Reads title, artist and album (several artists are joined with " / ").
bool dsa_read_tags(FILE *f, const DsaHeader *h, DsaTags *t);

// Reads the 128x128 cover (BGR555, ready for VRAM) into dst.
bool dsa_read_cover(FILE *f, const DsaHeader *h, uint16_t *dst);

// Reads the 32x32 thumbnail into dst.
bool dsa_read_thumb(FILE *f, const DsaHeader *h, uint16_t *dst);

#endif
