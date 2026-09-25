CC := clang
CFLAGS := -target i386-unknown-none -ffreestanding -fno-stack-protector \
          -fno-pic -mno-sse -mno-mmx -Wall -Wextra -O2 \
          -Iboot -Ikernel -Idrivers -Ilib -MMD -MP
LD := ld.lld

KERNEL_SRCS := kernel/gdt.c kernel/idt.c kernel/pic.c kernel/irq.c kernel/pmm.c \
               kernel/paging.c kernel/kheap.c kernel/task.c kernel/exec.c kernel/ring3.c kernel/syscall.c kernel/kernel.c
KERNEL_ASM  := kernel/isr.S kernel/irq_stubs.S kernel/ring3_asm.S
DRIVER_SRCS := drivers/ata.c drivers/blockdev.c drivers/ramdisk.c drivers/trash.c drivers/fat.c drivers/vfs.c drivers/ramfs.c drivers/pci.c drivers/vbe.c drivers/mouse.c drivers/vmmouse.c \
               drivers/window.c drivers/rtl8139.c drivers/ne2k.c drivers/net.c drivers/http.c drivers/html.c \
               drivers/json.c drivers/font.c drivers/serial.c drivers/png.c drivers/jpeg.c drivers/ttf.c
LIB_SRCS    := lib/libc.c

OBJS := boot/boot.o $(KERNEL_ASM:.S=.o) $(KERNEL_SRCS:.c=.o) $(DRIVER_SRCS:.c=.o) $(LIB_SRCS:.c=.o)

kernel.elf: $(OBJS) boot/linker.ld
	$(LD) -m elf_i386 -T boot/linker.ld -o $@ $(OBJS)
	@cp kernel.elf landing/v86/kernel.elf
# Real recurring gap, hit three times in one night: a subagent bumps
# VERSION, builds, verifies, commits, and forgets `cp kernel.elf
# landing/v86/kernel.elf`, since it's a separate manual step CLAUDE.md
# only documents, never enforces. Copying it here, every build, means
# the working tree is always in sync before `git add` even runs, so
# the only way to still ship a stale landing copy is to `git add` a
# stale file over this fresh one, not just forget a step.

# Real build artifact, not a snapshot: regenerated from VERSION on every
# build (unlike png_testdata.h below, never committed, see .gitignore).
# Bumping VERSION now always produces a genuinely different kernel.elf,
# fixing a real CI false-fail: a source change that's only a comment (no
# compiled-output difference) left kernel.elf byte-identical to what was
# already committed, so landing/v86/kernel.elf's own sync-check could
# never be satisfied, "cp kernel.elf landing/v86/kernel.elf" was a no-op
# git saw as "nothing to commit" while the check kept demanding a new one.
drivers/version.h: VERSION
	printf '#define JT_VERSION_STR "%s"\n' "$$(cat VERSION)" > drivers/version.h

kernel/kernel.o: drivers/version.h

# Generated from a sibling repo (tools/gen/gen_app.sh). Committed as a snapshot as of
# v54 (they used to be gitignored): a fresh clone or an isolated agent
# sandbox has no sibling repos to regenerate from, and the first parallel
# agent to hit that had to hand-copy them to build at all. Same "committed
# copy of a build artifact" relationship landing/v86/kernel.elf already has.
# These rules still regenerate one if it's genuinely missing; rerun
# tools/gen/gen_app.sh by hand to pick up a changed source app, then commit the result.
drivers/app_weather.h:
	./tools/gen/gen_app.sh

drivers/app_curbfind.h:
	./tools/gen/gen_app.sh "$$HOME/Documents/Code/curbfind/web/index.html" drivers/app_curbfind.h app_curbfind

drivers/app_keyrate.h:
	./tools/gen/gen_app.sh "$$HOME/Documents/Code/keyrate/index.html" drivers/app_keyrate.h app_keyrate

drivers/app_bookrank.h:
	./tools/gen/gen_app.sh "$$HOME/Documents/Code/bookrank/index.html" drivers/app_bookrank.h app_bookrank

drivers/app_quotestreak.h:
	./tools/gen/gen_app.sh "$$HOME/Documents/Code/quotestreak/index.html" drivers/app_quotestreak.h app_quotestreak

kernel/kernel.o: drivers/app_weather.h drivers/app_curbfind.h drivers/app_keyrate.h drivers/app_bookrank.h drivers/app_quotestreak.h
kernel/kernel.o: kernel/editor.h drivers/editor_fonts.h drivers/png.h drivers/png_testdata.h drivers/jpeg.h drivers/jpeg_testdata.h

# v74: pngtest's fixtures are real PNGs cut from drivers/wallpaper.h; the
# generator also computes the host-side reference hashes. Committed like
# the app_*.h snapshots above, regenerated only if genuinely missing or
# after the wallpaper source itself changes.
drivers/png_testdata.h:
	python3 tools/gen/gen_png_testdata.py

# v76: jpegtest's fixtures are real photographic JPEGs, re-encoded via PIL
# at several quality/subsampling settings from the same wallpaper.h source
# photo png_testdata.h already crops from. Committed like the other
# generated headers, regenerated only if genuinely missing or after the
# wallpaper source changes.
drivers/jpeg_testdata.h:
	python3 tools/gen/gen_jpeg_testdata.py

