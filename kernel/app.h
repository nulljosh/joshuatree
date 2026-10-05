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
#define KEY_F2         309   /* F2 make (push-to-talk down), ring-3 delivery paths only */
#define KEY_F2_UP      310   /* F2 break (push-to-talk released) */
#define KEY_SLEFT      311   /* Shift+arrow and Ctrl+A: text selection in a ring-3 editor, both delivery paths */
#define KEY_SRIGHT     312
#define KEY_SUP        313
#define KEY_SDOWN      314
#define KEY_SELALL     315   /* Ctrl+A */
/* 2.11.0: the Terminal's multiplexer keys. A ring-3 window gets Ctrl+T/W/D/E/O, Ctrl+1..9, Ctrl+Left/Right
   and Ctrl+Tab as these codes instead of a bare letter, digit or tab (the app switcher still owns Ctrl+Tab
   while two windows are open). KEY_CTL_1 + n is Ctrl+(n+1). */
#define KEY_CTL_TAB    320
#define KEY_CTL_LEFT   321
#define KEY_CTL_RIGHT  322
#define KEY_CTL_T      323
#define KEY_CTL_W      324
#define KEY_CTL_D      325
#define KEY_CTL_E      326
#define KEY_CTL_O      327
#define KEY_CTL_1      330
/* Scancode (make, 0x7F masked) under Ctrl to its KEY_CTL_* code, 0 when it is not one of them. */
static inline int key_ctl_code(int sc) {
    switch (sc) {
    case 0x0F: return KEY_CTL_TAB;
    case 0x14: return KEY_CTL_T;
    case 0x11: return KEY_CTL_W;
    case 0x20: return KEY_CTL_D;
    case 0x12: return KEY_CTL_E;
    case 0x18: return KEY_CTL_O;
    default: return sc >= 0x02 && sc <= 0x0A ? KEY_CTL_1 + (sc - 0x02) : 0;
    }
}

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
