// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared state of the player app: what the UI shows and how it is laid out.
// main.c changes it (input, playback), ui.c draws it.

#ifndef DSPLAYER_APP_H__
#define DSPLAYER_APP_H__

#include <stdbool.h>
#include <stdint.h>

#include "colorpick.h"
#include "gfx.h"
#include "library.h"
#include "palette.h"
#include "state.h"

// ---- colors ----
// the theme's colours (see palette.h)
#define C_BG        pal[P_BG]
#define C_PANEL     pal[P_PANEL]
#define C_ZEBRA     pal[P_ZEBRA]
#define C_LINE      pal[P_LINE]
#define C_TEXT      pal[P_TEXT]
#define C_DIM       pal[P_DIM]
#define C_FAINT     pal[P_FAINT]
#define C_ACC       pal[P_ACC]
#define C_SEL       pal[P_SEL]
#define C_ACC_DIM   pal[P_ACC_DIM]
#define C_GLARE     C_ACC               // light on the record's grooves and rim, from the top left

// ---- bottom screen layout ----
#define TAB_H       20
#define ROW_H       16
#define ROWS        9
#define LIST_Y      20
#define BAR_Y       164
#define SCROLL_X    244             // scroll bar column, 12 px wide
#define PANEL_DOWN  172             // panel offset in player mode: only the tabs show
#define SET_Y       24              // settings: first row
#define SET_H       24              // row pitch
#define THEME_CHIP_X 4              // theme page: the colours to choose (accent, background, text)
#define THEME_CHIP_W 84
#define THEME_CHIP_Y 28
#define THEME_CHIP_H 22
#define THEME_CHIP_PITCH 28
#define THEME_RESET_Y 162           // the Reset button under them
#define INFO_Y      24              // song info: first row
#define INFO_RATING_ROW 6           // the stars
#define INFO_STARS_X 72             // values (and the stars) start here
#define INFO_STAR_W 18              // a star and the gap after it

#define APP_NAME    "alya-bangers"
#define APP_VERSION "0.2"
#define APP_URL     "github.com/alya-can-eat"
// Animations run on real time (vblanks, 60 per second), so they take as long
// on the DS as planned even when it draws fewer frames.
#define SLIDE_TICKS 24              // browser <-> player, 0.4 s
#define TAB_TICKS   15              // sideways list slide, 0.25 s
#define OV_TICKS    15              // menus slide up / down, 0.25 s
#define PP_TICKS    10              // play <-> pause morph
#define KNOB_TICKS  8               // seek knob grows while dragged
#define LID_OUT     6               // lid closing: fade to black first
#define LID_IN      18              // lid opening: fade in from black

// Song change on the top screen (player layout). Forward: the cover leaves to
// the left over its record, the next cover grows from the Next spot, its
// record rolls out. Back: the same, played backwards. Jump: the chosen song
// first slides over the Next spot, then forward.
// In browse mode the covers are a stack in list order: a song further up
// is put on top (slides in from the left), for one further down the old
// cover is lifted off to the left. From an unrelated list: put on.
typedef enum { SA_NONE, SA_FORWARD, SA_BACK, SA_JUMP, SA_PUT, SA_LIFT } SongAnim;
#define SA_PLAYER(k) ((k) >= SA_FORWARD && (k) <= SA_JUMP)
#define SA_STACK(k)  ((k) == SA_PUT || (k) == SA_LIFT)
#define SA_STACK_TICKS 18           // a cover put on or lifted off, 0.3 s
#define SA_PRE_TICKS 15             // jump: the chosen song slides over "Next"
#define SA_OUT_TICKS 16             // the old cover leaves to the left
// The new cover only starts once the old one has cleared the middle (at 80 %
// of its way out its right edge is left of x = 64), so they never overlap.
#define SA_FLY_START ((SA_OUT_TICKS * 4 + 4) / 5)
#define SA_FLY_TICKS 18             // the new cover grows into the middle
#define SA_B_TICKS  18              // then its record rolls out
#define SA_TICKS    (SA_FLY_START + SA_FLY_TICKS + SA_B_TICKS)

