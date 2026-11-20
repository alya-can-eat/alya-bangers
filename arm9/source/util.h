// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSPLAYER_UTIL_H__
#define DSPLAYER_UTIL_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Decode one UTF-8 code point starting at *s and advance *s. Invalid bytes
// give U+FFFD. Returns 0 at the end of the string.
uint32_t utf8_next(const char **s);

// Remove an incomplete UTF-8 sequence at the end of s (left by truncation).
void utf8_trim_partial(char *s);

// Natural, ASCII case-insensitive order: "Track 2" < "Track 10".
int natural_compare(const char *a, const char *b);

// "m:ss" or "h:mm:ss".
void format_time(char *buf, size_t size, uint32_t ms);

bool ends_with_ci(const char *s, const char *suffix);

// Join a directory and a name with exactly one '/'. Returns false if it
// does not fit.
bool path_join(char *out, size_t size, const char *dir, const char *name);

// The last component of a path ("a/b/c" -> "c"; "fat:/" -> "fat:/").
const char *path_basename(const char *path);

// Remove the last component in place. Returns false at the root.
bool path_parent(char *path);

uint32_t crc32(const void *data, size_t len);

// Make the folders on the way to a file (errors just mean they exist).
void make_dirs_for(const char *file_path);

#endif
