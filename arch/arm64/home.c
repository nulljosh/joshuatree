/* Per-user home folders on the Pi, the part that is not drawing: where JT/HOME/<dir> is, who may open it, and the one path
   walker the Terminal's cat, ls, write, rm and mkdir share. The path is untrusted text: "." and ".." parts are refused so no
   spelling can step out of a folder into somebody else's, and the check is made on the parts before the card is touched.
   FAT compares names without case, so so does this. tools/checks/arm64-home-check.py runs this file on the host. */
#include "home.h"
#include "accounts.h"
#include "sudo.h"
#include "login.h"
#include "emmc.h"
#include "../../drivers/fat.h"

#define WALK_DEPTH 9
static unsigned saved_cwd;
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int ieq(const char *a, const char *b) { while (*a && low(*a) == low(*b)) { a++; b++; } return *a == *b; }

unsigned home_dirname(const char *user, char out[HOME_DIR_MAX + 1]) {
    static const char hx[] = "0123456789abcdef";
    unsigned n = slen(user), k = 0;
    if (n <= HOME_DIR_MAX) { for (; k < n; k++) out[k] = low(user[k]); out[k] = 0; return k; }
    unsigned h = 2166136261u;
    for (unsigned i = 0; i < n; i++) h = (h ^ (unsigned char)low(user[i])) * 16777619u;
    for (; k < 5; k++) out[k] = low(user[k]);
    out[k++] = '~'; out[k++] = hx[h >> 4 & 15]; out[k++] = hx[h & 15]; out[k] = 0;
    return k;
}
int home_active(void) { return accounts_count() && current_user[0]; }
int home_mine(char out[HOME_DIR_MAX + 1]) { if (!home_active()) return 0; home_dirname(current_user, out); return 1; }
int home_collides(const char *name) {
    char a[HOME_DIR_MAX + 1], b[HOME_DIR_MAX + 1];
    home_dirname(name, a);
    for (unsigned i = 0; i < accounts_count(); i++) {
        const char *o = accounts_name(i);
        if (ieq(o, name)) continue;
        home_dirname(o, b);
        if (ieq(a, b)) return 1;
    }
    return 0;
}
int home_may(const char *dir) {
    char mine[HOME_DIR_MAX + 1];
    if (!accounts_count()) return 1;
    if (home_mine(mine) && ieq(dir, mine)) return 1;
    return sudo_active();
}
int home_in(const char *dir, int make) {   /* standing in JT/ */
    unsigned at = fat_cwd_get();
    if (!fat_chdir("HOME")) {
        if (!make || !fat_mkdir("HOME") || !fat_chdir("HOME")) return 0;
    }
    if (!fat_chdir(dir)) {
        if (!make || !fat_mkdir(dir) || !fat_chdir(dir)) { fat_cwd_set(at); return 0; }
    }
    return 1;
}
int home_make(const char *user) {
    char dir[HOME_DIR_MAX + 1];
    if (!sd_ready() || !sd_enter_jt(1)) return 0;
    home_dirname(user, dir);
    int ok = home_in(dir, 1);
    fat_cwd_set(0);
    return ok;
}
int home_expand(const char *in, char *out, unsigned cap) {
    char dir[HOME_DIR_MAX + 1];
    unsigned k = 0;
    const char *rest = in;
    if (in[0] == '~' && (!in[1] || in[1] == '/')) {
        if (!home_mine(dir)) return 0;
        static const char pre[] = "JT/HOME/";
        for (const char *p = pre; *p; p++) { if (k + 1 >= cap) return -2; out[k++] = *p; }
        for (const char *p = dir; *p; p++) { if (k + 1 >= cap) return -2; out[k++] = *p; }
        rest = in + 1;
    }
    for (; *rest; rest++) { if (k + 1 >= cap) return -2; out[k++] = *rest; }
    out[k] = 0;
    return 1;
}
int home_walk(char *path, int keep, char **leaf) {
    char *comp[WALK_DEPTH + 1];
    int n = 0;
    unsigned len = slen(path);
    while (len > 1 && path[len - 1] == '/') path[--len] = 0;   /* a trailing slash names the folder */
    *leaf = path;
    if (path[0] == '/') return 0;
    if (path[0]) {
        for (char *p = path;;) {
            if (n >= WALK_DEPTH) return 0;
            comp[n++] = p;
            char *q = p;
            while (*q && *q != '/') q++;
            unsigned l = (unsigned)(q - p);
            if (l == 0 || (l == 1 && p[0] == '.') || (l == 2 && p[0] == '.' && p[1] == '.')) return 0;
            if (!*q) break;
            *q = 0;
            p = q + 1;
        }
    }
    if (n < keep) return 0;
    if (n >= 3 && ieq(comp[0], "JT") && ieq(comp[1], "HOME") && !home_may(comp[2])) return -1;
    saved_cwd = fat_cwd_get();
    fat_cwd_set(0);
    for (int i = 0; i < n - keep; i++) if (!fat_chdir(comp[i])) { fat_cwd_set(saved_cwd); return 0; }
    *leaf = n ? comp[n - 1] : path;
    return 1;
}
void home_back(void) { fat_cwd_set(saved_cwd); }
int home_owns(const char *path) {
    char mine[HOME_DIR_MAX + 1];
    static const char pre[] = "JT/HOME/";
    if (!home_mine(mine)) return 0;
    const char *p = path;
    for (const char *q = pre; *q; q++, p++) if (low(*p) != low(*q)) return 0;
    for (const char *q = mine; *q; q++, p++) if (low(*p) != *q) return 0;
    if (*p != '/' || !p[1]) return 0;
    for (const char *r = p + 1; *r; ) {   /* no ".." part on the way out: the walker refuses it, and so does this */
        if (r[0] == '.' && (r[1] == '/' || !r[1] || (r[1] == '.' && (r[2] == '/' || !r[2])))) return 0;
        while (*r && *r != '/') r++;
        if (*r) r++;
    }
    return 1;
}
static int name83(const char *s) {   /* the same shape emmc.c's sd_name_ok takes: 1 to 8, a dot, 1 to 3 */
    unsigned base = 0, ext = 0, dot = 0;
    for (; *s; s++) {
        char c = *s;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~';
        if (c == '.' && !dot) dot = 1;
        else if (!ok) return 0;
        else if (dot) ext++;
        else base++;
    }
    return base && base <= 8 && ext <= 3 && !(dot && !ext);
}
int home_op(int op, const char *path, const void *data, unsigned len) {
    char walk[96], *leaf;
    unsigned n = slen(path);
    if (!sd_ready()) return 0;
    if (n >= sizeof walk) return -2;
    for (unsigned i = 0; i <= n; i++) walk[i] = path[i];
    if (op != 1 && home_owns(path) && !home_make(current_user)) return 0;   /* the folder is there before the first file */
    if (!sd_enter_jt(op != 1)) return 0;   /* JT/ made for a write; the cursor is left in it, and home_walk starts from the root */
    fat_cwd_set(0);
    int r = home_walk(walk, 1, &leaf);
    if (r <= 0) return r;
    if (!name83(leaf)) { home_back(); return -2; }
    int ok = op == 0 ? fat_replace_file(leaf, data, len) : op == 1 ? fat_delete(leaf) : fat_mkdir(leaf);
    home_back();
    return ok;
}
int home_save(const char *name, const void *data, unsigned len) {
    char path[64];
    unsigned n = slen(name);
    if (!n || n > 12) return -2;
    for (unsigned i = 0; i < n; i++) if (name[i] == '/') return -2;
    path[0] = '~'; path[1] = '/';
    for (unsigned i = 0; i <= n; i++) path[2 + i] = name[i];
    char full[96];
    int e = home_expand(path, full, sizeof full);
    if (e <= 0) return e ? -2 : 0;
    return home_op(0, full, data, len);
}