// Buttons that light up when tapped.
enum { BTN_NONE, BTN_PLAY, BTN_NEXT, BTN_ADD, BTN_DOWN, BTN_SORT, BTN_BRIGHT, BTN_SETTINGS,
       BTN_SHUFFLE, BTN_PREV, BTN_PPLAY, BTN_PNEXT, BTN_REPEAT, BTN_INFO, BTN_UP, BTN_QUEUE };

typedef enum { TAB_ARTIST, TAB_ALBUM, TAB_TITLE, TAB_FOLDER, TAB_SEARCH, TAB_COUNT } Tab;
typedef enum { OV_NONE, OV_SORT, OV_ROWMENU, OV_SETTINGS, OV_INFO, OV_QUEUE } Overlay;

// Settings: a top page leading to pages of settings.
enum { PAGE_TOP, PAGE_PLAYBACK, PAGE_BROWSING, PAGE_PLAYER, PAGE_THEME, PAGE_REGIONAL, PAGE_ABOUT,
       PAGE_COUNT };
enum { SET_SCROLL_ALL, SET_CASE, SET_PAUSE_LID, SET_LR_LID, SET_RG, SET_TIME_RIGHT, SET_COVER_BG,
       SET_SHUFFLE_W };
enum { SI_TOGGLE, SI_VALUE, SI_PAGE };      // switch, value (tap: the next one), › another page
typedef struct {
    const char *label;
    uint8_t kind;
    uint8_t id;             // SET_... or, for SI_PAGE, PAGE_...
} SetItem;

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
    int tab_anim;               // vblanks left of the sideways slide between lists
    int tab_dir;                // +1: the new list comes in from the right
    Tab tab_prev;               // where the tab highlight slides from

    bool player_mode;           // where the slide is heading
    int slide;                  // 0 = browser up .. SLIDE_TICKS = player
    Overlay overlay;            // the open menu (gets the input)
    Overlay ov_vis;             // the menu on screen: stays while it slides down
    int ov_t;                   // 0 = hidden .. OV_TICKS = in place
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

    // song change animation (roles in app.h's SongAnim comment)
    int sa_kind;
    int sa_t;                   // vblanks since it started
    uint32_t sa_a, sa_b, sa_c, sa_n;    // leaves left / grows from Next / then in Next / old Next
    uint16_t *sa_cover;         // 128x128: the cover that was showing before the change
    uint16_t sa_thumb[32 * 32]; // the Next picture before the change
    bool sa_has_thumb;
    int pp_t;                   // play/pause shape: 0 = play .. PP_TICKS = pause
    int knob_t;                 // seek knob size while dragging
    int press_id;               // button lit up by the stylus (BTN_*)
    uint32_t press_until;       // stays lit until this frame after release
    bool press_down;            // the stylus is still on it
    int sel_vis;                // y of the row highlight in the list (glides to the selection)
    int menu_vis;               // the same for the highlight in a menu, from its first entry
    int q_vis;                  // and in the queue
    int vinyl_rot;              // degrees x 3
    int vinyl_speed;            // 0..256 of 33 1/3 rpm (spins up and down)
    int set_page;               // settings page wanted: PAGE_TOP or one of its pages
    int set_shown;              // the page sliding in or out (kept while it slides back)
    int page_t;                 // 0 = the top page .. TAB_TICKS = set_shown (slides in from the right)
    int set_back_sel;           // the top page's highlighted row, for coming back

    Settings set;
    Theme theme;
    int theme_sel;              // theme page: 0 accent, 1 background, 2 text
    Hsv theme_hsv;              // that colour, as the picker shows it
    int theme_drag;             // CP_RING or CP_TRIANGLE while the stylus drags
    bool album_queue;           // the queue is one album

    uint32_t frame;             // time in 1/30 s (scrolling text, the record)
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
int ui_set_count(int page);
int ui_cover_bg_contrast(const uint16_t *cover);    // for the tests
const SetItem *ui_set_item(int page, int i);
int app_song_rating(uint32_t t);        // stars shown: given on the DS, else from the tags, else 0
uint32_t app_song_plays(uint32_t t);    // plays from the tags + plays on the DS
int app_panel_offset(void);     // y of the browser panel (0 .. PANEL_DOWN)
int app_slide_p256(void);       // slide progress 0..256, eased

#endif
