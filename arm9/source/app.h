// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared state of the player app: what the UI shows and how it is laid out.
// main.c changes it (input, playback), ui.c draws it.

#ifndef DSPLAYER_APP_H__
#define DSPLAYER_APP_H__

#include <stdbool.h>
#include <stdint.h>

#include "colorpick.h"
#include "eqprof.h"
#include "gfx.h"
#include "i18n.h"
#include "library.h"
#include "palette.h"
#include "search.h"
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
#define THEME_PRESETS_Y 162         // the Presets button under them
#define PRESET_X0   4               // the list of presets, over the button
#define PRESET_X1   180
#define PRESET_Y    (THEME_PRESETS_Y - 4 - (20 + 16 * THEME_PRESETS + 4))
#define INFO_Y      24              // song info: first row
#define INFO_RATING_ROW 6           // the stars
#define INFO_STARS_X 72             // values (and the stars) start here at least (ui_info_value_x)
#define INFO_STAR_W 18              // a star and the gap after it
// equalizer panel: the profiles left (Flat, a line, the saved ones), the
// sliders right (4 px per dB), Save and Delete under them
#define EQ_LIST_W   112
#define EQ_OWN_Y    37              // first saved profile
#define EQ_ZERO_Y   86              // 0 dB; +12 at 38, -12 at 134
#define EQ_DB_PX    4
#define EQ_BAND_X(b) (132 + 50 * (b))   // centre of a slider
#define EQ_LINE_Y   151             // the line over the buttons
#define EQ_BTN_Y    153
#define EQ_BTN_H    18
#define EQ_ASK_X    40              // "Delete …?" box
#define EQ_ASK_Y    48
#define EQ_ASK_W    176
// search tab (🔍): the query, the filters, then the results (keyboard down)
// or the keyboard (typing; the results are on the top screen then)
#define FIELD_Y     22
#define CHIPS_Y     44
#define SEARCH_LIST_Y 64
#define SEARCH_ROWS 6
#define KB_Y        64              // the keyboard, when it's up
#define SHEET_Y     18              // the results on the top screen, under the clock
#define SHEET_ROWS  10
#define HINGE       16              // the gap between the screens, for the results moving across

#define APP_NAME    "alya-bangers"
#define APP_VERSION "pre-1.1"
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
#define KB_TICKS    18              // the keyboard up or down (the results move with it), 0.3 s
#define SHEET_TICKS 18              // results come up on the top screen while typing, 0.3 s
#define IM_TICKS    18              // another input method pushes the keyboard out, 0.3 s
#define ACC_TICKS   8               // a held letter's other forms pop up

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
       BTN_SHUFFLE, BTN_PREV, BTN_PPLAY, BTN_PNEXT, BTN_REPEAT, BTN_INFO, BTN_UP, BTN_QUEUE, BTN_EQ,
       BTN_EQ_SAVE, BTN_EQ_DELETE };

typedef enum { TAB_ARTIST, TAB_ALBUM, TAB_TITLE, TAB_FOLDER, TAB_SEARCH, TAB_COUNT } Tab;
typedef enum { OV_NONE, OV_SORT, OV_ROWMENU, OV_SETTINGS, OV_INFO, OV_QUEUE, OV_FILTER, OV_IM, OV_EQ,
               OV_EQ_NAME } Overlay;

// Settings: a top page leading to pages of settings.
enum { PAGE_TOP, PAGE_PLAYBACK, PAGE_SOUND, PAGE_BROWSING, PAGE_PLAYER, PAGE_THEME, PAGE_REGIONAL,
       PAGE_ABOUT, PAGE_COUNT };
enum { SET_SCROLL_ALL, SET_CASE, SET_PAUSE_LID, SET_LR_LID, SET_RG, SET_TIME_RIGHT, SET_COVER_BG,
       SET_SHUFFLE_W, SET_LANG, SET_KEYBOARD, SET_AUTOPLAY, SET_HIGHCUT, SET_DITHER, SET_CLOCK, SET_DATE };
