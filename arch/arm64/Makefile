# ARM64 port, M0. Separate from the i386 Makefile on purpose: nothing in that build changes.
#   make -C arch/arm64        builds kernel8.elf
#   make -C arch/arm64 run    boots it headless under qemu-system-aarch64 and prints the UART
CC = clang -target aarch64-none-elf -ffreestanding -fno-stack-protector -mgeneral-regs-only -O2 -Wall -Wextra
all: kernel8.elf
start.o: start.S
	$(CC) -c $< -o $@
main.o: main.c
	$(CC) -c $< -o $@
kernel8.elf: start.o main.o linker.ld
	ld.lld -T linker.ld -o $@ start.o main.o
run: kernel8.elf
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256 -nographic -kernel kernel8.elf
clean:
	rm -f *.o kernel8.elf
