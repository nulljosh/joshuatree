/* Crash-report symbol table. kernel/symtab.c is GENERATED (see Makefile's
   two-pass link and tools/gen/gen_symtab.py) from `nm -n` on a first-pass
   kernel.elf, then compiled and linked into the real one -- never hand
   edited, never committed (see .gitignore, same relationship
   drivers/version.h has to VERSION). This header and backtrace.c, which
   walk it, are the committed, hand-written half. */
#ifndef JT_SYMTAB_H
#define JT_SYMTAB_H

struct sym_entry {
    unsigned int addr;
    const char *name;
};

/* Sorted ascending by addr (nm -n's own order), text symbols only. */
extern const struct sym_entry kernel_symtab[];
extern const unsigned int kernel_symtab_count;

/* Largest symbol whose address is <= addr, with the byte offset into it.
   Returns 0 if addr falls before the first known symbol or the table is
   empty (the pass-1 stub always has kernel_symtab_count == 0). */
const char *symtab_lookup(unsigned int addr, unsigned int *offset);

/* Prints "at <func>+0x<off>" for eip, then walks the EBP frame-pointer
   chain a few frames further, over both serial and the text console.
   Requires -fno-omit-frame-pointer (see Makefile). */
void backtrace_print(unsigned int eip, unsigned int ebp);

/* Deliberately faults (int3) so a headless check can prove the backtrace
   names this exact function. Reachable only behind the "panictest"
   multiboot command-line flag -- see kernel/kernel.c's kmain -- never
   during a normal boot. */
void jt_panic_test_target(void);

#endif