// switch, value (tap: the next one), › another page, a slider (a taller row)
enum { SI_TOGGLE, SI_VALUE, SI_PAGE, SI_SLIDER };
#define SET_SLIDER_H 64
#define SLIDER_X0   16              // its scale: the first step, the last (Off)
#define SLIDER_X1   240
#define SLIDER_DY   34              // its line in the row
typedef struct {
    uint16_t label;         // S_... (i18n.h)
    uint8_t kind;
    uint8_t id;             // SET_... or, for SI_PAGE, PAGE_...
    uint32_t icon;          // in front of the label (0: none); the way to Language without reading
} SetItem;
#define ICON_PLAYBACK 0x1F50A       // 🔊
#define ICON_SOUND    0x1F3A7       // 🎧
#define ICON_BROWSING 0xE005        // a list (builtin_font.c)
#define ICON_PLAYER   0xE006        // a record
#define ICON_THEME    0x1F3A8       // 🎨
#define ICON_REGIONAL 0x1F310       // 🌐, also Language
#define ICON_ABOUT    0x24D8        // ⓘ
#define ICON_KEYBOARD 0x1F5AE       // 🖮
#define ICON_CLOCK    0x1F552       // 🕒
#define ICON_DATE     0x1F4C5       // 📅
#define ICON_EQ       0xE007        // three sliders (builtin_font.c)

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

    // equalizer panel (the gains and the profile are in set: eq_gain, eq_profile)
    bool eq_sliders;            // the D-pad moves the sliders, not the profiles (X switches)
    int eq_band;                // the slider the D-pad moves
    int eq_drag_band;           // the slider under the stylus, -1: none
    int eq_drag_from, eq_drag_to;   // a saved profile being moved (its place), -1: none
    bool eq_ask;                // "Delete …?" is open
    int eq_ask_sel;             // its highlighted answer: 0 delete, 1 keep
    char eq_name[EQP_NAME];     // a new profile's name, being typed

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
    bool slider_drag;           // settings: the high cut's slider under the stylus
    bool presets_open;          // the list of preset themes is open
    int presets_t;              // it rises out of its button: 0 .. OV_TICKS
    bool album_queue;           // the queue is one album

    uint32_t frame;             // time in 1/30 s (scrolling text, the record)
    bool idle_hint;             // nothing loaded yet

    // next song thumbnail
    uint32_t thumb_track;
    bool has_thumb;
    uint16_t thumb[32 * 32];

    int lib_error;              // why library.idx couldn't be used (LIB_ERR_..., library.h)

    // search (search.h); the results are app.items
    char query[128];
    int im;                     // input method, IM_...
    bool kb_num;                // the 123 keys
    bool shift;                 // the next letter in upper case
    bool kb_up;                 // typing: the keyboard is wanted up
    int kb_t;                   // 0 = down .. KB_TICKS = up; the results move with it
    int sheet_t;                // results on the top screen: 0 = out of sight .. SHEET_TICKS
    bool sel_on;                // the D-pad highlight shows in the results while typing
    int key_down;               // the key under the stylus, -1: none
    uint32_t key_since;         // since when (holding: other forms, ⌫ repeats)
    uint32_t text_t0;           // the song's texts scroll from here (frames): set when a song starts
    int key_flash;              // a typed key lights up until key_flash_until (frames)
    uint32_t key_flash_until;
    int im_from, im_t;          // input switch: the old layout leaves to the left (im_t counts down)
    int acc_key;                // the key held for its other forms, -1: none
    int acc_t;                  // their row grows: 0 .. ACC_TICKS
    int acc_sel;                // the form under the stylus, -1: none
    int acc_vis;                // x of its highlight (glides)
    SearchFilter sf;
    SearchMark *marks;          // per result row
    // filter menu: the filters, Tags (the list of tags), a filter's choices.
    // Two pages show while one pushes the other out: fbase on the left, fkind
    // on the right; fpage is the one in front.
    int fpage;                  // -1 the filters, F_TAGS, else the F_... whose choices show
    int fkind;                  // the page on the right (its choices stay while sliding back)
    int fbase;                  // the page on the left: -1 the filters, or F_TAGS
    int fpage_t;                // the right page pushes in: 0 .. TAB_TICKS
    int fscroll;                // first choice shown
    int fkey;                   // a tag chip's page: the tag key whose values show
    SearchOpt *fopts;
    int n_fopts;
    SearchOpt *tkeys;           // the Tags page: the tags, with how many songs
    int n_tkeys;
    int tscroll;
    int chip_w[F_COUNT];        // chips grow in and shrink away: their width now
} App;

extern App app;

// The browser's list: in the search tab it starts under the query and the
// filters, and while typing it's on the top screen (more rows).
static inline int list_y(void)
{
    return app.tab == TAB_SEARCH ? SEARCH_LIST_Y : LIST_Y;
}
static inline int list_rows(void)
{
    return app.tab != TAB_SEARCH ? ROWS : (app.kb_up ? SHEET_ROWS : SEARCH_ROWS);
}

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
// Where a settings row starts, how tall it is, the row at y (-1: none).
int ui_set_row_y(int page, int i);
int ui_set_row_h(int page, int i);
int ui_set_row_at(int page, int y);
int ui_info_value_x(void);
int ui_player_artist_at(int x, int y);     // which artist name, -1: none
int ui_cover_bg_contrast(const uint16_t *cover);    // for the tests
const SetItem *ui_set_item(int page, int i);
int app_song_rating(uint32_t t);        // stars shown: given on the DS, else from the tags, else 0
uint32_t app_song_plays(uint32_t t);    // plays from the tags + plays on the DS
int app_panel_offset(void);     // y of the browser panel (0 .. PANEL_DOWN)
int app_slide_p256(void);       // slide progress 0..256, eased
// search: the chip of a filter at x, y (F_COUNT: "+ Filter"; on_x: its ×), -1: none
int ui_chip_at(int x, int y, bool *on_x);
int ui_chip_full_w(int kind);   // its width when it's in
// the filter menu: the y of its first row
#define FMENU_Y     64
#define FMENU_ROWS  6
#define FOPTS_MAX   1024     // choices of one filter (the most common values of a tag)
// the input methods menu
int ui_im_menu_y(void);
// equalizer: a profile's gains (0: Flat), whether the sliders differ from the
// chosen profile's, where Save and Delete are
const int8_t *app_eq_profile_gain(int p);
bool app_eq_edited(void);
void ui_eq_buttons(int *save_x, int *save_w, int *del_x, int *del_w);

#endif
