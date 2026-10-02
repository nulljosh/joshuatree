/* brkpoke: grows its heap by 3MB through SYS_BRK, touches every page, then
 * crashes on purpose, so tools/checks/ring3brk-check.py can prove the
 * kernel hands every frame back (brk: released ... live=0) and the PMM
 * free count returns to its baseline. Also probes the fence: a top below
 * the base and one past the cap must both be -EINVAL. */
#include "jtsys.h"
#include "../kernel/memmap.h"
#define PAGES 768u

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
    put("brkpoke: start\n");
    int base = jt_brk(0);
    if (base != (int)JT_BRK_BASE) { put_num("brkpoke: BUG fresh brk is not JT_BRK_BASE, got ", base); jt_exit(2); }
    put_num("brkpoke: below base returned ", jt_brk(JT_BRK_BASE - 4096u));
    put_num("brkpoke: past cap returned ", jt_brk(JT_BRK_BASE + (JT_BRK_MAX_PAGES + 1) * 4096u));
    int top = jt_brk(JT_BRK_BASE + PAGES * 4096u);
    if (top != (int)(JT_BRK_BASE + PAGES * 4096u)) { put_num("brkpoke: BUG grow returned ", top); jt_exit(3); }
    volatile unsigned *h = (volatile unsigned *)JT_BRK_BASE;
    unsigned nz = 0;
    for (unsigned p = 0; p < PAGES; p++) { if (h[p * 1024]) nz++; h[p * 1024] = p + 1; }
    unsigned bad = 0;
    for (unsigned p = 0; p < PAGES; p++) if (h[p * 1024] != p + 1) bad++;
    put_num("brkpoke: touched pages ", (int)PAGES);
    put_num("brkpoke: dirty pages seen ", (int)nz);
    put_num("brkpoke: readback mismatches ", (int)bad);
    int shrunk = jt_brk(JT_BRK_BASE + (PAGES / 2) * 4096u);
    put_num("brkpoke: shrink returned ", shrunk);
    put("brkpoke: crashing on purpose\n");
    *(volatile unsigned *)0 = 1;
    put("brkpoke: BUG still alive after the null store\n");
    jt_exit(4);
}
