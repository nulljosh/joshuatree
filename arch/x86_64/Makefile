# x86-64 port, slice 1. Separate from the i386 and ARM64 builds: nothing in those changes.
#   make -C arch/x86_64        kernel64.bin, a flat multiboot image for `qemu-system-x86_64 -kernel`
#   make -C arch/x86_64 run    boots it headless and prints the serial port
CC = clang
LD = ld.lld
CFLAGS = -target x86_64-unknown-none -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mgeneral-regs-only -mcmodel=small -O2 -Wall -Wextra -Werror
all: kernel64.bin
boot.o: boot.S
	$(CC) -target x86_64-unknown-none -c $< -o $@
main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@
kernel64.bin: boot.o main.o linker.ld
	$(LD) -m elf_x86_64 -T linker.ld --oformat binary -o $@ boot.o main.o
run: kernel64.bin
	qemu-system-x86_64 -kernel kernel64.bin -vga std -display none -serial stdio -no-reboot
clean:
	rm -f *.o kernel64.bin
.PHONY: all run clean
