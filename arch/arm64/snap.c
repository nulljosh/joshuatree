/* Home snapshots: a recursive copy of ~ into ~/SNAP/<n>/ and back, by FAT folder handle (fat_cwd_get/set), so two folders can
   be worked on without any path text. tools/checks/arm64-snap-check.py runs this file on the host. */
#include "snap.h"
#include "home.h"
#include "accounts.h"
#include "login.h"
#include "emmc.h"
#include "../../drivers/fat.h"

#define DEPTH_MAX 6
#define DONE "_DONE"
struct ent { char name[13]; unsigned size; int dir; };
static char buf[SNAP_FILE_MAX];
static unsigned n_files, n_bytes, n_skipped;
static struct ent *col; static unsigned col_n, col_over;

static int ieq(const char *a, const char *b) { while (*a && ((*a | 32) == (*b | 32))) { a++; b++; } return *a == *b; }
static void collect_cb(const char *name, unsigned size, int dir) {
    if (col_n >= SNAP_LIST_MAX) { col_over++; return; }
    unsigned k = 0;
    for (; name[k] && k < 12; k++) col[col_n].name[k] = name[k];
    col[col_n].name[k] = 0; col[col_n].size = size; col[col_n].dir = dir; col_n++;
}
static unsigned collect(unsigned folder, struct ent *into) {   /* the entries of a folder; col_over counts what did not fit */
    fat_cwd_set(folder); col = into; col_n = 0; col_over = 0; fat_list(collect_cb);
    return col_n;
}
static unsigned sub_of(unsigned folder, const char *name) {   /* a subfolder's handle, 0 when there is none (0 is the root, never a subfolder) */
    fat_cwd_set(folder);
    if (!fat_chdir(name)) return 0;
    return fat_cwd_get();
}
static unsigned sub_make(unsigned folder, const char *name) {
    unsigned s = sub_of(folder, name);
    if (s) return s;
    fat_cwd_set(folder);
    if (!fat_mkdir(name)) return 0;
    return sub_of(folder, name);
}
static int top_skip(int top, const char *name) { return top && (ieq(name, "SNAP") || ieq(name, DONE)); }

static int copy_tree(unsigned src, unsigned dst, int depth, int top) {   /* 1 all went, 0 the card refused */
    struct ent list[SNAP_LIST_MAX];
    unsigned n = collect(src, list);
    n_skipped += col_over;
    for (unsigned i = 0; i < n; i++) {
        if (top_skip(top, list[i].name)) continue;
        if (list[i].dir) {
            unsigned s = sub_of(src, list[i].name), d;
            if (!s || depth >= DEPTH_MAX) { n_skipped++; continue; }
            d = sub_make(dst, list[i].name);
            if (!d) return 0;
            if (!copy_tree(s, d, depth + 1, 0)) return 0;
        } else {
            if (list[i].size > SNAP_FILE_MAX) { n_skipped++; continue; }
            fat_cwd_set(src);
            int got = fat_read_file(list[i].name, buf, sizeof buf);
            if (got < 0) { n_skipped++; continue; }
            fat_cwd_set(dst);
            if (!fat_replace_file(list[i].name, buf, (unsigned)got)) return 0;
            n_files++; n_bytes += (unsigned)got;
        }
    }
    return 1;
}
static int wipe(unsigned folder) {   /* every entry of a folder, subfolders included; 0 when the card would not */
    for (int pass = 0; pass < 40; pass++) {
        struct ent list[SNAP_LIST_MAX];
        unsigned n = collect(folder, list), gone = 0;
        if (!n) return 1;
        for (unsigned i = 0; i < n; i++) {
            if (list[i].dir) {
                unsigned s = sub_of(folder, list[i].name);
                if (s && !wipe(s)) return 0;
                fat_cwd_set(folder);
                if (fat_rmdir(list[i].name)) gone++;
            } else {
                fat_cwd_set(folder);
                if (fat_delete(list[i].name)) gone++;
            }
        }
        if (!gone) return 0;
    }
    return 0;
}
static int drop(unsigned parent, const char *name) {   /* a folder and everything in it */
    unsigned s = sub_of(parent, name);
    if (!s) return 1;
    if (!wipe(s)) return 0;
    fat_cwd_set(parent);
    return fat_rmdir(name);
}
static int prune(unsigned home, unsigned snap, int depth, int top) {   /* remove from home what the snapshot lacks */
    struct ent list[SNAP_LIST_MAX];
    unsigned n = collect(home, list);
    for (unsigned i = 0; i < n; i++) {
        if (top_skip(top, list[i].name)) continue;
        if (!list[i].dir && list[i].size > SNAP_FILE_MAX) continue;   /* too big to be in any snapshot: not the snapshot's to remove */
        unsigned s = list[i].dir ? sub_of(snap, list[i].name) : 0;
        int there;
        if (list[i].dir) there = s != 0;
        else { char one[1]; fat_cwd_set(snap); there = fat_read_file(list[i].name, one, 1) >= 0; }
        if (!there) {
            if (list[i].dir) { if (!drop(home, list[i].name)) return 0; }
            else { fat_cwd_set(home); if (!fat_delete(list[i].name)) return 0; }
        } else if (list[i].dir && depth < DEPTH_MAX) {
            unsigned h = sub_of(home, list[i].name);
            if (h && !prune(h, s, depth + 1, 0)) return 0;
        }
    }
    return 1;
}

