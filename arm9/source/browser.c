// SPDX-License-Identifier: MIT

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "browser.h"
#include "util.h"

static void clear_entries(Browser *b)
{
    for (int i = 0; i < b->count; i++)
        free(b->entries[i].name);
    b->count = 0;
}

void browser_free(Browser *b)
{
    clear_entries(b);
    free(b->entries);
    b->entries = NULL;
    b->capacity = 0;
}

static bool add_entry(Browser *b, const char *name, bool is_dir)
{
    if (b->count == b->capacity)
    {
        int cap = b->capacity ? b->capacity * 2 : 64;
        BrowserEntry *n = realloc(b->entries, cap * sizeof(*n));
        if (n == NULL)
            return false;
        b->entries = n;
        b->capacity = cap;
    }
    char *copy = strdup(name);
    if (copy == NULL)
        return false;
    b->entries[b->count].name = copy;
    b->entries[b->count].is_dir = is_dir;
    b->count++;
    return true;
}

static int compare_entries(const void *pa, const void *pb)
{
    const BrowserEntry *a = pa, *b = pb;
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    return natural_compare(a->name, b->name);
}

bool browser_open(Browser *b, const char *path, void (*service)(void))
{
    DIR *d = opendir(path);
    if (d == NULL)
        return false;

    clear_entries(b);
    snprintf(b->path, sizeof(b->path), "%s", path);
    b->selected = 0;
    b->scroll = 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (++n % 16 == 0 && service != NULL)
            service();
        const char *name = e->d_name;
        if (name[0] == '\0') // end of the listing (as in the BlocksDS examples)
            break;
        if (name[0] == '.') // hidden, ".", "..", .dsmusic
            continue;

        bool is_dir;
        if (e->d_type == DT_DIR)
            is_dir = true;
        else if (e->d_type == DT_REG)
            is_dir = false;
        else
        {
            char full[BROWSER_PATH_MAX];
            struct stat st;
            if (!path_join(full, sizeof(full), path, name) || stat(full, &st) != 0)
                continue;
            is_dir = S_ISDIR(st.st_mode);
        }
        if (!is_dir && !ends_with_ci(name, ".dsa"))
            continue;
        if (!add_entry(b, name, is_dir))
            break; // out of memory: show what we have
    }
    closedir(d);

    if (service != NULL)
        service();
    qsort(b->entries, b->count, sizeof(BrowserEntry), compare_entries);
    return true;
}

void browser_select_name(Browser *b, const char *name)
{
    for (int i = 0; i < b->count; i++)
    {
        if (strcmp(b->entries[i].name, name) == 0)
        {
            b->selected = i;
            return;
        }
    }
}

void browser_clamp(Browser *b, int rows)
{
    if (b->count == 0)
    {
        b->selected = 0;
        b->scroll = 0;
        return;
    }
    if (b->selected < 0)
        b->selected = 0;
    if (b->selected >= b->count)
        b->selected = b->count - 1;
    if (b->selected < b->scroll)
        b->scroll = b->selected;
    if (b->selected >= b->scroll + rows)
        b->scroll = b->selected - rows + 1;
    int max_scroll = b->count > rows ? b->count - rows : 0;
    if (b->scroll > max_scroll)
        b->scroll = max_scroll;
    if (b->scroll < 0)
        b->scroll = 0;
}

bool browser_entry_path(const Browser *b, int i, char *out, int size)
{
    if (i < 0 || i >= b->count)
        return false;
    return path_join(out, size, b->path, b->entries[i].name);
}
