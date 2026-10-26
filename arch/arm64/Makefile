# ARM64 port. Separate from the i386 Makefile on purpose: nothing in that build changes.
#   make -C arch/arm64            kernel8.elf for QEMU's virt machine (image at 0x40000000)
#   make -C arch/arm64 run        boots it headless and prints the UART
#   make -C arch/arm64 pi         kernel8.img for a Raspberry Pi 4 (image at 0x80000, console on the mini UART at 0xFE215040)
#   make -C arch/arm64 run-pi     boots that image on QEMU's raspi4b model
#   make -C arch/arm64 NETPORT=8080   also POSTs to the host (10.0.2.2:8080) at boot, through the shared IP stack
#   make -C arch/arm64 BT_HCD=f.hcd   embeds that Bluetooth patch instead of build/bt-fw/BCM4345C0.hcd (bt.c)
# Test builds, never shipped (each is driven by a tools/checks/arm64-*-check.py; main.c and fptest.c hold the hooks):
#   crashtest   crash-kernel8.elf / crash-kernel8.img: takes a kernel data abort right after the screen is up
#   fpsave      fp-kernel8.elf: floating point registers held across timer interrupts whose handler wipes them
#   fpnosave    fpnosave-kernel8.elf: the same with vectors.S not saving them (the control: the check must fail)
#   oomtest     oom-kernel8.elf / oom-kernel8.img: HEAP_SIZE shrunk to OOM_HEAP bytes (make -B oomtest OOM_HEAP=n)
#   hovertest   hover-kernel8.elf / hover-kernel8.img: the dock's hover label over slot 3 (Calendar) at boot
#   cursortest  cursor-kernel8.elf / cursor-kernel8.img: the pointer's arrow over slot 3 at boot, with its hover label
#   apptest     app-kernel8.img: APP_NAME (default calendar) opened at boot on the Pi image, for arm64-appsize-check.py at 1080p
#   dragtest    drag-kernel8.elf / drag-kernel8.img: the Console dragged 100 right and 60 down at boot (clamped), for arm64-drag-check.py at 1080p
#   dirtyrecttest dirtyrect-kernel8.elf: the glitch build (1080p ramfb) plus a `fb: flush` line per window move
#   keydbgtest  keydbg-kernel8.elf: the dev card's key debug line on, plus made-up HID reports (a report ID) at boot
#   calctest    calc-kernel8.elf: the Calculator open at boot with four sums typed into it
#   caltest     cal-kernel8.elf: the clock fixed at CAL_UTC, so the Calendar tile shows that month with that day in the accent
#   sdtest      sd-kernel8.img: the Terminal's ls and cat run at boot on the check's card, then a read past the card's end
#   sdwtest     sdw-kernel8.img writes, replaces and deletes files in JT/ at boot and tries the boot files (refused), then a
#               CMD25 at the card's end; sdr-kernel8.img, booted next on the same card, reads them back and deletes them
#   notestest   notesw-kernel8.img opens Notes from the dock with made-up USB key reports, types a note and saves it;
#               notesr-kernel8.img, booted next on the same card, opens Notes and the note again; notesa-kernel8.img types a line,
#               leaves it for four seconds and clicks the red dot, with no network clock (arm64-notes-check.py)
# The Console's ask> prompt asks the Claude relay. claude_cfg.sh writes its address and token (gitignored) on every build
# from CLAUDE_RELAY_HOST, CLAUDE_RELAY_PORT and CLAUDE_RELAY_TOKEN_FILE (default ~/.claude-relay-token), but only when
# JT_WIFI_DEV=1 (tools/flash-pi.sh) or CLAUDE_RELAY_TOKEN_FILE is set explicitly (the checks); a release build has neither.
JT_VERSION := $(shell tr -d '\n' < ../../VERSION)   # the tree menu's About line (main.c)
CC = clang -target aarch64-none-elf -ffreestanding -fno-stack-protector -mgeneral-regs-only -O2 -Wall -Wextra -DJT_VERSION='"$(JT_VERSION)"'
# ttf.c, text.c and libc.c are the one part that uses floating point (the TrueType rasterizer), so they skip
# -mgeneral-regs-only; start.S turns the FPU on at EL1. Everything else stays integer-only.
FPCC = clang -target aarch64-none-elf -ffreestanding -fno-stack-protector -fno-math-errno -fno-builtin -O2 -Wall -Wextra -Wno-unused-parameter
NETPORT ?= 0
# The IP stack is the i386 kernel's own drivers/net.c and drivers/http.c, built against drivers/nic.h (-DNET_NIC)
# instead of the PC network cards. -mstrict-align: a real board's firmware may leave alignment checks on.
NETCC = $(CC) -DNET_NIC -mstrict-align -I../../drivers -I../../kernel
NETOBJ = rng.o net.o http.o ip.o clock.o tls.o browser.o nimble.o $(BSSLOBJ)
NETLD = rng.o net.o http.o ip.o clock.o tls.o browser.o nimble.o --start-lib $(BSSLOBJ) --end-lib   # lld pulls only the BearSSL objects tls.c uses
all: kernel8.elf
rng.o: rng.c rng.h
	$(CC) -c $< -o $@
