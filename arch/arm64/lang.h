#pragma once
/* 9.2.0, languages: the words the desktop draws, in twelve languages (lang.c). Only drawn text goes through tr(); the
   UART stays English, so the checks read the same lines in every language. */
enum {
    S_DOCK0 = 0,                     /* the twelve dock names, in dock_names[] order (crash.c) */
    S_SEC0 = 12,                     /* the nine Settings sections, in set_names[] order (settings_ui.h) */
    S_SETTINGS = 21, S_READER_ON, S_APP, S_OF, S_RETURN, S_LANG_HINT, S_SELF, S_COUNT
};
#define LANG_COUNT 12
extern int lang_cur;                  /* 0 English, in the order of the Settings picker */
const char *tr(int id);               /* the current language's word, English where it has none */
const char *lang_name(int lang);      /* each language in its own words, for the picker */
const char *lang_english(int lang);   /* "Spanish": what the relay is told to answer in */
const char *lang_code(int lang);      /* "en", "es", ...: LANG.TXT and the relay */
void lang_set(int lang);              /* picks, saves JT/LANG.TXT, repaints */
void lang_load(void);                 /* after sd_mount, with settings_load */
