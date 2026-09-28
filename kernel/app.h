#ifndef APP_H
#define APP_H

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

/* Desktop services an app compiled in its own unit needs from kernel.c. */
void gui_draw_app_titlebar(const char *title);
int gui_getch_or_click(void);

/* Small helpers every app was writing by hand (kernel/app.c). */
void app_begin(const char *title, unsigned int bg); /* clear to bg, draw the titlebar: every app's first two lines */
int app_utoa(unsigned int v, char *buf);            /* decimal digits into buf (12 bytes is enough), returns the length */

#endif
