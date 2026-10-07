# ARM64 port. Separate from the i386 Makefile on purpose: nothing in that build changes.
#   make -C arch/arm64            kernel8.elf for QEMU's virt machine (image at 0x40000000)
#   make -C arch/arm64 run        boots it headless and prints the UART
#   make -C arch/arm64 pi         kernel8.img for a Raspberry Pi 4 (image at 0x80000, UART at 0xFE201000)
#   make -C arch/arm64 run-pi     boots that image on QEMU's raspi4b model
#   make -C arch/arm64 NETPORT=8080   also POSTs to the host (10.0.2.2:8080) at boot, through the shared IP stack
# The Console's ask> prompt asks the Claude relay. claude_cfg.sh writes its address and token (gitignored) on every build
# from CLAUDE_RELAY_HOST, CLAUDE_RELAY_PORT and CLAUDE_RELAY_TOKEN_FILE (default ~/.claude-relay-token); no file, no token.
CC = clang -target aarch64-none-elf -ffreestanding -fno-stack-protector -mgeneral-regs-only -O2 -Wall -Wextra
# ttf.c, text.c and libc.c are the one part that uses floating point (the TrueType rasterizer), so they skip
# -mgeneral-regs-only; start.S turns the FPU on at EL1. Everything else stays integer-only.
FPCC = clang -target aarch64-none-elf -ffreestanding -fno-stack-protector -fno-math-errno -fno-builtin -O2 -Wall -Wextra -Wno-unused-parameter
NETPORT ?= 0
# The IP stack is the i386 kernel's own drivers/net.c and drivers/http.c, built against drivers/nic.h (-DNET_NIC)
# instead of the PC network cards. -mstrict-align: a real board's firmware may leave alignment checks on.
NETCC = $(CC) -DNET_NIC -mstrict-align -I../../drivers -I../../kernel
NETOBJ = net.o http.o ip.o
all: kernel8.elf
net.o: ../../drivers/net.c ../../drivers/net.h ../../drivers/nic.h
	$(NETCC) -c $< -o $@
http.o: ../../drivers/http.c ../../drivers/http.h
	$(NETCC) -c $< -o $@
ip.o: ip.c ../../drivers/nic.h
	$(NETCC) -DNETPORT=$(NETPORT) -c $< -o $@
claude_cfg.h: claude_cfg.sh FORCE
	@./claude_cfg.sh
ask.o: ask.c claude_cfg.h ../../drivers/http.h ../../drivers/net.h
	$(NETCC) -c ask.c -o $@
pi-ask.o: ask.c claude_cfg.h ../../drivers/http.h ../../drivers/net.h
	$(NETCC) -DPI_BUILD -c ask.c -o $@
ttf.o: ../../drivers/ttf.c
	$(FPCC) -c $< -o $@
text.o: text.c ../../drivers/ttf.h
	$(FPCC) -c $< -o $@
libc.o: libc.c
	$(FPCC) -c $< -o $@
# The wallpaper: the shared PNG and inflate decoders (integer only; they want kmalloc and a few libc calls) plus wall.c, which scales the photo.
SHARED = -I../../kernel -I../../lib -I../../drivers
png.o: ../../drivers/png.c
	$(CC) $(SHARED) -c $< -o $@
inflate.o: ../../drivers/inflate.c
	$(CC) $(SHARED) -c $< -o $@
wall.o: wall.c ../../kernel/wall_sat.h
	$(CC) $(SHARED) -c $< -o $@
start.o: start.S
	$(CC) -c $< -o $@
user.o: user.S
	$(CC) -c $< -o $@
vectors.o: vectors.S
	$(CC) -c $< -o $@
main.o: main.c
	$(CC) -c $< -o $@
pci.o: pci.c
	$(CC) -c $< -o $@
xhci.o: xhci.c
	$(CC) -c $< -o $@
pi-pci.o: pci.c
	$(CC) -DPI_BUILD -c $< -o $@
wifi.o: wifi.c wifi_proto.h
	$(CC) -c -o $@ wifi.c
pi-wifi.o: wifi.c wifi_proto.h
	$(CC) -DPI_BUILD -c -o $@ wifi.c
wifi-fw:
	cd ../.. && tools/wifi-fw.sh
wifi_fw.S: wifi_fw_gen.sh wifi-fw
	./wifi_fw_gen.sh
wifi_fw.o: wifi_fw.S
	$(CC) -c -o $@ wifi_fw.S
pi-xhci.o: xhci.c
	$(CC) -DPI_BUILD -c $< -o $@
pi-start.o: start.S
	$(CC) -c $< -o $@
pi-main.o: main.c
	$(CC) -DPI_BUILD -c $< -o $@
kernel8.elf: start.o vectors.o user.o main.o pci.o xhci.o wifi.o ttf.o text.o libc.o ask.o png.o inflate.o wall.o $(NETOBJ) linker.ld
	ld.lld --defsym=KBASE=0x40000000 -T linker.ld -o $@ start.o vectors.o user.o main.o pci.o xhci.o wifi.o ttf.o text.o libc.o ask.o png.o inflate.o wall.o $(NETOBJ)
pi-kernel8.elf: pi-start.o vectors.o user.o pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o ttf.o text.o libc.o pi-ask.o png.o inflate.o wall.o $(NETOBJ) linker.ld
	ld.lld --defsym=KBASE=0x80000 -T linker.ld -o $@ pi-start.o vectors.o user.o pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o ttf.o text.o libc.o pi-ask.o png.o inflate.o wall.o $(NETOBJ)
kernel8.img: pi-kernel8.elf
	ld.lld --defsym=KBASE=0x80000 -T linker.ld --oformat binary -o $@ pi-start.o vectors.o user.o pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o ttf.o text.o libc.o pi-ask.o png.o inflate.o wall.o $(NETOBJ)
pi: wifi-fw kernel8.img
run: kernel8.elf
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256 -nographic -kernel kernel8.elf
run-pi: kernel8.img
	qemu-system-aarch64 -machine raspi4b -nographic -kernel kernel8.img
clean:
	rm -f *.o *.elf kernel8.img claude_cfg.h claude_cfg.h.tmp
FORCE:
.PHONY: wifi-fw FORCE
