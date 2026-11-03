/* The Pi's Settings, the part that is not drawing: what the choices are, the file they live in, and loading it at boot.
   JT/SETTINGS.TXT holds five lines, `volume=70`, `wall=0`, `dock=7`, `zoom=0` and `glass=2` (11.3.0) (7.4.0: the dock's magnify, 0 off, 1 small, 2 large; 8.9.0 renamed it from mag and made off the default), the same key=value shape the i386 SETTINGS.TXT uses.
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
#define SET_WALLS 5
#define SET_DOCK_MIN 5
#define SET_DOCK_MAX 25
#define WALL_PARK 0x01000000u          /* wall.c's: the picture, not a colour */
static const struct { const char *name; unsigned rgb; } walls[SET_WALLS] = {
    { "Blue", 0x003B6EA5u }, { "Terracotta", 0x00B5502Cu }, { "Clay", 0x00923F20u }, { "Ink", 0x00141311u },
    { "Park", WALL_PARK } };   /* 8.9.0: the picture, Joshua Tree at dusk */
#ifdef PI_BUILD
#define WALL_DEFAULT 4                /* 8.9.0: the park picture on the Pi */
#else
#define WALL_DEFAULT 0                /* the flat blue on QEMU, which the checks read */
#endif
static int wall_pick = WALL_DEFAULT;  /* index into walls */
static int saved_state;               /* 0 not yet saved, 1 on the card, 2 only the wallpaper (yours), -1 no card or the card refused it, -2 not an admin */
static int card_wall = WALL_DEFAULT;  /* JT/SETTINGS.TXT's wallpaper: the login screen's, and anyone's with no ~/WALL.TXT */
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
unsigned settings_wall_rgb(int i) { unsigned c = i >= 0 && i < SET_WALLS ? walls[i].rgb : walls[0].rgb; return c == WALL_PARK ? 0x00EC9E76u : c; }   /* the picture's swatch: its horizon */
int settings_walls(void) { return SET_WALLS; }
void settings_set_wall(int i) { wall_pick = ((i % SET_WALLS) + SET_WALLS) % SET_WALLS; wall_set(walls[wall_pick].rgb); }
#ifndef DOCK_ZOOM
#ifdef PI_BUILD
#define DOCK_ZOOM 0   /* 8.9.0: off on the Pi unless chosen in Settings (Joshua: "disable zoom") */
#else
#define DOCK_ZOOM 2   /* QEMU builds keep it on, so the magnify checks (dockmag, genie, ptrburst, traffic, calendar) still see it */
#endif
#endif
int dock_mag = DOCK_ZOOM;             /* 7.4.0: crash.c's magnify, an index into mags */
static const char *const mags[3] = { "Off", "Small", "Large" };
int settings_dock(void) { return dock_scale_pct; }
void settings_set_dock(int pct) { dock_scale_pct = pct < SET_DOCK_MIN ? SET_DOCK_MIN : pct > SET_DOCK_MAX ? SET_DOCK_MAX : pct; }
int settings_saved(void) { return saved_state; }
int settings_mag(void) { return dock_mag; }
/* 11.3.0, Glass (Display): how much white the menu bar's glass mixes into the wallpaper behind it, Clear to Tinted, the
   slider macOS 27 gives Liquid Glass. Standard, 128 of 256, is the half-and-half blend the bar always had. */
#define SET_GLASSES 5
static const struct { const char *name; int a; } glasses[SET_GLASSES] = { { "Clear", 64 }, { "Light", 96 }, { "Standard", 128 }, { "Frosted", 176 }, { "Tinted", 224 } };
static int glass_pick = 2;
/* 13.4.0, Energy Saver: how long the screen waits before its saver starts (screen_idle.h); saved as sleep= on the card */
#define SET_SLEEPS 7
static const struct { const char *name; unsigned cs; } sleeps[SET_SLEEPS] = { { "1 minute", 6000 }, { "2 minutes", 12000 }, { "5 minutes", 30000 },
    { "10 minutes", 60000 }, { "15 minutes", 90000 }, { "30 minutes", 180000 }, { "Never", 0 } };
static int sleep_pick = 2;
int settings_sleep(void) { return sleep_pick; }
const char *settings_sleep_name(int i) { return i >= 0 && i < SET_SLEEPS ? sleeps[i].name : ""; }
void settings_set_sleep(int i) { sleep_pick = i < 0 ? 0 : i >= SET_SLEEPS ? SET_SLEEPS - 1 : i; }
unsigned settings_sleep_cs(void) { return sleeps[sleep_pick].cs; }
int settings_glass(void) { return glass_pick; }
const char *settings_glass_name(int i) { return i >= 0 && i < SET_GLASSES ? glasses[i].name : ""; }
void settings_set_glass(int i) { glass_pick = i < 0 ? 0 : i >= SET_GLASSES ? SET_GLASSES - 1 : i; }
int settings_glass_alpha(void) { return glasses[glass_pick].a; }
const char *settings_mag_name(int i) { return i >= 0 && i < 3 ? mags[i] : ""; }
void settings_set_mag(int i) { dock_mag = i < 0 ? 0 : i > 2 ? 2 : i; }

