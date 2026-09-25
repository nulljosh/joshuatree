/* wc: counts lines, words and bytes, the way Unix wc does.
 *
 *     wc FILE     count FILE
 *     wc          count stdin
 *
 * The first program in this repo built on libjt instead of raw jtsys.h
 * calls: it links user/libjt.a and uses its stdio.h snprintf() to format
 * the summary line, so it is also the proof that libjt's printf family
 * works end to end against the real syscalls, not just the host harness.
 */
#include "libjt/stdio.h"
#include "libjt/string.h"
#include "jtsys.h"

static void put_counts(long lines, long words, long bytes, const char *name) {
    char line[128];
    int n = snprintf(line, sizeof line, "%7ld %7ld %7ld", lines, words, bytes);
    if (name) n += snprintf(line + n, sizeof line - (unsigned)n, " %s", name);
    n += snprintf(line + n, sizeof line - (unsigned)n, "\n");
    jt_write(1, line, (unsigned)n);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : 0;

    long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[255];

    int fd;
    if (path) {
        fd = jt_open(path, JT_O_RDONLY);
        if (fd < 0) {
            char line[80];
            int n = snprintf(line, sizeof line, "wc: cannot open %s, errno %d\n", path, -fd);
            jt_write(2, line, (unsigned)n);
            jt_exit(1);
        }
    } else {
        fd = 0;
    }

    for (;;) {
        int n = jt_read(fd, buf, sizeof buf);
        if (n <= 0) break;
        bytes += n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }

    if (path) jt_close(fd);
    put_counts(lines, words, bytes, path);
    jt_exit(0);
}
