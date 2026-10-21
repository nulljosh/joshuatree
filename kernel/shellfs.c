/* The shell's file commands: ls, cat, write, rm (into the trash), trash, restore, cd, mkdir, the text-mode browse,
 and fsuse/diskuse to switch the filesystem or disk underneath. Moved out of kernel.c's run() in 4.18.7; run() hands
 every line here after the self-tests. */
#include "libc.h"
#include "console.h"
#include "serial.h"
#include "vfs.h"
#include "blockdev.h"
#include "trash.h"
#include "app.h"
#include "selftest.h"

static void ls_cb(const char *name, unsigned int size, int is_dir) {
    puts(name); if (is_dir) putc('/');
    puts("  "); putn(size); puts(" bytes\n");
}

/* ---- text-mode file browser: arrow keys + Enter/Esc, not just a shell.
   ponytail: capped at BROWSE_MAX entries -- plenty for what fits on a
   25-line screen anyway. Enter on a directory descends into it (fat_chdir
   + refresh); esc/q at any depth just quits back to the shell, not up a
   level, matching browse's existing "in or out" model rather than growing
   its own breadcrumb stack. ---- */
#define BROWSE_MAX 20
static char browse_names[BROWSE_MAX][13];
static unsigned int browse_sizes[BROWSE_MAX];
static int browse_is_dir[BROWSE_MAX];
static int browse_count;

static void browse_collect_cb(const char *name, unsigned int size, int is_dir) {
    if (browse_count >= BROWSE_MAX) return;
    int i = 0;
    while (name[i] && i < 12) { browse_names[browse_count][i] = name[i]; i++; }
    browse_names[browse_count][i] = 0;
    browse_sizes[browse_count] = size;
    browse_is_dir[browse_count] = is_dir;
    browse_count++;
}

static void browse_draw(int sel){
    clear();
    puts("-- file browser: up/down, enter=view/open, esc=quit --\n\n");
    if (browse_count == 0) { puts("(empty)\n"); return; }
    for (int i = 0; i < browse_count; i++) {
        putc(i == sel ? '>' : ' '); putc(' ');
        puts(browse_names[i]);
        if (browse_is_dir[i]) { putc('/'); putc('\n'); }
        else { puts("  "); putn(browse_sizes[i]); puts(" bytes\n"); }
    }
}

static void browse(void){
    browse_count = 0;
    vfs_list(browse_collect_cb);
    int sel = 0;
    browse_draw(sel);
    for (;;) {
        int k = get_key();
        if (k == KEY_ESC || k == 'q') { clear(); return; }
        if (k == KEY_UP)   { if (sel > 0) sel--; browse_draw(sel); }
        if (k == KEY_DOWN) { if (sel < browse_count - 1) sel++; browse_draw(sel); }
        if (k == KEY_ENTER && browse_count > 0 && browse_is_dir[sel]) {
            vfs_chdir(browse_names[sel]);
            browse_count = 0;
            vfs_list(browse_collect_cb);
            sel = 0;
            browse_draw(sel);
        }
        else if (k == KEY_ENTER && browse_count > 0) {
            clear();
            puts(browse_names[sel]); puts(":\n\n");
            char buf[2048];
            int n = vfs_read_file(browse_names[sel], buf, sizeof(buf) - 1);
            if (n < 0) puts("(couldn't read)\n");
            else { buf[n] = 0; puts(buf); }
            puts("\n\n-- press any key to go back --\n");
            get_key();
            browse_draw(sel);
        }
    }
}