static int key_is(const char *t, unsigned len, const char *key) {   /* the first len bytes of t are exactly key */
    unsigned i = 0;
    for (; key[i]; i++) if (i >= len || t[i] != key[i]) return 0;
    return i == len;
}
/* The four values out of a file's text. Each is taken only when it is a whole number in range; the rest stay as they were. */
void settings_parse(const char *t, unsigned n, int *vol, int *wall, int *dock, int *mag, int *glass) {
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
                else if (key_is(t + i, k - i, "zoom") && x <= 2) *mag = (int)x;
                else if (key_is(t + i, k - i, "glass") && x < SET_GLASSES) *glass = (int)x;
            }
        }
        while (i < n && t[i] != '\n') i++;
        i++;
    }
}
unsigned settings_format(char *out, int vol, int wall, int dock, int mag, int glass) {
    unsigned n = 0;
    const char *keys[5] = { "volume=", "wall=", "dock=", "zoom=", "glass=" };   /* 8.9.0: zoom, not mag, so a card saved with the old mag=2 starts with it off */
    int vals[5] = { vol, wall, dock, mag, glass };
    for (int r = 0; r < 5; r++) {
        for (const char *p = keys[r]; *p; p++) out[n++] = *p;
        char d[4]; int dn = 0, v = vals[r] < 0 ? 0 : vals[r] > 999 ? 999 : vals[r];
        do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (dn) out[n++] = d[--dn];
        out[n++] = '\n';
    }
    return n;
}
void settings_save(void) {
    char buf[80];
    int mine = 0;
    if (home_active()) {                                      /* 6.0: the wallpaper is yours, saved in your home with no admin needed */
        char w[8] = "wall=0\n"; w[5] = (char)('0' + wall_pick);
        mine = sd_ready() && home_save(WALL_FILE, w, 7) == 1;
    }
    unsigned n = settings_format(buf, audio_volume(), wall_pick, dock_scale_pct, dock_mag, glass_pick);
    buf[n++] = 's'; buf[n++] = 'l'; buf[n++] = 'e'; buf[n++] = 'e'; buf[n++] = 'p'; buf[n++] = '='; buf[n++] = (char)('0' + sleep_pick); buf[n++] = '\n';
    if (!sudo_may_write()) { saved_state = mine ? 2 : -2; return; }     /* 4.10.0: only an admin saves the rest; anyone else's lasts until restart */
    saved_state = sd_ready() && sd_write(SET_FILE, buf, n) == 1 ? 1 : -1;
    if (saved_state == 1) card_wall = wall_pick;
}
/* At sign-in and at the login screen (login_ui.h): the signed-in person's ~/WALL.TXT, else the card's wallpaper. The caller
   repaints. */
void settings_user_wall(void) {
    int w = card_wall, v = 0, d = 0, m = 0, g = 0;
    char dir[9], t[16];
    if (home_active() && home_mine(dir) && sd_ready() && sd_enter_jt(0)) {
        if (home_in(dir, 0)) { int got = fat_read_file(WALL_FILE, t, sizeof t); if (got > 0) settings_parse(t, (unsigned)got, &v, &w, &d, &m, &g); }
        fat_cwd_set(0);
    }
    if (w != wall_pick) settings_set_wall(w);
}
/* After sd_mount, before the desktop is first painted on the Pi. The choices that need the screen (the wallpaper) repaint it
   when it is already up, as on QEMU's virt machine, where the desktop is drawn before the card is looked at. */
void settings_load(void) {
    char buf[128];
    int vol = audio_volume(), wall = wall_pick, dock = dock_scale_pct, mag = dock_mag, glass = glass_pick;
    if (!sd_ready() || !sd_enter_jt(0)) return;
    int got = fat_read_file(SET_FILE, buf, sizeof buf);
    fat_chdir("..");
    if (got <= 0) return;
    int mag0 = mag; mag = -1;
    settings_parse(buf, (unsigned)got, &vol, &wall, &dock, &mag, &glass);
    if (mag < 0) { mag = mag0; wall = wall_pick; }   /* 8.9.0: a file from before 8.9 (no zoom= line) keeps its volume and dock,
                                                       but its wallpaper was the old default, so the new default (the park on
                                                       the Pi) shows once; the next save writes zoom= and the choice sticks */
    for (int i = 0; i + 7 < got; i++)   /* 13.4.0: sleep=N, a line settings_parse does not know */
        if ((!i || buf[i - 1] == '\n') && key_is(buf + i, 6, "sleep=") && buf[i + 6] >= '0' && buf[i + 6] < '0' + SET_SLEEPS) settings_set_sleep(buf[i + 6] - '0');
    audio_set_volume(vol);
    settings_set_dock(dock);
    settings_set_mag(mag);
    card_wall = wall;
    int was = glass_pick; settings_set_glass(glass);
    if (wall != wall_pick) { settings_set_wall(wall); desktop_wall_changed(); }
    else if (glass_pick != was) desktop_wall_changed();   /* 11.3.0: the bar's glass is part of the desktop's paint */
    saved_state = 1;
    kputs("settings loaded volume "); kdec((unsigned)audio_volume()); kputs(" wall "); kdec((unsigned)wall_pick); kputs(" dock "); kdec((unsigned)dock_scale_pct); kputs(" mag "); kdec((unsigned)dock_mag); kputs(" glass "); kdec((unsigned)glass_pick); kputs("\n");
}
