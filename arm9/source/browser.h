// SPDX-License-Identifier: MIT
//
// Folder listing: subfolders first, then .dsa files, both in natural order.
// No limit on the number of entries.

#ifndef DSPLAYER_BROWSER_H__
#define DSPLAYER_BROWSER_H__

#include <stdbool.h>

#define BROWSER_PATH_MAX 768

typedef struct {
    char *name;     // file or folder name as on the card
    bool is_dir;
} BrowserEntry;

typedef struct {
    char path[BROWSER_PATH_MAX];
    BrowserEntry *entries;
    int count;
    int capacity;
    int selected;
    int scroll;     // first visible row
} Browser;

// Read a folder. Calls `service` regularly while reading, so music keeps
// playing during long listings. Returns false if the folder can't be opened.
bool browser_open(Browser *b, const char *path, void (*service)(void));
void browser_free(Browser *b);

// Select the entry with this name, if present.
void browser_select_name(Browser *b, const char *name);

// Keep the selection visible in a window of `rows` rows.
void browser_clamp(Browser *b, int rows);

// Full path of entry i.
bool browser_entry_path(const Browser *b, int i, char *out, int size);

#endif
