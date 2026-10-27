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
/* Here, not in main.c, so boot.c sees APP_BOOT too and runs its loop on QEMU's raspi4b (4.23.3 lost the windows gate that way). */
#ifdef WIN_GATE
#define APP_BOOT "calendar,toroid,notes"   /* the windows gate's three apps (Makefile: wingate) */
#endif
#ifdef HANG_TEST
#define APP_BOOT "calendar,notes"   /* 4.5: slot 0 hangs on its first instruction, slot 1 must keep going (Makefile: hang) */
#endif
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
extern int br_frame;   /* browser_ui.h: the Browser's frame needs drawing (windows.c sets it after a move) */
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
    int min;                              /* 6.5.0: minimized into the dock (the yellow dot): alive, off screen, out of the z order */
};
#define APP_SLOTS 4      /* 4.0: apps that can run at once, one 16 MiB arena each, back to back above USER_BASE */
extern struct app *ap;
extern struct app apps[APP_SLOTS];
unsigned long cnt_hz(void);
unsigned long cnt_now(void);
void key_selftest(void);
#define app_cur (ap->cur)
#define app_len (ap->len)
#define app_code (ap->code)
#define app_img (ap->img)
#define app_lx (ap->lx)
#define app_ly (ap->ly)
#define app_shown (ap->shown)
void grab(const char *t);
int has(const char *w);
extern char seen[256];
#define MENUBAR_H 26             /* docs/ARCHITECTURE.md: GUI_MENUBAR_H, on the 960x540 grid */
extern char _heap_start[];
#define app_ch (ap->ch)
#define app_cw (ap->cw)
int app_dock(const char *name);
void app_kill(const char *how);
#define app_lh (ap->lh)
#define app_lw (ap->lw)
extern int ask_char(unsigned code);
extern int ask_edit(unsigned code);
extern int ask_select(unsigned pane);
void brick_close(void);
void browser_close(void);
int browser_key(unsigned code);
extern int browser_live;
extern int cal_shown;
int cal_today(void);
unsigned long cal_utc(void);
extern int calc_lh;
extern int calc_lw;
extern int calc_lx;
extern int calc_ly;
void calc_open(void);
void clip_put(const char *s, unsigned n);
unsigned clip_take(char *o, unsigned room);
void clock_close(void);
extern int clock_live;
extern long clock_year;
extern int con_ch;
extern int con_cols;
extern int con_cw;
void con_layout(int win_x, int win_y, int win_w, int win_h);
extern int con_rows;
void con_start(void);
extern int con_x;
extern int con_y;
void console_close(void);
void console_frame(void);
void console_open(void);
void cur_show(void);
extern unsigned disp_h;
extern unsigned disp_w;
void dock_activate(int slot);
extern unsigned *dock_band;
void dock_hover(int slot);
extern unsigned *dock_mem;
void dock_paint(void);
extern int dock_sel;
void dock_select(int slot);
int dock_tile(int slot, int shadow);
unsigned fb_color(unsigned c);
extern unsigned fb_pitch;
#define FB_RAM_BASE ((HEAP_END + (2UL << 20) - 1) & ~((2UL << 20) - 1))
#define FB_RAM_MAX (16UL << 20)
extern unsigned *fb_scan;
int fb_setup(void);
extern int fb_swap;
int front_close(void);
int app_cycle(void);
#define GICC(o) REG(GICC_BASE + (o))
extern unsigned long heap_next;
extern unsigned heap_oom;
#ifndef HEAP_SIZE   /* the oomtest builds (Makefile) shrink it so an allocation fails on purpose */
#define HEAP_SIZE (16UL << 20)
#endif
extern int hover_slot;
int in_box(int x, int y, int bx, int by, int bw, int bh);
void *kmalloc(unsigned int n);
int lp_on_arm(int slot);
void m1b_selftest(void);
#define MENUBAR_RULE 0x00BDB8B0  /* docs/ARCHITECTURE.md: MENUBAR_RULE */
extern int mono_adv;
extern int mono_h;
extern int mono_ok;
extern int mono_w;
int off_begin(int x, int y, int w, int h);
void off_end(void);
void off_reserve(int w, int h);
int sc(int v);
void settings_close(void);
int settings_key(unsigned code);
extern int settings_live;
#define SPOT_FINDER (GUI_ICON_COUNT + 6)  /* 3.14.0: the Finder, user/finder.c at EL0; the dock has no Finder tile, so Spotlight and Samantha's open reach it */
int spot_hmax(void);
int spot_key(unsigned code);
#define SPOT_LAST (GUI_ICON_COUNT + 18)   /* 4.24.0: the games end the list (game_names, windows.c); 4.25.0 added Chess, 6.1.0 Movies */
extern int spot_lx;
extern int spot_ly;
const char *spot_name(int slot);
void spot_open(void);
#define SPOT_ROW 20
extern int spot_tree;
extern unsigned *spot_under;
#define SPOT_W 300
extern unsigned term_index;
#define term_p terminals[term_index]
extern int text_draw(int which, const char *s, int x, int baseline, int px10, unsigned fg, unsigned *fb, unsigned pitch, int w, int h);
extern int text_init(void);
extern int text_width(int which, const char *s, int px10);
extern volatile unsigned irq_ticks;
#define TIMER_INTID 30   /* the EL1 physical timer, a private interrupt on every GIC */
#define TREE_H 112
#define TREE_W 220
extern char vectors[];
void wall_fill(int x, int y, int w, int h);
extern int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap);
extern int win_def[3][2];
extern int win_lh;
extern int win_lw;
extern int win_lx;
extern int win_ly;
extern int win_sizing;
#define WT_APP  2
#define WT_CALC 1
#define WT_MAIN 0
#define HEAP_END ((unsigned long)_heap_start + HEAP_SIZE)
extern unsigned *off_buf;
unsigned long off_screen_hash(int x, int y, int w, int h);
extern int calc_press(const char *k);
extern int drag_on;
void win_move_to(int nx, int ny);
extern unsigned long fp_spin(volatile unsigned *t, unsigned want);
void uart_hex(unsigned long v);
void splash_mark(int cx, int cy, int T, unsigned ink, unsigned bg);
#define APPLE_N 48   /* apple_mask.h: the bitten apple after the tribute line, one copy in main.c */
extern const unsigned char apple_mask[APPLE_N * APPLE_N];
#define app_close_at (ap->close_at)
void term_output(int on);
#define BUSY (1u << 3)
extern volatile unsigned mbox[36] __attribute__((aligned(64)));
int mbox_call(void);
#define UART_FR   (UART_BASE + 0x18)
void brick_mouse(int lx, int ly);
void brick_open(void);
void brick_paint(void);
void browser_click(int lx, int ly);
void browser_open(void);
void browser_go(const char *url);   /* 6.32.0 */
int mail_signed_in(void);           /* mail.c */
void browser_paint(int report);
unsigned long civil_days(long y, unsigned m, unsigned d);
void clock_local(unsigned long utc, unsigned *hh, unsigned *mm, unsigned *mon, unsigned *day);
long clock_offset(unsigned long utc);
void clock_open(void);
void clock_paint(void);
int menubar_click(int x, int y);
void set_paint(void);
void settings_click(int lx, int ly);
void settings_open(int sec);
int tree_bar_hit(void);
void tree_click(int lx, int ly);
void tree_hover(int lx, int ly);
void tree_key(unsigned code);
void tree_open(void);
void dcache_clean(void *p, unsigned long n);
void splash_abort(void);
void app_paint(void);
void cal_overlay(unsigned *tile, int pw, int today);
const char *dock_app_name(int slot);
unsigned long heap_mark(void);
void heap_release(unsigned long m);
extern int lp_mode;
#define SPOT_BOOKS (GUI_ICON_COUNT + 4)   /* 2.66: Books, the same: Spotlight only, not on the dock */
#define SPOT_BROWSER (GUI_ICON_COUNT + 3) /* the Browser is in Spotlight only too: the dock's 11 slots stay as they are */
#define SPOT_CALC (GUI_ICON_COUNT + 5)    /* 3.7.5: the Calculator, in Spotlight too (its dock tile is Apps); opens the calc window */
#define SPOT_GAME0 (GUI_ICON_COUNT + 8)   /* 3.19.0: Mines, Toroid, Hamurabi and Keyrate (4.24.0: then Solitaire, Paddle, Bricks, Depths, Snake), the unchanged user/NAME.c at EL0, Spotlight only */
#define SPOT_MUSIC (GUI_ICON_COUNT + 2)   /* M3e: Music is in Spotlight (and the app table) but not on the dock, a decision */
#define SPOT_SETTINGS (GUI_ICON_COUNT + 7) /* 3.13.0: Settings, in Spotlight and the menu bar's status icons; the dock keeps its 11 tiles */
extern const struct user_app user_apps[];
void lp_click(int lx, int ly);
void lp_open(void);
#define app_bh (ap->bh)
#define app_bw (ap->bw)
#define app_kick (ap->kick)
#define app_q (ap->q)
#define app_qh (ap->qh)
#define app_qt (ap->qt)
void app_raise(void); void app_minimize(void);
int tl_state(int id); void traffic_tick(void);   /* windows.c, 6.6.0: 0 the Console, 1 the Calculator, 2 + slot an app */
extern int app_top;
#define app_wbytes (ap->wbytes)
extern unsigned char app_z[APP_SLOTS];
extern unsigned app_zn;
extern void ask_redraw(void);
void calc_cell(int i, int *x, int *y, int *w, int *h);
extern const char *calc_key(int i);
extern int calc_keys(int *cols);
void calc_paint(void);
void calc_raise(void);
void con_fit(void);
extern int con_wx;
extern int con_wy;
extern int cur_frozen;
extern int cur_wanted;
void fb_hold(void);
extern int fb_mute;
void fb_release(void);
extern int fb_tag;
void pane_draw(void);
void tribute_paint(void);
#define UWIN_MAX (0x1000000UL - UP_WIN)   /* the arena's end, 16 MiB, less where the buffer starts */
void wall_minus(int ox, int oy, int ow, int oh, int nx, int ny, int nw, int nh);
void wall_vacate(int lx, int ly, int lw, int lh, int ring_only);
#define UP_WIN   0xD00000UL
#define app_hi (ap->hi)
void app_kill_one(const char *how);
#define app_rs_log (ap->rs_log)
#define app_running (ap->running)
int app_start(const struct user_app *a);
void app_z_drop(unsigned sl);
void app_z_front(unsigned sl);
extern unsigned long l3[];
#define UB (ap->base)
int user_in_range(unsigned long a, unsigned long n);
int user_writable(unsigned long a, unsigned long n);
#define CON_BG 0x00ffffff
void con_draw(unsigned i);
void con_hint(void);
unsigned con_line_off(unsigned line);
unsigned con_nlines(void);
void con_place(int win_x, int win_y, int win_w, int win_h);
int con_rows_total(void);
void con_summary(void);
void con_wipe(void);
extern int cur_on;
extern int tui_face_rows;
void con_tx(char c);
extern int fb_defer;
extern int off_on;
extern int con_to_term;
extern int face_mouth;
extern const char *const dock_names[GUI_ICON_COUNT];   /* crash.c */
extern const char *const game_names[11];
#define DOCK_BAND_LIFT 6   /* logical px of the hover band copy above band_top (crash.c dock_band_y) */
int dock_band_y(void);
int dock_mag_on(void); int dock_mag_zone(int lx, int ly); void dock_magnify(int mx, int slot); void dock_unmagnify(void); void dock_mag_forget(void); void dock_mag_tick(void); void dock_magnify_now(int mx, int slot);   /* crash.c, 6.3.0 and 6.4.0 */               /* windows.c */
#define VGAFONT_FIRST 32                              /* drivers/vgafont.h: the fallback 8x16 font, one copy, in console.c */
#define VGAFONT_LAST 126
extern const unsigned char *const vgafont;
long app_text(unsigned long req);
void app_window_close(void);
long app_window_open(unsigned long info, unsigned long flags);
long app_window_poll(unsigned long ev, unsigned long flags);
unsigned long cal_local(void);
void crash(const struct frame *f);
#define ICON_ART_SIZE 148                             /* kernel/icon_art.h: the app art, one copy, in crash.c */
extern const unsigned char *const *const icon_art;
extern const unsigned int *const icon_art_len;
extern char _start[];
void arena_only(int slot);
extern unsigned long enter_user(unsigned long entry, unsigned long sp, unsigned long arg, unsigned long arg2);
#define UAPP_MAX 0x40000UL  /* the largest image user_load takes, and what user_c.ld asserts */
#define UP_APP    0x10000UL /* M3c: the app window, where user_load puts a program from the app table: code pages, data pages, zeroed pages */
#define UP_ASTACK 0x80000UL /* ...and the app's stack: 16 pages (M3e: minimp3 keeps about 16 KB of scratch there), nothing mapped below them */
#define UP_CODE  0x0000UL   /* EL0 code, read-only and executable */
#define UP_KERN  0x4000UL   /* a page in the same arena that only EL1 may touch */
#define UP_STACK 0x2000UL   /* EL0 stack, one page; sp starts at its top */
extern int user_faulted;
const struct user_app *user_find(const char *name, unsigned n);
void user_init(const char *img, unsigned long len);
unsigned long user_load(const struct user_app *a);
int user_run(const struct user_app *a);
extern char user_start[], user_end[], user_entry_a[], user_entry_b[], user_entry_c[], user_entry_d[], user_entry_e[], user_entry_f[];
extern unsigned long user_status;
void dock_dots_tick(void);   /* crash.c, 4.26.0: the running-app dots under the dock, every main-loop pass */
#endif /* KMAIN_H */
