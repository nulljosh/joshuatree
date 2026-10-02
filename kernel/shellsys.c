/* SYS_SHELL_RUN's shell: the few text-mode commands a ring-3 Terminal may run.

   Why this is not run(): the syscall executes inside the int 0x80 gate on the
   caller's 4KB kernel stack with IF clear. run() is one huge function with
   4KB buffers in its frame (cat alone has char buf[4096]) and commands that
   sleep, wait on the network, open windows or halt the machine, none of which
   may happen here. So the window terminal gets its own tiny dispatcher with an
   explicit allowlist, static buffers only, the same wording the text shell
   uses, and the text-mode shell keeps run() untouched.

   Allowed: help echo uptime mem ps ls cat. Everything else, including every
   command that blocks (sleep, bench, web, say, chat...), waits on the network
   (nettest, netscan, ifconfig...), opens a GUI app or window (gui, browse,
   notes...), reboots or halts, or re-enters the window system, is refused
   with a one-line message. cd is refused too: a syscall must not move the
   desktop's cwd. The terminal keeps its own cwd and sends it with each line;
   ls and cat resolve against it with path_enter/path_leave, which restore the
   desktop's cwd exactly. */
#include "shellsys.h"
#include "vfs.h"
#include "task.h"
#include "pmm.h"
#include "irq.h"

#define SH_OUT_MAX 2048
#define SH_PATH_MAX 63
extern int path_enter(char *path, int keep, char **leaf, int *depth);
extern int path_leave(int depth);
static char sh_out[SH_OUT_MAX];
static unsigned int sh_len;
static char sh_file[SH_OUT_MAX];
/* `panicdesk` boot flag (r3stress_arm): turns on a deliberate `crash` command
   so tools/checks/panic-check.py can fault ring 0 under a live desktop from
   the real Terminal. Unarmed, `crash` is refused like any other command. */
int shell_crash_armed;

static void sh_putc(char c) { if (sh_len + 1 < SH_OUT_MAX) sh_out[sh_len++] = c; }
static void sh_puts(const char *s) { while (*s) sh_putc(*s++); }
static void sh_putn(unsigned int v) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) sh_putc(t[--n]);
}
static int sh_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void sh_ls_cb(const char *name, unsigned int size, int is_dir) {
    sh_puts(name); if (is_dir) sh_putc('/');
    sh_puts("  "); sh_putn(size); sh_puts(" bytes\n");
}

/* line is a kernel copy, NUL terminated, already length bounded: "<cwd>\n<command>".
   The cwd is the caller's own, a relative path from the root ("" = root); the
   desktop's cwd is never moved (path_enter/path_leave restore it). */
unsigned int shellsys_run(char *line, char **out) {
    sh_len = 0;
    char *cwd = "", *nl = line;
    while (*nl && *nl != '\n') nl++;
    if (*nl) { *nl = 0; cwd = line; line = nl + 1; } /* no newline: root, whole line is the command */
    while (*line == ' ') line++;
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;
    while (*arg == ' ') arg++;

    if (!*line) {
    } else if (sh_eq(line, "help")) {
        sh_puts("help echo uptime mem ps ls cat\n"
                "other shell commands need the text-mode shell or the desktop\n");
    } else if (sh_eq(line, "echo")) { sh_puts(arg); sh_putc('\n'); }
    else if (shell_crash_armed && sh_eq(line, "crash")) __asm__ volatile ("int $3");
    else if (sh_eq(line, "uptime")) { sh_putn(ticks() / 100); sh_puts("s\n"); }
    else if (sh_eq(line, "mem")) {
        sh_putn(pmm_free_frames() * 4); sh_puts("K free / ");
        sh_putn(pmm_total_frames() * 4); sh_puts("K total (4K frames)\n");
    } else if (sh_eq(line, "ps")) {
        for (int i = 0; i < task_max(); i++) {
            sh_putn((unsigned int)i); sh_putc(' ');
            sh_puts(task_used(i) ? "used\n" : "free\n");
        }
    } else if (sh_eq(line, "ls") || sh_eq(line, "cat")) {
        /* cwd (caller's, relative to the root) joined with the argument. */
        char path[SH_PATH_MAX + 1];
        unsigned int pn = 0, i;
        int ok = 1;
        for (i = 0; cwd[i]; i++) { if (pn >= SH_PATH_MAX) ok = 0; else path[pn++] = cwd[i]; }
        if (*arg && pn) { if (pn >= SH_PATH_MAX) ok = 0; else path[pn++] = '/'; }
        for (i = 0; arg[i]; i++) { if (pn >= SH_PATH_MAX) ok = 0; else path[pn++] = arg[i]; }
        path[pn] = 0;
        char *leaf; int depth;
        if (!ok) sh_puts("path too long\n");
        else if (line[0] == 'l') {
            if (path_enter(path, 0, &leaf, &depth) < 0) sh_puts("ls: no such folder\n");
            else { vfs_list(sh_ls_cb); path_leave(depth); }
        } else if (!*arg) sh_puts("usage: cat <file>\n");
        else if (path_enter(path, 1, &leaf, &depth) < 0) { sh_puts(arg); sh_puts(": not found\n"); }
        else {
            int n = vfs_read_file(leaf, sh_file, sizeof(sh_file) - 1);
            path_leave(depth);
            if (n < 0) { sh_puts(arg); sh_puts(": not found\n"); }
            else {
                for (int j = 0; j < n; j++) {
                    char c = sh_file[j];
                    sh_putc(c == '\n' || (c >= 32 && c < 127) ? c : '.');
                }
                sh_putc('\n');
            }
        }
    } else {
        sh_puts(line);
        sh_puts(": not available in the window terminal (allowed: help echo uptime mem ps ls cat)\n");
    }
    sh_out[sh_len] = 0;
    *out = sh_out;
    return sh_len;
}