# ---- Ring-3 user space ------------------------------------------------------
# user/ is not kernel code and is built with its own flags: no kernel
# include paths at all, so it physically cannot reach a kernel header, and
# nothing links against the kernel. Its only interface is user/jtsys.h's
# int 0x80 wrappers, which is the whole point -- docs/SYSCALL-ABI.md is the
# contract, and the reference program is a real external consumer of it,
# not a kernel file in a different directory.
#
# --oformat binary gives a headerless flat image (there is no objcopy in
# this toolchain and none is needed). user/hello.ld pins the link address
# to the window boot/linker.ld reserves.
USER_CFLAGS := -target i386-unknown-none -ffreestanding -fno-stack-protector \
               -fno-pic -mno-sse -mno-mmx -Wall -Wextra -Os -Iuser

user/hello.o: user/hello.c user/jtsys.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/hello.bin: user/hello.o user/hello.ld
	$(LD) -m elf_i386 -T user/hello.ld --oformat binary -o $@ user/hello.o

user/note.o: user/note.c user/jtsys.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/note.bin: user/note.o user/note.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/note.o

# libjt: a small static archive of userland library code (string/ctype/
# stdlib/stdio over jtsys.h), built with the same freestanding flags as
# every other ring-3 program so a program can link it in and get libc
# shaped calls without a real libc or a kernel include path. llvm-ar
# rather than plain `ar` because this toolchain is clang/lld throughout,
# see USER_CFLAGS above.
LIBJT_SRCS := user/libjt/string.c user/libjt/stdlib.c user/libjt/stdio.c
LIBJT_OBJS := $(LIBJT_SRCS:.c=.o)
AR := $(shell command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)

user/libjt/%.o: user/libjt/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/libjt.a: $(LIBJT_OBJS)
	$(AR) rcs $@ $(LIBJT_OBJS)

user/wc.o: user/wc.c user/jtsys.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/wc.bin: user/wc.o user/libjt.a user/note.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/wc.o user/libjt.a

# The built binaries, embedded so `usertest`/`notetest`/`shell` can seed
# them into the VFS on a machine with no disk (every headless check boot,
# and the browser embed).
drivers/user_hello.h: user/hello.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/hello.bin drivers/user_hello.h user_hello

drivers/user_note.h: user/note.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/note.bin drivers/user_note.h user_note

drivers/user_wc.h: user/wc.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/wc.bin drivers/user_wc.h user_wc

kernel/kernel.o: drivers/user_hello.h drivers/user_note.h drivers/user_wc.h

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

dotfiles.img:
	./tools/gen/sync_dotfiles.sh

run: kernel.elf dotfiles.img
	qemu-system-i386 -kernel kernel.elf -display cocoa,zoom-to-fit=on -rtc base=localtime -net nic,model=rtl8139 -net user -drive file=dotfiles.img,format=raw,if=ide,index=0

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) kernel.elf user/hello.o user/hello.bin drivers/user_hello.h \
	      user/note.o user/note.bin drivers/user_note.h
	rm -f joshuatree.iso
	rm -rf build/iso_root

# This machine has a global core.hooksPath (~/.git-hooks); this opts THIS
# repo into its own fast pre-push gate (tools/hooks/pre-push) instead.
hooks:
	git config core.hooksPath tools/hooks

# v75 (0.66.x): real gap found root-causing the reaptest bug (paging.h's
# PAGING_PRIVATE_PDE), the hard way -- a header-only edit left the stale
# .o linked in twice in a row, silently "fixing" nothing and then
# "reverting" nothing either, until `make clean` was used to force it.
# -MMD -MP above makes clang emit a real per-object .d dependency file
# (every header it actually #included, not a hand-maintained guess like
# kernel.o's two explicit lines above), included here so `make` sees a
# .h change and rebuilds exactly what depends on it, same as any normal
# C project's incremental build.
# ISO/USB image via Limine. Limine (not grub-mkrescue) because it ships
# prebuilt binaries (bios stage, uefi stubs, the `limine` deploy tool's
# source) that build with a plain host `cc`, on both this Mac (no
# grub-mkrescue, no brew installs allowed) and CI's ubuntu-24.04 runner
# alike -- one path instead of two. Fetched at build time into build/limine,
# gitignored, never committed. `xorriso` (already at /opt/homebrew/bin on
# this Mac, installed via apt on CI) does the actual ISO packing.
LIMINE_BRANCH := v9.x-binary
LIMINE_DIR := build/limine

$(LIMINE_DIR)/limine.h:
	rm -rf $(LIMINE_DIR)
	mkdir -p build
	git clone --depth 1 --branch $(LIMINE_BRANCH) https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)

$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.h
	$(MAKE) -C $(LIMINE_DIR)

# Multiboot1 needs no long-mode UEFI trampoline of its own; Limine's
# prebuilt BOOTX64.EFI/BOOTIA32.EFI do the UEFI side, its bios-cd stage
# does BIOS. Same kernel.elf either way, boot/boot.S never changes.
joshuatree.iso: kernel.elf $(LIMINE_DIR)/limine tools/iso/limine.conf
	rm -rf build/iso_root
	mkdir -p build/iso_root/boot/limine build/iso_root/EFI/BOOT
	cp kernel.elf build/iso_root/boot/kernel.elf
	cp tools/iso/limine.conf build/iso_root/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin build/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI build/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
	  -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	  --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label \
	  build/iso_root -o joshuatree.iso
	$(LIMINE_DIR)/limine bios-install joshuatree.iso

iso: joshuatree.iso

-include $(OBJS:.o=.d)

.PHONY: run clean hooks iso
