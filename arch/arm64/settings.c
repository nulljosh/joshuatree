/* The Pi's Settings, the part that is not drawing: what the choices are, the file they live in, and loading it at boot.
   JT/SETTINGS.TXT holds three lines, `volume=70`, `wall=0` and `dock=7`, the same key=value shape the i386 SETTINGS.TXT uses.
   With accounts (6.0) the wallpaper is each person's own: ~/WALL.TXT holds `wall=N`, and the card's is the login screen's.
   A missing, short or hand-edited file is never an error: every value is range-checked and a bad line is ignored. With no
   card the choices live in memory until the next boot and the screen says so. main.c draws the window (settings_ui.h);
   tools/checks/arm64-settings-check.py runs this file on the host under sanitizers, then drives the window in QEMU. */
#include "audio.h"
#include "emmc.h"
#include "../../drivers/fat.h"

extern int dock_scale_pct;            /* main.c: the dock's tile size as a percent of the screen height (5 to 25, 7 by default) */
void wall_set(unsigned rgb);          /* wall.c */
void desktop_wall_changed(void);      /* main.c: repaint the desktop if it is up */
void kputs(const char *s); void kdec(unsigned v);   /* main.c: the UART (and the Console) */
int sudo_may_write(void);             /* sudo.c: no accounts, or the signed-in user is an admin */
int home_active(void);                /* home.c (6.0): accounts exist and somebody is signed in */
int home_mine(char out[9]);
int home_in(const char *dir, int make);
int home_save(const char *name, const void *data, unsigned len);

#define SET_FILE "SETTINGS.TXT"
#define SET_WALLS 4
#define SET_DOCK_MIN 5
#define SET_DOCK_MAX 25
static const struct { const char *name; unsigned rgb; } walls[SET_WALLS] = {
    { "Grey", 0x006E6E6Eu }, { "Terracotta", 0x00B5502Cu }, { "Clay", 0x00923F20u }, { "Ink", 0x00141311u } };
static int wall_pick;                 /* index into walls */
static int saved_state;               /* 0 not yet saved, 1 on the card, 2 only the wallpaper (yours), -1 no card or the card refused it, -2 not an admin */
static int card_wall;                 /* JT/SETTINGS.TXT's wallpaper: the login screen's, and anyone's with no ~/WALL.TXT */
#define WALL_FILE "WALL.TXT"          /* 6.0, Yours: each person's wallpaper, `wall=N`, in their own home folder */

const char *settings_version(void) {
#ifdef JT_VERSION
    return JT_VERSION;
#else
    return "dev";
#endif
}
int settings_wall(void) { return wall_pick; }
const char *settings_wall_name(int i) { return i >= 0 && i < SET_WALLS ? walls[i].name : ""; }
unsigned settings_wall_rgb(int i) { return i >= 0 && i < SET_WALLS ? walls[i].rgb : walls[0].rgb; }
int settings_walls(void) { return SET_WALLS; }
void settings_set_wall(int i) { wall_pick = ((i % SET_WALLS) + SET_WALLS) % SET_WALLS; wall_set(walls[wall_pick].rgb); }
int settings_dock(void) { return dock_scale_pct; }
void settings_set_dock(int pct) { dock_scale_pct = pct < SET_DOCK_MIN ? SET_DOCK_MIN : pct > SET_DOCK_MAX ? SET_DOCK_MAX : pct; }
int settings_saved(void) { return saved_state; }

