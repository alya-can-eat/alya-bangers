// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playlists: see playlist.h. The file:
//
//   #EXTM3U
//   #PLAYLIST:Night drive
//   Bandcamp/Kivara/Archive vol.1/01 Persist.dsa
//   ...
//
// It is written to name.new first and then renamed, like the play counts.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "library.h"
#include "playlist.h"
#include "util.h"

typedef struct {
    char name[PL_NAME];
    char file[64];              // its file name in the folder
    uint32_t *tracks;
    int n, cap;
    char *missing;              // paths not in the library, one per line (kept for the file)
} Playlist;

static char dir_path[300];         // the music folder's (main.c): <music>/.dsmusic/playlists
static Playlist pls[PL_MAX];
static int n_pls;

void pl_set_dir(const char *dir)
{
    snprintf(dir_path, sizeof(dir_path), "%s", dir);
}

// Playlists from where they were kept before (one folder for the card) into
// this music folder's, once: only if it has none yet.
void pl_move_old(const char *old_dir)
{
    DIR *d = opendir(dir_path);
    if (d != NULL)
    {
        closedir(d);
        return;                         // this music folder has its own already
    }
    d = opendir(old_dir);
    if (d == NULL)
        return;
    struct dirent *e;
    char from[600], to[600];
    while ((e = readdir(d)) != NULL)
    {
        size_t l = strlen(e->d_name);
        if (l < 5 || !casefold_equal(e->d_name + l - 4, ".m3u"))
            continue;
        snprintf(from, sizeof(from), "%s/%s", old_dir, e->d_name);
        snprintf(to, sizeof(to), "%s/%s", dir_path, e->d_name);
        make_dirs_for(to);
        if (rename(from, to) == 0)
            continue;
        // another drive: copied, then the old one goes
        FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
        bool ok = out != NULL;
        char buf[512];
        size_t n;
        while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
            ok = fwrite(buf, 1, n, out) == n;
        if (in)
            fclose(in);
        if (out)
            ok = fclose(out) == 0 && ok;
        if (ok)
            remove(from);
    }
    closedir(d);
    rmdir(old_dir);                     // empty now (if not, it stays)
}

static void free_one(Playlist *p)
{
    free(p->tracks);
    free(p->missing);
    memset(p, 0, sizeof(*p));
}

static int cmp_names(const void *a, const void *b)
{
    const Playlist *x = a, *y = b;
    // case doesn't matter for the order either (ASCII is enough here)
    for (const char *p = x->name, *q = y->name;; p++, q++)
    {
        int c = (*p >= 'A' && *p <= 'Z') ? *p + 32 : (uint8_t)*p;
        int d = (*q >= 'A' && *q <= 'Z') ? *q + 32 : (uint8_t)*q;
        if (c != d || c == 0)
            return c - d;
    }
}

static void sort_all(void)
{
    qsort(pls, n_pls, sizeof(Playlist), cmp_names);
}

static bool has(const Playlist *p, uint32_t t)
{
    for (int i = 0; i < p->n; i++)
        if (p->tracks[i] == t)
            return true;
    return false;
}

static bool push(Playlist *p, uint32_t t)
{
    if (p->n >= PL_SONGS)
        return false;
    if (p->n == p->cap)
    {
        int cap = p->cap ? p->cap * 2 : 32;
        uint32_t *nt = realloc(p->tracks, cap * sizeof(uint32_t));
        if (nt == NULL)
            return false;
        p->tracks = nt;
        p->cap = cap;
    }
    p->tracks[p->n++] = t;
    return true;
}

static void keep_missing(Playlist *p, const char *line)
{
    size_t old = p->missing ? strlen(p->missing) : 0, l = strlen(line);
    char *m = realloc(p->missing, old + l + 2);
    if (m == NULL)
        return;
    memcpy(m + old, line, l);
    m[old + l] = '\n';
    m[old + l + 1] = 0;
    p->missing = m;
}

// A file name for a name: the characters FAT can't have become _, and one
// another playlist has already gets a number.
static void file_name(const char *name, char *out, size_t size)
{
    char base[48];
    size_t n = 0;
    for (const char *s = name; *s && n + 1 < sizeof(base); s++)
        base[n++] = strchr("/\\:*?\"<>|", *s) || (uint8_t)*s < 0x20 ? '_' : *s;
    base[n] = 0;
    utf8_trim_partial(base);
    n = strlen(base);
    while (n && (base[n - 1] == '.' || base[n - 1] == ' '))
        base[--n] = 0;      // FAT drops these
    if (n == 0)
        snprintf(base, sizeof(base), "Playlist");
    // (built here: out may be the file name of the playlist being renamed)
    char f[64];
    snprintf(f, sizeof(f), "%s.m3u", base);
    for (int k = 2; k < 1000; k++)
    {
        bool taken = false;
        for (int i = 0; i < n_pls && !taken; i++)
            taken = pls[i].file != out && casefold_equal(pls[i].file, f);
        if (!taken)
            break;
        snprintf(f, sizeof(f), "%s %d.m3u", base, k);
    }
    snprintf(out, size, "%s", f);
}

static bool save(const Playlist *p)
{
    char path[400], tmp[410];
    snprintf(path, sizeof(path), "%s/%s", dir_path, p->file);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    make_dirs_for(path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL)
        return false;
    bool ok = fprintf(f, "#EXTM3U\n#PLAYLIST:%s\n", p->name) > 0;
    for (int i = 0; ok && i < p->n; i++)
        ok = fprintf(f, "%s\n", lib_path(p->tracks[i])) > 0;
    if (ok && p->missing)
        ok = fputs(p->missing, f) >= 0;
    ok = fclose(f) == 0 && ok;
    if (!ok)
        return false;
    remove(path);
    return rename(tmp, path) == 0;
}

