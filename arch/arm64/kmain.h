#ifndef KMAIN_H
#define KMAIN_H
/* The Pi kernel core's shared names: main.c was one 4,000-line file, and the parts cut out of it (boot.c, ...) reach
   its state and helpers through these. main.c includes this at its end, so every line here is checked against the
   definition it declares. Built only into main.o (MAINSRC), never by another file. */
/* The platform's addresses: the Pi 4's peripherals, or QEMU virt's. */
#ifdef PI_BUILD
#define UART_BASE 0xFE201000UL
#define GPIO_BASE 0xFE200000UL
#define AUX_ENABLES 0xFE215004UL   /* the mini UART (UART1): bit 0 of AUX_ENABLES turns it on */
#define MU_IO       0xFE215040UL
#define MU_IER      0xFE215044UL
#define MU_LCR      0xFE21504CUL
#define MU_MCR      0xFE215050UL
#define MU_LSR      0xFE215054UL
#define MU_CNTL     0xFE215060UL
#define MU_BAUD     0xFE215068UL
#define GICD_BASE 0xFF841000UL   /* the Pi 4's GIC-400, the same GICv2 programming model as QEMU's virt */
#define GICC_BASE 0xFF842000UL
#else
#define UART_BASE 0x09000000UL
#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL
#endif
#define REG(a) (*(volatile unsigned int *)(a))
#include "bt.h"
#include "audio.h"
#include "accounts.h"
#include "login.h"
#include "sudo.h"
#include "emmc.h"
#include "../../drivers/window.h"
#include "../../drivers/png.h"
#include "../../kernel/dock_geom.h"
#include "../../kernel/gui_paint.h"
#if defined(PI_BUILD) && !defined(QUIET_BOOT)
#define QUIET_BOOT   /* the desktop comes up bare: the boot log is kept but its window is not drawn (a dock click or the Terminal's Esc brings it up) */
#endif
#ifdef QUIET_BOOT
#define SPLASH(n, name) splash_step(n, name)
#else
#define SPLASH(n, name) ((void)0)
#endif
#define LOG_MAX 16384
/* Two windows share the console renderer, each with its own text: the Console (the boot log, logs only, no input) and
   the Terminal (the ask> command line, docs/GUIDE.md). cp is the one in front; the other only keeps its text. */