static int num_of(const char *s, unsigned *v) {   /* a snapshot folder is named by a plain number */
    unsigned x = 0, k = 0;
    for (; s[k]; k++) { if (s[k] < '0' || s[k] > '9' || k > 7) return 0; x = x * 10 + (unsigned)(s[k] - '0'); }
    *v = x; return k > 0;
}
static void put_num(char *out, unsigned v) {
    char t[10]; unsigned n = 0, k = 0;
    do t[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n) out[k++] = t[--n];
    out[k] = 0;
}
static int done_in(unsigned folder, char *info, unsigned cap) {   /* 1 when this snapshot was finished; info is the marker text */
    char t[40];
    fat_cwd_set(folder);
    int got = fat_read_file(DONE, t, sizeof t - 1);
    if (got < 0) return 0;
    unsigned k = 0;
    for (; (int)k < got && k + 1 < cap; k++) info[k] = t[k] == '\n' ? ' ' : t[k];
    info[k] = 0;
    return 1;
}
static int ready(unsigned *home, unsigned *snaps, int make) {   /* the home folder and its SNAP, as handles; a negative answer is the caller's */
    char dir[HOME_DIR_MAX + 1];
    if (!sd_ready()) return -1;
    if (!home_mine(dir)) return -2;
    if (make && !home_make(current_user)) return 0;
    if (!sd_enter_jt(0)) return 0;
    fat_cwd_set(0);
    if (!fat_chdir("JT")) return 0;
    if (!home_in(dir, 0)) { fat_cwd_set(0); return 0; }
    *home = fat_cwd_get();
    *snaps = make ? sub_make(*home, "SNAP") : sub_of(*home, "SNAP");
    fat_cwd_set(0);
    return make && !*snaps ? 0 : 1;
}
static unsigned list_ids(unsigned snaps, unsigned *ids, unsigned cap, int only_done) {   /* ascending */
    struct ent list[SNAP_LIST_MAX];
    unsigned n = snaps ? collect(snaps, list) : 0, m = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned id; char info[24];
        if (!list[i].dir || !num_of(list[i].name, &id) || m >= cap) continue;
        if (only_done) { unsigned s = sub_of(snaps, list[i].name); if (!s || !done_in(s, info, sizeof info)) continue; }
        unsigned j = m++;
        while (j && ids[j - 1] > id) { ids[j] = ids[j - 1]; j--; }
        ids[j] = id;
    }
    return m;
}

int snap_take(unsigned *id, unsigned *files, unsigned *skipped) {
    unsigned home, snaps, ids[SNAP_LIST_MAX], all[SNAP_LIST_MAX];
    int r = ready(&home, &snaps, 1);
    if (r <= 0) return r;
    unsigned nall = list_ids(snaps, all, SNAP_LIST_MAX, 0), nd = list_ids(snaps, ids, SNAP_LIST_MAX, 1);
    unsigned next = nall ? all[nall - 1] + 1 : 1;
    char name[12], marker[40];
    for (unsigned i = 0; i < nall; i++) {   /* a snapshot with no _DONE is one that was cut short */
        int fin = 0; for (unsigned j = 0; j < nd; j++) if (ids[j] == all[i]) fin = 1;
        if (!fin) { put_num(name, all[i]); drop(snaps, name); }
    }
    while (nd >= SNAP_KEEP) { put_num(name, ids[0]); if (!drop(snaps, name)) return 0; for (unsigned j = 1; j < nd; j++) ids[j - 1] = ids[j]; nd--; }
    put_num(name, next);
    unsigned dst = sub_make(snaps, name);
    if (!dst) return 0;
    n_files = n_bytes = n_skipped = 0;
    if (!copy_tree(home, dst, 0, 1)) { drop(snaps, name); fat_cwd_set(0); return 0; }
    unsigned k = 0; char num[12];
    put_num(num, n_files); for (unsigned i = 0; num[i]; i++) marker[k++] = num[i];
    const char *a = " files "; for (; *a; a++) marker[k++] = *a;
    put_num(num, n_bytes); for (unsigned i = 0; num[i]; i++) marker[k++] = num[i];
    const char *b = " bytes\n"; for (; *b; b++) marker[k++] = *b;
    fat_cwd_set(dst);
    int ok = fat_replace_file(DONE, marker, k);
    if (!ok) drop(snaps, name);
    fat_cwd_set(0);
    *id = next; *files = n_files; *skipped = n_skipped;
    return ok;
}
int snap_list(void (*cb)(unsigned id, const char *info)) {
    unsigned home, snaps, ids[SNAP_LIST_MAX];
    int r = ready(&home, &snaps, 0);
    if (r <= 0) return r;
    unsigned n = list_ids(snaps, ids, SNAP_LIST_MAX, 1);
    for (unsigned i = 0; i < n; i++) {
        char name[12], info[24];
        put_num(name, ids[i]);
        unsigned s = sub_of(snaps, name);
        if (s && done_in(s, info, sizeof info)) cb(ids[i], info);
    }
    fat_cwd_set(0);
    return (int)n;
}
int snap_restore(unsigned id, unsigned *files, unsigned *skipped) {
    unsigned home, snaps;
    int r = ready(&home, &snaps, 0);
    if (r <= 0) return r;
    char name[12], info[24];
    put_num(name, id);
    unsigned s = snaps ? sub_of(snaps, name) : 0;
    if (!s || !done_in(s, info, sizeof info)) { fat_cwd_set(0); return -3; }
    n_files = n_bytes = n_skipped = 0;
    int ok = copy_tree(s, home, 0, 1) && prune(home, s, 0, 1);
    fat_cwd_set(0);
    *files = n_files; *skipped = n_skipped;
    return ok;
}