static void load_one(const char *file)
{
    if (n_pls >= PL_MAX)
        return;
    char path[400];
    snprintf(path, sizeof(path), "%s/%s", dir_path, file);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return;
    Playlist *p = &pls[n_pls];
    memset(p, 0, sizeof(*p));
    snprintf(p->file, sizeof(p->file), "%s", file);
    // without a #PLAYLIST line: the file name without .m3u
    snprintf(p->name, sizeof(p->name), "%.*s", (int)(strlen(file) - 4), file);
    utf8_trim_partial(p->name);
    char line[600];
    while (fgets(line, sizeof(line), f))
    {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (l == 0)
            continue;
        if (strncmp(line, "#PLAYLIST:", 10) == 0)
        {
            snprintf(p->name, sizeof(p->name), "%.*s", (int)sizeof(p->name) - 1, line + 10);
            utf8_trim_partial(p->name);
            continue;
        }
        if (line[0] == '#')
            continue;
        const char *rel = line;
        if (rel[0] == '/')
            rel++;
        uint32_t t = lib_find_path(rel);
        if (t == LIB_NONE)
            keep_missing(p, rel);
        else if (!has(p, t))
            push(p, t);
    }
    fclose(f);
    if (p->name[0])
        n_pls++;
    else
        free_one(p);
}

void pl_load(void)
{
    for (int i = 0; i < n_pls; i++)
        free_one(&pls[i]);
    n_pls = 0;
    DIR *d = opendir(dir_path);
    if (d == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        size_t l = strlen(e->d_name);
        if (l > 4 && l < sizeof(pls[0].file) && (strcmp(e->d_name + l - 4, ".m3u") == 0
                                                 || strcmp(e->d_name + l - 4, ".M3U") == 0))
            load_one(e->d_name);
    }
    closedir(d);
    sort_all();
}

int pl_count(void)
{
    return n_pls;
}

const char *pl_name(int i)
{
    return i >= 0 && i < n_pls ? pls[i].name : "";
}

int pl_size(int i)
{
    return i >= 0 && i < n_pls ? pls[i].n : 0;
}

const uint32_t *pl_tracks(int i)
{
    return i >= 0 && i < n_pls ? pls[i].tracks : NULL;
}

int pl_find(const char *name)
{
    for (int i = 0; i < n_pls; i++)
        if (casefold_equal(pls[i].name, name))
            return i;
    return -1;
}

int pl_create(const char *name)
{
    if (n_pls >= PL_MAX || !name[0] || pl_find(name) >= 0)
        return -1;
    Playlist *p = &pls[n_pls];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    utf8_trim_partial(p->name);
    file_name(p->name, p->file, sizeof(p->file));
    n_pls++;
    save(p);
    sort_all();
    return pl_find(name);
}

int pl_add(int i, const uint32_t *tracks, int n)
{
    if (i < 0 || i >= n_pls)
        return 0;
    Playlist *p = &pls[i];
    int added = 0;
    for (int k = 0; k < n; k++)
    {
        if (has(p, tracks[k]))
            continue;
        if (!push(p, tracks[k]))
        {
            if (added)
                save(p);
            return added ? added : -1;
        }
        added++;
    }
    if (added)
        save(p);
    return added;
}

void pl_remove(int i, int pos)
{
    if (i < 0 || i >= n_pls || pos < 0 || pos >= pls[i].n)
        return;
    Playlist *p = &pls[i];
    memmove(&p->tracks[pos], &p->tracks[pos + 1], (p->n - pos - 1) * sizeof(uint32_t));
    p->n--;
    save(p);
}

void pl_move(int i, int from, int to)
{
    if (i < 0 || i >= n_pls || from == to || from < 0 || to < 0 || from >= pls[i].n || to >= pls[i].n)
        return;
    uint32_t *t = pls[i].tracks, x = t[from];
    if (from < to)
        memmove(&t[from], &t[from + 1], (to - from) * sizeof(uint32_t));
    else
        memmove(&t[to + 1], &t[to], (from - to) * sizeof(uint32_t));
    t[to] = x;
    save(&pls[i]);
}

int pl_rename(int i, const char *name)
{
    int other = pl_find(name);
    if (i < 0 || i >= n_pls || !name[0] || (other >= 0 && other != i))
        return -1;
    Playlist *p = &pls[i];
    char old_file[sizeof(p->file)], old_path[400];
    snprintf(old_file, sizeof(old_file), "%s", p->file);
    snprintf(old_path, sizeof(old_path), "%s/%s", dir_path, p->file);
    snprintf(p->name, sizeof(p->name), "%s", name);
    utf8_trim_partial(p->name);
    file_name(p->name, p->file, sizeof(p->file));     // (its own file name is free for it)
    if (!save(p))
        return i;
    // (only case changed: on FAT that's the same file, already replaced)
    if (!casefold_equal(old_file, p->file))
        remove(old_path);
    sort_all();
    return pl_find(name);
}

void pl_delete(int i)
{
    if (i < 0 || i >= n_pls)
        return;
    char path[400];
    snprintf(path, sizeof(path), "%s/%s", dir_path, pls[i].file);
    remove(path);
    free_one(&pls[i]);
    memmove(&pls[i], &pls[i + 1], (n_pls - i - 1) * sizeof(Playlist));
    n_pls--;
    memset(&pls[n_pls], 0, sizeof(Playlist));
}
