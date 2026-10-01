#ifndef RING3APP_H
#define RING3APP_H
/* The apps that have left the kernel. Each dock entry runs user/<app>.c
   as a real ring-3 process through one table-driven launcher (RING3_APPS
   in ring3app.c). The launcher is also the supervisor: it waits for the
   process, and whether the program exited on its own or was reaped by
   idt.c's ring-3 fault path, it tears the window down and hands the
   desktop back. Keyrate went first (1.7.7), Toroid second (1.7.11),
   Calculator third (1.7.12), Quotes fourth (1.7.14), Bookrank fifth (2.0), Homeqi sixth (1.8.22), Lexly seventh (1.9.1), Plan eighth (1.9.2), Fieldbook ninth (1.9.3), Clock tenth (1.9.4), Portfolio eleventh (1.9.5), Activity twelfth (1.9.6), Contacts thirteenth (1.9.7), Sparkjar fourteenth (1.9.8), Reminders fifteenth (1.9.9), Curbfind sixteenth (1.9.11), Calendar seventeenth (1.9.12), Search eighteenth (1.9.13). */
void keyrate_ring3_open(void);
void toroid_ring3_open(void);
void calculator_ring3_open(void);
void quotestreak_ring3_open(void);
void bookrank_ring3_open(void);
void homeqi_ring3_open(void);
void lexly_ring3_open(void);
void plan_ring3_open(void);
void fieldbook_ring3_open(void);
void clock_ring3_open(void);
void portfolio_ring3_open(void);
void activity_ring3_open(void);
void contacts_ring3_open(void);
void sparkjar_ring3_open(void);
void reminders_ring3_open(void);
void curbfind_ring3_open(void);
void calendar_ring3_open(void);
void search_ring3_open(void);
void epiphany_ring3_open(void);

/* `open=keyrate` / `open=toroid` / `open=calc` / `open=quote` / `open=bookr` / `open=homeqi` / `open=lexly` / `open=plan` / `open=field` / `open=clock` / `open=portf` / `open=activ` / `open=remi` / `open=curb` / `open=cale` / `open=sear` / `open=epip`
   launches that app from the dock path the moment the desktop is up, so
   tools/checks/ring3app-check.py, ring3toroid-check.py, ring3calc-check.py,
   ring3quotes-check.py, ring3bookrank-check.py, ring3homeqi-check.py, ring3lexly-check.py, ring3plan-check.py, ring3fieldbook-check.py, ring3clock-check.py, ring3portfolio-check.py, ring3activity-check.py, ring3contacts-check.py ring3sparkjar-check.py, ring3reminders-check.py, ring3curbfind-check.py, ring3calendar-check.py, ring3search-check.py and ring3epiphany-check.py can drive a ring-3 app
   without locating its tile in the Apps folder first.
   ring3app_autoopen_arm: kmain calls this with the boot command line;
   remembers the APPS slot if it says `open=keyrate`, `open=toroid`,
   `open=calc`, `open=quote`, `open=bookr`, `open=homeqi`, `open=lexly`, `open=plan`, `open=field`, `open=clock`, `open=portf`, `open=activ`, `open=remi`, `open=sear` or `open=epip`.
   ring3app_autoopen_run: gui_run calls this once, right after the first
   desktop paint; if armed, launches that app from the dock path (mx, my
   are the cursor position to restore after) and disarms. No-op otherwise. */
void ring3app_autoopen_arm(const char *cmdline);
void ring3app_autoopen_run(int mx, int my);

/* SYS_WINDOW_POLL / SYS_WINDOW_OPEN support, called from kernel/syscall.c;
   defined in ring3app.c alongside the rest of the ring-3 window plumbing.
   The internals they read (kbd_map, gui_app_mouse_tick, gui_close_was_click,
   gui_app_windowed, app_view_w/h, cursor_saved_x/y, editor_mouse_x/y,
   gui_draw_desktop, gui_cursor_save, gui_draw_cursor, gui_launch_from_dock)
   are kernel.c statics turned into plain globals/functions for this one
   external user. */
int gui_poll_event(int *a, int *b);
int gui_app_view_size(unsigned int *w, unsigned int *h);

char kbd_map(int sc);
void gui_app_mouse_tick(void);
extern int gui_close_was_click;
extern int gui_app_windowed;
extern int app_view_w, app_view_h;

extern int cursor_saved_x, cursor_saved_y;
extern int editor_mouse_x, editor_mouse_y;
void gui_draw_desktop(int hover_slot, int drag_slot, int drag_mx, int drag_my);
void gui_cursor_save(int x, int y);
void gui_draw_cursor(int x, int y);
void gui_launch_from_dock(int icon);
#endif
