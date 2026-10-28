/* store.c (6.55.0): `install NAME`, the app store. The store is a folder on the landing, joshuatree.jaybulb.com/apps/, holding
   name.app and name.sig for each app (tools/store-add.sh puts them there, signed). install GETs both over HTTPS, checks the
   signature with appsig.c before it writes a byte, then puts NAME.APP and NAME.SIG in JT/APPS, where userrun and Spotlight
   find them (and check the signature again when it loads). STORE_BASE= points it at a stub for arm64-store-check.py only;
   FORCE rebuilds this object on every make, so a stub address never survives into a build made without it. cmd.c gates the
   command as a write to JT/ (an admin's, or anyone's with no accounts). */
#include "appsig.h"
#include "uimg.h"
#ifndef STORE_BASE
#define STORE_BASE "https://joshuatree.jaybulb.com/apps/"
#endif
extern void (*cmd_out)(const char *s);   /* cmd.c */
void cmd_dec(unsigned v);
int browser_get(const char *url, char **body, unsigned *n);   /* browser.c */
unsigned long heap_mark(void); void heap_release(unsigned long m);
int card_name_taken(const char *low, unsigned n);              /* main.c */
int sd_ready(void); int sd_enter_jt(int make);
int fat_chdir(const char *name); int fat_mkdir(const char *name); int fat_replace_file(const char *name, const void *data, unsigned len); void fat_cwd_set(unsigned cluster);

static int get(const char *low, unsigned n, const char *ext, char **body, unsigned *bn) {   /* STORE_BASE name.ext; the HTTP status */
    char url[128]; unsigned o = 0;
    for (const char *p = STORE_BASE; *p && o < 100; p++) url[o++] = *p;
    for (unsigned i = 0; i < n; i++) url[o++] = low[i];
    for (const char *p = ext; *p; p++) url[o++] = *p;
    url[o] = 0;
    return browser_get(url, body, bn);
}
int fat_delete(const char *name);
/* 6.59.0: an unsigned app (Samantha's [[app NAME]]) into JT/APPS: NAME.APP, and a NAME.SIG left from before goes, since it
   cannot match the new image. 1 written. */
int store_put(const char *low, unsigned n, const char *img, unsigned il) {
    char f[13];
    for (unsigned i = 0; i < n; i++) f[i] = (char)(low[i] >= 'a' && low[i] <= 'z' ? low[i] - 32 : low[i]);
    if (card_name_taken(low, n) || il > UIMG_MAX) return 0;
    int ok = sd_ready() && sd_enter_jt(1);
    if (ok) { fat_mkdir("APPS"); ok = fat_chdir("APPS"); }
    f[n] = '.'; f[n + 1] = 'A'; f[n + 2] = 'P'; f[n + 3] = 'P'; f[n + 4] = 0;
    if (ok) ok = fat_replace_file(f, img, il);
    f[n + 1] = 'S'; f[n + 2] = 'I'; f[n + 3] = 'G';
    if (ok) fat_delete(f);
    fat_cwd_set(0);
    return ok;
}
void store_install(const char *name, unsigned n) {
    char low[9], f[13];
    cmd_out("install: ");
    if (!n || n > 8) { cmd_out("use install NAME, a name of 1 to 8 letters, digits, - or _\n"); return; }
    for (unsigned i = 0; i < n; i++) {
        char c = (char)(name[i] | 32);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) { cmd_out("use install NAME, a name of 1 to 8 letters, digits, - or _\n"); return; }
        low[i] = c; f[i] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    low[n] = 0;
    if (card_name_taken(low, n)) { cmd_out(low); cmd_out(" is a built-in app's name\n"); return; }
    unsigned long mark = heap_mark();
    char *img, *sig; unsigned il, sl;
    int st = get(low, n, ".app", &img, &il);
    if (st != 200 || !img) {
        cmd_out(st == 404 ? "the store has no app called " : "the store could not be reached for ");
        cmd_out(low);
        if (st > 0 && st != 404) { cmd_out(", it said "); cmd_dec((unsigned)st); }
        cmd_out("\n"); heap_release(mark); return;
    }
    if (il > UIMG_MAX) { cmd_out(low); cmd_out(" is too big for an app\n"); heap_release(mark); return; }
    st = get(low, n, ".sig", &sig, &sl);
    if (st != 200 || !sig || !appsig_ok(low, n, img, il, (const unsigned char *)sig, sl)) {   /* nothing is written unless it is signed */
        cmd_out(low); cmd_out(st == 200 ? "'s signature does not match it, so it was not installed\n" : " has no signature in the store, so it was not installed\n");
        heap_release(mark); return;
    }
    int ok = sd_ready() && sd_enter_jt(1);
    if (ok) { fat_mkdir("APPS"); ok = fat_chdir("APPS"); }
    f[n] = '.'; f[n + 1] = 'A'; f[n + 2] = 'P'; f[n + 3] = 'P'; f[n + 4] = 0;
    if (ok) ok = fat_replace_file(f, img, il);
    f[n + 1] = 'S'; f[n + 2] = 'I'; f[n + 3] = 'G';
    if (ok) ok = fat_replace_file(f, sig, sl);
    fat_cwd_set(0);
    heap_release(mark);
    if (!ok) { cmd_out("the card did not take "); cmd_out(low); cmd_out("\n"); return; }
    cmd_out(low); cmd_out(", "); cmd_dec(il); cmd_out(" bytes, signed, is in JT/APPS: userrun "); cmd_out(low); cmd_out(" or Spotlight opens it\n");
}
