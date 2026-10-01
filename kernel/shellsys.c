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
   desktop's cwd, so the terminal always works at the root. ls and cat run
   there and put the cwd back exactly as they found it. */
#include "shellsys.h"
#include "vfs.h"
#include "task.h"
#include "pmm.h"
#include "irq.h"

#define SH_OUT_MAX 2048
static char sh_out[SH_OUT_MAX];
static unsigned int sh_len;
static char sh_file[SH_OUT_MAX];

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

/* line is a kernel copy, NUL terminated, already length bounded. */
unsigned int shellsys_run(char *line, char **out) {
    sh_len = 0;
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
        unsigned int saved = vfs_cwd_get();
        vfs_cwd_set(0); /* the desktop may be standing inside NOTES/; this starts at the root */
        if (line[0] == 'l') vfs_list(sh_ls_cb);
        else if (!*arg) sh_puts("usage: cat <file>\n");
        else {
            int slash = 0;
            for (const char *p = arg; *p; p++) if (*p == '/') slash = 1;
            if (slash) sh_puts("cat: root files only, no folders\n");
            else {
                int n = vfs_read_file(arg, sh_file, sizeof(sh_file) - 1);
                if (n < 0) { sh_puts(arg); sh_puts(": not found\n"); }
                else {
                    for (int i = 0; i < n; i++) {
                        char c = sh_file[i];
                        sh_putc(c == '\n' || (c >= 32 && c < 127) ? c : '.');
                    }
                    sh_putc('\n');
                }
            }
        }
        vfs_cwd_set(saved);
    } else {
        sh_puts(line);
        sh_puts(": not available in the window terminal (allowed: help echo uptime mem ps ls cat)\n");
    }
    sh_out[sh_len] = 0;
    *out = sh_out;
    return sh_len;
}
