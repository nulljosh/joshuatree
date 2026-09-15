/* multiboot1 header: QEMU -kernel loads us straight into 32-bit protected mode.
   ponytail: 32-bit, no paging, no long mode. Add a long-mode trampoline when
   you need >4GB or 64-bit registers.

   Higher-half kernel: everything from here down to higher_half_entry runs
   BEFORE paging is on, with paging off there is no virtual/physical split
   yet, so this code has to live at an address that's valid either way. It
   stays in .text.boot, deliberately NOT relocated to the 0xC0000000 base
   the rest of the kernel uses (see linker.ld). Its only job is: build a
   temporary page directory that maps the first 4MB both identity (0..4MB,
   needed since EIP is still a low address the instant paging turns on)
   and at the kernel's real higher-half base (0xC0000000..0xC0400000, so
   the higher_half_entry jump below lands somewhere mapped), turn paging
   on, and jump into the real, higher-half-linked kernel. paging_install()
   (paging.c) throws this away and builds the kernel's permanent page
   tables once kmain is running for real. */
.set MAGIC,    0x1BADB002
.set FLAGS,    0
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .text.boot, "ax"
.align 4096
boot_page_directory:
    .skip 4096
/* v74 (0.66.0): two consecutive page tables, 8MB, not one. The kernel
   image (1MB load address + text + the 1.5MB baked wallpaper + .bss)
   crossed the 4MB line for the first time when the PNG decoder's test
   fixtures landed: _kernel_end went to 0x40f000, and the first .bss touch
   after irq_install page-faulted on memory no table covered. The image
   had been sitting ~50KB under that cliff for several releases already,
   so any next addition of static data would have tripped it. paging.c's
   BASE_MAP_TABLES must match this count. */
boot_page_table:
    .skip 8192

.align 16
boot_stack_bottom:
    .skip 4096
boot_stack_top:

.global _start
_start:
    mov $boot_stack_top, %esp

    /* identity-map the first 8MB: entry i maps physical page i to virtual
       page i, present + read/write, across two back-to-back tables */
    mov $boot_page_table, %edi
    mov $0, %eax
    mov $2048, %ecx
1:  mov %eax, %edx
    or $0x3, %edx
    mov %edx, (%edi)
    add $0x1000, %eax
    add $4, %edi
    loop 1b

    /* two page tables, each mapped at two page-directory slots: entries
       0/1 (the identity range, 0..8MB) and entries 768/769 (0xC0000000 /
       4MB and the next), the kernel's own higher-half range. Same
       physical memory, reachable both ways during the transition below. */
    mov $boot_page_table, %eax
    or $0x3, %eax
    mov $boot_page_directory, %edi
    mov %eax, (%edi)
    mov %eax, 768*4(%edi)
    add $4096, %eax
    mov %eax, 4(%edi)
    mov %eax, 769*4(%edi)

    mov $boot_page_directory, %eax
    mov %eax, %cr3

    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    /* still running from the low, identity-mapped alias at this exact
       instruction; this jump's target is linked at the high address (see
       linker.ld), and since it's a normal relative jump, the displacement
       the linker computed already accounts for that, landing EIP on the
       high address in one step, no indirection needed */
    jmp higher_half_entry

.section .text
higher_half_entry:
    mov $stack_top, %esp   /* the kernel's real, higher-half-linked stack */
    push %ebx              /* multiboot info struct, GRUB/QEMU set this per spec */
    call kmain
1:  cli
    hlt
    jmp 1b

.section .bss
.align 16
stack_bottom: .skip 16384
stack_top:
