// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"

uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = p[0];
    if (c == 0)
        return 0;

    int extra;
    uint32_t min;
    if (c < 0x80)
    {
        *s += 1;
        return c;
    }
    else if ((c & 0xE0) == 0xC0)
    {
        extra = 1; c &= 0x1F; min = 0x80;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        extra = 2; c &= 0x0F; min = 0x800;
    }
    else if ((c & 0xF8) == 0xF0)
    {
        extra = 3; c &= 0x07; min = 0x10000;
    }
    else
    {
        *s += 1;
        return 0xFFFD;
    }

    for (int i = 1; i <= extra; i++)
    {
        if ((p[i] & 0xC0) != 0x80)
        {
            *s += i;
            return 0xFFFD;
        }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *s += 1 + extra;
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        return 0xFFFD;
    return c;
}

void utf8_trim_partial(char *s)
{
    size_t len = strlen(s);
    size_t start = len;
    // Find the start of the last character (at most 3 continuation bytes back).
    while (start > 0 && len - start < 4 && ((uint8_t)s[start - 1] & 0xC0) == 0x80)
        start--;
    if (start == 0)
        return;
    uint8_t lead = (uint8_t)s[start - 1];
    size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3
                : (lead & 0xF8) == 0xF0 ? 4 : 1;
    if (len - (start - 1) < need)
        s[start - 1] = '\0';
}

static int lower_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

int natural_compare(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (*x && *y)
    {
        if (is_digit(*x) && is_digit(*y))
        {
            while (*x == '0')
                x++;
            while (*y == '0')
                y++;
            const unsigned char *xs = x, *ys = y;
            while (is_digit(*x))
                x++;
            while (is_digit(*y))
                y++;
            int lx = x - xs, ly = y - ys;
            if (lx != ly)
                return lx - ly;
            int c = memcmp(xs, ys, lx);
            if (c != 0)
                return c;
            continue;
        }
        int cx = lower_ascii(*x), cy = lower_ascii(*y);
        if (cx != cy)
            return cx - cy;
        x++;
        y++;
    }
    if (*x || *y)
        return *x ? 1 : -1;
    return strcmp(a, b); // equal apart from case or leading zeros
}

void format_time(char *buf, size_t size, uint32_t ms)
{
    uint32_t s = ms / 1000;
    uint32_t h = s / 3600, m = (s / 60) % 60;
    s %= 60;
    if (h)
        snprintf(buf, size, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    else
        snprintf(buf, size, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
}

bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls)
        return false;
    s += ls - lx;
    for (size_t i = 0; i < lx; i++)
    {
        if (lower_ascii((unsigned char)s[i]) != lower_ascii((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

bool path_join(char *out, size_t size, const char *dir, const char *name)
{
    size_t ld = strlen(dir);
    bool slash = ld > 0 && dir[ld - 1] == '/';
    int n = snprintf(out, size, slash ? "%s%s" : "%s/%s", dir, name);
    return n > 0 && (size_t)n < size;
}

const char *path_basename(const char *path)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    const char *slash = NULL;
    for (size_t i = 0; i + 1 < len; i++)
    {
        if (path[i] == '/')
            slash = path + i;
    }
    return slash ? slash + 1 : path;
}

bool path_parent(char *path)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/' && path[len - 2] != ':')
        path[--len] = '\0';
    char *slash = strrchr(path, '/');
    if (slash == NULL)
        return false;
    if (slash == path || slash[-1] == ':')
    {
        // "/x" -> "/", "fat:/x" -> "fat:/"
        if (slash[1] == '\0')
            return false;
        slash[1] = '\0';
        return true;
    }
    *slash = '\0';
    return true;
}

uint32_t crc32(const void *data, size_t len)
{
    static uint32_t table[256];
    static bool ready;
    if (!ready)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void make_dirs_for(const char *file_path)
{
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
}
