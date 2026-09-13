/* multiboot1 header: QEMU -kernel loads us straight into 32-bit protected mode.
   ponytail: 32-bit, no paging, no long mode. Add a long-mode trampoline when
   you need >4GB or 64-bit registers. */
.set MAGIC,    0x1BADB002
.set FLAGS,    0
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bss
.align 16
stack_bottom: .skip 16384
stack_top:

.section .text
.global _start
_start:
    mov $stack_top, %esp
    push %ebx           /* multiboot info struct, GRUB/QEMU set this per spec */
    call kmain
1:  cli
    hlt
    jmp 1b
