/* The Settings window (3.13.0), in the Console's rectangle like the Clock and Brick, drawn by main.c. A sidebar of five
   sections, each with a flat glyph on a terracotta tile (glyphs.h), and a card of rows beside it. The layout numbers come from
   kernel/settings_ui.h (the i386 Settings: sidebar and detail pane, 36 pixel rows, 22 pixel tiles). What each choice is, and
   JT/SETTINGS.TXT, are in settings.c. Left and Right (or the - and + buttons) change a value, Tab picks the row, Up and Down
   the section, Esc closes. Every change is saved at once, and the UART says so: `settings open`, `settings section N Name`,
   `settings wall N Name`, `settings volume N`, `settings dock N`, `settings saved` (what arm64-settings-check.py reads). */
const char *settings_version(void);
int settings_wall(void);
const char *settings_wall_name(int i);
unsigned settings_wall_rgb(int i);
int settings_walls(void);
void settings_set_wall(int i);
int settings_dock(void);
void settings_set_dock(int pct);
int settings_saved(void);
void settings_save(void);
void settings_load(void);
unsigned wall_get(void);
void menubar_wifi(int state);
void desktop_wall_changed(void);

enum { SET_ABOUT, SET_DISPLAY, SET_SOUND, SET_DOCK, SET_TIME, SET_SECTIONS };
enum { SK_INFO, SK_STEP, SK_TOGGLE };
static const char *const set_names[SET_SECTIONS] = { "About", "Display", "Sound", "Dock", "Date and Time" };
static const int set_glyph[SET_SECTIONS] = { GL_ABOUT, GL_DISPLAY, GL_SOUND, GL_DOCK, GL_CLOCK };
static int set_sec, set_row, set_con, set_minute = -1;
static int dock_pct_boot = 7;   /* the dock size this boot drew; a changed size shows from the next one */
#define SET_SB_W 150            /* the sidebar, inside the frame's 8 pixel margin */
#define SET_ROW_H 30
#define SET_CARD_W 312
#define SET_INK 0x001C1C1E
#define SET_HINT 0x0075726E
#define SET_ACCENT 0x00B5502C

