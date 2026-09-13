CC := clang
CFLAGS := -target i386-unknown-none -ffreestanding -fno-stack-protector \
          -fno-pic -mno-sse -mno-mmx -Wall -Wextra -O2
LD := ld.lld

kernel.elf: boot.o gdt.o kernel.o linker.ld
	$(LD) -m elf_i386 -T linker.ld -o $@ boot.o gdt.o kernel.o

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: kernel.elf
	qemu-system-i386 -kernel kernel.elf

clean:
	rm -f *.o kernel.elf

.PHONY: run clean
