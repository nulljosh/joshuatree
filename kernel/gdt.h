#ifndef GDT_H
#define GDT_H
void gdt_install(void);
void gdt_set_kernel_stack(unsigned int esp0); /* v64: TSS esp0, the kernel stack a ring-3 trap lands on; 0 = the boot static */
#endif