static void s_cat(char *b, const char *s) { unsigned n = 0; while (b[n]) n++; for (; *s && n < 39; s++) b[n++] = *s; b[n] = 0; }
static void s_num(char *b, unsigned v) { char d[11], t[12]; int n = 0, k = 0; do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) t[k++] = d[--n]; t[k] = 0; s_cat(b, t); }
static int set_rows(int sec) { static const char n[SET_SECTIONS] = { 5, 2, 3, 2, 4 }; return n[sec]; }
static const char *set_label(int sec, int r) {
    static const char *const l[SET_SECTIONS][5] = { { "Version", "Board", "Memory", "Wi-Fi", "Power" }, { "Wallpaper", "Screen" },
        { "Volume", "Mute", "Output" }, { "Tile size", "Tiles" }, { "Time", "Date", "Time zone", "Source" } };
    return l[sec][r];
}
static int set_kind(int sec, int r) {
    if (r == 0 && (sec == SET_DISPLAY || sec == SET_SOUND || sec == SET_DOCK)) return SK_STEP;
    return sec == SET_SOUND && r == 1 ? SK_TOGGLE : SK_INFO;
}
static int set_first_row(int sec) { for (int r = 0; r < set_rows(sec); r++) if (set_kind(sec, r) != SK_INFO) return r; return -1; }
static int set_next_row(int sec, int from) {   /* Tab: the next row that can be changed, wrapping; -1 when the section has none */
    for (int i = 1; i <= set_rows(sec); i++) { int r = (from + i + set_rows(sec)) % set_rows(sec); if (set_kind(sec, r) != SK_INFO) return r; }
    return -1;
}
static void set_time_text(char *time, char *date, char *zone) {
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    unsigned long utc = cal_utc(); unsigned h, m, mo, d;
    time[0] = date[0] = zone[0] = 0;
    s_cat(zone, "Vancouver, UTC-");
    if (!utc) { s_cat(time, "Waiting for network time"); s_cat(date, "Not known yet"); s_cat(zone, "7 or 8"); return; }
    clock_local(utc, &h, &m, &mo, &d);
    unsigned h12 = h % 12 ? h % 12 : 12;
    s_num(time, h12); s_cat(time, ":"); s_num(time, m / 10); s_num(time, m % 10); s_cat(time, h >= 12 ? " PM" : " AM");
    s_cat(date, months[mo - 1]); s_cat(date, " "); s_num(date, d); s_cat(date, ", "); s_num(date, (unsigned)clock_year);
    s_num(zone, (unsigned)((utc / 3600 % 24 + 24 - h) % 24));   /* the offset the conversion used: 7 in summer time, 8 in winter */
    set_minute = (int)(utc / 60);
}
static void set_value(int sec, int r, char *b) {
    char date[40], zone[40]; b[0] = 0;
    if (sec == SET_ABOUT) {
        if (r == 0) s_cat(b, settings_version());
#ifdef PI_BUILD
        else if (r == 1) s_cat(b, "Raspberry Pi 4");
#else
        else if (r == 1) s_cat(b, "QEMU virt (arm64)");
#endif
        else if (r == 2) { unsigned tenth = (unsigned)((heap_next - (unsigned long)_heap_start) * 10 / (1UL << 20)); s_cat(b, "Heap "); s_num(b, tenth / 10); s_cat(b, "."); s_num(b, tenth % 10); s_cat(b, " of "); s_num(b, (unsigned)(HEAP_SIZE >> 20)); s_cat(b, " MB"); }
        else if (r == 3) s_cat(b, mb_wifi == 2 ? "Connected" : mb_wifi == 1 ? "Connecting" : "Not connected");
        else s_cat(b, "Wall power, no battery");
    } else if (sec == SET_DISPLAY) {
        if (r == 0) s_cat(b, settings_wall_name(settings_wall()));
        else { s_num(b, fb_w); s_cat(b, " x "); s_num(b, fb_h); }
    } else if (sec == SET_SOUND) {
        if (r == 0) { s_num(b, (unsigned)audio_volume()); s_cat(b, "%"); }
        else if (r == 2) {
#ifdef PI_BUILD
            s_cat(b, "3.5 mm jack");
#else
            s_cat(b, "None on QEMU");
#endif
        }
    } else if (sec == SET_DOCK) {
        if (r == 0) { s_num(b, (unsigned)settings_dock()); s_cat(b, "%"); } else s_num(b, GUI_ICON_COUNT);
    } else {
        char time[40]; set_time_text(time, date, zone);
        s_cat(b, r == 0 ? time : r == 1 ? date : r == 2 ? zone : "Network, over HTTPS");
    }
}
static void set_log(const char *what, int n, const char *name) {
    con_quiet = 1; uart_puts("settings "); uart_puts(what);
    if (n >= 0) { uart_putc(' '); uart_dec((unsigned)n); }
    if (name) { uart_putc(' '); uart_puts(name); }
    uart_putc('\n');
}
static void set_saved(void) {
    settings_save();
    set_log(settings_saved() > 0 ? "saved" : settings_saved() == -2 ? "kept until restart, admins save" : "save failed, no card", -1, 0);
}
static void set_card(int *cx, int *cy) { *cx = win_lx + 8 + SET_SB_W + 16; *cy = win_ly + 74; }
static void set_step_box(int cx, int ry, int plus, int *x, int *y) {   /* the - and + buttons: 16 pixel squares either side of a 70 pixel value */
    *x = cx + SET_CARD_W - 14 - 16 - (plus ? 0 : 16 + 6 + 70 + 6); *y = ry + 7;
}
static void set_paint(void) {
    int s = (int)window_scale(), wx = win_lx, wy = win_ly, ww = win_lw, wh = win_lh, cx, cy;
    set_card(&cx, &cy);
    cur_hide();
    gui_draw_window_frame(wx, wy, ww, wh, "Settings");
    window_rect(wx + 8, wy + 30, ww - 16, wh - 38, 0x00faf8f4);
    window_rect(wx + 8, wy + 30, SET_SB_W, wh - 38, 0x00F0EAE1);
    window_rect(wx + 8 + SET_SB_W, wy + 30, 1, wh - 38, 0x00DDD5C8);
    for (int i = 0; i < SET_SECTIONS; i++) {
        int y = wy + 40 + i * 30;
        if (i == set_sec) gui_rounded_rect_on_wallpaper(wx + 14, y - 4, SET_SB_W - 12, 26, 0x00E2D9C9, 7);
        gui_rounded_rect_on_wallpaper(wx + 20, y, 18, 18, SET_ACCENT, 5);
        glyph_paint(set_glyph[i], 2, (wx + 22) * s, (y + 2) * s, 14 * s, 0x00FAF8F4);
        gui_text(set_names[i], wx + 46, y + 1, SET_INK);
    }
    gui_icon_text(set_names[set_sec], cx, wy + 38, 1, s, SET_INK);
    int n = set_rows(set_sec);
    gui_rounded_rect_on_wallpaper(cx, cy, SET_CARD_W, n * SET_ROW_H, 0x00FFFFFF, 10);
    for (int r = 0; r < n; r++) {
        int ry = cy + r * SET_ROW_H, kind = set_kind(set_sec, r);
        char v[40]; set_value(set_sec, r, v);
        if (r) gui_hairline_h(cx + 12, ry, SET_CARD_W - 24, 0x00E8E2D8);
        gui_text(set_label(set_sec, r), cx + 14, ry + 7, r == set_row ? SET_ACCENT : SET_INK);
        if (kind == SK_STEP) {
            int bx, by, px, vx0 = 0;
            for (int plus = 0; plus < 2; plus++) {
                set_step_box(cx, ry, plus, &bx, &by);
                gui_rounded_rect_on_wallpaper(bx, by, 16, 16, 0x00EFEBE4, 5);
                window_rect(bx + 4, by + 8, 8, 1, SET_INK);
                if (plus) window_rect(bx + 8, by + 4, 1, 8, SET_INK);
                if (!plus) vx0 = bx + 16 + 6;
            }
            set_step_box(cx, ry, 1, &px, &by);
            int tw = gui_text_width(v), vx = vx0 + (70 - tw) / 2;
            if (set_sec == SET_DISPLAY) {   /* the colour itself, a small round chip, then its name */
                vx = vx0 + (70 - tw - 18) / 2 + 18;
                gui_rounded_rect_on_wallpaper(vx - 18, ry + 9, 11, 11, settings_wall_rgb(settings_wall()), 5);
            }
            gui_text(v, vx, ry + (set_sec == SET_SOUND ? 2 : 7), SET_INK);
            if (set_sec == SET_SOUND) {   /* a thin bar under the number: the level, to the pixel */
                window_rect(vx0, ry + 21, 70, 2, 0x00E2D9C9);
                window_rect(vx0, ry + 21, 70 * audio_volume() / 100, 2, SET_ACCENT);
                gui_rounded_rect_on_wallpaper(vx0 + 70 * audio_volume() / 100 - 4, ry + 18, 8, 8, SET_ACCENT, 4);   /* a small round thumb */
            }
        } else if (kind == SK_TOGGLE) {
            int on = audio_volume() == 0, tx = cx + SET_CARD_W - 14 - 26;
            unsigned track = on ? SET_ACCENT : 0x00C7C0B4;
            gui_rounded_rect_on_wallpaper(tx, ry + 7, 26, 15, track, 7);
            gui_rounded_rect_on_wallpaper(on ? tx + 13 : tx + 2, ry + 9, 11, 11, 0x00FFFFFF, 5);
        } else {
            gui_text(v, cx + SET_CARD_W - 14 - gui_text_width(v), ry + 7, SET_HINT);
        }
    }
    int hy = cy + n * SET_ROW_H + 8;
    if (set_sec == SET_DISPLAY) gui_text("Flat colours. The change shows at once.", cx, hy, SET_HINT);
    else if (set_sec == SET_SOUND) gui_text("The same level the volume keys change.", cx, hy, SET_HINT);
    else if (set_sec == SET_DOCK) gui_text(settings_dock() == dock_pct_boot ? "A new size shows from the next restart." : "Restart to see the new size.", cx, hy, SET_HINT);
    else if (set_sec == SET_TIME) gui_text("The Pi has no battery clock; it asks the network.", cx, hy, SET_HINT);
    const char *st = settings_saved() > 0 ? "Saved on the card." : settings_saved() == -2 ? "Only an admin saves: kept until restart." : settings_saved() < 0 ? "No card: kept until restart." : "";
    gui_text(st, cx, wy + wh - 52, SET_HINT);
    gui_text("Arrow keys pick and change, Esc closes", cx, wy + wh - 32, SET_HINT);
    fb_flush(wx * s, wy * s, ww * s, wh * s);
    cur_show();
}
static void set_section(int sec) {
    set_sec = (sec % SET_SECTIONS + SET_SECTIONS) % SET_SECTIONS; set_row = set_first_row(set_sec); set_minute = -1;
    set_log("section", set_sec, set_names[set_sec]);
    if (settings_live) set_paint();
}
static void settings_open(int sec) {
    if (!fb) return;
    if (settings_live) { set_section(sec); return; }
    brick_close(); calc_close(); browser_close(); clock_close(); app_kill("closed for Settings");
    set_con = con_live;
    console_close();
    settings_live = 1;
    set_log("open", -1, 0);
    set_section(sec);
}
static void settings_close(void) {
    if (!settings_live) return;
    int s = (int)window_scale(), x = win_lx * s, y = win_ly * s, w = win_lw * s, h = win_lh * s;
    settings_live = 0;
    cur_hide();
    wall_fill(x, y, w, h);
    fb_flush(x, y, w, h);
    set_log("closed", -1, 0);
    if (set_con) { con_live = 0; pane_open(cp); }
    cur_show();
}
static void set_adjust(int r, int dir) {   /* a value in the open section, one step */
    int kind = set_kind(set_sec, r);
    if (kind == SK_INFO) return;
    if (set_sec == SET_DISPLAY) {
        settings_set_wall(settings_wall() + dir);
        set_log("wall", settings_wall(), settings_wall_name(settings_wall()));
        set_saved();
        desktop_wall_changed();   /* repaints the desktop, then this window on top of it */
        return;
    } else if (set_sec == SET_SOUND && r == 0) {
        int v = audio_volume() + 10 * dir; audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
        set_log("volume", audio_volume(), 0);
    } else if (set_sec == SET_SOUND) {
        audio_mute();
        set_log("volume", audio_volume(), 0);
    } else if (set_sec == SET_DOCK) {
        settings_set_dock(settings_dock() + dir);
        set_log("dock", settings_dock(), 0);
    }
    set_saved();
    if (set_sec == SET_SOUND) { cur_hide(); menubar_wifi(mb_wifi); cur_show(); }   /* the speaker in the menu bar follows the level */
    set_paint();
}
static int settings_key(unsigned code) {   /* a key down with Settings in front: every key is its own */
    if (code == 1) settings_close();
    else if (code == 103 || code == 108) set_section(set_sec + (code == 108 ? 1 : -1));
    else if ((code == 105 || code == 106) && set_row >= 0) set_adjust(set_row, code == 106 ? 1 : -1);
    else if ((code == 57 || code == 28 || code == 96) && set_row >= 0 && set_kind(set_sec, set_row) == SK_TOGGLE) set_adjust(set_row, 1);
    else if (code == 50 && set_sec == SET_SOUND) set_adjust(1, 1);   /* m mutes, as the Mute row */
    else if (code == 15 && set_row >= 0) { set_row = set_next_row(set_sec, set_row); set_paint(); }
    return 1;
}
static void settings_click(int lx, int ly) {   /* a click inside the window (the close dot is the caller's) */
    int cx, cy; set_card(&cx, &cy);
    for (int i = 0; i < SET_SECTIONS; i++) {
        int y = win_ly + 40 + i * 30;
        if (lx >= win_lx + 14 && lx < win_lx + 8 + SET_SB_W && ly >= y - 4 && ly < y + 22) { set_section(i); return; }
    }
    for (int r = 0; r < set_rows(set_sec); r++) {
        int ry = cy + r * SET_ROW_H, kind = set_kind(set_sec, r);
        if (ly < ry || ly >= ry + SET_ROW_H || lx < cx || lx >= cx + SET_CARD_W) continue;
        if (kind == SK_TOGGLE) { set_row = r; set_adjust(r, 1); return; }
        for (int plus = 0; plus < 2 && kind == SK_STEP; plus++) {
            int bx, by; set_step_box(cx, ry, plus, &bx, &by);
            if (lx >= bx && lx < bx + 16 && ly >= by && ly < by + 16) { set_row = r; set_adjust(r, plus ? 1 : -1); return; }
        }
    }
}
