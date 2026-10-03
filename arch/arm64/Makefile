# ARM64 port. Separate from the i386 Makefile on purpose: nothing in that build changes.
#   make -C arch/arm64            kernel8.elf for QEMU's virt machine (image at 0x40000000)
#   make -C arch/arm64 run        boots it headless and prints the UART
#   make -C arch/arm64 pi         kernel8.img for a Raspberry Pi 4 (image at 0x80000, UART at 0xFE201000)
#   make -C arch/arm64 run-pi     boots that image on QEMU's raspi4b model
CC = clang -target aarch64-none-elf -ffreestanding -fno-stack-protector -mgeneral-regs-only -O2 -Wall -Wextra
all: kernel8.elf
start.o: start.S
	$(CC) -c $< -o $@
main.o: main.c
	$(CC) -c $< -o $@
pi-start.o: start.S
	$(CC) -c $< -o $@
pi-main.o: main.c
	$(CC) -DPI_BUILD -c $< -o $@
kernel8.elf: start.o main.o linker.ld
	ld.lld --defsym=KBASE=0x40000000 -T linker.ld -o $@ start.o main.o
pi-kernel8.elf: pi-start.o pi-main.o linker.ld
	ld.lld --defsym=KBASE=0x80000 -T linker.ld -o $@ pi-start.o pi-main.o
kernel8.img: pi-kernel8.elf
	ld.lld --defsym=KBASE=0x80000 -T linker.ld --oformat binary -o $@ pi-start.o pi-main.o
pi: kernel8.img
run: kernel8.elf
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256 -nographic -kernel kernel8.elf
run-pi: kernel8.img
	qemu-system-aarch64 -machine raspi4b -nographic -kernel kernel8.img
clean:
	rm -f *.o *.elf kernel8.img
