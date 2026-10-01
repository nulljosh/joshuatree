#ifndef APP_H
#define APP_H

/* Desktop background every app clears to before drawing its own content. */
#define GUI_BG 0x00FAF8F6

/* Keys get_key_or_click(_until) can hand back on top of a plain ASCII
   char: an arrow, enter, esc, a click standing in for a key (a phone
   visitor has no keyboard), or a wheel tick. Shared with kernel.c's own
   copy of the same loop, so an app moved to its own unit reads the exact
   values the desktop already returns. */
#define KEY_UP         256
#define KEY_DOWN       257
#define KEY_ENTER      258
#define KEY_ESC        259
#define KEY_LEFT       261
#define KEY_RIGHT      262
#define KEY_CLICK      260
#define KEY_WHEEL_UP   300
#define KEY_WHEEL_DOWN 301
#define KEY_COPY       302
#define KEY_CUT        303
#define KEY_PASTE      304
#define KEY_HOME       305   /* ring-3 delivery paths only (gui_poll_event, compositor push) */
#define KEY_END        306
#define KEY_DELETE     307
#define KEY_SAVE       308   /* Ctrl+S */

/* One app, one entry. Every place the desktop used to switch on an app's
   index (launch, dock glyph, tile color, label, multiwindow content and
   keys) now reads this struct from the single APPS[] table in kernel.c.
   A hook the desktop never calls for an app stays NULL. */
struct app {
    const char *name;                                    /* dock label, window title, chat's open_app match */
    unsigned int color;                                  /* tile background behind the glyph */
    void (*icon)(int cx, int cy, int size, unsigned int bg); /* primitive glyph, drawn when no authored art covers it */
    void (*open)(void);                                  /* runs the app full-window until it closes */
    void (*draw)(void);                                  /* multiwindow content repaint; NULL = single-window only */
    int (*key)(int k);                                   /* multiwindow keystroke, returns 1 to close the window */
};

/* The single APPS[] table, defined in kernel.c; non-static so dock_draw.c
   can read a slot's name/color for the dock label and glyph fallback. */
extern const struct app APPS[];

/* Desktop services an app compiled in its own unit needs from kernel.c. */
void gui_draw_app_titlebar(const char *title);
int gui_getch_or_click(void);
int get_key_or_click(void);                    /* blocks for a key or a click; see KEY_* above */
int get_key_or_click_until(unsigned int deadline); /* same, but 0 (never a real key) at the deadline tick; 0 deadline blocks */
int gui_app_dy(void);                          /* vertical shift for a dock window's content, 0 full screen */
char *wx_put_int(char *o, int v);              /* decimal digits, no leading zero handling beyond app_utoa's */
extern int app_cursor_x, app_cursor_y;         /* pointer, screen coordinates */
extern int app_view_x, app_view_y;             /* this app's viewport origin, screen coordinates */

/* Small helpers every app was writing by hand (kernel/app.c). */
void app_begin(const char *title, unsigned int bg); /* clear to bg, draw the titlebar: every app's first two lines */
int app_utoa(unsigned int v, char *buf);            /* decimal digits into buf (12 bytes is enough), returns the length */

#endif
