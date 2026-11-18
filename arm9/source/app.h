// SPDX-License-Identifier: MIT
//
// Shared state of the player app: what the UI shows and how it is laid out.
// main.c changes it (input, playback), ui.c draws it.

#ifndef DSPLAYER_APP_H__
#define DSPLAYER_APP_H__

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "library.h"
#include "state.h"

// ---- colors (the default theme of the mockup) ----
#define C_BG        COLOR(2, 2, 4)
#define C_PANEL     COLOR(4, 4, 7)
#define C_ZEBRA     COLOR(3, 3, 5)
#define C_LINE      COLOR(7, 7, 10)
#define C_TEXT      COLOR(29, 29, 30)
#define C_DIM       COLOR(15, 15, 19)
#define C_FAINT     COLOR(9, 9, 12)
#define C_ACC       COLOR(31, 17, 23)
#define C_SEL       COLOR(11, 6, 10)
#define C_ACC_DIM   COLOR(18, 10, 14)

// ---- bottom screen layout ----
#define TAB_H       20
#define ROW_H       16
#define ROWS        9
#define LIST_Y      20
#define BAR_Y       164
#define SCROLL_X    244             // scroll bar column, 12 px wide
#define PANEL_DOWN  172             // panel offset in player mode: only the tabs show
#define SLIDE_FRAMES 15             // 0.5 s at 30 fps
#define TAB_FRAMES  8               // sideways list slide, 0.27 s

typedef enum { TAB_ARTIST, TAB_ALBUM, TAB_TITLE, TAB_FOLDER, TAB_SEARCH, TAB_COUNT } Tab;
typedef enum { OV_NONE, OV_SORT, OV_ROWMENU, OV_SETTINGS, OV_INFO, OV_QUEUE } Overlay;

extern const int tab_x[TAB_COUNT + 1];

typedef struct {
    // what the browser shows
    Tab tab;
    int artist;                 // artist group filter, -1 = none
    uint32_t album;             // album filter, LIB_NONE = none
    char folder[512];           // folder tab: relative path, "" = music root
    Item *items;
    int n_items;
    int sel, scroll;            // D-pad selection, first visible row
    uint32_t sel_frame;         // frame the selection last changed (scrolling text restarts)

    // modes and panels
    int tab_anim;               // frames left of the sideways slide between lists
    int tab_dir;                // +1: the new list comes in from the right
    Tab tab_prev;               // where the tab highlight slides from

    bool player_mode;           // where the slide is heading
    int slide;                  // 0 = browser up .. SLIDE_FRAMES = player
    Overlay overlay;
    int menu_item;              // row menu: the item it belongs to
    int menu_sel;               // highlighted entry of a menu (D-pad)
    bool add_mode;
    uint8_t *added;             // bit per track: added while in add mode
    char toast[128];
    uint32_t toast_until;

    // queue panel
    int q_scroll, q_sel;
    int q_drag_from, q_drag_to; // -1 = not dragging

    // stylus drags
    bool scroll_drag;           // dragging the list scroll bar
    bool seek_drag;
    uint32_t seek_preview_ms;

    Settings set;
    bool album_queue;           // the queue is one album

    uint32_t frame;             // drawn frames (30 per second)
    bool idle_hint;             // nothing loaded yet

    // next song thumbnail
    uint32_t thumb_track;
    bool has_thumb;
    uint16_t thumb[32 * 32];

    char lib_error[96];         // why library.idx couldn't be used
} App;

extern App app;

// ui.c
void ui_init(void);
// Keep the list as it is on screen now, to slide it out when the list changes.
void ui_capture_list(void);
// Draw both screens. Returns true if something on them moves by itself
// (scrolling text, the record, the visualizer), so they need redrawing.
bool ui_draw_top(void);
bool ui_draw_bottom(void);

// Text for list rows and the current song (shared by drawing and input).
void ui_item_label(const Item *it, char *buf, int size);
int ui_sort_slot(void);
int ui_sort_option_count(int slot);
const char *ui_sort_option(int slot, int i);

// Row menu box position for an item row (y of its top edge).
int ui_rowmenu_y(int row_y);

// Song currently loaded, or LIB_NONE.
uint32_t app_current_track(void);
int app_panel_offset(void);     // y of the browser panel (0 .. PANEL_DOWN)
int app_slide_p256(void);       // slide progress 0..256, eased

#endif
