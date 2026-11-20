// SPDX-License-Identifier: GPL-3.0-or-later
//
// The interface in English and German: every text the player shows comes
// from one table, txt(S_...) gives it in the language chosen in Settings.

#ifndef DSPLAYER_I18N_H__
#define DSPLAYER_I18N_H__

#include <stdint.h>

enum { LANG_EN, LANG_DE, LANG_COUNT };

typedef enum {
    S_NONE,
    // browser
    S_NO_ARTIST, S_TAB_ARTIST, S_TAB_ALBUM, S_TAB_TITLE,
    S_SORT_NAME_UP, S_SORT_NAME_DOWN, S_SORT_YEAR_UP, S_SORT_YEAR_DOWN, S_SORT_TRACK,
    S_SORT_ARTISTS, S_SORT_ALBUMS, S_SORT_SONGS, S_SORT_FOLDERS,
    S_PLAY_NOW, S_PLAY_NEXT, S_ADD_QUEUE,
    S_LIB_MISSING, S_LIB_DAMAGED, S_LIB_TOO_BIG, S_LIB_VERSION, S_RUN_DSCONV,
    S_SEARCH_LATER,
    // top screen
    S_SUN, S_MON, S_TUE, S_WED, S_THU, S_FRI, S_SAT,
    S_DROPS,
    S_NOTHING_1, S_NOTHING_2, S_NOTHING_PLAYING, S_NEXT,
    // song info and queue
    S_SONG_INFO,
    S_INFO_TITLE, S_INFO_ARTIST, S_INFO_ALBUM, S_INFO_TRACK, S_INFO_YEAR, S_INFO_GENRE,
    S_INFO_RATING, S_INFO_PLAYS, S_INFO_FORMAT,
    S_QUEUE_ONE, S_QUEUE_N,
    // toasts
    S_CANT_PLAY, S_QUEUE_FULL, S_PLAYS_NEXT, S_ADDED, S_N_PLAY_NEXT, S_N_ADDED,
    // why a song can't be played (after S_CANT_PLAY)
    S_ERR_OPEN, S_ERR_SHORT, S_ERR_NOT_DSA, S_ERR_HEADER, S_ERR_VERSION, S_ERR_AUDIO,
    S_ERR_INCONSISTENT, S_ERR_BLOCK,
    // settings
    S_SETTINGS, S_PLAYBACK, S_BROWSING, S_PLAYER, S_THEME, S_REGIONAL, S_ABOUT,
    S_REPLAYGAIN, S_PAUSE_LID, S_LR_LID, S_CASE, S_SCROLL_ALL,
    S_TIME_RIGHT, S_COVER_BG, S_SHUFFLE_W, S_LANGUAGE, S_KEYBOARD,
    S_OFF, S_RG_TRACK, S_RG_ALBUM, S_AUTO, S_LEFT, S_LENGTH,
    S_LATER_PAGE, S_LATER,
    S_EDITION, S_TAGLINE, S_LICENCE, S_FREE, S_GLYPHS,
    S_ACCENT, S_BACKGROUND, S_TEXT, S_RESET,
    // can't start
    S_PRESS_START, S_NO_SD, S_DLDI, S_NO_MUSIC, S_MAKE_MUSIC,
    S_COUNT
} StrId;

extern uint8_t i18n_lang;          // LANG_EN, LANG_DE

const char *txt(StrId id);
const char *txt_in(int lang, StrId id);
const char *lang_name(int lang);   // each in its own language: "English", "Deutsch"
StrId i18n_missing(int lang);      // the first text the language lacks (S_NONE: none)

#endif
