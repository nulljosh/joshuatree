/* update.c (9.3.0): self-update with a fallback kernel.
   `update` asks UPDATE_BASE for version.txt (the release's version, then a line on what is new) and, when it names another
   version, opens Software Update (desk.c's centred card). Update and Restart (or `update install`) fetches kernel8.img and
   kernel8.sig from the same place and checks the signature with appsig.c, under the name "os.image" (a dot no app name can
   have, so no signed app is ever a signed kernel), before it writes a byte. tools/update-sign.sh signs the release's image on Joshua's Mac with the app key; CI never holds it. Then:
   - KERNEL8.NEW beside the running KERNEL8.IMG, TRYBOOT.TXT (config.txt plus kernel=kernel8.new), JT/UPDATE.VER;
   - a restart with the firmware's tryboot flag (mailbox tag 0x00038064), so that one boot reads TRYBOOT.TXT, and any boot
     after it reads config.txt again.
   The new kernel, once it has run UPDATE_SETTLE seconds, commits: KERNEL8.NEW becomes KERNEL8.IMG and the staged files go.
   If it never gets that far, the next power-on is the old kernel from config.txt: it finds UPDATE.VER naming another
   version, drops the staged files and says so on the menu bar. arm64-update-check.py proves the signature gate, the
   staging, the commit and the fallback on QEMU; the tryboot flag is the firmware's and needs the board. UPDATE_BASE= points
   it at a stub for that check only (FORCE rebuilds the object, as store.c's). */
#include "appsig.h"
int ask_dev_build(void);   /* ask.c: 1 when this kernel carries the relay token (a dev card from tools/flash-pi.sh) */
#ifndef UPDATE_BASE
#define UPDATE_BASE "https://github.com/nulljosh/joshuatree/releases/latest/download/"
#endif
#define UPDATE_SETTLE 20   /* seconds the new kernel runs before it is kept */
extern void (*cmd_out)(const char *s);   /* cmd.c */
void cmd_dec(unsigned v);
int browser_get(const char *url, char **body, unsigned *n);   /* browser.c */
unsigned long heap_mark(void); void heap_release(unsigned long m);
void *kmalloc(unsigned n);
int sd_ready(void); int sd_enter_jt(int make);
int fat_read_file(const char *name, void *buf, unsigned bufsize); int fat_replace_file(const char *name, const void *data, unsigned len);
int fat_delete(const char *name); void fat_cwd_set(unsigned cluster);
void uart_puts(const char *s); void menubar_notify(const char *message); unsigned ticks(void);
void update_dialog(const char *ver, const char *what);   /* desk.c: the Software Update card */
void power_restart_tryboot(void);                       /* power.h */

static char latest[16], news[96];
static int settle = -1;   /* 0 or more: this boot is the staged version, kept at that tick */

