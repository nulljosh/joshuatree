/* x86-64, slice 1: from the boot loader's 32-bit protected mode into long mode.

   QEMU's -kernel and GRUB both load a multiboot1 image, and neither loads a 64-bit ELF that way. So this kernel is a flat
   binary whose multiboot header carries its own load addresses (flag bit 16, the "a.out kludge"): the loader copies the
   file to 1 MiB, zeroes the bss and jumps to start32 with paging off.

   start32 identity-maps the first 4 GiB with 2 MiB pages (one PML4 entry, one PDPT, four page directories), turns on
   PAE, sets EFER.LME, turns on paging, and far-jumps through a 64-bit code segment into start64, which loads the data
   segments, takes a stack and calls kmain (main.c) with the multiboot magic and info pointer. A CPU without long mode
   says so on the serial port and halts. */
#define MB_MAGIC 0x1BADB002
#define MB_FLAGS 0x00010003   /* page-align modules, memory info, and the address fields below */

.section .multiboot, "a"
.align 4
mb_header:
    .long MB_MAGIC
    .long MB_FLAGS
    .long -(MB_MAGIC + MB_FLAGS)
    .long mb_header          /* header_addr */
    .long _load_start        /* load_addr */
    .long _load_end          /* load_end_addr */
    .long _bss_end           /* bss_end_addr */
    .long start32            /* entry_addr */

.section .text
.code32
.globl start32
start32:
    cli
    movl $stack_top, %esp
    movl %eax, %edi               /* multiboot magic, kept for kmain (first argument) */
    movl %ebx, %esi               /* multiboot info (second argument) */

    movl $0x80000000, %eax        /* is there an extended CPUID leaf with long mode in it? */
    cpuid
    cmpl $0x80000001, %eax
    jb no_long_mode
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz no_long_mode

    /* PML4[0] -> PDPT; PDPT[0..3] -> PD0..PD3; each PD entry a 2 MiB page: present, writable, large */
    movl $pdpt + 3, pml4
    movl $pd + 3, pdpt
    movl $pd + 0x1000 + 3, pdpt + 8
    movl $pd + 0x2000 + 3, pdpt + 16
    movl $pd + 0x3000 + 3, pdpt + 24
    xorl %ecx, %ecx
1:  movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, pd(, %ecx, 8)
    movl %ecx, %eax
    shrl $11, %eax                /* the high half: bits 32 and up of ecx << 21 */
    movl %eax, pd + 4(, %ecx, 8)
    incl %ecx
    cmpl $2048, %ecx
    jne 1b

    movl %cr4, %eax
    orl $(1 << 5), %eax           /* PAE */
    movl %eax, %cr4
    movl $pml4, %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx        /* EFER */
    rdmsr
    orl $(1 << 8), %eax           /* LME */
    wrmsr
    movl %cr0, %eax
    orl $0x80000001, %eax         /* PG, PE */
    movl %eax, %cr0
    lgdt gdt_ptr
    ljmp $0x08, $start64

no_long_mode:                     /* "JT64 no long mode" on COM1, then stop */
    movl $nolm, %esi
2:  lodsb
    testb %al, %al
    jz 3f
    movw $0x3F8, %dx
    outb %al, %dx
    jmp 2b
3:  hlt
    jmp 3b

.code64
start64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movq $stack_top, %rsp
    xorl %ebp, %ebp
    movl %edi, %edi               /* zero-extend the two arguments */
    movl %esi, %esi
    call kmain
4:  hlt
    jmp 4b

.section .rodata
nolm: .asciz "JT64 no long mode\n"
.align 16
gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF      /* 0x08: 64-bit code, ring 0 */
    .quad 0x00CF92000000FFFF      /* 0x10: data, ring 0 */
gdt_ptr:
    .word . - gdt - 1
    .long gdt

.section .bss
.align 4096
pml4: .skip 4096
pdpt: .skip 4096
pd:   .skip 4096 * 4
stack: .skip 16384
stack_top:
