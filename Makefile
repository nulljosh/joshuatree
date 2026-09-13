CC := clang
CFLAGS := -target i386-unknown-none -ffreestanding -fno-stack-protector \
          -fno-pic -mno-sse -mno-mmx -Wall -Wextra -O2 \
          -Iboot -Ikernel -Idrivers -Ilib
LD := ld.lld

KERNEL_SRCS := kernel/gdt.c kernel/idt.c kernel/pic.c kernel/irq.c kernel/pmm.c \
               kernel/paging.c kernel/kheap.c kernel/task.c kernel/exec.c kernel/kernel.c
KERNEL_ASM  := kernel/isr.S kernel/irq_stubs.S kernel/task_switch.S
DRIVER_SRCS := drivers/ata.c drivers/fat.c drivers/pci.c drivers/vbe.c drivers/mouse.c \
               drivers/window.c drivers/rtl8139.c drivers/net.c
LIB_SRCS    := lib/libc.c

OBJS := boot/boot.o $(KERNEL_ASM:.S=.o) $(KERNEL_SRCS:.c=.o) $(DRIVER_SRCS:.c=.o) $(LIB_SRCS:.c=.o)

kernel.elf: $(OBJS) boot/linker.ld
	$(LD) -m elf_i386 -T boot/linker.ld -o $@ $(OBJS)

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: kernel.elf
	qemu-system-i386 -kernel kernel.elf

clean:
	rm -f $(OBJS) kernel.elf

.PHONY: run clean