static int get(const char *file, char **body, unsigned *n) {
    char url[160]; unsigned o = 0;
    for (const char *p = UPDATE_BASE; *p && o < 120; p++) url[o++] = *p;
    for (const char *p = file; *p; p++) url[o++] = *p;
    url[o] = 0;
    return browser_get(url, body, n);
}
static int newer(const char *a, const char *b) {   /* 10.0.0 audit: is version a after b? Numbers, dot by dot; an old release is never offered */
    for (;;) {
        unsigned x = 0, y = 0;
        while (*a >= '0' && *a <= '9') x = x * 10 + (unsigned)(*a++ - '0');
        while (*b >= '0' && *b <= '9') y = y * 10 + (unsigned)(*b++ - '0');
        if (x != y) return x > y;
        if (*a != '.' && *b != '.') return 0;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
}
static unsigned trimlen(const char *s, unsigned n) { while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--; return n; }
static int same(const char *a, unsigned an, const char *b) { unsigned bn = 0; while (b[bn]) bn++; bn = trimlen(b, bn); an = trimlen(a, an); if (an != bn) return 0; for (unsigned i = 0; i < an; i++) if (a[i] != b[i]) return 0; return 1; }
static void drop_staged(void) {
    fat_cwd_set(0); fat_delete("KERNEL8.NEW"); fat_delete("TRYBOOT.TXT");
    if (sd_enter_jt(0)) fat_delete("UPDATE.VER");
    fat_cwd_set(0);
}

void update_check(void) {   /* `update` */
    unsigned long mark = heap_mark();
    char *b; unsigned n;
    int st = get("version.txt", &b, &n);
    if (st != 200 || !b || !n) { cmd_out("update: the release could not be reached"); if (st > 0) { cmd_out(", it said "); cmd_dec((unsigned)st); } cmd_out("\n"); heap_release(mark); return; }
    unsigned i = 0, k = 0;
    while (i < n && b[i] != '\n' && k + 1 < sizeof latest) latest[k++] = b[i++];
    latest[k] = 0; k = trimlen(latest, k); latest[k] = 0;
    k = 0; if (i < n) i++;
    while (i < n && b[i] != '\n' && k + 1 < sizeof news) news[k++] = b[i++];
    news[k] = 0;
    heap_release(mark);
    if (same(JT_VERSION, sizeof JT_VERSION - 1, latest)) { cmd_out("update: "); cmd_out(latest); cmd_out(" is the newest; this is it\n"); return; }
    if (!newer(latest, JT_VERSION)) { cmd_out("update: the release is "); cmd_out(latest); cmd_out(", older than this; nothing to do\n"); latest[0] = 0; return; }
    cmd_out("update: "); cmd_out(latest); cmd_out(" is out (this is " JT_VERSION "); update install gets it\n");
    update_dialog(latest, news);
}

void update_install(void) {   /* `update install`, or Update and Restart */
    if (!latest[0]) { cmd_out("update: run update first, to see what is out\n"); return; }
    if (!sd_ready()) { cmd_out("update: no card to put it on\n"); return; }
    if (ask_dev_build()) {   /* a dev card: its kernel carries the relay key and CA, which the release image does not */
        cmd_out("update: this card's kernel carries your relay key; the release would drop it. Update it with tools/flash-pi.sh\n");
        uart_puts("update: dev card, not replaced\n"); return;
    }
    unsigned long mark = heap_mark();
    char *img, *sig; unsigned il, sl;
    int st = get("kernel8.img", &img, &il);
    if (st != 200 || !img || il < 4096) { cmd_out("update: the image could not be fetched\n"); heap_release(mark); return; }
    st = get("kernel8.sig", &sig, &sl);
    if (st != 200 || !sig || !appsig_ok("os.image", 8, img, il, (const unsigned char *)sig, sl)) {   /* nothing is written unless it is signed */
        cmd_out(st == 200 ? "update: the image's signature does not match, so nothing was changed\n" : "update: the release has no signature, so nothing was changed\n");
        uart_puts("update: refused\n"); heap_release(mark); return;
    }
    char *cfg = kmalloc(8192 + 64); int cl = 0;
    fat_cwd_set(0);
    if (cfg) { cl = fat_read_file("CONFIG.TXT", cfg, 8192); if (cl < 0) cl = 0; for (const char *p = "\n[all]\nkernel=kernel8.new\n"; *p; p++) cfg[cl++] = *p; }
    int ok = cfg && fat_replace_file("KERNEL8.NEW", img, il) && fat_replace_file("TRYBOOT.TXT", cfg, (unsigned)cl);
    unsigned vl = 0; while (latest[vl]) vl++;
    if (ok) ok = sd_enter_jt(1) && fat_replace_file("UPDATE.VER", latest, vl);
    fat_cwd_set(0);
    heap_release(mark);
    if (!ok) { drop_staged(); cmd_out("update: the card did not take it; nothing changed\n"); return; }
    cmd_out("update: "); cmd_out(latest); cmd_out(", "); cmd_dec(il); cmd_out(" bytes, signed, is staged; restarting into it\n");
    uart_puts("update: restarting into "); uart_puts(latest); uart_puts(" (tryboot)\n");
    power_restart_tryboot();
}

void update_boot(void) {   /* after sd_mount: is an update staged, and is this it? */
    char v[16];
    if (!sd_ready() || !sd_enter_jt(0)) return;
    int n = fat_read_file("UPDATE.VER", v, sizeof v - 1);
    fat_cwd_set(0);
    if (n <= 0) return;
    v[n] = 0;
    if (same(v, (unsigned)n, JT_VERSION)) { settle = (int)ticks() + UPDATE_SETTLE * 100; uart_puts("update: "); uart_puts(v); uart_puts(" started from KERNEL8.NEW; kept after it runs a while\n"); return; }
    drop_staged();
    uart_puts("update: "); uart_puts(v); uart_puts(" did not start; this is " JT_VERSION ", kept\n");
    menubar_notify("The update did not start; kept this version");
}

void update_tick(void) {   /* the poll loop: commit once the new kernel has run UPDATE_SETTLE seconds */
    if (settle < 0 || (int)(ticks() - (unsigned)settle) < 0) return;
    settle = -1;
    unsigned long mark = heap_mark();
    unsigned cap = 6u << 20;
    char *img = kmalloc(cap);
    fat_cwd_set(0);
    int n = img ? fat_read_file("KERNEL8.NEW", img, cap) : -1;
    int ok = n > 4096 && fat_replace_file("KERNEL8.IMG", img, (unsigned)n);
    heap_release(mark);
    if (!ok) { uart_puts("update: could not make KERNEL8.NEW the kernel; the next boot tries it again\n"); return; }
    drop_staged();
    uart_puts("update: committed, KERNEL8.IMG is " JT_VERSION "\n");
    menubar_notify("Updated to " JT_VERSION);
}
