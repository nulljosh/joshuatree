# ARM64 port. Separate from the i386 Makefile on purpose: nothing in that build changes.
#   make -C arch/arm64            kernel8.elf for QEMU's virt machine (image at 0x40000000)
#   make -C arch/arm64 run        boots it headless and prints the UART
#   make -C arch/arm64 pi         kernel8.img for a Raspberry Pi 4 (image at 0x80000, UART at 0xFE201000)
#   make -C arch/arm64 run-pi     boots that image on QEMU's raspi4b model
#   make -C arch/arm64 NETPORT=8080   also POSTs to the host (10.0.2.2:8080) at boot, through the shared IP stack
# Test builds, never shipped (each is driven by a tools/checks/arm64-*-check.py; main.c and fptest.c hold the hooks):
#   crashtest   crash-kernel8.elf / crash-kernel8.img: takes a kernel data abort right after the screen is up
#   fpsave      fp-kernel8.elf: floating point registers held across timer interrupts whose handler wipes them
#   fpnosave    fpnosave-kernel8.elf: the same with vectors.S not saving them (the control: the check must fail)
#   oomtest     oom-kernel8.elf / oom-kernel8.img: HEAP_SIZE shrunk to OOM_HEAP bytes (make -B oomtest OOM_HEAP=n)
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
ttf.o: ../../drivers/ttf.c
	$(FPCC) -c $< -o $@
text.o: text.c ../../drivers/ttf.h
	$(FPCC) -c $< -o $@
libc.o: libc.c
	$(FPCC) -c $< -o $@
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
kernel8.elf: start.o vectors.o user.o main.o pci.o xhci.o wifi.o ttf.o text.o libc.o $(NETOBJ) linker.ld
	ld.lld --defsym=KBASE=0x40000000 -T linker.ld -o $@ start.o vectors.o user.o main.o pci.o xhci.o wifi.o ttf.o text.o libc.o $(NETOBJ)
pi-kernel8.elf: pi-start.o vectors.o user.o pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o ttf.o text.o libc.o $(NETOBJ) linker.ld
	ld.lld --defsym=KBASE=0x80000 -T linker.ld -o $@ pi-start.o vectors.o user.o pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o ttf.o text.o libc.o $(NETOBJ)
kernel8.img: pi-kernel8.elf
	ld.lld --defsym=KBASE=0x80000 -T linker.ld --oformat binary -o $@ pi-start.o vectors.o user.o pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o ttf.o text.o libc.o $(NETOBJ)
pi: wifi-fw kernel8.img
# Variant builds: $(1) names the output, $(2) the main.c flags, $(3) extra objects, $(4) the vectors.S flags.
define variant
$(1)-main.o: main.c
	$(CC) $(2) -c $$< -o $$@
$(1)-pi-main.o: main.c
	$(CC) -DPI_BUILD $(2) -c $$< -o $$@
$(1)-vectors.o: vectors.S
	$(CC) $(4) -c $$< -o $$@
$(1)-kernel8.elf: start.o $(1)-vectors.o user.o $(1)-main.o pci.o xhci.o wifi.o ttf.o text.o libc.o $(NETOBJ) $(3) linker.ld
	ld.lld --defsym=KBASE=0x40000000 -T linker.ld -o $$@ start.o $(1)-vectors.o user.o $(1)-main.o pci.o xhci.o wifi.o ttf.o text.o libc.o $(NETOBJ) $(3)
$(1)-pi-kernel8.elf: pi-start.o $(1)-vectors.o user.o $(1)-pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o ttf.o text.o libc.o $(NETOBJ) $(3) linker.ld
	ld.lld --defsym=KBASE=0x80000 -T linker.ld -o $$@ pi-start.o $(1)-vectors.o user.o $(1)-pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o ttf.o text.o libc.o $(NETOBJ) $(3)
$(1)-kernel8.img: $(1)-pi-kernel8.elf
	ld.lld --defsym=KBASE=0x80000 -T linker.ld --oformat binary -o $$@ pi-start.o $(1)-vectors.o user.o $(1)-pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o ttf.o text.o libc.o $(NETOBJ) $(3)
endef
fptest.o: fptest.c
	$(FPCC) -c $< -o $@
OOM_HEAP ?= 65536
$(eval $(call variant,crash,-DCRASH_TEST,,))
$(eval $(call variant,fp,-DFP_TEST,fptest.o,))
$(eval $(call variant,fpnosave,-DFP_TEST,fptest.o,-DFP_NOSAVE))
$(eval $(call variant,oom,-DOOM_TEST -DHEAP_SIZE=$(OOM_HEAP)UL,,))
crashtest: crash-kernel8.elf crash-kernel8.img
fpsave: fp-kernel8.elf
fpnosave: fpnosave-kernel8.elf
oomtest: oom-kernel8.elf oom-kernel8.img
run: kernel8.elf
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256 -nographic -kernel kernel8.elf
run-pi: kernel8.img
	qemu-system-aarch64 -machine raspi4b -nographic -kernel kernel8.img
clean:
	rm -f *.o *.elf *.img
.PHONY: wifi-fw crashtest fpsave fpnosave oomtest
