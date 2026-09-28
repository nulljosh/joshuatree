/* fbpoke: the program that must not work.
 *
 * Runs right after a windowed program has exited, with no window of its
 * own, and tries the two things 1.7.7 left open: a syscall handed a
 * pointer into the released window framebuffer, and a plain store into
 * it. The first must come back -EFAULT (14), the second must be a page
 * fault the kernel reaps. It also hands write() a pointer into kernel
 * text, which must be -EFAULT too, never an echo of kernel bytes.
 *
 * Every line here is asserted by tools/checks/userfb-release-check.py off
 * the serial log. A "BUG" line from this program is a failed check. The
 * two addresses are copied from kernel/exec.h on purpose: this program
 * must not include a kernel header, and if the layout moves the check
 * fails loudly rather than testing the wrong page.
 */
#include "jtsys.h"

#define KERNEL_TEXT 0xC0100000u /* inside the kernel image, never user */
#define USER_FB     0xC0520000u /* kernel/exec.h JT_USER_FB */
#define EFAULT      14

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { jt_write(1, s, slen(s)); }

static void put_num(const char *label, int v) {
    char line[80], tmp[12];
    unsigned i = 0, neg = v < 0, u = neg ? (unsigned)(-v) : (unsigned)v;
    int t = 0;
    for (const char *p = label; *p && i < 60; p++) line[i++] = *p;
    if (neg) line[i++] = '-';
    do { tmp[t++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (t) line[i++] = tmp[--t];
    line[i++] = '\n';
    jt_write(1, line, i);
}

__attribute__((section(".text.start"), used))
void _start(void) {
    put("fbpoke: start, no window\n");

    int r = jt_write(1, (const void *)KERNEL_TEXT, 8);
    put_num("fbpoke: kernel pointer write returned ", r);
    if (r != -EFAULT) { put("fbpoke: BUG kernel accepted a kernel pointer\n"); jt_exit(1); }

    r = jt_write(1, (const void *)USER_FB, 8);
    put_num("fbpoke: released framebuffer pointer returned ", r);
    if (r != -EFAULT) { put("fbpoke: BUG kernel accepted a pointer into the released framebuffer\n"); jt_exit(2); }

    put("fbpoke: storing into the released framebuffer\n");
    *(volatile unsigned *)USER_FB = 0xDEADBEEFu;
    put("fbpoke: BUG store into the released framebuffer succeeded\n");
    jt_exit(3);
}
