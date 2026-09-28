/* Crash-report backtrace: binary search over the generated symbol table
   (kernel/symtab.c) plus an EBP frame-pointer walk. See symtab.h. */
#include "symtab.h"
#include "console.h"
#include "serial.h"

static void hex8(unsigned int v, char *out /* 9 bytes incl NUL */) {
    static const char *hexd = "0123456789ABCDEF";
    for (int j = 0; j < 8; j++) out[j] = hexd[(v >> (28 - j * 4)) & 0xF];
    out[8] = 0;
}

const char *symtab_lookup(unsigned int addr, unsigned int *offset) {
    if (kernel_symtab_count == 0) return 0;
    int lo = 0, hi = (int)kernel_symtab_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kernel_symtab[mid].addr <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return 0;
    if (offset) *offset = addr - kernel_symtab[best].addr;
    return kernel_symtab[best].name;
}

static void print_frame(unsigned int addr) {
    unsigned int off = 0;
    const char *name = symtab_lookup(addr, &off);
    char hex[9];
    hex8(off, hex);
    puts("  at "); serial_puts("  at ");
    puts(name ? name : "?"); serial_puts(name ? name : "?");
    puts("+0x"); serial_puts("+0x");
    puts(hex); serial_puts(hex);
    puts("\n"); serial_puts("\n");
}

void backtrace_print(unsigned int eip, unsigned int ebp) {
    print_frame(eip);
    unsigned int fp = ebp;
    /* Kernel stacks live in the higher half (boot/linker.ld links the
       kernel at 0xC0000000+); an ebp below that, unaligned, or a chain
       that stops growing means end-of-chain or a corrupt frame, not a
       real caller -- stop rather than walking into garbage. */
    for (int i = 0; i < 6; i++) {
        if (fp < 0xC0000000 || (fp & 0x3)) break;
        unsigned int ret_addr = *(unsigned int *)(fp + 4);
        unsigned int saved_fp = *(unsigned int *)fp;
        if (ret_addr < 0xC0000000) break;
        print_frame(ret_addr);
        if (saved_fp <= fp) break;
        fp = saved_fp;
    }
}

void jt_panic_test_target(void) {
    /* int3 (breakpoint, vector 3), not a null-pointer write: this runs
       before paging_install() sets up page-level protection, and a plain
       memory write to address 0 while paging is off is real, unprotected
       low memory -- it succeeds silently instead of faulting. int3 is a
       CPU exception unconditionally, with or without paging, so this
       reliably reaches isr_handler's ring-0 path every time. */
    __asm__ volatile ("int $3");
}