int shellfs_run(const char *line, char *arg) {
    if (0) {}
    else if (!strcmp(line, "fsuse")) {
        if (!*arg) { puts("current: "); puts(vfs_current_name()); puts(" (usage: fsuse fat|ramfs)\n"); }
        else puts(vfs_switch(arg) ? "switched\n" : "no such backend\n");
    }
    else if (!strcmp(line, "diskuse")) {
        if (!*arg) { puts("current: "); puts(blockdev_current_name()); puts(" (usage: diskuse ata|ramdisk; run 'fsuse fat' then 'disktest'/'ls' after switching to see it take effect)\n"); }
        else puts(blockdev_switch(arg) ? "switched\n" : "no such backend\n");
    }
    else if (!strcmp(line, "ls"))    vfs_list(ls_cb);
    else if (!strcmp(line, "browse")) browse();
    else if (!strcmp(line, "cat")) {
        if (!*arg) { puts("usage: cat <file>\n"); }
        else {
            char buf[4096];
            int n = vfs_read_file(arg, buf, sizeof(buf) - 1);
            /* v1.0.10: a named serial marker with the real returned length,
               so tools/checks/filerobust-check.py can prove an empty file
               reads back as n=0 and an oversized (or corrupted: a lying
               file_size field plus a FAT chain that loops back on itself)
               file comes back clamped to this buffer's own 4095-byte
               capacity, not whatever its directory entry or cluster chain
               claims -- headless, with no screen to scrape. fat_read_file's
               own `remaining = min(file_size, bufsize)` already bounds the
               copy regardless of what the on-disk metadata says, so this
               is a proof marker, not a fix; see that function's own
               comments in drivers/fat.c for why a corrupted size/chain
               can't run past bufsize here. */
            serial_puts("cat "); serial_puts(arg); serial_puts(": ");
            if (n < 0) serial_puts("not found\n");
            else {
                char nb[8]; int ni = 0; unsigned int un = (unsigned int)n;
                if (un == 0) nb[ni++] = '0';
                while (un) { nb[ni++] = (char)('0' + un % 10); un /= 10; }
                nb[ni] = 0;
                for (int j = 0; j < ni / 2; j++) { char t = nb[j]; nb[j] = nb[ni - 1 - j]; nb[ni - 1 - j] = t; }
                serial_puts("n="); serial_puts(nb); serial_puts("\n");
            }
            if (n < 0) { puts(arg); puts(": not found\n"); }
            else { buf[n] = 0; puts(buf); putc('\n'); }
        }
    }
    else if (!strcmp(line, "rm")) {
        if (!*arg) { puts("usage: rm <file>\n"); }
        else {
            /* v39: read it before deleting so it can be recovered. A file
               too big for the trash, or a full trash, still deletes, but
               says so plainly rather than implying it's recoverable. */
            static char rmbuf[TRASH_MAX_SIZE];
            int n = vfs_read_file(arg, rmbuf, sizeof(rmbuf));
            int kept = (n > 0) && trash_put(arg, rmbuf, (unsigned int)n);
            if (!vfs_delete(arg)) puts("not found\n");
            else puts(kept ? "moved to trash\n" : "deleted permanently (too big for trash, or trash full)\n");
        }
    }
    else if (!strcmp(line, "trash")) {
        int n = trash_count();
        if (!n) { puts("trash is empty\n"); }
        else for (int i = 0; i < n; i++) {
            putn((unsigned int)i); puts(" "); puts(trash_name(i));
            puts("  "); putn(trash_size(i)); puts(" bytes\n");
        }
    }
    else if (!strcmp(line, "restore")) {
        if (!*arg) { puts("usage: restore <number, see trash>\n"); }
        else {
            int id = 0; const char *p = arg; while (*p >= '0' && *p <= '9') { id = id*10 + (*p-'0'); p++; }
            puts(trash_restore(id) ? "restored\n" : "could not restore (bad number, or the write failed)\n");
        }
    }
    else if (!strcmp(line, "cd")) {
        if (!*arg) { puts("usage: cd <dir> (or ..)\n"); }
        else { puts(vfs_chdir(arg) ? "ok\n" : "not found or not a directory\n"); }
    }
    else if (!strcmp(line, "mkdir")) {
        if (!*arg) { puts("usage: mkdir <name>\n"); }
        else { puts(vfs_mkdir(arg) ? "created\n" : "failed (name taken, disk full, or directory full)\n"); }
    }
    else if (!strcmp(line, "write")) {
        if (!*arg) { puts("usage: write <file> <content>\n"); }
        else {
            char *content = arg;
            while (*content && *content != ' ') content++;
            if (*content) *content++ = 0;
            int ok = vfs_write_file(arg, content, strlen(content));
            /* v1.0.10: named serial marker so a full-disk write failure
               (alloc_cluster's free-cluster scan in drivers/fat.c coming up
               empty) can be told apart from a hang, headless, and so the
               very next command in the same boot can be checked for a real
               "ok" -- proof the shell is still responsive, not just that
               this one call returned. See tools/checks/filerobust-check.py. */
            serial_puts("write "); serial_puts(arg); serial_puts(ok ? ": ok\n" : ": failed\n");
            puts(ok ? "written\n" : "failed (name taken or disk full)\n");
        }
    }
    else return 0;
    return 1;
}