static int key_is(const char *t, unsigned len, const char *key) {   /* the first len bytes of t are exactly key */
    unsigned i = 0;
    for (; key[i]; i++) if (i >= len || t[i] != key[i]) return 0;
    return i == len;
}
/* The three values out of a file's text. Each is taken only when it is a whole number in range; the rest stay as they were. */
void settings_parse(const char *t, unsigned n, int *vol, int *wall, int *dock) {
    for (unsigned i = 0; i < n;) {
        unsigned k = i; while (k < n && t[k] != '=' && t[k] != '\n') k++;
        if (k < n && t[k] == '=') {
            unsigned v = k + 1; long x = 0; int digits = 0;
            while (v < n && t[v] >= '0' && t[v] <= '9' && digits < 4) { x = x * 10 + (t[v] - '0'); v++; digits++; }
            int whole = digits && (v >= n || t[v] == '\n' || t[v] == '\r');
            if (whole) {
                if (key_is(t + i, k - i, "volume") && x <= 100) *vol = (int)x;
                else if (key_is(t + i, k - i, "wall") && x < SET_WALLS) *wall = (int)x;
                else if (key_is(t + i, k - i, "dock") && x >= SET_DOCK_MIN && x <= SET_DOCK_MAX) *dock = (int)x;
            }
        }
        while (i < n && t[i] != '\n') i++;
        i++;
    }
}
unsigned settings_format(char *out, int vol, int wall, int dock) {
    unsigned n = 0;
    const char *keys[3] = { "volume=", "wall=", "dock=" };
    int vals[3] = { vol, wall, dock };
    for (int r = 0; r < 3; r++) {
        for (const char *p = keys[r]; *p; p++) out[n++] = *p;
        char d[4]; int dn = 0, v = vals[r] < 0 ? 0 : vals[r] > 999 ? 999 : vals[r];
        do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (dn) out[n++] = d[--dn];
        out[n++] = '\n';
    }
    return n;
}
void settings_save(void) {
    char buf[48];
    int mine = 0;
    if (home_active()) {                                      /* 6.0: the wallpaper is yours, saved in your home with no admin needed */
        char w[8] = "wall=0\n"; w[5] = (char)('0' + wall_pick);
        mine = sd_ready() && home_save(WALL_FILE, w, 7) == 1;
    }
    unsigned n = settings_format(buf, audio_volume(), wall_pick, dock_scale_pct);
    if (!sudo_may_write()) { saved_state = mine ? 2 : -2; return; }     /* 4.10.0: only an admin saves the rest; anyone else's lasts until restart */
    saved_state = sd_ready() && sd_write(SET_FILE, buf, n) == 1 ? 1 : -1;
    if (saved_state == 1) card_wall = wall_pick;
}
/* At sign-in and at the login screen (login_ui.h): the signed-in person's ~/WALL.TXT, else the card's wallpaper. The caller
   repaints. */
void settings_user_wall(void) {
    int w = card_wall, v = 0, d = 0;
    char dir[9], t[16];
    if (home_active() && home_mine(dir) && sd_ready() && sd_enter_jt(0)) {
        if (home_in(dir, 0)) { int got = fat_read_file(WALL_FILE, t, sizeof t); if (got > 0) settings_parse(t, (unsigned)got, &v, &w, &d); }
        fat_cwd_set(0);
    }
    if (w != wall_pick) settings_set_wall(w);
}
/* After sd_mount, before the desktop is first painted on the Pi. The choices that need the screen (the wallpaper) repaint it
   when it is already up, as on QEMU's virt machine, where the desktop is drawn before the card is looked at. */
void settings_load(void) {
    char buf[96];
    int vol = audio_volume(), wall = wall_pick, dock = dock_scale_pct;
    if (!sd_ready() || !sd_enter_jt(0)) return;
    int got = fat_read_file(SET_FILE, buf, sizeof buf);
    fat_chdir("..");
    if (got <= 0) return;
    settings_parse(buf, (unsigned)got, &vol, &wall, &dock);
    audio_set_volume(vol);
    settings_set_dock(dock);
    card_wall = wall;
    if (wall != wall_pick) { settings_set_wall(wall); desktop_wall_changed(); }
    saved_state = 1;
    kputs("settings loaded volume "); kdec((unsigned)audio_volume()); kputs(" wall "); kdec((unsigned)wall_pick); kputs(" dock "); kdec((unsigned)dock_scale_pct); kputs("\n");
}
