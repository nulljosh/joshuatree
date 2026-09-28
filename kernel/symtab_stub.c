/* Empty symbol table, linked only into kernel.elf.pass1 (see Makefile) so
   pass 1 has something to satisfy kernel/backtrace.c's references to
   kernel_symtab/kernel_symtab_count while every real .text address is
   still being finalized. The real, generated kernel/symtab.c (built from
   pass 1's own `nm -n`) replaces this for the final kernel.elf link --
   this file is never linked into a real, bootable kernel.elf. */
#include "symtab.h"

const struct sym_entry kernel_symtab[] = {};
const unsigned int kernel_symtab_count = 0;