pi-rng.o: rng.c rng.h
	$(CC) -DPI_BUILD -c $< -o $@
net.o: ../../drivers/net.c ../../drivers/net.h ../../drivers/nic.h
	$(NETCC) -c $< -o $@
http.o: ../../drivers/http.c ../../drivers/http.h
	$(NETCC) -c $< -o $@
# TLSPORT=n: at boot, GET https://10.0.2.2:n/hello and print the result (tools/checks/arm64-tls-check.py). 0 skips it.
TLSPORT ?= 0
BUILD_UTC ?= $(shell date +%s)
clock.o: clock.c tls.h ../../drivers/net.h ../../drivers/http.h
	$(NETCC) -DBUILD_UTC=$(BUILD_UTC)UL -c $< -o $@
ip.o: ip.c ../../drivers/nic.h tls.h
	$(NETCC) -DNETPORT=$(NETPORT) -DTLSPORT=$(TLSPORT) -c $< -o $@
# HTTPS (tls.c) over BearSSL. The trust anchors are generated from certs/*.pem (plus TLS_TA=extra.pem, the check's
# throwaway server cert, or the relay's private CA) into tls_ta.h, gitignored; nothing is fetched at run time. The
# Claude relay is trusted only through the TLS_TA anchors, never a public root. The whole vendored subset goes
# behind --start-lib so the link pulls only what tls.c uses.
TLS_TA ?=
BSSL = ../../third_party/bearssl
BSSLCC = $(CC) -mstrict-align -Wno-unused-parameter -DBR_INT128=0 -DBR_UMUL128=0 -I$(BSSL)/inc -I$(BSSL)/src -I$(BSSL)/shim -I../../lib
BSSLOBJ = $(patsubst $(BSSL)/src/%.c,bearssl/%.o,$(wildcard $(BSSL)/src/*.c))
bearssl/%.o: $(BSSL)/src/%.c $(BSSL)/src/inner.h $(BSSL)/src/config.h
	@mkdir -p bearssl
	$(BSSLCC) -c $< -o $@
tls_ta.h: ../../tools/gen/tls-ta.py $(wildcard certs/*.pem) $(TLS_TA)
	python3 ../../tools/gen/tls-ta.py $@ $(wildcard certs/*.pem) --private $(TLS_TA)
# Nimble's answer API (nimble.c). NIMBLE_BASE= points it at a stub on the host for tools/checks/arm64-nimble-check.py only; FORCE
# rebuilds this one object on every make, so a stub address can never survive into a build made without it.
NIMBLE_BASE ?=
nimble.o: nimble.c tls.h ../../drivers/net.h ../../drivers/http.h FORCE
	$(NETCC) $(if $(NIMBLE_BASE),-DNIMBLE_BASE='"$(NIMBLE_BASE)"') -c $< -o $@
browser.o: browser.c tls.h ../../drivers/net.h ../../drivers/http.h
	$(NETCC) -c $< -o $@
tls.o: tls.c rng.h tls.h tls_ta.h ../../drivers/net.h ../../drivers/http.h
	$(NETCC) -I$(BSSL)/inc -I$(BSSL)/shim -I../../lib -c tls.c -o $@
claude_cfg.h: claude_cfg.sh FORCE
	@./claude_cfg.sh
wifi_cfg.h: wifi_cfg.sh FORCE
	@./wifi_cfg.sh
ask.o: ask.c home.h claude_cfg.h tls.h ../../drivers/http.h ../../drivers/net.h
	$(NETCC) -c ask.c -o $@
# A Pi image carries the relay token only on the dev card (JT_WIFI_DEV=1, the same flag that carries the Wi-Fi key);
# a release build has no relay config at all. tools/checks/pi-release-notoken-check.sh proves it. QEMU builds are unchanged.
pi-ask.o: ask.c home.h claude_cfg.h tls.h ../../drivers/http.h ../../drivers/net.h
	$(NETCC) -DPI_BUILD $(if $(filter 1,$(JT_WIFI_DEV)),,-DCLAUDE_RELEASE) -c ask.c -o $@
ttf.o: ../../drivers/ttf.c
	$(FPCC) -c $< -o $@
text.o: text.c ../../drivers/ttf.h ../../lib/text_ink.h
	$(FPCC) -c $< -o $@
libc.o: libc.c
	$(FPCC) -c $< -o $@
# The Calculator's evaluator: the FPU does the sums, so it is built like text.c; main.c draws its window.
cmd.o: cmd.c snap.h bt.h emmc.h accounts.h sudo.h login.h home.h ../../drivers/fat.h
	$(CC) -c $< -o $@
calc.o: calc.c
	$(FPCC) -c $< -o $@
# Sound out of the 3.5 mm jack (PWM1, polled). Only the Pi image touches the registers; the virt build has none.
audio.o: audio.c audio.h
	$(CC) -c $< -o $@
pi-audio.o: audio.c audio.h
	$(CC) -DPI_BUILD -c $< -o $@
# The local language model (llm.c, llama2.c's forward pass). LLM_MODEL= and LLM_TOK= name a llama2.c checkpoint and
# tokenizer.bin to bake into the image (llm_model.S); without them `llm` says there is no model. llm.cfg holds the last
# values, so changing them rebuilds. LLM_CFLAGS=-DLLM_BREAK is the check's broken control build.
LLMOBJ = llm.o llm_model.o
LLMCFG = $(abspath $(LLM_MODEL)) $(abspath $(LLM_TOK)) $(LLM_CFLAGS)
llm.cfg: FORCE
	@echo '$(LLMCFG)' | cmp -s - $@ || echo '$(LLMCFG)' > $@
llm.o: llm.c llm.cfg
	$(FPCC) -mstrict-align $(LLM_CFLAGS) -c $< -o $@
llm_model.o: llm_model.S llm.cfg $(LLM_MODEL) $(LLM_TOK)
	$(CC) $(if $(LLM_MODEL),-DLLM_MODEL='"$(abspath $(LLM_MODEL))"') $(if $(LLM_TOK),-DLLM_TOK='"$(abspath $(LLM_TOK))"') -c $< -o $@
# The wallpaper: the shared PNG and inflate decoders (integer only; they want kmalloc and a few libc calls) plus wall.c, which scales the photo.
SHARED = -I../../kernel -I../../lib -I../../drivers
png.o: ../../drivers/png.c
	$(CC) $(SHARED) -c $< -o $@
inflate.o: ../../drivers/inflate.c
	$(CC) $(SHARED) -c $< -o $@
jpeg.o: ../../drivers/jpeg.c ../../drivers/jpeg.h   # 6.14.0: Blackcomb's inline JPEGs (Movies has its own ring-3 copy)
	$(CC) $(SHARED) -c $< -o $@
wall.o: wall.c ../../kernel/wall_sat.h
	$(CC) $(SHARED) -c $< -o $@
# The dock: the i386 desktop's own painters and dock layout, the same source files, drawn through main.c's window shim.
GUIOBJ = gui_paint.o dock_geom.o
gui_paint.o: ../../kernel/gui_paint.c ../../kernel/gui_paint.h ../../kernel/dock_geom.h
	$(CC) $(SHARED) -c $< -o $@
dock_geom.o: ../../kernel/dock_geom.c ../../kernel/dock_geom.h
	$(CC) $(SHARED) -c $< -o $@
# The SD card: emmc.c (EMMC2 on the Pi, the virtio disk on virt) and the i386 kernel's own FAT code, drivers/fat.c,
# reading and writing through emmc.c's blockdev_read_sector and blockdev_write_sector. -mstrict-align, as for
# drivers/net.c. The Pi writes only inside the card's JT folder, never a boot file. ufile.c (M3d) is the apps' file
# calls over the same code, rooted in JT/.
SDCC = $(CC) -mstrict-align -I../../drivers -I../../kernel -I../../lib
SDOBJ = emmc.o fat.o ufile.o settings.o auth_kdf.o accounts.o login.o sudo.o home.o snap.o
PISDOBJ = pi-emmc.o fat.o ufile.o settings.o auth_kdf.o accounts.o login.o sudo.o home.o snap.o
fat.o: ../../drivers/fat.c ../../drivers/fat.h ../../drivers/blockdev.h
	$(SDCC) -c $< -o $@
ufile.o: ufile.c home.h emmc.h ../../drivers/fat.h
	$(SDCC) -c $< -o $@
# Settings (3.13.0): the choices, JT/SETTINGS.TXT and the version the About page shows, read from VERSION at build time.
settings.o: settings.c audio.h emmc.h ../../drivers/fat.h ../../VERSION
	$(SDCC) -DJT_VERSION='"$(shell cat ../../VERSION)"' -c $< -o $@
# Accounts (4.8.0): the i386 kernel's own PBKDF2 over BearSSL (already linked through --start-lib) and the Pi's account store.
auth_kdf.o: ../../kernel/auth_kdf.c ../../kernel/auth_kdf.h
	$(SDCC) -I$(BSSL)/inc -I$(BSSL)/shim -c $< -o $@
accounts.o: accounts.c accounts.h rng.h emmc.h ../../drivers/fat.h ../../kernel/auth_kdf.h
	$(SDCC) -c $< -o $@
# The login form (4.9.0): its keys and the wrong-password delay; the screen is login_ui.h.
login.o: login.c login.h accounts.h
	$(SDCC) -c $< -o $@
# Admins and sudo (4.10.0): JT/SUDOERS.TXT, the password check and the five minutes it is remembered.
sudo.o: sudo.c sudo.h accounts.h login.h emmc.h ../../drivers/fat.h
	$(SDCC) -c $< -o $@
# Home folders (4.11.0): JT/HOME/<name>, where apps, ~ and Samantha's save put a signed-in person's files.
home.o: home.c home.h accounts.h sudo.h login.h emmc.h ../../drivers/fat.h
	$(SDCC) -c $< -o $@
snap.o: snap.c snap.h home.h accounts.h login.h emmc.h ../../drivers/fat.h
	$(SDCC) -c $< -o $@
emmc.o: emmc.c emmc.h ../../drivers/fat.h
	$(SDCC) -c $< -o $@
pi-emmc.o: emmc.c emmc.h ../../drivers/fat.h
	$(SDCC) -DPI_BUILD -c $< -o $@
start.o: start.S
	$(CC) -c $< -o $@
user.o: user.S
	$(CC) -c $< -o $@
# M3b and M3c: the user programs. Each one is the unchanged user/NAME.c (the source the i386 build runs in ring 3) built for
# EL0: position independent (-fPIE, and -fdirect-access-external-data so an extern such as calendar's _user_end is adrp/add
# and not a GOT slot), linked -pie at 0 by user_c.ld into one flat image, with the pointer tables it needs relocated by the
# kernel's loader. They reach the kernel through jtsys.h's aarch64 `svc #0` path with the Joshua Tree numbers. libjt is
# built here too, under arch/arm64 and under other names (text.o is the kernel's TTF blender), never next to its i386
# objects in user/libjt; --start-lib pulls only the members a program uses (calendar: string.c and text.c; notes: text.c
# and osk.c; music, M3e: string.c, text.c, stdlib.c's malloc over JT_SYS_BRK, wav.c, mp3.c, and jpeg.c and png.c for album art). -Os keeps images small.
# mp3.c (minimp3) does its sums in floating point, so it alone is built without -mgeneral-regs-only (UFPCC); EL0 may use
# the FPU and vectors.S keeps an app's FP registers across every call and park. The kernel stays integer-only.
# userapps.S bakes the images in with .incbin and lists them; a new app is a name in UAPPS and a row there.
UAPPS = hello calendar notes music books finder mail weather stocks reminders mines toroid hamurabi keyrate solitaire paddle bricks depths snake chess movies
USEROBJ = user.o userapps.o
UCC = clang -target aarch64-none-elf -ffreestanding -fno-stack-protector -mgeneral-regs-only -mstrict-align -fPIE -fdirect-access-external-data -fno-builtin -fno-asynchronous-unwind-tables -Os -Wall -Wextra -I../../user
UFPCC = $(filter-out -mgeneral-regs-only,$(UCC))
UJT = ../../user/libjt
UJTOBJ = ujt_string.o ujt_text.o ujt_osk.o ujt_stdlib.o ujt_wav.o ujt_mp3.o ujt_jpeg.o ujt_png.o ujt_avi.o
ujt_string.o: $(UJT)/string.c $(UJT)/string.h
	$(UCC) -c $< -o $@
ujt_text.o: $(UJT)/text.c $(UJT)/text.h $(UJT)/aafont.h ../../user/jtsys.h
	$(UCC) -c $< -o $@
ujt_osk.o: $(UJT)/osk.c $(UJT)/osk.h $(UJT)/text.h ../../user/jtsys.h
	$(UCC) -c $< -o $@
ujt_stdlib.o: $(UJT)/stdlib.c $(UJT)/stdlib.h $(UJT)/ctype.h $(UJT)/string.h ../../user/jtsys.h
	$(UCC) -c $< -o $@
ujt_wav.o: $(UJT)/wav.c $(UJT)/wav.h
	$(UCC) -c $< -o $@
ujt_mp3.o: $(UJT)/mp3.c $(UJT)/mp3.h $(UJT)/stdlib.h $(UJT)/string.h ../../third_party/minimp3/minimp3.h
	$(UFPCC) -I../../third_party/minimp3 -c $< -o $@
ujt_png.o: $(UJT)/png.c ../../drivers/png.c ../../drivers/inflate.c ../../drivers/png.h ../../drivers/inflate.h $(UJT)/stdlib.h $(UJT)/string.h
	$(UCC) -c $< -o $@
ujt_jpeg.o: $(UJT)/jpeg.c ../../drivers/jpeg.c ../../drivers/jpeg.h $(UJT)/stdlib.h $(UJT)/string.h
	$(UCC) -c $< -o $@
ujt_avi.o: $(UJT)/avi.c $(UJT)/avi.h   # 6.1.0: Movies' AVI reader
	$(UCC) -c $< -o $@
# (the recipe line below was missing: make then built i386's weather.c through the pattern rule)
uapp_weather.o: ../../user/weather_pi.c ../../user/jtsys.h $(UJT)/text.h $(UJT)/hires.h
	$(UCC) -c $< -o $@
uapp_stocks.o: ../../user/stocks_pi.c ../../user/jtsys.h $(UJT)/text.h
	$(UCC) -c $< -o $@
uapp_%.o: ../../user/%.c ../../user/jtsys.h $(UJT)/text.h $(UJT)/osk.h $(UJT)/hires.h
	$(UCC) -c $< -o $@
# 3.17.0: the app called reminders is user/remindpi.c (the Pi's macOS-shaped list), not user/reminders.c, which is the i386 one; this rule beats the pattern above.
uapp_reminders.o: ../../user/remindpi.c ../../user/jtsys.h $(UJT)/text.h
	$(UCC) -c $< -o $@
uapp_music.o: $(UJT)/wav.h $(UJT)/mp3.h $(UJT)/stdlib.h $(UJT)/string.h $(UJT)/mascot.h mascot_art.h
uapp_books.o: $(UJT)/mascot.h mascot_art.h
# Mail (3.15.0) is the three-pane user/mailpi.c, not i386's user/mail.c: the app is still called mail.
uapp_mail.o: ../../user/mailpi.c ../../user/jtsys.h $(UJT)/text.h
	$(UCC) -c $< -o $@
# notes.c finds its arena with (unsigned)_user_end, a 32-bit cast that is still exact here: the app window sits below 4 GiB.
uapp_notes.o: UCC += -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
uapp_%.bin: uapp_%.o $(UJTOBJ) user_c.ld
	ld.lld -pie --no-dynamic-linker -z norelro -T user_c.ld --oformat binary -o $@ $< --start-lib $(UJTOBJ) --end-lib
	@test "$$(tail -c 16 $@ | head -c 4)" = JTU1 || { rm -f $@; echo "$@: the JTU1 trailer is not the last 16 bytes (this linker added sections after it)"; exit 1; }
userapps.o: userapps.S $(UAPPS:%=uapp_%.bin)
	$(CC) -c userapps.S -o $@
vectors.o: vectors.S
	$(CC) -c $< -o $@
# main.c is being split (4.23.2 on): MAINSRC is main.c and the files cut out of it. Every build that compiles main.c
# compiles each of them with the same flags (a test build's -D, PI_BUILD, MAINFLAGS) and links them into the one
# object (ld -r) the image links, so the split never changes what a test build turns on, nor any link line.
MAINSRC = main.c virtio.c boot.c desk.c keys.c windows.c appwin.c pointer.c crash.c console.c eltests.c
# Each rule sets MAINFLAGS_PART (a target-specific variable, so flags holding commas, like APP_NAME=a,b,c, survive)
# and runs MAINLINK.
MAINLINK = set -e; for f in $(MAINSRC); do $(CC) $(MAINFLAGS_PART) -c $$f -o $@.$${f%.c}.o; done; ld.lld -r -o $@ $(patsubst %.c,$@.%.o,$(MAINSRC))
MAINDEP = ../../VERSION virtio.h kmain.h settings_ui.h users_ui.h login_ui.h login.h sudo.h glyphs.h screen_idle.h bt.h notice.h brick.h brick_ui.h browser_ui.h mascot_art.h face_art.h power.h clock_ui.h audio.h ../../kernel/gui_paint.h ../../kernel/dock_geom.h ../../kernel/icon_art.h
main.o: MAINFLAGS_PART = $(MAINFLAGS)
main.o: $(MAINSRC) $(MAINDEP)
	$(MAINLINK)
pci.o: pci.c
	$(CC) -c $< -o $@
xhci.o: xhci.c
	$(CC) -c $< -o $@
pi-pci.o: pci.c
	$(CC) -DPI_BUILD -c $< -o $@
wifi.o: wifi.c wifi_proto.h
	$(CC) -c -o $@ wifi.c
pi-wifi.o: wifi.c wifi_proto.h wifi_cfg.h wpa.h
	$(CC) -DPI_BUILD -c -o $@ wifi.c
wifi-fw:
	cd ../.. && tools/wifi-fw.sh
wifi_fw.S: wifi_fw_gen.sh wifi-fw
	./wifi_fw_gen.sh
wifi_fw.o: wifi_fw.S
	$(CC) -c -o $@ wifi_fw.S
pi-xhci.o: xhci.c
	$(CC) -DPI_BUILD -c $< -o $@
# Bluetooth (bt.c, slices BT1 and BT2): HCI over the PL011, then the mouse over LE. The BCM4345C0 patch, BCM4345C0.hcd from
# RPi-Distro/bluez-firmware, is fetched by tools/bt-fw.sh into build/bt-fw/ (gitignored, never committed, LICENSE.cypress
# alongside) and embedded by bt_fw.S. BT_HCD= names another file (tools/checks/arm64-bt-check.py passes a small made-up
# one); with no file at all the radio runs its own ROM firmware. bt_fw.S is rewritten only when the path or the file's
# presence changes. Pairing's AES-128 is BearSSL's aes_ct, pulled from the same --start-lib objects tls.c uses.
BT_HCD ?= ../../build/bt-fw/BCM4345C0.hcd
bt.o: bt.c bt.h rng.h btpair.h emmc.h ../../drivers/fat.h
	$(CC) -I$(BSSL)/inc -c $< -o $@
pi-bt.o: bt.c bt.h rng.h btpair.h emmc.h ../../drivers/fat.h
	$(CC) -DPI_BUILD $(PICON) -I$(BSSL)/inc -c $< -o $@
bt-fw:
	cd ../.. && tools/bt-fw.sh
bt_fw.S: bt_fw_gen.sh FORCE
	@./bt_fw_gen.sh $(BT_HCD)
bt_fw.o: bt_fw.S $(wildcard $(BT_HCD))
	$(CC) -c -o $@ bt_fw.S
pi-start.o: start.S
	$(CC) -c $< -o $@
# JT_KEY_DEBUG=1 turns on the key debug line in the menu bar (main.c KEY_DEBUG). It used to ride on JT_WIFI_DEV=1, so the dev card showed raw reports; release builds and the dev card never do now.
# PICON: every Pi image prints its console on the mini UART (GPIO 14/15, ALT5); the PL011 is the Bluetooth radio's (bt.c).
PICON = -DCONSOLE_MINI_UART
pi-main.o: MAINFLAGS_PART = -DPI_BUILD $(PICON) $(if $(filter 1,$(JT_KEY_DEBUG)),-DKEY_DEBUG)
pi-main.o: $(MAINSRC) $(MAINDEP)
	$(MAINLINK)
kernel8.elf: start.o vectors.o $(USEROBJ) main.o pci.o xhci.o wifi.o bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(SDOBJ) audio.o ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(NETOBJ) $(LLMOBJ) linker.ld
	ld.lld --defsym=KBASE=0x40000000 -T linker.ld -o $@ start.o vectors.o $(USEROBJ) main.o pci.o xhci.o wifi.o bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(SDOBJ) audio.o ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(NETLD) $(LLMOBJ)
pi-kernel8.elf: pi-start.o vectors.o $(USEROBJ) pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o pi-bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(PISDOBJ) pi-audio.o pi-ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(filter-out rng.o,$(NETOBJ)) pi-rng.o $(LLMOBJ) linker.ld
	ld.lld --defsym=KBASE=0x80000 -T linker.ld -o $@ pi-start.o vectors.o $(USEROBJ) pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o pi-bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(PISDOBJ) pi-audio.o pi-ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(filter-out rng.o,$(NETLD)) pi-rng.o $(LLMOBJ)
kernel8.img: pi-kernel8.elf
	ld.lld --defsym=KBASE=0x80000 -T linker.ld --oformat binary -o $@ pi-start.o vectors.o $(USEROBJ) pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o pi-bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(PISDOBJ) pi-audio.o pi-ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(filter-out rng.o,$(NETLD)) pi-rng.o $(LLMOBJ)
pi: wifi-fw bt-fw kernel8.img
# Variant builds: $(1) names the output, $(2) the main.c flags, $(3) extra objects, $(4) the vectors.S flags.
define variant
$(1)-main.o: MAINFLAGS_PART = $(2)
$(1)-main.o: $(MAINSRC) $(MAINDEP)
	$$(MAINLINK)
$(1)-pi-main.o: MAINFLAGS_PART = -DPI_BUILD $(PICON) $(2)
$(1)-pi-main.o: $(MAINSRC) $(MAINDEP)
	$$(MAINLINK)
$(1)-vectors.o: vectors.S
	$(CC) $(4) -c $$< -o $$@
$(1)-kernel8.elf: start.o $(1)-vectors.o $(USEROBJ) $(1)-main.o pci.o xhci.o wifi.o bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(SDOBJ) audio.o ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(NETOBJ) $(3) $(LLMOBJ) linker.ld
	ld.lld --defsym=KBASE=0x40000000 -T linker.ld -o $$@ start.o $(1)-vectors.o $(USEROBJ) $(1)-main.o pci.o xhci.o wifi.o bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(SDOBJ) audio.o ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(NETLD) $(3) $(LLMOBJ)
$(1)-pi-kernel8.elf: pi-start.o $(1)-vectors.o $(USEROBJ) $(1)-pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o pi-bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(PISDOBJ) pi-audio.o pi-ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(filter-out rng.o,$(NETOBJ)) pi-rng.o $(3) $(LLMOBJ) linker.ld
	ld.lld --defsym=KBASE=0x80000 -T linker.ld -o $$@ pi-start.o $(1)-vectors.o $(USEROBJ) $(1)-pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o pi-bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(PISDOBJ) pi-audio.o pi-ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(filter-out rng.o,$(NETLD)) pi-rng.o $(3) $(LLMOBJ)
$(1)-kernel8.img: $(1)-pi-kernel8.elf
	ld.lld --defsym=KBASE=0x80000 -T linker.ld --oformat binary -o $$@ pi-start.o $(1)-vectors.o $(USEROBJ) $(1)-pi-main.o pi-pci.o pi-xhci.o pi-wifi.o wifi_fw.o pi-bt.o bt_fw.o ttf.o text.o libc.o calc.o cmd.o $(PISDOBJ) pi-audio.o pi-ask.o png.o jpeg.o inflate.o wall.o $(GUIOBJ) $(filter-out rng.o,$(NETLD)) pi-rng.o $(3) $(LLMOBJ)
endef
fptest.o: fptest.c
	$(FPCC) -c $< -o $@
OOM_HEAP ?= 65536
$(eval $(call variant,crash,-DCRASH_TEST,,))
$(eval $(call variant,fp,-DFP_TEST,fptest.o,))
$(eval $(call variant,fpnosave,-DFP_TEST,fptest.o,-DFP_NOSAVE))
$(eval $(call variant,oom,-DOOM_TEST -DHEAP_SIZE=$(OOM_HEAP)UL,,))
$(eval $(call variant,hover,-DDOCK_HOVER_TEST=3,,))
$(eval $(call variant,cursor,-DCURSOR_TEST,,))
$(eval $(call variant,drag,-DDRAG_TEST,,))
APP_NAME ?= calendar
$(eval $(call variant,app,-DAPP_BOOT=\"$(APP_NAME)\",,))
$(eval $(call variant,wingate,-DWIN_GATE,,))
$(eval $(call variant,hang,-DHANG_TEST,,))
hangtest: hang-kernel8.elf
wintest: wingate-kernel8.img
apptest: app-kernel8.img
$(eval $(call variant,brtrace,-DBR_TRACE,,))
brtracetest: brtrace-kernel8.elf
$(eval $(call variant,keydbg,-DKEY_DEBUG -DKEY_SELFTEST,,))
crashtest: crash-kernel8.elf crash-kernel8.img
fpsave: fp-kernel8.elf
fpnosave: fpnosave-kernel8.elf
oomtest: oom-kernel8.elf oom-kernel8.img
hovertest: hover-kernel8.elf hover-kernel8.img
cursortest: cursor-kernel8.elf cursor-kernel8.img
dragtest: drag-kernel8.elf drag-kernel8.img
keydbgtest: keydbg-kernel8.elf
$(eval $(call variant,glitch,-DFB_BOOT_W=1920 -DFB_BOOT_H=1080 -DHEAP_SIZE=40UL*1024*1024,,))
glitchtest: glitch-kernel8.elf
# 4.3.0: the glitch build plus FB_TRACE's `fb: flush` line per window move (arm64-dirtyrect-check.py)
$(eval $(call variant,dirtyrect,-DFB_BOOT_W=1920 -DFB_BOOT_H=1080 -DHEAP_SIZE=40UL*1024*1024 -DFB_TRACE,,))
dirtyrecttest: dirtyrect-kernel8.elf
$(eval $(call variant,calc,-DCALC_TEST,,))
calctest: calc-kernel8.elf
CAL_UTC ?= 1775000000
$(eval $(call variant,cal,-DCAL_TEST_UTC=$(CAL_UTC)UL,,))
caltest: cal-kernel8.elf
$(eval $(call variant,sd,-DSD_SELFTEST=1,,))
sdtest: sd-kernel8.img
$(eval $(call variant,sdw,-DSD_SELFTEST=2,,))
$(eval $(call variant,sdr,-DSD_SELFTEST=3,,))
sdwtest: sdw-kernel8.img sdr-kernel8.img
$(eval $(call variant,notesw,-DNOTES_SELFTEST=1,,))
$(eval $(call variant,notesr,-DNOTES_SELFTEST=2,,))
$(eval $(call variant,notesa,-DNOTES_SELFTEST=5,,))
notestest: notesw-kernel8.img notesr-kernel8.img notesa-kernel8.img
$(eval $(call variant,remw,-DNOTES_SELFTEST=3,,))
$(eval $(call variant,remr,-DNOTES_SELFTEST=4,,))
reminderstest: remw-kernel8.img remr-kernel8.img
run: kernel8.elf
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256 -nographic -kernel kernel8.elf
run-pi: kernel8.img
	qemu-system-aarch64 -machine raspi4b -nographic -serial null -serial mon:stdio -kernel kernel8.img   # serial 0 is the PL011 (the radio's), serial 1 the mini UART console
clean:
	rm -rf *.o *.a *.elf *.img hello_c.bin uapp_*.bin claude_cfg.h claude_cfg.h.tmp tls_ta.h bearssl llm.cfg bt_fw.S bt_fw.S.tmp
FORCE:
.PHONY: glitchtest dirtyrecttest brtracetest wifi-fw bt-fw FORCE crashtest fpsave fpnosave oomtest hovertest cursortest dragtest apptest keydbgtest calctest caltest sdtest sdwtest notestest reminderstest

# Power checks invoke the real keyboard confirmation and Pi watchdog.
$(eval $(call variant,poweroff,-DPOWER_SELFTEST=1,,))
$(eval $(call variant,restart,-DPOWER_SELFTEST=2,,))

# power-check: a file left unsaved, `reboot`, the file read back, `shutdown`; the second build skips the flush and must lose the file.
$(eval $(call variant,powerfile,-DPOWER_FILE_TEST=1,,))
$(eval $(call variant,powernoflush,-DPOWER_FILE_TEST=1 -DPOWER_NOFLUSH,,))
$(eval $(call variant,treerestart,-DTREEMENU_SELFTEST=1,,))
$(eval $(call variant,treeshutdown,-DTREEMENU_SELFTEST=2,,))
$(eval $(call variant,notice,-DNOTICE_SELFTEST,,))
$(eval $(call variant,screen,-DSCREEN_SELFTEST,,))
$(eval $(call variant,sudoshort,-DSUDO_TTL_MS=12000,,))   # sudo remembers a password for 12 s, for tools/checks/arm64-sudo-check.py


# audiotest: runs `beep` at boot on virt, which has no PWM, so arm64-audio-check.py reads back the tone's ring checksum.
$(eval $(call variant,audio,-DAUDIO_TEST,,))
audiotest: audio-kernel8.elf
.PHONY: audiotest
# usbaudiotest: a USB speaker (QEMU's usb-audio) is fed 440 Hz through isochronous OUT transfers at boot; arm64-usbaudio-check.py reads the WAV QEMU wrote.
$(eval $(call variant,usbaudio,-DUSB_AUDIO_TEST,,))
usbaudiotest: usbaudio-kernel8.elf
.PHONY: usbaudiotest
# heartest: voice mode sends half a second of a tone instead of recording (virt has no microphone); arm64-hear-check.py stubs the words service.
$(eval $(call variant,hear,-DHEAR_TEST,,))
heartest: hear-kernel8.elf
.PHONY: heartest
# musictest: Music's clock (JT_SYS_TIME) ticks from MUSIC_UTC with the generic counter, so its `music: at` lines come once a
# second on virt, which has no network time, and fptest.c's fp_clobber wipes the FPU inside every app call and before every
# resume, so `userrun fpkeep` proves vectors.S keeps an app's FP registers (tools/checks/arm64-music-check.py).
MUSIC_UTC ?= 1775000000
$(eval $(call variant,music,-DMUSIC_TEST_UTC=$(MUSIC_UTC)UL,fptest.o,))
musictest: music-kernel8.elf
.PHONY: musictest
