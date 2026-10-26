/* eltests.c: cut out of main.c (the god-file split). The EL0 self-tests that run at boot: a fault, a jump into kernel text, a good exit, arena isolation, and (QEMU only) hello.c, file-call abuse and calendar's page probes. Built as part of main.o (the Makefile's MAINSRC) with
   main.c's own flags; what it shares with main.c is declared in kmain.h. */
#include "kmain.h"
#ifndef PI_BUILD
/* Probe the pages of the app that is loaded, from EL0: user.S's entry D gets { bss word, data page, code base, window size }
   in a block at the bottom of its own stack page and checks them (see user.S). It ends on a store to the code page, which must
   fault; the kernel survives and says so. */
static void user_probe(int pass) {
    unsigned long base = UB + UP_APP, *args = (unsigned long *)(UB + UP_STACK);
    args[0] = base + ((app_img + 15) & ~15UL); args[1] = base + app_code; args[2] = base; args[3] = UAPP_MAX;
    uart_puts("M3c probe "); uart_dec((unsigned)pass); uart_puts(" starts\n");
    user_faulted = 0;
    enter_user(UB + UP_CODE + (unsigned long)(user_entry_d - user_start), UB + UP_STACK + 4096, (unsigned long)args, 0);
    uart_puts(user_faulted ? "M3c probe survived the code write\n" : "M3c probe FAIL: the code write did not fault\n");
}
/* Branch to an address inside the loaded app that is not code: user.S's entry C does the `br`, which must be an instruction
   abort (the pages are never executable), and the kernel carries on. */
static void user_exec_probe(const char *what, unsigned long target) {
    uart_puts("M3c exec "); uart_puts(what); uart_putc(' '); uart_hex(target); uart_putc('\n');
    user_faulted = 0;
    enter_user(UB + UP_CODE + (unsigned long)(user_entry_c - user_start), UB + UP_STACK + 4096, UB + UP_KERN, target);
    uart_puts(user_faulted ? "M3c exec refused\n" : "M3c exec FAIL: it ran\n");
}
#endif
void user_demo(void) {
    /* 4.5: an APP_BOOT build has opened its apps by now (fb_desktop runs first). These tests load programs into slot 0 and its
       low pages, so with apps live they would write over them, or, with `ap` on another slot, into pages that slot never mapped
       (a kernel data abort at the third app's arena on QEMU). They run only on a machine with no app open. */
    if (apps_live()) { uart_puts("M3 self-tests skipped: apps are open\n"); return; }
    ap = &apps[0];
    user_init(user_start, (unsigned long)(user_end - user_start));
    unsigned long code = UB + UP_CODE, sp = UB + UP_STACK + 4096, kpage = UB + UP_KERN;
    uart_puts("M3 EL0 task starts\n");
    enter_user(code + (unsigned long)(user_entry_a - user_start), sp, kpage, 0);
    uart_puts("M3 kernel survived the fault\n");
    uart_puts("M3 EL0 jump task starts\n");   /* branch to kernel text: EL0 may not execute it */
    enter_user(code + (unsigned long)(user_entry_c - user_start), sp, kpage, (unsigned long)_start);
    uart_puts("M3 kernel survived the jump\n");
    uart_puts("M3 EL0 second task starts\n");
    user_status = enter_user(code + (unsigned long)(user_entry_b - user_start), sp, kpage, 0);
    if (user_status == 7) uart_puts("M3 userland ok\n"); else uart_puts("M3 userland FAIL\n");
    {   /* 4.4: slot 1 gets one page EL0 could use, and from slot 0 user.S's entry F loads from it, which must fault */
        const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10), AP_EL0_RW = 1UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
        unsigned long pg = apps[1].base + UP_STACK, *e = &l3[apps[1].l3off + (UP_STACK >> 12)];
        *e = pg | PAGE | AP_EL0_RW | PXN | UXN;
        arena_only(0);
        uart_puts("M4 arena probe starts\n");
        user_faulted = 0;
        enter_user(code + (unsigned long)(user_entry_f - user_start), sp, pg, 0);
        uart_puts(user_faulted ? "M4 arena isolation ok\n" : "M4 arena isolation FAIL: slot 1 was readable from slot 0\n");
        *e = 0;
        __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    }
#ifndef PI_BUILD
    /* M3b: user/hello.c itself, built for aarch64 and talking Joshua Tree syscall numbers over svc. This runs before any
       card is mounted, so since M3d its open of HELLO.TXT is answered -ENOENT (-2), which hello.c reports and answers with
       exit 1. The kernel takes that exit as the pass. With a card holding JT/HELLO.TXT, `userrun hello` goes all the way
       to exit 9 (tools/checks/arm64-m3c-check.py). */
    const struct user_app *hello = user_find("hello", 5), *cal = user_find("calendar", 8);
    uart_puts("M3b EL0 C task starts\n");
    if (hello && !user_run(hello) && !user_faulted && user_status == 1) uart_puts("M3b userland C ok\n"); else uart_puts("M3b userland C FAIL\n");
    /* M3d: user.S's entry E abuses the file calls once each: a kernel pointer as a path, a read into the kernel-only page,
       a path with "..", a 4 GB readfile, a readdir whose records run past its page. Each must come back an errno (the
       checks run before any card is touched), then it exits 0 and the kernel carries on. */
    uart_puts("M3d file call abuse starts\n");
    user_faulted = 0;
    user_status = enter_user(code + (unsigned long)(user_entry_e - user_start), sp, kpage, 0);
    uart_puts(!user_faulted && user_status == 0 ? "M3d file call abuse ok\n" : "M3d file call abuse FAIL\n");
    /* M3c: user/calendar.c, about 34 KB with relocated pointer tables, a data page and a zeroed pool. It opens its window
       first; run here, outside the scheduler, there is none to give (-ENODEV, as i386 answers a program run from the text
       shell), so it writes "calendar: no window" to fd 2 and exits 1, which is the pass. Then
       user.S's probe runs against calendar's pages, twice with a reload between: the second time proves the loader zeroes
       the pool again and puts the data back. */
    uart_puts("M3c EL0 calendar starts\n");
    if (cal && !user_run(cal) && !user_faulted && user_status == 1) uart_puts("M3c userland calendar ok\n"); else uart_puts("M3c userland calendar FAIL\n");
    if (cal) {
        user_probe(1);
        if (user_load(cal)) user_probe(2); else uart_puts("M3c probe FAIL: calendar did not load again\n");
        unsigned long base = UB + UP_APP;   /* never executable: a data page, the first whole page of the zeroed pool, the stack */
        user_exec_probe("data", base + app_code);
        user_exec_probe("pool", base + (app_img + 4095) / 4096 * 4096);
        user_exec_probe("stack", UB + UP_ASTACK);
    }
#endif
}