struct pane {
    char log[LOG_MAX];
    unsigned len, col, row;
    unsigned anchor;          /* offset in the log of the line at the top of the window while it follows the newest output */
    int scrolled;             /* the keyboard moved the view back: new output is only logged until End */
    unsigned vtop, vlast;     /* while scrolled: the first line shown (0 based) and the last (1 based) */
};
extern struct pane con_p, terminals[2], *cp;
struct menu_notice;
extern struct menu_notice notice;
int notice_up(void);
extern int alt_held;
extern int app_front;
void app_push(unsigned kind, int a, int b);
void app_tick(void);
int apps_live(void);
extern int brick_live;
void calc_close(void);
void calc_did(int ran);
extern int calc_live, calc_front;
int con_key(unsigned code, unsigned value);
extern int con_live;
extern int con_quiet;
void console_putc(char c);
void cur_hide(void);
int cur_hold(void);
void cur_release(int held);
extern int desktop_deferred;
void drag_end(void);
extern unsigned int *fb;
void fb_desktop(void);
void fb_diag(void);
void fb_flush(int x, int y, int w, int h);
extern unsigned fb_w, fb_h;
void fb_rect(int x, int y, int w, int h, unsigned c);
#define GICD(o) REG(GICD_BASE + (o))
void gui_icon_text(const char *s, int lx, int ly, int face, int mul, unsigned int fg);
int gui_icon_text_w(const char *s, int face, int mul);
void gui_text(const char *s, int x, int y, unsigned int fg);
int gui_text_width(const char *s);
void kd_key(unsigned code);
void keydbg_tick(void);
extern int login_live;
void m1_selftest(void);
void menu_mark_paint(int cx, int cy, int T, unsigned ink);
void menubar_tick(void);
void menubar_wifi(int state);
extern unsigned mouse_x, mouse_y, mouse_moved;
void pane_open(struct pane *p);
void pointer_click(void);
void pointer_moved(void);
int sg(int v);
void spot_close(void);
int term_front(void);
extern int text_ok;
extern unsigned long timer_step;
void tree_close_all(void);
void uart_dec(unsigned v);
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int ui_key(unsigned code);
void user_demo(void);
void wall_repaint(void);
extern int wheel_acc;
unsigned int window_scale(void);
int ask_pending(void);
int calc_type(int ch);
unsigned wall_get(void);
extern unsigned int boot_el;
int brick_key(unsigned code, unsigned value);
void brick_tick(void);
void browser_wheel(int v);
extern int ctrl_held;
extern int gui_held;
int set_secret(void);
extern void settings_load(void);
extern int spot_live;
void menubar_notify(const char *message);
unsigned notice_now(void);
int screen_blank(unsigned blank);
void splash_creep(void);
void splash_finish(void);
extern int splash_on;
void splash_step(int n, const char *name);
struct frame { unsigned long x[31]; unsigned long elr, esr; };   /* what vectors.S saved */
struct user_app { const char *name, *start, *end; };   /* userapps.S: one row per program, ended by a zero name */
#define APP_EVQ 16
struct app {   /* 4.0: one of APP_SLOTS apps, each in its own arena; the old app_* names below are macros onto `ap`, the slot being run or shown */
    const struct user_app *cur;
    struct frame frame;
    unsigned long fp[66];                 /* M3e: its q0-q31, FPCR and FPSR where it parked (vectors.S's save_fp layout) */
    unsigned long sp, last, wbytes;
    int hi;                               /* 3.9.0: the window buffer is device size (scale 2, the app asked for it) */
    unsigned long brk;                    /* M3e: the heap top JT_SYS_BRK set, 0 while it has none */
    unsigned long close_at;               /* the red dot was clicked: past this count, an app that did not close itself is ended */
    int running, parked, kick;            /* inside resume_user now; it yielded rather than ended; run it on the next pass */
    unsigned runs;                        /* times the desktop's loop resumed it, said when it ends */
    struct { unsigned kind; int a, b; } q[APP_EVQ];
    unsigned qh, qt;
    unsigned long len, code, img;         /* the loaded app: bytes of window mapped, how many of those are code, and the image's own length */
    int lx, ly, lw, lh;                   /* the frame on the logical grid, set in fb_init */
    unsigned cw, ch;                      /* the content area, what the frame holds */
    unsigned bw, bh;                      /* the app's buffer, pixel for pixel: the content's size as of its last JT_SYS_WINDOW_OPEN */
    int rs_log;                           /* a resized buffer is waiting for the app's first frame at the new size */
    unsigned presents;                    /* frames it has handed to JT_SYS_WINDOW_POLL, for the windows gate */
    int shown;                            /* the app has presented a frame since its window opened */
    unsigned long base;                   /* its arena: USER_BASE + slot * ARENA_SIZE (apps are position independent, so each loads where its arena is) */
    unsigned l3off, slot;                 /* its first l3 entry; its index in apps */
};
#define APP_SLOTS 4      /* 4.0: apps that can run at once, one 16 MiB arena each, back to back above USER_BASE */
extern struct app *ap;
extern struct app apps[APP_SLOTS];
unsigned long cnt_hz(void);
unsigned long cnt_now(void);
void key_selftest(void);
#define app_cur (ap->cur)
#define app_lx (ap->lx)
#define app_ly (ap->ly)
#define app_shown (ap->shown)
void grab(const char *t);
int has(const char *w);
extern char seen[256];
#define MENUBAR_H 26             /* docs/ARCHITECTURE.md: GUI_MENUBAR_H, on the 960x540 grid */
#endif /* KMAIN_H */
