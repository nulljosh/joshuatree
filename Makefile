CC := clang
CFLAGS := -target i386-unknown-none -ffreestanding -fno-stack-protector \
          -fno-pic -mno-sse -mno-mmx -Wall -Wextra -O2
LD := ld.lld

kernel.elf: boot.o gdt.o idt.o isr.o pic.o irq.o irq_stubs.o pmm.o paging.o kheap.o task.o task_switch.o ata.o fat.o exec.o libc.o pci.o vbe.o mouse.o window.o rtl8139.o kernel.o linker.ld
	$(LD) -m elf_i386 -T linker.ld -o $@ boot.o gdt.o idt.o isr.o pic.o irq.o irq_stubs.o pmm.o paging.o kheap.o task.o task_switch.o ata.o fat.o exec.o libc.o pci.o vbe.o mouse.o window.o rtl8139.o kernel.o

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: kernel.elf
	qemu-system-i386 -kernel kernel.elf

clean:
	rm -f *.o kernel.elf

.PHONY: run clean
